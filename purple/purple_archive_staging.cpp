/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_staging.h"

#include "purple/purple_sync_json.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <limits>

namespace Purple {
namespace {

QString Digest(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool JsonBound(const QJsonValue &value, int &left, int depth = 0) {
	if (depth > 16 || --left < 0) { return false; }
	if (value.isString()) {
		const auto size = value.toString().size();
		if (size > left / 3) { return false; }
		left -= int(size) * 3;
	} else if (value.isArray()) {
		for (const auto &item : value.toArray()) {
			if (!JsonBound(item, left, depth + 1)) { return false; }
		}
	} else if (value.isObject()) {
		const auto object = value.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			if (!JsonBound(i.key(), left, depth + 1) || !JsonBound(i.value(), left, depth + 1)) { return false; }
		}
	}
	return true;
}

bool BindingShape(const ArchiveStagingBinding &binding) {
	const auto &source = binding.source;
	auto budget = 4096;
	if (!JsonBound(source.key.preserved, budget) || !JsonBound(source.key.chat.preserved, budget)) { return false; }
	return IsArchiveMessageKey(source.key) && IsArchiveToken(binding.jobId)
		&& IsArchiveToken(source.version) && IsArchiveToken(source.revision)
		&& IsArchiveToken(source.mediaIdentity) && IsArchiveHash(source.contentHash)
		&& source.epoch.account == source.key.account && CanRunArchiveWork(source.epoch, source.epoch)
		&& (!binding.expectedBlobHash || IsArchiveHash(*binding.expectedBlobHash));
}

QJsonObject BindingObject(const ArchiveStagingBinding &binding) {
	auto job = ArchiveMediaJob();
	job.id = binding.jobId;
	job.key = binding.source.key;
	job.version = binding.source.version;
	job.mediaIdentity = binding.source.mediaIdentity;
	job.generation = binding.source.generation;
	job.expectedSize = binding.totalBytes;
	job.expectedHash = binding.expectedBlobHash.value_or(QString());
	job.revision = binding.source.revision;
	job.contentHash = binding.source.contentHash;
	const auto encoded = EncodeArchiveRow({ job });
	return encoded ? QJsonObject{
		{ u"job"_q, QJsonDocument::fromJson(*encoded).object() },
		{ u"store"_q, binding.source.epoch.storeId },
		{ u"epoch"_q, QString::number(binding.source.epoch.generation) },
	} : QJsonObject();
}

QByteArray BindingBytes(const ArchiveStagingBinding &binding) {
	if (!BindingShape(binding)) { return {}; }
	const auto object = BindingObject(binding);
	if (object.isEmpty()) { return {}; }
	const auto bytes = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return bytes && bytes.canonical.size() <= 16 * 1024 ? bytes.canonical : QByteArray();
}

bool SameBinding(const ArchiveStagingBinding &a, const ArchiveStagingBinding &b) {
	const auto encoded = BindingBytes(a);
	return !encoded.isEmpty() && encoded == BindingBytes(b);
}

bool String(QJsonObject &object, const QString &key, QString &out) {
	const auto value = object.take(key);
	if (!value.isString()) { return false; }
	out = value.toString();
	return true;
}

bool Number(QJsonObject &object, const QString &key, uint64_t &out) {
	auto text = QString();
	if (!String(object, key, text)) { return false; }
	auto ok = false;
	out = text.toULongLong(&ok);
	return ok && QString::number(out) == text;
}

bool BindingRead(const QJsonValue &value, ArchiveStagingBinding &out) {
	if (!value.isObject()) { return false; }
	auto object = value.toObject();
	const auto jobValue = object.take(u"job"_q);
	if (!jobValue.isObject()) { return false; }
	const auto decoded = DecodeArchiveRow(QJsonDocument(jobValue.toObject()).toJson(QJsonDocument::Compact));
	if (!decoded.row || !std::holds_alternative<ArchiveMediaJob>(decoded.row->value)) { return false; }
	const auto &job = std::get<ArchiveMediaJob>(decoded.row->value);
	if (!job.expectedSize || job.state != ArchiveJobState::Staged || !job.blobId.isEmpty()
		|| job.completedSize || !job.failure.isEmpty() || !job.preserved.isEmpty()) { return false; }
	out.source.key = job.key;
	out.source.epoch.account = job.key.account;
	out.source.version = job.version;
	out.source.revision = job.revision;
	out.source.contentHash = job.contentHash;
	out.source.mediaIdentity = job.mediaIdentity;
	out.source.generation = job.generation;
	out.jobId = job.id;
	out.totalBytes = *job.expectedSize;
	if (!job.expectedHash.isEmpty()) { out.expectedBlobHash = job.expectedHash; }
	return String(object, u"store"_q, out.source.epoch.storeId)
		&& Number(object, u"epoch"_q, out.source.epoch.generation)
		&& object.isEmpty() && BindingShape(out);
}

QString EmptyLeaf() { return Digest("purple-archive/staging/empty/v1"); }

QString Parent(int level, const QString &left, const QString &right) {
	return Digest(QByteArray("purple-archive/staging/node/v1") + char(level)
		+ QByteArray::fromHex(left.toLatin1()) + QByteArray::fromHex(right.toLatin1()));
}

std::optional<std::vector<ArchiveStagingNode>> Nodes(uint64_t index, QString leaf,
		const ArchiveStagingProof &proof) {
	if (index >= (uint64_t(1) << kArchiveStagingProofDepth) || !IsArchiveHash(leaf)
		|| proof.siblings.size() != kArchiveStagingProofDepth) { return {}; }
	auto nodes = std::vector<ArchiveStagingNode>();
	nodes.reserve(kArchiveStagingProofDepth);
	for (auto level = 0; level != kArchiveStagingProofDepth; ++level) {
		const auto &other = proof.siblings[level];
		if (!IsArchiveHash(other)) { return {}; }
		leaf = ((index >> level) & 1) ? Parent(level + 1, other, leaf) : Parent(level + 1, leaf, other);
		nodes.push_back({ level + 1, index >> (level + 1), leaf });
	}
	return nodes;
}

std::optional<uint32_t> WindowSize(const ArchiveStagingBinding &binding, uint64_t index) {
	if (index >= ArchiveStagingPageCount(binding.totalBytes)) { return {}; }
	const auto offset = index * uint64_t(kArchiveStagingWindowBytes);
	return uint32_t(std::min(uint64_t(kArchiveStagingWindowBytes), binding.totalBytes - offset));
}

bool Covered(const ArchiveStagingCoveragePage &page, uint32_t offset) {
	return page.complete || (static_cast<unsigned char>(page.coverage[offset / 8]) & (1u << (offset % 8)));
}

std::optional<uint32_t> PageCovered(const ArchiveStagingCoveragePage &page) {
	if (!IsArchiveHash(page.bindingId) || !page.checkpoint || page.ranges.size() != 1
		|| page.ranges.size() > kArchiveStagingRangeLimit
		|| page.index >= (uint64_t(1) << kArchiveStagingProofDepth)) { return {}; }
	const auto &range = page.ranges[0];
	if (range.offset != page.index * uint64_t(kArchiveStagingWindowBytes)
		|| !range.length || range.length > kArchiveStagingWindowBytes || !IsArchiveHash(range.pieceHash)) { return {}; }
	if (page.complete) { return page.coverage.isEmpty() ? std::optional(range.length) : std::nullopt; }
	if (page.coverage.size() != (range.length + 7) / 8) { return {}; }
	auto count = uint32_t(0);
	for (auto offset = uint32_t(0); offset < uint32_t(page.coverage.size()) * 8; ++offset) {
		if (Covered(page, offset)) {
			if (offset >= range.length) { return {}; }
			++count;
		}
	}
	return count && count < range.length ? std::optional(count) : std::nullopt;
}

bool HeaderShape(const ArchiveStagingHeader &header) {
	if (!BindingShape(header.binding) || !IsArchiveHash(header.rootHash)
		|| header.coveredBytes > header.binding.totalBytes
		|| header.pages > ArchiveStagingPageCount(header.binding.totalBytes)
		|| header.pages > header.checkpoint) { return false; }
	if (!header.pages) { return !header.coveredBytes && header.rootHash == ArchiveStagingEmptyRoot(); }
	return header.coveredBytes >= header.pages && header.rootHash != ArchiveStagingEmptyRoot()
		&& (header.coveredBytes - 1) / kArchiveStagingWindowBytes + 1 <= header.pages;
}

QJsonObject HeaderObject(const ArchiveStagingHeader &header) {
	return { { u"binding"_q, BindingObject(header.binding) },
		{ u"checkpoint"_q, QString::number(header.checkpoint) },
		{ u"root"_q, header.rootHash }, { u"covered"_q, QString::number(header.coveredBytes) },
		{ u"pages"_q, QString::number(header.pages) } };
}

bool HeaderRead(const QJsonValue &value, ArchiveStagingHeader &out) {
	if (!value.isObject()) { return false; }
	auto object = value.toObject();
	return BindingRead(object.take(u"binding"_q), out.binding)
		&& Number(object, u"checkpoint"_q, out.checkpoint)
		&& String(object, u"root"_q, out.rootHash)
		&& Number(object, u"covered"_q, out.coveredBytes)
		&& Number(object, u"pages"_q, out.pages) && object.isEmpty() && HeaderShape(out);
}

QJsonObject PageObject(const ArchiveStagingCoveragePage &page) {
	auto ranges = QJsonArray();
	for (const auto &range : page.ranges) {
		ranges.push_back(QJsonObject{ { u"offset"_q, QString::number(range.offset) },
			{ u"length"_q, QString::number(range.length) }, { u"hash"_q, range.pieceHash } });
	}
	return { { u"binding"_q, page.bindingId }, { u"index"_q, QString::number(page.index) },
		{ u"checkpoint"_q, QString::number(page.checkpoint) }, { u"ranges"_q, ranges },
		{ u"complete"_q, page.complete }, { u"coverage"_q, QString::fromLatin1(page.coverage.toBase64()) } };
}

bool PageRead(const QJsonValue &value, ArchiveStagingCoveragePage &out) {
	if (!value.isObject()) { return false; }
	auto object = value.toObject();
	const auto ranges = object.take(u"ranges"_q);
	const auto complete = object.take(u"complete"_q);
	auto bits = QString();
	if (!ranges.isArray() || ranges.toArray().size() > kArchiveStagingRangeLimit || !complete.isBool()
		|| !String(object, u"binding"_q, out.bindingId) || !Number(object, u"index"_q, out.index)
		|| !Number(object, u"checkpoint"_q, out.checkpoint) || !String(object, u"coverage"_q, bits)
		|| !object.isEmpty()) { return false; }
	out.complete = complete.toBool();
	const auto decoded = QByteArray::fromBase64Encoding(bits.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded || QString::fromLatin1(decoded.decoded.toBase64()) != bits) { return false; }
	out.coverage = decoded.decoded;
	for (const auto &entry : ranges.toArray()) {
		if (!entry.isObject()) { return false; }
		auto rangeObject = entry.toObject();
		auto range = ArchiveStagingRange();
		auto length = uint64_t(0);
		if (!Number(rangeObject, u"offset"_q, range.offset) || !Number(rangeObject, u"length"_q, length)
			|| length > kArchiveStagingWindowBytes || !String(rangeObject, u"hash"_q, range.pieceHash)
			|| !rangeObject.isEmpty()) { return false; }
		range.length = uint32_t(length);
		out.ranges.push_back(std::move(range));
	}
	return PageCovered(out).has_value();
}

bool CheckpointShape(const ArchiveStagingCheckpoint &checkpoint) {
	if (!HeaderShape(checkpoint.header) || !IsArchiveHash(checkpoint.previousHeaderHash)) { return false; }
	if (!checkpoint.pageIndex) {
		return checkpoint.pageHash.isEmpty() && checkpoint.proof.siblings.empty()
			&& checkpoint.previousHeaderHash == ArchiveStagingHeaderHash(checkpoint.header);
	}
	if (*checkpoint.pageIndex >= ArchiveStagingPageCount(checkpoint.header.binding.totalBytes)
		|| !checkpoint.header.checkpoint) { return false; }
	const auto nodes = Nodes(*checkpoint.pageIndex, checkpoint.pageHash, checkpoint.proof);
	return nodes && nodes->back().hash == checkpoint.header.rootHash;
}

QJsonObject CheckpointObject(const ArchiveStagingCheckpoint &checkpoint) {
	auto siblings = QJsonArray();
	for (const auto &hash : checkpoint.proof.siblings) { siblings.push_back(hash); }
	return { { u"header"_q, HeaderObject(checkpoint.header) },
		{ u"previous"_q, checkpoint.previousHeaderHash },
		{ u"index"_q, checkpoint.pageIndex ? QJsonValue(QString::number(*checkpoint.pageIndex)) : QJsonValue(QJsonValue::Null) },
		{ u"pageHash"_q, checkpoint.pageHash }, { u"proof"_q, siblings } };
}

bool CheckpointRead(const QJsonValue &value, ArchiveStagingCheckpoint &out) {
	if (!value.isObject()) { return false; }
	auto object = value.toObject();
	const auto index = object.take(u"index"_q);
	if (index.isString()) {
		auto item = QJsonObject{ { u"index"_q, index } };
		auto number = uint64_t(0);
		if (!Number(item, u"index"_q, number)) { return false; }
		out.pageIndex = number;
	} else if (!index.isNull()) { return false; }
	const auto proof = object.take(u"proof"_q);
	if (!proof.isArray() || proof.toArray().size() > kArchiveStagingProofDepth) { return false; }
	for (const auto &hash : proof.toArray()) {
		if (!hash.isString()) { return false; }
		out.proof.siblings.push_back(hash.toString());
	}
	return HeaderRead(object.take(u"header"_q), out.header)
		&& String(object, u"previous"_q, out.previousHeaderHash)
		&& String(object, u"pageHash"_q, out.pageHash) && object.isEmpty() && CheckpointShape(out);
}

bool SameHeader(const ArchiveStagingHeader &a, const ArchiveStagingHeader &b) {
	const auto hash = ArchiveStagingHeaderHash(a);
	return !hash.isEmpty() && hash == ArchiveStagingHeaderHash(b);
}

bool PageMatches(const ArchiveStagingCoveragePage &page, const ArchiveStagingHeader &header,
		const ArchiveSqlSecurity &security) {
	const auto size = WindowSize(header.binding, page.index);
	return PageCovered(page) && size && page.ranges[0].length == *size
		&& page.bindingId == ArchiveStagingBindingId(header.binding, security)
		&& page.checkpoint <= header.checkpoint;
}

std::optional<QByteArray> OpenPiece(const ArchiveStagingBinding &binding,
		const ArchiveStagingCoveragePage &page, const QByteArray &frame,
		const ArchiveSqlSecurity &security) {
	const auto context = ArchiveStagingPieceContext(binding, page.ranges[0], security);
	if (!context || frame.size() > kArchiveStagingWindowBytes + 4608) { return {}; }
	const auto decoded = OpenArchiveEnvelope(*context, security.keyId, frame, security.crypto);
	if (!decoded || decoded.bytes.size() != page.ranges[0].length
		|| Digest(decoded.bytes) != page.ranges[0].pieceHash) { return {}; }
	for (auto offset = uint32_t(0); offset != page.ranges[0].length; ++offset) {
		if (!Covered(page, offset) && decoded.bytes[offset] != '\0') { return {}; }
	}
	return decoded.bytes;
}

bool RecordMatches(const ArchiveStagingBinding &binding,
		const ArchiveStagingRecord &record, const ArchiveSqlSecurity &security) {
	return std::visit([&](const auto &value) {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, ArchiveStagingCoveragePage>) {
			const auto size = WindowSize(binding, value.index);
			return PageCovered(value) && size && value.ranges[0].length == *size
				&& value.bindingId == ArchiveStagingBindingId(binding, security);
		} else if constexpr (std::is_same_v<T, ArchiveStagingHeader>) { return SameBinding(binding, value.binding); }
		else { return SameBinding(binding, value.header.binding); }
	}, record);
}

}

