/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_sync.h"

#include "purple/purple_playlists.h"
#include "purple/purple_playlists_codec.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_sync_chat.h"
#include "purple/purple_sync_inventory.h"
#include "purple/purple_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QString>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include <zlib.h>

#ifdef PURPLE_PLAYLISTS_RECORDS_EXTRACT_SETTINGS
namespace Purple {

// EXTRACT_SYNC_INVENTORY_START
QString SyncRecordCaptionTag() {
	return u"#purplesync"_q;
}

QString SyncSettingsRecordFileName() {
	return u"Purple settings sync.json"_q;
}

QString SyncPlaylistsRecordFileName() {
	return u"Purple playlists sync.json"_q;
}

bool IsSyncHistoryCandidate(const SyncHistoryMessageMeta &meta) {
	if (!meta.isMessage || meta.forwarded || !meta.isDocument) {
		return false;
	}
	if (meta.caption.contains(SyncRecordCaptionTag())) {
		return true;
	}
	for (const auto &name : meta.fileNames) {
		if (name == SyncSettingsRecordFileName()
			|| name == SyncPlaylistsRecordFileName()) {
			return true;
		}
	}
	return false;
}
// EXTRACT_SYNC_INVENTORY_END

} // namespace Purple
#endif // PURPLE_PLAYLISTS_RECORDS_EXTRACT_SETTINGS

namespace {

using namespace Purple;

int Checks = 0;
int Failures = 0;

void Check(bool ok, const char *expr, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, expr);
	}
}

#define CHECK(cond) Check(bool(cond), #cond, __LINE__)