ArchiveStagingStatus CheckArchiveStagingBinding(const ArchiveStagingBinding &binding,
		const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	if (current.version > kArchiveSchemaVersion) { return ArchiveStagingStatus::ReadOnly; }
	if (!BindingShape(binding) || !IsArchiveState(current)) { return ArchiveStagingStatus::Invalid; }
	const auto &source = binding.source;
	if (!CanRunArchiveWork(source.epoch, epoch) || epoch.account != current.account
		|| source.key.account != current.account
		|| source.generation != ArchiveGenerationFor(current, source.key.chat)) { return ArchiveStagingStatus::Stale; }
	const auto job = std::find_if(current.jobs.begin(), current.jobs.end(),
		[&](const auto &entry) { return entry.id == binding.jobId; });
	const auto version = FindArchiveVersion(current, source.key, source.version);
	if (job == current.jobs.end() || job->state != ArchiveJobState::Staged || !version
		|| !SameArchiveMessage(job->key, source.key) || job->version != source.version
		|| job->revision != source.revision || job->contentHash != source.contentHash
		|| job->mediaIdentity != source.mediaIdentity || job->generation != source.generation
		|| version->revision != source.revision || ArchiveContentHash(version->content) != source.contentHash
		|| (job->expectedSize && *job->expectedSize != binding.totalBytes)
		|| (!job->expectedHash.isEmpty() && job->expectedHash != binding.expectedBlobHash.value_or(QString()))
		|| (!binding.totalBytes && (!job->expectedSize || *job->expectedSize))) { return ArchiveStagingStatus::Stale; }
	return ArchiveStagingStatus::Ready;
}

QString ArchiveStagingBindingId(const ArchiveStagingBinding &binding, const ArchiveSqlSecurity &security) {
	const auto bytes = BindingBytes(binding);
	return bytes.isEmpty() || binding.source.epoch.storeId != security.storeId ? QString()
		: ArchiveSqlToken(binding.source.key.account, ArchiveIndexKind::Job,
			QByteArray("staging-binding/v1/") + bytes, security);
}

uint64_t ArchiveStagingPageCount(uint64_t totalBytes) {
	return totalBytes ? (totalBytes - 1) / kArchiveStagingWindowBytes + 1 : 0;
}

std::optional<uint32_t> ArchiveStagingInputSize(const ArchiveStagingBinding &binding,
		uint64_t offset, uint64_t available) {
	if (!BindingShape(binding) || !available || offset >= binding.totalBytes) { return {}; }
	return uint32_t(std::min({ available, uint64_t(kArchiveStagingWindowBytes) - offset % kArchiveStagingWindowBytes,
		binding.totalBytes - offset }));
}

ArchiveStagingProof ArchiveStagingEmptyProof() {
	auto result = ArchiveStagingProof();
	auto hash = EmptyLeaf();
	for (auto level = 1; level <= kArchiveStagingProofDepth; ++level) {
		result.siblings.push_back(hash);
		hash = Parent(level, hash, hash);
	}
	return result;
}

QString ArchiveStagingEmptyRoot() {
	static const auto root = [] {
		const auto proof = ArchiveStagingEmptyProof();
		return Nodes(0, EmptyLeaf(), proof)->back().hash;
	}();
	return root;
}

std::optional<ArchiveStagingHeader> BeginArchiveStaging(const ArchiveStagingBinding &binding,
		const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	if (CheckArchiveStagingBinding(binding, current, epoch) != ArchiveStagingStatus::Ready) { return {}; }
	return ArchiveStagingHeader{ binding, 0, ArchiveStagingEmptyRoot(), 0, 0 };
}

std::optional<QByteArray> EncodeArchiveStagingRecord(const ArchiveStagingRecord &record) {
	auto payload = QJsonObject();
	auto valid = false;
	std::visit([&](const auto &value) {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, ArchiveStagingHeader>) { valid = HeaderShape(value); if (valid) { payload = HeaderObject(value); } }
		else if constexpr (std::is_same_v<T, ArchiveStagingCoveragePage>) { valid = PageCovered(value).has_value(); if (valid) { payload = PageObject(value); } }
		else { valid = CheckpointShape(value); if (valid) { payload = CheckpointObject(value); } }
	}, record);
	if (!valid) { return {}; }
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(QJsonObject{
		{ u"format"_q, u"purple-archive-staging"_q }, { u"version"_q, kArchiveStagingVersion },
		{ u"kind"_q, int(record.index()) }, { u"value"_q, payload } }).toJson(QJsonDocument::Compact));
	return encoded && encoded.canonical.size() <= kArchiveStagingRecordByteLimit
		? std::optional(encoded.canonical) : std::nullopt;
}