[[nodiscard]] QByteArray DeflateGzip(
		const QByteArray &plain,
		int level = Z_DEFAULT_COMPRESSION,
		gz_header *header = nullptr) {
	z_stream strm{};
	if (deflateInit2(
			&strm,
			level,
			Z_DEFLATED,
			16 + MAX_WBITS,
			8,
			Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	if (header != nullptr && deflateSetHeader(&strm, header) != Z_OK) {
		deflateEnd(&strm);
		return {};
	}
	static constexpr Bytef kEmpty = 0;
	QByteArray out;
	std::vector<Bytef> chunk(32768);
	qsizetype offset = 0;
	while (true) {
		if (strm.avail_in == 0 && offset < plain.size()) {
			const auto n = std::min<qsizetype>(plain.size() - offset, 1 << 20);
			strm.next_in = const_cast<Bytef *>(
				reinterpret_cast<const Bytef *>(plain.constData() + offset));
			strm.avail_in = static_cast<uInt>(n);
			offset += n;
		} else if (strm.avail_in == 0) {
			strm.next_in = const_cast<Bytef *>(&kEmpty);
		}
		const auto flush = (offset >= plain.size()) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_out = chunk.data();
		strm.avail_out = static_cast<uInt>(chunk.size());
		const auto rc = deflate(&strm, flush);
		if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
			deflateEnd(&strm);
			return {};
		}
		const auto have = static_cast<qsizetype>(chunk.size() - strm.avail_out);
		if (have > 0) {
			out.append(reinterpret_cast<const char *>(chunk.data()), have);
		}
		if (rc == Z_STREAM_END) {
			deflateEnd(&strm);
			return out;
		}
	}
}

[[nodiscard]] QByteArray DeflateWithWindowBits(
		const QByteArray &plain,
		int windowBits) {
	z_stream strm{};
	if (deflateInit2(&strm, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	strm.next_in = const_cast<Bytef *>(
		reinterpret_cast<const Bytef *>(plain.constData()));
	strm.avail_in = static_cast<uInt>(plain.size());
	QByteArray out(deflateBound(&strm, strm.avail_in) + 64, '\0');
	strm.next_out = reinterpret_cast<Bytef *>(out.data());
	strm.avail_out = static_cast<uInt>(out.size());
	const auto rc = deflate(&strm, Z_FINISH);
	const auto produced = static_cast<qsizetype>(strm.total_out);
	deflateEnd(&strm);
	if (rc != Z_STREAM_END) {
		return {};
	}
	out.truncate(produced);
	return out;
}

[[nodiscard]] SyncHistoryMessageMeta ToSyncHistoryMeta(
		const PlaylistRecordMessageMeta &meta) {
	return SyncHistoryMessageMeta{
		.isMessage = meta.isRegularMessage,
		.forwarded = meta.forwarded,
		.isDocument = meta.isDocument,
		.caption = meta.caption,
		.fileNames = meta.fileNames,
	};
}

[[nodiscard]] PlaylistRecordMessageMeta ValidPlaylistMeta(
		QString caption = PlaylistRecordCaption(),
		QString mimeType = PlaylistRecordMimeType()) {
	return PlaylistRecordMessageMeta{
		.isRegularMessage = true,
		.forwarded = false,
		.isDocument = true,
		.caption = std::move(caption),
		.mimeType = std::move(mimeType),
		.fileNames = { PlaylistRecordFileName() },
	};
}

[[nodiscard]] QByteArray SampleLibraryJsonA(const QString &selfPeerText) {
	return QByteArray(
		"{\n"
		"  \"purged\": {\"pl-zq5wd2ha6kcm3tve\": 1},\n"
		"  \"playlists\": {\n"
		"    \"pl-k3f7q2abx7mnp4rt\": {\n"
		"      \"title\": [\"Morning Mix\", 1790000000123],\n"
		"      \"deleted\": [false, 1790000000123],\n"
		"      \"order\": [[\"5361234567890123456\"], 1790000000456],\n"
		"      \"songs\": {\n"
		"        \"5361234567890123456\": [\n"
		"          {\n"
		"            \"peer\": \"")
		+ selfPeerText.toUtf8()
		+ QByteArray(
			"\",\n"
			"            \"msg\": 5521,\n"
			"            \"alt\": [{\"peer\": \"channel:9001\", \"msg\": 88}],\n"
			"            \"meta\": {\"title\": \"Song\", \"perf\": \"Artist\", \"dur\": 245},\n"
			"            \"since\": 1790000000200\n"
			"          },\n"
			"          1790000000300\n"
			"        ]\n"
			"      }\n"
			"    }\n"
			"  },\n"
			"  \"format\": 1\n"
			"}\n");
}

[[nodiscard]] QByteArray SampleLibraryJsonBReordered(const QString &selfPeerText) {
	return QByteArray(
		"{\"format\":1,\"playlists\":{\"pl-k3f7q2abx7mnp4rt\":{"
		"\"deleted\":[false,1790000000123],"
		"\"songs\":{\"5361234567890123456\":[{\"since\":1790000000200,"
		"\"meta\":{\"dur\":245,\"perf\":\"Artist\",\"title\":\"Song\"},"
		"\"msg\":5521,\"alt\":[{\"msg\":88,\"peer\":\"channel:9001\"}],"
		"\"peer\":\"")
		+ selfPeerText.toUtf8()
		+ QByteArray(
			"\"},1790000000300]},"
			"\"order\":[[\"5361234567890123456\"],1790000000456],"
			"\"title\":[\"Morning Mix\",1790000000123]}},"
			"\"purged\":{\"pl-zq5wd2ha6kcm3tve\":1}}");
}

[[nodiscard]] QString IndependentSha256Hex(const QByteArray &bytes) {
	return QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void CheckClearedPayloadFields(const PlaylistRecordClassification &classified) {
	CHECK(!bool(classified));
	CHECK(!classified.library.has_value());
	CHECK(!classified.canonicalBytes.has_value());
	CHECK(!classified.compressedBytes.has_value());
	CHECK(!classified.canonicalHash.has_value());
}

void TestConstantsAndFailClosedDefaults() {
	CHECK(kPlaylistRecordClassifierVersion == 1);
	CHECK(PlaylistRecordFileName() == u"Purple playlists.json.gz"_q);
	CHECK(PlaylistRecordMimeType() == u"application/gzip"_q);
	CHECK(PlaylistRecordCaption() == u"Purple playlists sync data #purpleplaylists"_q);
	CHECK(PlaylistRecordCaptionTag() == u"#purpleplaylists"_q);

	CHECK(PlaylistRecordCaption().contains(PlaylistRecordCaptionTag()));
	CHECK(!PlaylistRecordCaption().contains(SyncRecordCaptionTag()));
	for (const auto ch : PlaylistRecordCaption()) {
		CHECK(ch < u'0' || ch > u'9');
	}

	const PlaylistRecordMessageMeta defaultMeta;
	CHECK(!defaultMeta.isRegularMessage);
	CHECK(!defaultMeta.forwarded);
	CHECK(!defaultMeta.isDocument);
	CHECK(defaultMeta.caption.isEmpty());
	CHECK(defaultMeta.mimeType.isEmpty());
	CHECK(defaultMeta.fileNames.empty());
	CHECK(InspectPlaylistRecordMetadata(defaultMeta, SyncChatMessageOwnership::Own)
		== PlaylistRecordCandidateVerdict::Ignored);
	CHECK(!IsPlaylistRecordCandidate(defaultMeta, SyncChatMessageOwnership::Own));

	const PlaylistRecordInput defaultInput;
	CHECK(defaultInput.messageId == 0);
	CHECK(defaultInput.documentId == 0);
	CHECK(defaultInput.ownership == SyncChatMessageOwnership::Unknown);
	CHECK(defaultInput.readOutcome == PlaylistRecordReadOutcome::Failed);
	CHECK(defaultInput.compressedBytes.isEmpty());
	CHECK(defaultInput.selfUserId == 0);

	const auto defaultClassified = ClassifyPlaylistRecord(defaultInput);
	CHECK(defaultClassified.status == PlaylistRecordStatus::InvalidInput);
	CHECK(defaultClassified.error == PlaylistRecordError::InvalidMessageId);
	CHECK(!defaultClassified.memoEntry.has_value());
	CheckClearedPayloadFields(defaultClassified);
}

void TestMetadataRecognitionAndSettingsDisjointness() {
	const auto validMeta = ValidPlaylistMeta();
	CHECK(InspectPlaylistRecordMetadata(validMeta, SyncChatMessageOwnership::Own)
		== PlaylistRecordCandidateVerdict::Candidate);
	CHECK(IsPlaylistRecordCandidate(validMeta, SyncChatMessageOwnership::Own));

	CHECK(InspectPlaylistRecordMetadata(validMeta, SyncChatMessageOwnership::Unknown)
		== PlaylistRecordCandidateVerdict::UnknownOwnership);
	CHECK(!IsPlaylistRecordCandidate(validMeta, SyncChatMessageOwnership::Unknown));

	CHECK(InspectPlaylistRecordMetadata(validMeta, SyncChatMessageOwnership::Other)
		== PlaylistRecordCandidateVerdict::Ignored);
	CHECK(!IsPlaylistRecordCandidate(validMeta, SyncChatMessageOwnership::Other));

	const auto bogusOwnership = static_cast<SyncChatMessageOwnership>(99);
	CHECK(InspectPlaylistRecordMetadata(validMeta, bogusOwnership)
		== PlaylistRecordCandidateVerdict::Ignored);
	CHECK(!IsPlaylistRecordCandidate(validMeta, bogusOwnership));

	// Non-authoritative MIME types and empty caption still recognize exact filename.
	for (const QString &mime : {
			QString(),
			u"application/gzip"_q,
			u"application/octet-stream"_q,
			u"application/x-gzip"_q,
			u"text/plain"_q,
		}) {
		for (const QString &caption : {
				PlaylistRecordCaption(),
				QString(),
				u"Uploaded playlists backup"_q,
			}) {
			const auto meta = ValidPlaylistMeta(caption, mime);
			CHECK(InspectPlaylistRecordMetadata(meta, SyncChatMessageOwnership::Own)
				== PlaylistRecordCandidateVerdict::Candidate);
			CHECK(!IsSyncHistoryCandidate(ToSyncHistoryMeta(meta)));
		}
	}

	// Non-regular, forwarded, or non-document envelopes fail closed.
	{
		auto notRegular = validMeta;
		notRegular.isRegularMessage = false;
		CHECK(InspectPlaylistRecordMetadata(notRegular, SyncChatMessageOwnership::Own)
			== PlaylistRecordCandidateVerdict::Ignored);

		auto forwarded = validMeta;
		forwarded.forwarded = true;
		CHECK(InspectPlaylistRecordMetadata(forwarded, SyncChatMessageOwnership::Own)
			== PlaylistRecordCandidateVerdict::Ignored);

		auto notDoc = validMeta;
		notDoc.isDocument = false;
		CHECK(InspectPlaylistRecordMetadata(notDoc, SyncChatMessageOwnership::Own)
			== PlaylistRecordCandidateVerdict::Ignored);
	}

	// Reject *.purpleplaylist, case variations, extra files, empty names, MIME-only.
	for (const std::vector<QString> &badNames : {
			std::vector<QString>{},
			std::vector<QString>{ u""_q },
			std::vector<QString>{ u"export.purpleplaylist"_q },
			std::vector<QString>{ u"Purple playlists.purpleplaylist"_q },
			std::vector<QString>{ u"purple playlists.json.gz"_q },
			std::vector<QString>{ u"PURPLE PLAYLISTS.JSON.GZ"_q },
			std::vector<QString>{ u"Purple playlists.json.gz.bak"_q },
			std::vector<QString>{ u"copy-Purple playlists.json.gz"_q },
			std::vector<QString>{ PlaylistRecordFileName(), u"extra.json"_q },
			std::vector<QString>{ u"extra.json"_q, PlaylistRecordFileName() },
			std::vector<QString>{ PlaylistRecordFileName(), QString() },
		}) {
		const PlaylistRecordMessageMeta meta{
			.isRegularMessage = true,
			.forwarded = false,
			.isDocument = true,
			.caption = PlaylistRecordCaption(),
			.mimeType = PlaylistRecordMimeType(),
			.fileNames = badNames,
		};
		CHECK(InspectPlaylistRecordMetadata(meta, SyncChatMessageOwnership::Own)
			== PlaylistRecordCandidateVerdict::Ignored);
		CHECK(!IsPlaylistRecordCandidate(meta, SyncChatMessageOwnership::Own));
	}

	// Pure settings records are ignored without ambiguity.
	for (const PlaylistRecordMessageMeta &settingsMeta : {
			PlaylistRecordMessageMeta{
				true, false, true, u"Settings #purplesync"_q, u"application/json"_q,
				{ SyncSettingsRecordFileName() },
			},
			PlaylistRecordMessageMeta{
				true, false, true, QString(), u"application/json"_q,
				{ SyncPlaylistsRecordFileName() },
			},
			PlaylistRecordMessageMeta{
				true, false, true, SyncRecordCaptionTag(), QString(), {},
			},
		}) {
		CHECK(IsSyncHistoryCandidate(ToSyncHistoryMeta(settingsMeta)));
		CHECK(InspectPlaylistRecordMetadata(settingsMeta, SyncChatMessageOwnership::Own)
			== PlaylistRecordCandidateVerdict::Ignored);
		CHECK(InspectPlaylistRecordMetadata(settingsMeta, SyncChatMessageOwnership::Unknown)
			== PlaylistRecordCandidateVerdict::Ignored);
	}

	// Colliding settings + playlist markers MUST yield AmbiguousSettings (never UnknownOwnership/Candidate).
	const std::vector<PlaylistRecordMessageMeta> ambiguousCases = {
		ValidPlaylistMeta(u"Mixed #purplesync #purpleplaylists"_q, PlaylistRecordMimeType()),
		ValidPlaylistMeta(SyncRecordCaptionTag(), PlaylistRecordMimeType()),
		PlaylistRecordMessageMeta{
			true, false, true, PlaylistRecordCaption(), PlaylistRecordMimeType(),
			{ PlaylistRecordFileName(), SyncSettingsRecordFileName() },
		},
		PlaylistRecordMessageMeta{
			true, false, true, PlaylistRecordCaption(), PlaylistRecordMimeType(),
			{ SyncPlaylistsRecordFileName() },
		},
		PlaylistRecordMessageMeta{
			true, false, true, u"#purplesync #purpleplaylists"_q, QString(), { u"other.bin"_q },
		},
	};
	for (const auto &ambiguous : ambiguousCases) {
		CHECK(IsSyncHistoryCandidate(ToSyncHistoryMeta(ambiguous)));
		for (const auto ownership : {
				SyncChatMessageOwnership::Own,
				SyncChatMessageOwnership::Unknown,
				SyncChatMessageOwnership::Other,
			}) {
			CHECK(InspectPlaylistRecordMetadata(ambiguous, ownership)
				== PlaylistRecordCandidateVerdict::AmbiguousSettings);
			CHECK(!IsPlaylistRecordCandidate(ambiguous, ownership));
		}
	}

	// Exhaustive Cartesian disjointness check against extracted IsSyncHistoryCandidate.
	const std::vector<QString> captions = {
		QString(),
		PlaylistRecordCaption(),
		u"#purplesync"_q,
		u"#purplesync #purpleplaylists"_q,
		u"other caption"_q,
	};
	const std::vector<QString> mimes = {
		QString(),
		PlaylistRecordMimeType(),
		u"application/octet-stream"_q,
	};
	const std::vector<std::vector<QString>> nameLists = {
		{},
		{ PlaylistRecordFileName() },
		{ SyncSettingsRecordFileName() },
		{ SyncPlaylistsRecordFileName() },
		{ PlaylistRecordFileName(), SyncSettingsRecordFileName() },
		{ u"sample.purpleplaylist"_q },
	};
	for (const bool isMsg : { false, true }) {
		for (const bool fwd : { false, true }) {
			for (const bool isDoc : { false, true }) {
				for (const auto &cap : captions) {
					for (const auto &mime : mimes) {
						for (const auto &names : nameLists) {
							const PlaylistRecordMessageMeta meta{
								isMsg, fwd, isDoc, cap, mime, names
							};
							const auto hist = ToSyncHistoryMeta(meta);
							const bool isSettings = IsSyncHistoryCandidate(hist);
							for (const auto ownership : {
									SyncChatMessageOwnership::Own,
									SyncChatMessageOwnership::Unknown,
									SyncChatMessageOwnership::Other,
								}) {
								const auto verdict = InspectPlaylistRecordMetadata(meta, ownership);
								const bool isPlaylist = IsPlaylistRecordCandidate(meta, ownership);
								CHECK(isPlaylist == (verdict == PlaylistRecordCandidateVerdict::Candidate));
								CHECK(!(isSettings && isPlaylist));
								if (isSettings) {
									CHECK(verdict == PlaylistRecordCandidateVerdict::Ignored
										|| verdict == PlaylistRecordCandidateVerdict::AmbiguousSettings);
								}
							}
						}
					}
				}
			}
		}
	}
}

void TestIdentityAndReadOutcomeClassification() {
	const auto validJson = SampleLibraryJsonA(u"self"_q);
	const auto validGzip = DeflateGzip(validJson);
	CHECK(!validGzip.isEmpty());

	const auto makeValidInput = [&]() {
		return PlaylistRecordInput{
			.messageId = 42,
			.documentId = 90001ULL,
			.ownership = SyncChatMessageOwnership::Own,
			.meta = ValidPlaylistMeta(),
			.readOutcome = PlaylistRecordReadOutcome::Succeeded,
			.compressedBytes = validGzip,
			.selfUserId = 7001ULL,
		};
	};

	// Invalid messageId / documentId / selfUserId never memoize or retain bytes.
	for (const int64_t badMsgId : {
			int64_t(0),
			int64_t(-1),
			std::numeric_limits<int64_t>::min(),
			int64_t(std::numeric_limits<int32_t>::max()) + 1,
		}) {
		auto input = makeValidInput();
		input.messageId = badMsgId;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::InvalidInput);
		CHECK(res.error == PlaylistRecordError::InvalidMessageId);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	{
		auto input = makeValidInput();
		input.documentId = 0;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::InvalidInput);
		CHECK(res.error == PlaylistRecordError::InvalidDocumentId);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	{
		auto input = makeValidInput();
		input.selfUserId = uint64_t(std::numeric_limits<int64_t>::max()) + 1ULL;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::InvalidInput);
		CHECK(res.error == PlaylistRecordError::InvalidSelfUserId);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	// Unknown ownership -> Incomplete (no memo, ignore valid gzip bytes).
	{
		auto input = makeValidInput();
		input.ownership = SyncChatMessageOwnership::Unknown;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::Incomplete);
		CHECK(res.error == PlaylistRecordError::UnknownOwnership);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	// Other ownership -> Ignored (no memo, ignore valid gzip bytes).
	{
		auto input = makeValidInput();
		input.ownership = SyncChatMessageOwnership::Other;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::Ignored);
		CHECK(res.error == PlaylistRecordError::NotOwnRecord);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	// Ambiguous settings + playlist record -> Ignored (no memo, ignore valid gzip bytes).
	{
		auto input = makeValidInput();
		input.meta.caption = u"#purplesync #purpleplaylists"_q;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::Ignored);
		CHECK(res.error == PlaylistRecordError::AmbiguousSettingsConflict);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	// Unrecognized metadata -> Ignored (no memo, ignore valid gzip bytes).
	{
		auto input = makeValidInput();
		input.meta.fileNames = { u"legacy.purpleplaylist"_q };
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::Ignored);
		CHECK(res.error == PlaylistRecordError::UnrecognizedRecord);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	// Transient read failures: ignore even valid gzipBytes and do NOT memoize.
	struct OutcomeExpectation {
		PlaylistRecordReadOutcome outcome;
		PlaylistRecordError error;
	};
	for (const OutcomeExpectation &item : {
			OutcomeExpectation{ PlaylistRecordReadOutcome::Failed, PlaylistRecordError::ReadFailed },
			OutcomeExpectation{ PlaylistRecordReadOutcome::Vanished, PlaylistRecordError::ReadVanished },
			OutcomeExpectation{ PlaylistRecordReadOutcome::Inaccessible, PlaylistRecordError::ReadInaccessible },
			OutcomeExpectation{ PlaylistRecordReadOutcome::Cancelled, PlaylistRecordError::ReadCancelled },
			OutcomeExpectation{ static_cast<PlaylistRecordReadOutcome>(77), PlaylistRecordError::ReadFailed },
		}) {
		auto input = makeValidInput();
		input.readOutcome = item.outcome;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(res.status == PlaylistRecordStatus::Incomplete);
		CHECK(res.error == item.error);
		CHECK(!res.memoEntry.has_value());
		CheckClearedPayloadFields(res);
	}

	// Succeeded 0-byte download MUST classify as Invalid (not Incomplete), and memoize Invalid.
	{
		auto input = makeValidInput();
		input.compressedBytes = QByteArray();
		const auto inputSnapshot = input;
		const auto res = ClassifyPlaylistRecord(input);
		CHECK(input == inputSnapshot);
		CHECK(res.status == PlaylistRecordStatus::Invalid);
		CHECK(res.error == PlaylistRecordError::CodecError);
		CHECK(res.codecError == PlaylistCodecError::InvalidGzip);
		CHECK(res.memoEntry.has_value());
		if (res.memoEntry.has_value()) {
			CHECK(res.memoEntry->state == PlaylistLocalMemoState::Invalid);
			CHECK(res.memoEntry->documentId == input.documentId);
			CHECK(res.memoEntry->classifier == kPlaylistRecordClassifierVersion);
			CHECK(!res.memoEntry->canonicalHash.has_value());
		}
		CheckClearedPayloadFields(res);
	}
}

void TestValidNormalizationHashingAndFourMiBExemption() {
	constexpr uint64_t kSelfId = 7001ULL;
	const auto jsonPrettySelf = SampleLibraryJsonA(u"self"_q);
	const auto jsonCompactUser = SampleLibraryJsonBReordered(u"user:7001"_q);

	gz_header hdr{};
	Bytef gzName[] = "custom.json.gz";
	Bytef gzComment[] = "variant header";
	hdr.name = gzName;
	hdr.comment = gzComment;
	hdr.time = 1790000000UL;
	hdr.os = 3;

	const auto gz1 = DeflateGzip(jsonPrettySelf, Z_BEST_SPEED, nullptr);
	const auto gz2 = DeflateGzip(jsonCompactUser, Z_BEST_COMPRESSION, &hdr);
	CHECK(!gz1.isEmpty() && !gz2.isEmpty());
	CHECK(gz1 != gz2);

	const PlaylistRecordInput input1{
		.messageId = 101,
		.documentId = 80001ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidPlaylistMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = gz1,
		.selfUserId = kSelfId,
	};
	const auto snapshot1 = input1;

	PlaylistRecordInput input2 = input1;
	input2.messageId = 202;
	input2.meta = ValidPlaylistMeta(QString(), u"application/octet-stream"_q);
	input2.compressedBytes = gz2;

	const auto res1 = ClassifyPlaylistRecord(input1);
	const auto res2 = ClassifyPlaylistRecord(input2);
	CHECK(input1 == snapshot1);

	CHECK(bool(res1) && bool(res2));
	CHECK(res1.status == PlaylistRecordStatus::Valid);
	CHECK(res2.status == PlaylistRecordStatus::Valid);
	CHECK(res1.error == PlaylistRecordError::None);
	CHECK(res1.codecError == PlaylistCodecError::None);
	CHECK(res1.libraryError == PlaylistLibraryError::None);
	CHECK(res1.library.has_value() && res2.library.has_value());
	if (res1.library.has_value() && res2.library.has_value()) {
		CHECK(*res1.library == *res2.library);
	}
	CHECK(res1.canonicalBytes.has_value() && res2.canonicalBytes.has_value());
	if (res1.canonicalBytes.has_value() && res2.canonicalBytes.has_value()) {
		CHECK(*res1.canonicalBytes == *res2.canonicalBytes);
	}
	CHECK(res1.compressedBytes.has_value() && res1.compressedBytes == gz1);
	CHECK(res2.compressedBytes.has_value() && res2.compressedBytes == gz2);
	CHECK(res1.canonicalHash.has_value() && res2.canonicalHash.has_value());
	if (res1.canonicalHash.has_value() && res2.canonicalHash.has_value()
			&& res1.canonicalBytes.has_value()) {
		CHECK(*res1.canonicalHash == *res2.canonicalHash);
		CHECK(res1.canonicalHash->size() == 64);
		CHECK(*res1.canonicalHash == IndependentSha256Hex(*res1.canonicalBytes));
		CHECK(*res1.canonicalHash != IndependentSha256Hex(gz1));
		CHECK(*res1.canonicalHash != IndependentSha256Hex(jsonPrettySelf));
	}

	CHECK(res1.memoEntry.has_value());
	if (res1.memoEntry.has_value()) {
		CHECK(res1.memoEntry->state == PlaylistLocalMemoState::Valid);
		CHECK(res1.memoEntry->documentId == 80001ULL);
		CHECK(res1.memoEntry->classifier == kPlaylistRecordClassifierVersion);
		CHECK(res1.memoEntry->canonicalHash == res1.canonicalHash);
		CHECK(PlaylistRecordMemoEntry(res1) == res1.memoEntry);
	}

	// Zero selfUserId also classifies when payload uses channel/self tokens without user:0.
	{
		const auto gzZeroSelf = DeflateGzip(SampleLibraryJsonA(u"channel:555"_q));
		const PlaylistRecordInput zeroSelfInput{
			.messageId = 103,
			.documentId = 80002ULL,
			.ownership = SyncChatMessageOwnership::Own,
			.meta = ValidPlaylistMeta(),
			.readOutcome = PlaylistRecordReadOutcome::Succeeded,
			.compressedBytes = gzZeroSelf,
			.selfUserId = 0,
		};
		const auto resZero = ClassifyPlaylistRecord(zeroSelfInput);
		CHECK(resZero.status == PlaylistRecordStatus::Valid);
		CHECK(resZero.canonicalBytes.has_value() && resZero.canonicalHash.has_value());
		if (resZero.canonicalBytes.has_value()) {
			CHECK(resZero.canonicalHash == IndependentSha256Hex(*resZero.canonicalBytes));
		}
	}

	// Valid library exceeding 4 MiB global cap (> 4 MiB uncompressed, <= 10 MiB / 32 MiB caps) succeeds.
	auto bigLibJson = QByteArray("{\"format\":1,\"playlists\":{");
	const auto entrySuffix = QByteArray(
		":{\"deleted\":[false,100],\"order\":[[\"1\"],100],\"songs\":{\"1\":[false,100]},\"title\":[\"Mix\",100]}");
	const auto alphabet = "abcdefghijklmnopqrstuvwxyz234567";
	for (auto i = 0; bigLibJson.size() <= 4 * 1024 * 1024 + 65536; ++i) {
		if (i > 0) {
			bigLibJson.append(',');
		}
		auto id = QByteArray("\"pl-");
		auto v = uint32_t(i);
		for (auto j = 0; j < 16; ++j) {
			id.append(alphabet[v & 31]);
			v >>= 5;
		}
		id.append('"');
		bigLibJson.append(id);
		bigLibJson.append(entrySuffix);
	}
	bigLibJson.append("},\"purged\":{}}");
	CHECK(bigLibJson.size() > 4 * 1024 * 1024);

	const auto bigGzip = DeflateGzip(bigLibJson);
	CHECK(!bigGzip.isEmpty() && bigGzip.size() < kPlaylistRecordMaximumCompressedBytes);
	const PlaylistRecordInput bigInput{
		.messageId = 303,
		.documentId = 80003ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidPlaylistMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = bigGzip,
		.selfUserId = kSelfId,
	};
	const auto bigRes = ClassifyPlaylistRecord(bigInput);
	CHECK(bigRes.status == PlaylistRecordStatus::Valid);
	CHECK(bigRes.canonicalBytes.has_value());
	if (bigRes.canonicalBytes.has_value()) {
		CHECK(bigRes.canonicalBytes->size() > 4 * 1024 * 1024);
	}
	CHECK(bigRes.library.has_value());
	if (bigRes.library.has_value()) {
		CHECK(!bigRes.library->playlists.empty());
	}
	CHECK(bigRes.memoEntry.has_value());
	if (bigRes.memoEntry.has_value()) {
		CHECK(bigRes.memoEntry->state == PlaylistLocalMemoState::Valid);
		CHECK(bigRes.memoEntry->canonicalHash == bigRes.canonicalHash);
	}
}

void TestFailuresCapsBombsAndNoRetainedPayload() {
	const auto validJson = SampleLibraryJsonA(u"self"_q);
	const auto validGzip = DeflateGzip(validJson);
	CHECK(!validGzip.isEmpty());

	const auto classifyWithBytes = [&](const QByteArray &bytes, uint64_t docId = 50001ULL) {
		return ClassifyPlaylistRecord(PlaylistRecordInput{
			.messageId = 400,
			.documentId = docId,
			.ownership = SyncChatMessageOwnership::Own,
			.meta = ValidPlaylistMeta(),
			.readOutcome = PlaylistRecordReadOutcome::Succeeded,
			.compressedBytes = bytes,
			.selfUserId = 7001ULL,
		});
	};

	// Codec corruption cases: raw deflate, zlib, truncated, corrupt CRC32, multi-member gzip, trailing garbage.
	auto badCrc = validGzip;
	badCrc[badCrc.size() - 6] = static_cast<char>(badCrc.at(badCrc.size() - 6) ^ 0xFF);
	auto truncated = validGzip.left(validGzip.size() - 5);
	auto trailing = validGzip + QByteArray("junk");
	auto multiMember = validGzip + validGzip;

	const auto checkMemoState = [](
			const PlaylistRecordClassification &res,
			uint64_t expectedDocId,
			PlaylistLocalMemoState expectedState) {
		CHECK(res.memoEntry.has_value());
		if (res.memoEntry.has_value()) {
			CHECK(res.memoEntry->documentId == expectedDocId);
			CHECK(res.memoEntry->state == expectedState);
			CHECK(res.memoEntry->classifier == kPlaylistRecordClassifierVersion);
			CHECK(!res.memoEntry->canonicalHash.has_value());
		}
		CheckClearedPayloadFields(res);
	};

	for (const QByteArray &badCodec : {
			QByteArray("not gzip"),
			DeflateWithWindowBits(validJson, MAX_WBITS),
			DeflateWithWindowBits(validJson, -MAX_WBITS),
			badCrc,
			truncated,
			trailing,
			multiMember,
		}) {
		const auto res = classifyWithBytes(badCodec);
		CHECK(res.status == PlaylistRecordStatus::Invalid);
		CHECK(res.error == PlaylistRecordError::CodecError);
		CHECK(res.codecError != PlaylistCodecError::None);
		checkMemoState(res, 50001ULL, PlaylistLocalMemoState::Invalid);
	}

	// Schema failures (valid gzip wrapping bad JSON / schema).
	for (const QByteArray &badJson : {
			QByteArray("{"),
			QByteArray("[]"),
			QByteArray("{\"format\":1,\"format\":1}"),
			QByteArray("{\"format\":1,\"playlists\":{},\"purged\":{\"bad\":1}}"),
			QByteArray("{\"format\":0,\"playlists\":{},\"purged\":{}}"),
			QByteArray("\xFF\xFE"),
		}) {
		const auto res = classifyWithBytes(DeflateGzip(badJson));
		CHECK(res.status == PlaylistRecordStatus::Invalid);
		CHECK(res.error == PlaylistRecordError::SchemaError);
		CHECK(res.libraryError != PlaylistLibraryError::None);
		checkMemoState(res, 50001ULL, PlaylistLocalMemoState::Invalid);
	}

	// NewerFormat schema -> NewerFormat status + NewerFormat memo without hash or retained bytes.
	{
		const auto newerRes = classifyWithBytes(
			DeflateGzip("{\"format\":2,\"playlists\":{},\"purged\":{},\"future\":true}"),
			50002ULL);
		CHECK(newerRes.status == PlaylistRecordStatus::NewerFormat);
		CHECK(newerRes.error == PlaylistRecordError::SchemaError);
		checkMemoState(newerRes, 50002ULL, PlaylistLocalMemoState::NewerFormat);
	}

	// > 10 MiB compressed (non-gzip AND gzip-header-prefixed) -> Oversized / CompressedSizeLimit.
	{
		QByteArray overComp(kPlaylistRecordMaximumCompressedBytes + 1, '\xAA');
		const auto resNonGzip = classifyWithBytes(overComp, 50003ULL);
		CHECK(resNonGzip.status == PlaylistRecordStatus::Oversized);
		CHECK(resNonGzip.error == PlaylistRecordError::CodecError);
		CHECK(resNonGzip.codecError == PlaylistCodecError::CompressedSizeLimit);
		checkMemoState(resNonGzip, 50003ULL, PlaylistLocalMemoState::Oversized);

		overComp[0] = '\x1F';
		overComp[1] = static_cast<char>(0x8B);
		const auto resGzip = classifyWithBytes(overComp, 50004ULL);
		CHECK(resGzip.status == PlaylistRecordStatus::Oversized);
		CHECK(resGzip.error == PlaylistRecordError::CodecError);
		CHECK(resGzip.codecError == PlaylistCodecError::CompressedSizeLimit);
		checkMemoState(resGzip, 50004ULL, PlaylistLocalMemoState::Oversized);
	}

	// Exact 32 MiB passes codec and reaches schema (Invalid JSON syntax); 32 MiB + 1 bomb -> Oversized.
	{
		const QByteArray exact32(kPlaylistRecordMaximumUncompressedBytes, ' ');
		const auto gzExact32 = DeflateGzip(exact32);
		CHECK(!gzExact32.isEmpty() && gzExact32.size() < 64 * 1024);
		const auto res32 = classifyWithBytes(gzExact32, 50005ULL);
		CHECK(res32.status == PlaylistRecordStatus::Invalid);
		CHECK(res32.error == PlaylistRecordError::SchemaError);
		checkMemoState(res32, 50005ULL, PlaylistLocalMemoState::Invalid);

		const QByteArray bombPlain(kPlaylistRecordMaximumUncompressedBytes + 1, ' ');
		const auto bombGzip = DeflateGzip(bombPlain);
		CHECK(!bombGzip.isEmpty() && bombGzip.size() < 64 * 1024);
		const auto bombRes = classifyWithBytes(bombGzip, 50006ULL);
		CHECK(bombRes.status == PlaylistRecordStatus::Oversized);
		CHECK(bombRes.error == PlaylistRecordError::CodecError);
		CHECK(bombRes.codecError == PlaylistCodecError::UncompressedSizeLimit);
		checkMemoState(bombRes, 50006ULL, PlaylistLocalMemoState::Oversized);
	}
}

void TestMemoEligibilityAndLocalStateRoundTrip() {
	constexpr uint64_t kSelfId = 7001ULL;
	const auto gz = DeflateGzip(SampleLibraryJsonA(u"user:7001"_q));
	CHECK(!gz.isEmpty());

	const auto valid = ClassifyPlaylistRecord(PlaylistRecordInput{
		.messageId = 10,
		.documentId = 9101ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidPlaylistMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = gz,
		.selfUserId = kSelfId,
	});
	const auto invalid = ClassifyPlaylistRecord(PlaylistRecordInput{
		.messageId = 11,
		.documentId = 9102ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidPlaylistMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = QByteArray("bad"),
		.selfUserId = kSelfId,
	});
	const auto newer = ClassifyPlaylistRecord(PlaylistRecordInput{
		.messageId = 12,
		.documentId = 9103ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidPlaylistMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = DeflateGzip("{\"format\":99}"),
		.selfUserId = kSelfId,
	});
	const auto oversized = ClassifyPlaylistRecord(PlaylistRecordInput{
		.messageId = 13,
		.documentId = 9104ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidPlaylistMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = DeflateGzip(
			QByteArray(kPlaylistRecordMaximumUncompressedBytes + 1, 'X')),
		.selfUserId = kSelfId,
	});

	CHECK(valid.memoEntry.has_value());
	CHECK(invalid.memoEntry.has_value());
	CHECK(newer.memoEntry.has_value());
	CHECK(oversized.memoEntry.has_value());
	if (!valid.memoEntry.has_value()
			|| !invalid.memoEntry.has_value()
			|| !newer.memoEntry.has_value()
			|| !oversized.memoEntry.has_value()) {
		return;
	}

	// Direct edge guards on PlaylistRecordMemoEntry.
	{
		auto missingHash = valid;
		missingHash.canonicalHash.reset();
		CHECK(!PlaylistRecordMemoEntry(missingHash).has_value());

		auto badHash = valid;
		badHash.canonicalHash = QString(64, u'Z');
		CHECK(!PlaylistRecordMemoEntry(badHash).has_value());

		auto zeroClassifier = invalid;
		zeroClassifier.classifierVersion = 0;
		CHECK(!PlaylistRecordMemoEntry(zeroClassifier).has_value());

		auto zeroDoc = invalid;
		zeroDoc.documentId = 0;
		CHECK(!PlaylistRecordMemoEntry(zeroDoc).has_value());

		auto badMsg = invalid;
		badMsg.messageId = 0;
		CHECK(!PlaylistRecordMemoEntry(badMsg).has_value());
	}

	PlaylistLocalState localState = DefaultPlaylistLocalState();
	localState.chat = PeerToken{ PeerTokenKind::Self, 0 };
	localState.memo.emplace(10, *valid.memoEntry);
	localState.memo.emplace(11, *invalid.memoEntry);
	localState.memo.emplace(12, *newer.memoEntry);
	localState.memo.emplace(13, *oversized.memoEntry);

	const auto validation = ValidatePlaylistLocalState(localState, kSelfId);
	CHECK(bool(validation));
	CHECK(validation.status == PlaylistLocalStatus::Valid);

	const auto serialized = SerializePlaylistLocalState(localState, kSelfId);
	CHECK(bool(serialized));
	const auto parsedBack = ParsePlaylistLocalState(serialized.json, kSelfId);
	CHECK(bool(parsedBack));
	CHECK(parsedBack.state == localState);
}

} // namespace

int main() {
	TestConstantsAndFailClosedDefaults();
	TestMetadataRecognitionAndSettingsDisjointness();
	TestIdentityAndReadOutcomeClassification();
	TestValidNormalizationHashingAndFourMiBExemption();
	TestFailuresCapsBombsAndNoRetainedPayload();
	TestMemoEligibilityAndLocalStateRoundTrip();

	if (Failures > 0) {
		std::printf("FAILED %d/%d checks\n", Failures, Checks);
		return 1;
	}
	std::printf("PASSED %d checks\n", Checks);
	return 0;
}