ArchiveStagingDecodeResult DecodeArchiveStagingRecord(const QByteArray &bytes) {
	if (bytes.size() > kArchiveStagingRecordByteLimit) { return { ArchiveStagingStatus::TooLarge }; }
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) { return {}; }
	auto object = QJsonDocument::fromJson(canonical.canonical).object();
	if (object.take(u"format"_q) != QJsonValue(u"purple-archive-staging"_q)) { return { ArchiveStagingStatus::Unsupported }; }
	const auto version = object.take(u"version"_q);
	if (!version.isDouble() || version.toInteger(-1) < 1) { return {}; }
	if (version.toInteger() > kArchiveStagingVersion) { return { ArchiveStagingStatus::NewerFormat }; }
	const auto kind = object.take(u"kind"_q);
	const auto value = object.take(u"value"_q);
	if (!kind.isDouble() || !object.isEmpty()) { return { ArchiveStagingStatus::Unsupported }; }
	auto record = ArchiveStagingRecord();
	auto valid = false;
	switch (kind.toInteger(-1)) {
	case 0: { auto header = ArchiveStagingHeader(); valid = HeaderRead(value, header); record = std::move(header); } break;
	case 1: { auto page = ArchiveStagingCoveragePage(); valid = PageRead(value, page); record = std::move(page); } break;
	case 2: { auto checkpoint = ArchiveStagingCheckpoint(); valid = CheckpointRead(value, checkpoint); record = std::move(checkpoint); } break;
	default: return { ArchiveStagingStatus::Unsupported };
	}
	return valid ? ArchiveStagingDecodeResult{ ArchiveStagingStatus::Ready, std::move(record) } : ArchiveStagingDecodeResult();
}

QString ArchiveStagingHeaderHash(const ArchiveStagingHeader &header) {
	const auto bytes = EncodeArchiveStagingRecord(header);
	return bytes ? Digest(QByteArray("purple-archive/staging/header/v1/") + *bytes) : QString();
}

QString ArchiveStagingPageHash(const ArchiveStagingCoveragePage &page) {
	const auto bytes = EncodeArchiveStagingRecord(page);
	return bytes ? Digest(QByteArray("purple-archive/staging/page/v1/") + *bytes) : QString();
}

std::optional<std::vector<ArchiveStagingNode>> ArchiveStagingNodes(const ArchiveStagingCheckpoint &checkpoint) {
	if (!CheckpointShape(checkpoint)) { return {}; }
	return checkpoint.pageIndex ? Nodes(*checkpoint.pageIndex, checkpoint.pageHash, checkpoint.proof)
		: std::optional(std::vector<ArchiveStagingNode>());
}

ArchiveStagingRecordAddress ArchiveStagingAddress(const ArchiveStagingRecord &record) {
	return std::visit([](const auto &value) -> ArchiveStagingRecordAddress {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, ArchiveStagingHeader>) { return { ArchiveStagingRecordKind::Header, 0, value.checkpoint }; }
		else if constexpr (std::is_same_v<T, ArchiveStagingCoveragePage>) { return { ArchiveStagingRecordKind::CoveragePage, value.index, value.checkpoint }; }
		else { return { ArchiveStagingRecordKind::Checkpoint, value.pageIndex ? uint64_t(1) : uint64_t(0), value.header.checkpoint }; }
	}, record);
}

std::optional<ArchiveEnvelopeContext> ArchiveStagingPieceContext(const ArchiveStagingBinding &binding,
		const ArchiveStagingRange &range, const ArchiveSqlSecurity &security) {
	const auto id = ArchiveStagingBindingId(binding, security);
	const auto size = ArchiveStagingInputSize(binding, range.offset, range.length);
	if (id.isEmpty() || !size || *size != range.length || range.offset % kArchiveStagingWindowBytes
		|| !IsArchiveHash(range.pieceHash)) { return {}; }
	const auto token = ArchiveSqlToken(binding.source.key.account, ArchiveIndexKind::Job,
		(QByteArray("staging-piece/v1/") + id.toUtf8() + '/' + QByteArray::number(range.offset)
			+ '/' + QByteArray::number(range.length) + '/' + range.pieceHash.toUtf8()), security);
	if (token.isEmpty()) { return {}; }
	return ArchiveEnvelopeContext{ binding.source.key.account, security.storeId,
		ArchiveRecordKind::StagingPiece, token.toUtf8() };
}

std::optional<ArchiveEnvelopeContext> ArchiveStagingRecordContext(const ArchiveStagingBinding &binding,
		const ArchiveStagingRecordAddress &address, const ArchiveSqlSecurity &security) {
	const auto id = ArchiveStagingBindingId(binding, security);
	if (id.isEmpty() || int(address.kind) < 0 || int(address.kind) > int(ArchiveStagingRecordKind::Checkpoint)
		|| (address.kind == ArchiveStagingRecordKind::CoveragePage
			? address.position >= ArchiveStagingPageCount(binding.totalBytes) || !address.checkpoint
			: address.kind == ArchiveStagingRecordKind::Header ? address.position != 0 : address.position > 1)) { return {}; }
	const auto token = ArchiveSqlToken(binding.source.key.account, ArchiveIndexKind::Job,
		(QByteArray("staging-record/v1/") + id.toUtf8() + '/' + QByteArray::number(int(address.kind))
			+ '/' + QByteArray::number(address.position) + '/' + QByteArray::number(address.checkpoint)), security);
	if (token.isEmpty()) { return {}; }
	return ArchiveEnvelopeContext{ binding.source.key.account, security.storeId,
		ArchiveRecordKind::StagingProgress, token.toUtf8() };
}

std::optional<QByteArray> SealArchiveStagingRecord(const ArchiveStagingBinding &binding,
		const ArchiveStagingRecord &record, const ArchiveSqlSecurity &security) {
	const auto encoded = RecordMatches(binding, record, security) ? EncodeArchiveStagingRecord(record) : std::nullopt;
	const auto context = ArchiveStagingRecordContext(binding, ArchiveStagingAddress(record), security);
	if (!encoded || !context) { return {}; }
	const auto sealed = SealArchiveEnvelope(*context, security.keyId, *encoded, security.crypto);
	return sealed ? std::optional(sealed.bytes) : std::nullopt;
}

ArchiveStagingDecodeResult OpenArchiveStagingRecord(const ArchiveStagingBinding &binding,
		const ArchiveStagingRecordAddress &address, const QByteArray &frame, const ArchiveSqlSecurity &security) {
	if (frame.size() > kArchiveStagingRecordByteLimit + 4608) { return { ArchiveStagingStatus::TooLarge }; }
	const auto context = ArchiveStagingRecordContext(binding, address, security);
	if (!context) { return {}; }
	const auto opened = OpenArchiveEnvelope(*context, security.keyId, frame, security.crypto);
	if (!opened) { return { ArchiveStagingStatus::IntegrityFailure }; }
	auto decoded = DecodeArchiveStagingRecord(opened.bytes);
	if (!decoded) { return decoded; }
	const auto actual = ArchiveStagingAddress(*decoded.record);
	if (actual.kind != address.kind || actual.position != address.position || actual.checkpoint != address.checkpoint
		|| !RecordMatches(binding, *decoded.record, security)) { return { ArchiveStagingStatus::IntegrityFailure }; }
	return decoded;
}

ArchiveStagingUpdate PlanArchiveStagingPiece(const ArchiveStagingHeader &header,
		const std::optional<ArchiveStagingCoveragePage> &page, const ArchiveStagingProof &proof,
		const QByteArray &oldPieceFrame, const ArchiveStagingPieceFact &fact,
		const ArchiveSqlSecurity &security, const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	const auto fence = CheckArchiveStagingBinding(header.binding, current, epoch);
	if (fence != ArchiveStagingStatus::Ready) { return { fence }; }
	if (!HeaderShape(header) || fact.bytes.isEmpty()) { return {}; }
	if (fact.bytes.size() > kArchiveMediaChunkBytes) { return { ArchiveStagingStatus::TooLarge }; }
	const auto inputSize = ArchiveStagingInputSize(header.binding, fact.offset, fact.bytes.size());
	if (!inputSize) { return {}; }
	if (*inputSize != fact.bytes.size()) { return { ArchiveStagingStatus::TooLarge }; }
	const auto index = fact.offset / kArchiveStagingWindowBytes;
	const auto window = WindowSize(header.binding, index);
	const auto bindingId = ArchiveStagingBindingId(header.binding, security);
	if (!window || bindingId.isEmpty()) { return {}; }
	if (page && (!PageMatches(*page, header, security) || page->index != index)) {
		return { ArchiveStagingStatus::IntegrityFailure };
	}
	const auto oldNodes = Nodes(index, page ? ArchiveStagingPageHash(*page) : EmptyLeaf(), proof);
	if (!oldNodes || oldNodes->back().hash != header.rootHash) { return { ArchiveStagingStatus::IntegrityFailure }; }
	auto bytes = QByteArray();
	auto mask = QByteArray((*window + 7) / 8, '\0');
	auto oldCovered = uint32_t(0);
	if (page) {
		const auto opened = OpenPiece(header.binding, *page, oldPieceFrame, security);
		if (!opened) { return { ArchiveStagingStatus::IntegrityFailure }; }
		bytes = *opened;
		oldCovered = *PageCovered(*page);
		if (page->complete) {
			mask.fill(char(0xff));
			if (*window % 8) { mask[mask.size() - 1] = char((1u << (*window % 8)) - 1); }
		} else {
			mask = page->coverage;
		}
	} else {
		if (!oldPieceFrame.isEmpty()) { return {}; }
		bytes = QByteArray(*window, '\0');
	}
	const auto start = uint32_t(fact.offset % kArchiveStagingWindowBytes);
	auto added = uint32_t(0);
	for (auto i = uint32_t(0); i != uint32_t(fact.bytes.size()); ++i) {
		const auto offset = start + i;
		const auto bit = 1u << (offset % 8);
		const auto known = static_cast<unsigned char>(mask[offset / 8]);
		if (known & bit) {
			if (bytes[offset] != fact.bytes[i]) { return { ArchiveStagingStatus::Conflict }; }
		} else {
			++added;
			mask[offset / 8] = char(known | bit);
			bytes[offset] = fact.bytes[i];
		}
	}
	auto result = ArchiveStagingUpdate();
	result.expected = header;
	result.previousPage = page;
	if (page) { result.previousPieceFrame = oldPieceFrame; }
	if (!added) { result.status = ArchiveStagingStatus::NoChange; return result; }
	if (header.checkpoint == std::numeric_limits<uint64_t>::max()) { return { ArchiveStagingStatus::Exhausted }; }
	if (added > header.binding.totalBytes - header.coveredBytes) { return { ArchiveStagingStatus::IntegrityFailure }; }
	result.page.bindingId = bindingId;
	result.page.index = index;
	result.page.checkpoint = header.checkpoint + 1;
	result.page.complete = oldCovered + added == *window;
	if (!result.page.complete) { result.page.coverage = std::move(mask); }
	result.piece = { index * uint64_t(kArchiveStagingWindowBytes), *window, Digest(bytes) };
	result.page.ranges.push_back(result.piece);
	const auto context = ArchiveStagingPieceContext(header.binding, result.piece, security);
	if (!context) { return {}; }
	const auto sealed = SealArchiveEnvelope(*context, security.keyId, bytes, security.crypto);
	if (!sealed) { return { ArchiveStagingStatus::IntegrityFailure }; }
	result.pieceId = QString::fromLatin1(context->rowId);
	result.pieceFrame = sealed.bytes;
	result.next.header = header;
	++result.next.header.checkpoint;
	result.next.header.coveredBytes += added;
	if (!page) { ++result.next.header.pages; }
	result.next.previousHeaderHash = ArchiveStagingHeaderHash(header);
	result.next.pageIndex = index;
	result.next.pageHash = ArchiveStagingPageHash(result.page);
	result.next.proof = proof;
	const auto nodes = Nodes(index, result.next.pageHash, proof);
	if (!nodes) { return {}; }
	result.next.header.rootHash = nodes->back().hash;
	if (!EncodeArchiveStagingRecord(result.page) || !EncodeArchiveStagingRecord(result.next)) {
		return { ArchiveStagingStatus::TooLarge };
	}
	result.status = ArchiveStagingStatus::Ready;
	return result;
}

ArchiveStagingCommit CompleteArchiveStagingPiece(const ArchiveStagingHeader &currentHeader,
		const ArchiveStagingUpdate &update, const ArchiveStagingDurability &ack,
		const ArchiveSqlSecurity &security, const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	const auto fence = CheckArchiveStagingBinding(update.expected.binding, current, epoch);
	if (fence != ArchiveStagingStatus::Ready) { return { fence }; }
	if (update.status == ArchiveStagingStatus::NoChange) {
		return SameHeader(currentHeader, update.expected)
			? ArchiveStagingCommit{ ArchiveStagingStatus::NoChange, currentHeader }
			: ArchiveStagingCommit{ ArchiveStagingStatus::Stale };
	}
	if (!update || !CheckpointShape(update.next) || !SameBinding(update.expected.binding, update.next.header.binding)
		|| update.expected.checkpoint == std::numeric_limits<uint64_t>::max()
		|| update.next.header.checkpoint != update.expected.checkpoint + 1
		|| update.next.previousHeaderHash != ArchiveStagingHeaderHash(update.expected)
		|| update.next.pageIndex != std::optional(update.page.index)
		|| update.next.pageHash != ArchiveStagingPageHash(update.page)
		|| update.page.checkpoint != update.next.header.checkpoint
		|| !PageMatches(update.page, update.next.header, security) || update.page.ranges[0] != update.piece) { return {}; }
	const auto previous = update.previousPage ? PageCovered(*update.previousPage) : std::optional(uint32_t(0));
	const auto covered = PageCovered(update.page);
	if (!previous || !covered || *covered <= *previous
		|| (update.previousPage && (!PageMatches(*update.previousPage, update.expected, security)
			|| update.previousPage->index != update.page.index))) { return {}; }
	const auto oldNodes = Nodes(update.page.index,
		update.previousPage ? ArchiveStagingPageHash(*update.previousPage) : EmptyLeaf(), update.next.proof);
	const auto added = *covered - *previous;
	if (!oldNodes || oldNodes->back().hash != update.expected.rootHash
		|| added > update.expected.binding.totalBytes - update.expected.coveredBytes
		|| update.next.header.coveredBytes != update.expected.coveredBytes + added
		|| update.next.header.pages != update.expected.pages + (update.previousPage ? 0 : 1)) { return {}; }
	if (update.previousPage) {
		for (auto offset = uint32_t(0); offset != update.piece.length; ++offset) {
			if (Covered(*update.previousPage, offset) && !Covered(update.page, offset)) { return {}; }
		}
	}
	const auto context = ArchiveStagingPieceContext(update.expected.binding, update.piece, security);
	const auto nextBytes = OpenPiece(update.expected.binding, update.page, update.pieceFrame, security);
	if (!context || update.pieceId != QString::fromLatin1(context->rowId) || !nextBytes) {
		return { ArchiveStagingStatus::IntegrityFailure };
	}
	if (update.previousPage) {
		const auto oldBytes = OpenPiece(update.expected.binding, *update.previousPage, update.previousPieceFrame, security);
		if (!oldBytes) { return { ArchiveStagingStatus::IntegrityFailure }; }
		for (auto offset = uint32_t(0); offset != update.piece.length; ++offset) {
			if (Covered(*update.previousPage, offset) && (*oldBytes)[offset] != (*nextBytes)[offset]) {
				return { ArchiveStagingStatus::Conflict };
			}
		}
	} else if (!update.previousPieceFrame.isEmpty()) { return {}; }
	const auto applied = SameHeader(currentHeader, update.next.header);
	if (!applied && !SameHeader(currentHeader, update.expected)) { return { ArchiveStagingStatus::Stale }; }
	if (ack.bindingId != ArchiveStagingBindingId(update.expected.binding, security)
		|| ack.pieceId != update.pieceId || ack.expectedHeaderHash != ArchiveStagingHeaderHash(update.expected)
		|| ack.nextHeaderHash != ArchiveStagingHeaderHash(update.next.header)) { return { ArchiveStagingStatus::Stale }; }
	if (!ack.pieceDurable || !ack.checkpointCasDurable) { return { ArchiveStagingStatus::NotCommitted }; }
	return { applied ? ArchiveStagingStatus::AlreadyApplied : ArchiveStagingStatus::Ready, update.next.header };
}

std::optional<ArchiveStagingCheckpoint> FreezeArchiveStagingCheckpoint(const ArchiveStagingHeader &header,
		const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	if (!HeaderShape(header) || CheckArchiveStagingBinding(header.binding, current, epoch) != ArchiveStagingStatus::Ready
		|| header.coveredBytes != header.binding.totalBytes
		|| header.pages != ArchiveStagingPageCount(header.binding.totalBytes)) { return {}; }
	return ArchiveStagingCheckpoint{ header, ArchiveStagingHeaderHash(header) };
}

ArchiveStagingStatus CheckArchiveStagingPublication(const ArchiveStagingPublication &publication,
		const ArchiveStagingHeader &currentHeader, const ArchiveSqlSecurity &security,
		const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	const auto fence = CheckArchiveStagingBinding(publication.binding, current, epoch);
	if (fence != ArchiveStagingStatus::Ready) { return fence; }
	if (!HeaderShape(currentHeader) || currentHeader.coveredBytes != publication.binding.totalBytes
		|| currentHeader.pages != ArchiveStagingPageCount(publication.binding.totalBytes)
		|| !SameBinding(publication.binding, currentHeader.binding)
		|| publication.checkpointHash != ArchiveStagingHeaderHash(currentHeader)
		|| publication.bindingId != ArchiveStagingBindingId(publication.binding, security)) { return ArchiveStagingStatus::Stale; }
	const auto size = ArchiveMediaChunkSize(publication.binding.totalBytes, publication.chunkIndex);
	if (!publication.requestId || !size || uint32_t(*size) != publication.plaintextBytes
		|| !IsArchiveHash(publication.blobHash) || !IsArchiveHash(publication.plaintextHash)
		|| (publication.binding.expectedBlobHash && *publication.binding.expectedBlobHash != publication.blobHash)
		|| publication.frame.size() > kArchiveMediaChunkBytes + 4608
		|| publication.frameHash != Digest(publication.frame)) { return ArchiveStagingStatus::IntegrityFailure; }
	const auto token = ArchiveSqlToken(publication.binding.source.key.account, ArchiveIndexKind::Blob,
		publication.blobHash.toUtf8(), security);
	if (token.isEmpty() || token != publication.blobToken) { return ArchiveStagingStatus::IntegrityFailure; }
	const auto context = ArchiveMediaChunkContext(publication.binding.source.key.account,
		security.storeId, token, publication.binding.totalBytes, publication.chunkIndex);
	if (!context) { return ArchiveStagingStatus::IntegrityFailure; }
	const auto opened = OpenArchiveEnvelope(*context, security.keyId, publication.frame, security.crypto);
	return opened && opened.bytes.size() == *size && Digest(opened.bytes) == publication.plaintextHash
		? ArchiveStagingStatus::Ready : ArchiveStagingStatus::IntegrityFailure;
}

ArchiveStagingPublishAck ArchiveStagingPublicationAck(const ArchiveStagingPublication &publication, bool durable) {
	return { publication.requestId, publication.bindingId, publication.checkpointHash,
		publication.blobToken, publication.chunkIndex, publication.frameHash, durable, {} };
}

struct ArchiveStagingFinalizer::Data {
	ArchiveStagingCheckpoint checkpoint;
	QString checkpointHash;
	QString bindingId;
	QString keyId;
	ArchiveStagingFinalizePhase phase = ArchiveStagingFinalizePhase::Verify;
	ArchiveStagingStatus failure = ArchiveStagingStatus::Ready;
	ArchiveStagingRequest request;
	std::optional<ArchiveStagingCoveragePage> page;
	QCryptographicHash hash = QCryptographicHash(QCryptographicHash::Sha256);
	QString verifiedHash;
	QString blobToken;
	QByteArray buffer;
	std::optional<ArchiveStagingPublication> pending;
	uint64_t pageIndex = 0;
	uint64_t verifiedBytes = 0;
	uint64_t resealedBytes = 0;
	uint64_t publishedChunks = 0;
};

namespace {

ArchiveStagingFinalizeStep Step(const ArchiveStagingFinalizer::Data &data, ArchiveStagingStatus status) {
	auto result = ArchiveStagingFinalizeStep();
	result.status = status;
	result.phase = data.phase;
	result.request = data.request;
	result.verifiedBytes = data.verifiedBytes;
	result.resealedBytes = data.resealedBytes;
	result.publishedChunks = data.publishedChunks;
	result.bufferedPlaintextBytes = uint32_t(data.buffer.size());
	if (status == ArchiveStagingStatus::Ready || status == ArchiveStagingStatus::NoChange
		|| status == ArchiveStagingStatus::NotCommitted) { result.publication = data.pending; }
	return result;
}

ArchiveStagingFinalizeStep Fail(ArchiveStagingFinalizer::Data &data, ArchiveStagingStatus status) {
	data.phase = ArchiveStagingFinalizePhase::Refused;
	data.failure = status;
	data.pending.reset();
	data.page.reset();
	data.buffer.fill('\0');
	data.buffer.clear();
	data.hash.reset();
	return Step(data, status);
}

bool Request(ArchiveStagingFinalizer::Data &data, ArchiveStagingRequestKind kind) {
	if (data.request.id == std::numeric_limits<uint64_t>::max()) { return false; }
	data.request = { kind, data.request.id + 1, data.pageIndex, data.publishedChunks, {} };
	return true;
}

ArchiveStagingStatus Verified(ArchiveStagingFinalizer::Data &data, const ArchiveSqlSecurity &security) {
	data.verifiedHash = QString::fromLatin1(data.hash.result().toHex());
	const auto &binding = data.checkpoint.header.binding;
	if (data.verifiedBytes != binding.totalBytes
		|| (binding.expectedBlobHash && *binding.expectedBlobHash != data.verifiedHash)) {
		return ArchiveStagingStatus::IntegrityFailure;
	}
	data.blobToken = ArchiveSqlToken(binding.source.key.account, ArchiveIndexKind::Blob,
		data.verifiedHash.toUtf8(), security);
	if (data.blobToken.isEmpty()) { return ArchiveStagingStatus::IntegrityFailure; }
	data.hash.reset();
	data.pageIndex = 0;
	data.phase = ArchiveStagingFinalizePhase::Reseal;
	data.buffer.reserve(kArchiveMediaChunkBytes);
	return ArchiveStagingStatus::Ready;
}

ArchiveStagingStatus Publish(ArchiveStagingFinalizer::Data &data,
		const ArchiveStagingHeader &currentHeader, const ArchiveSqlSecurity &security,
		const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	const auto &binding = data.checkpoint.header.binding;
	const auto size = ArchiveMediaChunkSize(binding.totalBytes, data.publishedChunks);
	if (!size || data.buffer.size() != *size || data.buffer.size() > kArchiveMediaChunkBytes
		|| (data.resealedBytes == binding.totalBytes
			&& QString::fromLatin1(data.hash.result().toHex()) != data.verifiedHash)) {
		return ArchiveStagingStatus::IntegrityFailure;
	}
	const auto context = ArchiveMediaChunkContext(binding.source.key.account,
		security.storeId, data.blobToken, binding.totalBytes, data.publishedChunks);
	if (!context) { return ArchiveStagingStatus::IntegrityFailure; }
	const auto sealed = SealArchiveEnvelope(*context, security.keyId, data.buffer, security.crypto);
	if (!sealed) { return ArchiveStagingStatus::IntegrityFailure; }
	if (!Request(data, ArchiveStagingRequestKind::Publish)) { return ArchiveStagingStatus::Exhausted; }
	data.pending = ArchiveStagingPublication{ binding, data.bindingId, data.checkpointHash,
		data.verifiedHash, data.blobToken, data.request.id, data.publishedChunks,
		uint32_t(data.buffer.size()), Digest(data.buffer), Digest(sealed.bytes), sealed.bytes };
	const auto checked = CheckArchiveStagingPublication(*data.pending, currentHeader, security, current, epoch);
	if (checked != ArchiveStagingStatus::Ready) { return checked; }
	data.phase = ArchiveStagingFinalizePhase::AwaitPublication;
	return ArchiveStagingStatus::Ready;
}

ArchiveStagingStatus Translate(ArchivePlanStatus status) {
	switch (status) {
	case ArchivePlanStatus::Ready: return ArchiveStagingStatus::Ready;
	case ArchivePlanStatus::NoChange: return ArchiveStagingStatus::NoChange;
	case ArchivePlanStatus::ReadOnly: return ArchiveStagingStatus::ReadOnly;
	case ArchivePlanStatus::Stale:
	case ArchivePlanStatus::Purged:
	case ArchivePlanStatus::NotFound: return ArchiveStagingStatus::Stale;
	case ArchivePlanStatus::Conflict: return ArchiveStagingStatus::Conflict;
	case ArchivePlanStatus::Exhausted: return ArchiveStagingStatus::Exhausted;
	case ArchivePlanStatus::Invalid: return ArchiveStagingStatus::Invalid;
	}
	return ArchiveStagingStatus::Invalid;
}

}

ArchiveStagingFinalizer::ArchiveStagingFinalizer(std::unique_ptr<Data> data) : _data(std::move(data)) {
}

ArchiveStagingFinalizer::~ArchiveStagingFinalizer() = default;
ArchiveStagingFinalizer::ArchiveStagingFinalizer(ArchiveStagingFinalizer &&) noexcept = default;
ArchiveStagingFinalizer &ArchiveStagingFinalizer::operator=(ArchiveStagingFinalizer &&) noexcept = default;

ArchiveStagingRequest ArchiveStagingFinalizer::request() const {
	return _data ? _data->request : ArchiveStagingRequest{ ArchiveStagingRequestKind::Finished };
}

ArchiveStagingFinalizeStart BeginArchiveStagingFinalize(const ArchiveStagingHeader &currentHeader,
		const ArchiveStagingCheckpoint &checkpoint, const ArchiveSqlSecurity &security,
		const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	const auto fence = CheckArchiveStagingBinding(checkpoint.header.binding, current, epoch);
	if (fence != ArchiveStagingStatus::Ready) { return { fence }; }
	if (!CheckpointShape(checkpoint)) { return {}; }
	if (!SameHeader(currentHeader, checkpoint.header)) { return { ArchiveStagingStatus::Stale }; }
	if (checkpoint.header.coveredBytes != checkpoint.header.binding.totalBytes
		|| checkpoint.header.pages != ArchiveStagingPageCount(checkpoint.header.binding.totalBytes)) {
		return { ArchiveStagingStatus::Incomplete };
	}
	auto data = std::make_unique<ArchiveStagingFinalizer::Data>();
	data->checkpoint = checkpoint;
	data->checkpointHash = ArchiveStagingHeaderHash(checkpoint.header);
	data->bindingId = ArchiveStagingBindingId(checkpoint.header.binding, security);
	data->keyId = security.keyId;
	if (data->bindingId.isEmpty()) { return {}; }
	if (checkpoint.header.binding.totalBytes) {
		Request(*data, ArchiveStagingRequestKind::Page);
	} else {
		const auto verified = Verified(*data, security);
		if (verified != ArchiveStagingStatus::Ready) { return { verified }; }
		const auto published = Publish(*data, currentHeader, security, current, epoch);
		if (published != ArchiveStagingStatus::Ready) { return { published }; }
	}
	auto result = ArchiveStagingFinalizeStart();
	result.status = ArchiveStagingStatus::Ready;
	result.step = Step(*data, ArchiveStagingStatus::Ready);
	result.session = std::unique_ptr<ArchiveStagingFinalizer>(new ArchiveStagingFinalizer(std::move(data)));
	return result;
}

ArchiveStagingFinalizeStep AdvanceArchiveStagingFinalize(ArchiveStagingFinalizer &session,
		const ArchiveStagingFinalizeInput &input, const ArchiveStagingHeader &currentHeader,
		const ArchiveSqlSecurity &security, const ArchiveState &current, const ArchiveStoreEpoch &epoch) {
	if (!session._data) { return {}; }
	auto &data = *session._data;
	if (data.phase == ArchiveStagingFinalizePhase::Refused) { return Step(data, data.failure); }
	const auto &binding = data.checkpoint.header.binding;
	const auto fence = CheckArchiveStagingBinding(binding, current, epoch);
	if (fence != ArchiveStagingStatus::Ready) { return Fail(data, fence); }
	if (!SameHeader(currentHeader, data.checkpoint.header) || security.keyId != data.keyId
		|| data.bindingId != ArchiveStagingBindingId(binding, security)) { return Fail(data, ArchiveStagingStatus::Stale); }
	const auto requestId = std::visit([](const auto &value) { return value.requestId; }, input);
	if (requestId < data.request.id) { return Step(data, ArchiveStagingStatus::NoChange); }
	if (requestId != data.request.id || data.phase == ArchiveStagingFinalizePhase::Finished) {
		return Step(data, ArchiveStagingStatus::Invalid);
	}
	if (data.request.kind == ArchiveStagingRequestKind::Page) {
		const auto pageInput = std::get_if<ArchiveStagingPageInput>(&input);
		if (!pageInput) { return Step(data, ArchiveStagingStatus::Invalid); }
		if (pageInput->frame.isEmpty()) { return Fail(data, ArchiveStagingStatus::Incomplete); }
		const auto opened = OpenArchiveStagingRecord(binding,
			{ ArchiveStagingRecordKind::CoveragePage, data.pageIndex, pageInput->checkpoint }, pageInput->frame, security);
		if (!opened) { return Fail(data, opened.status); }
		const auto page = std::get_if<ArchiveStagingCoveragePage>(&*opened.record);
		if (!page || !PageMatches(*page, data.checkpoint.header, security) || page->index != data.pageIndex) {
			return Fail(data, ArchiveStagingStatus::IntegrityFailure);
		}
		const auto nodes = Nodes(data.pageIndex, ArchiveStagingPageHash(*page), pageInput->proof);
		if (!nodes || nodes->back().hash != data.checkpoint.header.rootHash) {
			return Fail(data, ArchiveStagingStatus::IntegrityFailure);
		}
		if (!page->complete) { return Fail(data, ArchiveStagingStatus::Incomplete); }
		data.page = *page;
		if (!Request(data, ArchiveStagingRequestKind::Piece)) { return Fail(data, ArchiveStagingStatus::Exhausted); }
		data.request.range = page->ranges[0];
		return Step(data, ArchiveStagingStatus::Ready);
	}
	if (data.request.kind == ArchiveStagingRequestKind::Piece) {
		const auto pieceInput = std::get_if<ArchiveStagingPieceInput>(&input);
		if (!pieceInput || !data.page) { return Step(data, ArchiveStagingStatus::Invalid); }
		const auto bytes = OpenPiece(binding, *data.page, pieceInput->frame, security);
		if (!bytes) { return Fail(data, ArchiveStagingStatus::IntegrityFailure); }
		data.page.reset();
		if (data.phase == ArchiveStagingFinalizePhase::Verify) {
			if (uint64_t(bytes->size()) > binding.totalBytes - data.verifiedBytes) {
				return Fail(data, ArchiveStagingStatus::IntegrityFailure);
			}
			data.hash.addData(*bytes);
			data.verifiedBytes += bytes->size();
			++data.pageIndex;
			if (data.verifiedBytes == binding.totalBytes) {
				const auto checked = Verified(data, security);
				if (checked != ArchiveStagingStatus::Ready) { return Fail(data, checked); }
			}
			if (!Request(data, ArchiveStagingRequestKind::Page)) { return Fail(data, ArchiveStagingStatus::Exhausted); }
			return Step(data, ArchiveStagingStatus::Ready);
		}
		if (data.phase != ArchiveStagingFinalizePhase::Reseal
			|| uint64_t(bytes->size()) > binding.totalBytes - data.resealedBytes
			|| bytes->size() > kArchiveMediaChunkBytes - data.buffer.size()) {
			return Fail(data, ArchiveStagingStatus::IntegrityFailure);
		}
		data.hash.addData(*bytes);
		data.buffer.append(*bytes);
		data.resealedBytes += bytes->size();
		++data.pageIndex;
		if (data.buffer.size() == kArchiveMediaChunkBytes || data.resealedBytes == binding.totalBytes) {
			const auto published = Publish(data, currentHeader, security, current, epoch);
			return published == ArchiveStagingStatus::Ready ? Step(data, published) : Fail(data, published);
		}
		if (!Request(data, ArchiveStagingRequestKind::Page)) { return Fail(data, ArchiveStagingStatus::Exhausted); }
		return Step(data, ArchiveStagingStatus::Ready);
	}
	if (data.request.kind == ArchiveStagingRequestKind::Publish) {
		const auto ack = std::get_if<ArchiveStagingPublishAck>(&input);
		if (!ack || !data.pending) { return Step(data, ArchiveStagingStatus::Invalid); }
		const auto &pending = *data.pending;
		if (ack->bindingId != pending.bindingId || ack->checkpointHash != pending.checkpointHash
			|| ack->blobToken != pending.blobToken || ack->chunkIndex != pending.chunkIndex) {
			return Fail(data, ArchiveStagingStatus::Stale);
		}
		auto published = pending;
		if (!ack->existingFrame.isEmpty()) { published.frame = ack->existingFrame; }
		published.frameHash = ack->frameHash;
		const auto checked = CheckArchiveStagingPublication(published, currentHeader, security, current, epoch);
		if (checked != ArchiveStagingStatus::Ready) { return Fail(data, checked); }
		if (!ack->durable) { return Step(data, ArchiveStagingStatus::NotCommitted); }
		++data.publishedChunks;
		data.pending.reset();
		data.buffer.fill('\0');
		data.buffer.clear();
		if (data.resealedBytes == binding.totalBytes) {
			data.phase = ArchiveStagingFinalizePhase::AwaitCompletion;
			if (!Request(data, ArchiveStagingRequestKind::Complete)) { return Fail(data, ArchiveStagingStatus::Exhausted); }
		} else {
			data.phase = ArchiveStagingFinalizePhase::Reseal;
			if (!Request(data, ArchiveStagingRequestKind::Page)) { return Fail(data, ArchiveStagingStatus::Exhausted); }
		}
		return Step(data, ArchiveStagingStatus::Ready);
	}
	if (data.request.kind == ArchiveStagingRequestKind::Complete) {
		if (!std::holds_alternative<ArchiveStagingFinish>(input)) { return Step(data, ArchiveStagingStatus::Invalid); }
		if (data.resealedBytes != binding.totalBytes || data.verifiedBytes != binding.totalBytes
			|| QString::fromLatin1(data.hash.result().toHex()) != data.verifiedHash
			|| data.publishedChunks != (binding.totalBytes ? (binding.totalBytes - 1) / kArchiveMediaChunkBytes + 1 : 1)) {
			return Fail(data, ArchiveStagingStatus::IntegrityFailure);
		}
		auto completion = PlanArchiveScopedMediaCompletion(current, binding.source.epoch, epoch,
			binding.jobId, { true, data.verifiedHash, data.verifiedHash, binding.totalBytes, {} });
		const auto checked = Translate(completion.status);
		if (checked != ArchiveStagingStatus::Ready && checked != ArchiveStagingStatus::NoChange) { return Fail(data, checked); }
		data.phase = ArchiveStagingFinalizePhase::Finished;
		Request(data, ArchiveStagingRequestKind::Finished);
		auto result = Step(data, ArchiveStagingStatus::Done);
		result.completion = std::move(completion);
		return result;
	}
	return Step(data, ArchiveStagingStatus::Invalid);
}

}
