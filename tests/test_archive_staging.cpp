/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "tests/archive_storage_test_support.h"
#include "purple/purple_archive_staging.h"

#include <map>

using namespace ArchiveTest;
using namespace ArchiveStorageTest;

namespace {

QString Hash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct Fixture {
	ArchiveState state = Empty();
	ArchiveSqlSecurity security = Security();
	ArchiveStoreEpoch epoch = { state.account, security.storeId, 1 };
	ArchiveStagingBinding binding;
	ArchiveStagingHeader header;
	std::map<uint64_t, ArchiveStagingCoveragePage> pages;
	std::map<QString, QByteArray> pieces;
	std::map<std::pair<int, uint64_t>, QString> nodes;

	explicit Fixture(uint64_t size, std::optional<QString> expected = {}) {
		auto event = Event(state, u"v1"_q, 1, u"streamed media"_q);
		AddMedia(event);
		CHECK(Apply(state, PlanArchiveEvent(state, event)));
		auto job = Job(state, u"copy-v1"_q, u"v1"_q);
		job.expectedSize = size;
		job.expectedHash = expected.value_or(QString());
		CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
		const auto &version = Message(state).versions[0];
		binding = { { epoch, Key(), version.id, version.revision,
			ArchiveContentHash(version.content), u"document:555"_q, ArchiveGenerationFor(state, Key().chat) },
			job.id, size, expected };
		const auto opened = BeginArchiveStaging(binding, state, epoch);
		CHECK(opened);
		if (opened) { header = *opened; }
	}

	ArchiveStagingProof proof(uint64_t index) const {
		auto result = ArchiveStagingEmptyProof();
		for (auto level = 0; level != kArchiveStagingProofDepth; ++level) {
			const auto found = nodes.find({ level, (index >> level) ^ 1 });
			if (found != nodes.end()) { result.siblings[level] = found->second; }
		}
		return result;
	}

	std::optional<ArchiveStagingCoveragePage> page(uint64_t index) const {
		const auto found = pages.find(index);
		return found == pages.end() ? std::nullopt : std::optional(found->second);
	}

	QByteArray frame(uint64_t index) const {
		const auto found = pages.find(index);
		if (found == pages.end()) { return {}; }
		const auto context = ArchiveStagingPieceContext(binding, found->second.ranges[0], security);
		CHECK(context);
		const auto stored = context ? pieces.find(QString::fromLatin1(context->rowId)) : pieces.end();
		return stored == pieces.end() ? QByteArray() : stored->second;
	}

	ArchiveStagingUpdate plan(uint64_t offset, const QByteArray &bytes) const {
		const auto index = offset / kArchiveStagingWindowBytes;
		return PlanArchiveStagingPiece(header, page(index), proof(index), frame(index),
			{ offset, bytes }, security, state, epoch);
	}

	ArchiveStagingDurability ack(const ArchiveStagingUpdate &update, bool piece = true, bool cas = true) const {
		return { piece, cas, ArchiveStagingBindingId(binding, security), update.pieceId,
			ArchiveStagingHeaderHash(update.expected), ArchiveStagingHeaderHash(update.next.header) };
	}

	bool install(const ArchiveStagingUpdate &update) {
		const auto result = CompleteArchiveStagingPiece(header, update, ack(update), security, state, epoch);
		CHECK(result.status == ArchiveStagingStatus::Ready && result.header);
		if (!result.header) { return false; }
		pieces[update.pieceId] = update.pieceFrame;
		pages[update.page.index] = update.page;
		nodes[{ 0, update.page.index }] = ArchiveStagingPageHash(update.page);
		const auto changes = ArchiveStagingNodes(update.next);
		CHECK(changes && changes->size() == kArchiveStagingProofDepth);
		if (changes) { for (const auto &node : *changes) { nodes[{ node.level, node.index }] = node.hash; } }
		header = *result.header;
		return true;
	}

	bool put(uint64_t offset, const QByteArray &bytes) {
		const auto update = plan(offset, bytes);
		CHECK(update);
		return update && install(update);
	}
};

void RoundTripRecord(const ArchiveStagingBinding &binding, const ArchiveStagingRecord &record,
		const ArchiveSqlSecurity &security) {
	const auto encoded = EncodeArchiveStagingRecord(record);
	CHECK(encoded && encoded->size() <= kArchiveStagingRecordByteLimit);
	if (!encoded) { return; }
	const auto decoded = DecodeArchiveStagingRecord(*encoded);
	CHECK(decoded && EncodeArchiveStagingRecord(*decoded.record) == encoded);
	const auto sealed = SealArchiveStagingRecord(binding, record, security);
	CHECK(sealed);
	if (!sealed) { return; }
	const auto opened = OpenArchiveStagingRecord(binding, ArchiveStagingAddress(record), *sealed, security);
	CHECK(opened && EncodeArchiveStagingRecord(*opened.record) == encoded);
	auto readOnly = security;
	readOnly.crypto.random = {};
	readOnly.crypto.sealAes256Gcm = {};
	CHECK(OpenArchiveStagingRecord(binding, ArchiveStagingAddress(record), *sealed, readOnly));
}

void ContextsAndCodec() {
	Section = "staging contexts and bounded versioned codec";
	auto f = Fixture(100);
	RoundTripRecord(f.binding, f.header, f.security);
	const auto headerBytes = EncodeArchiveStagingRecord(f.header);
	CHECK(headerBytes);
	if (!headerBytes) { return; }
	for (const auto version : { int64(2), int64(2147483648LL) }) {
		auto object = QJsonDocument::fromJson(*headerBytes).object();
		object[u"version"_q] = version;
		CHECK(DecodeArchiveStagingRecord(QJsonDocument(object).toJson()).status == ArchiveStagingStatus::NewerFormat);
	}
	for (const auto kind : { int64(-1), int64(3), int64(2147483648LL) }) {
		auto object = QJsonDocument::fromJson(*headerBytes).object();
		object[u"kind"_q] = kind;
		CHECK(DecodeArchiveStagingRecord(QJsonDocument(object).toJson()).status == ArchiveStagingStatus::Unsupported);
	}
	CHECK(DecodeArchiveStagingRecord(QByteArray(kArchiveStagingRecordByteLimit + 1, 'x')).status == ArchiveStagingStatus::TooLarge);
	CHECK(!DecodeArchiveStagingRecord("{\"format\":\"purple-archive-staging\",\"version\":1,\"version\":2}"));
	for (const auto &number : { u"01"_q, u"-1"_q, u"18446744073709551616"_q }) {
		auto object = QJsonDocument::fromJson(*headerBytes).object();
		auto payload = object[u"value"_q].toObject();
		payload[u"covered"_q] = number;
		object[u"value"_q] = payload;
		CHECK(!DecodeArchiveStagingRecord(QJsonDocument(object).toJson()));
	}
	auto extra = QJsonDocument::fromJson(*headerBytes).object();
	extra[u"futureRequiredFlag"_q] = true;
	CHECK(!DecodeArchiveStagingRecord(QJsonDocument(extra).toJson()));
	CHECK(f.put(17, "observed"));
	RoundTripRecord(f.binding, f.pages.at(0), f.security);
	const auto next = f.plan(0, "prefix");
	CHECK(next);
	RoundTripRecord(f.binding, next.next, f.security);
	const auto context = ArchiveStagingPieceContext(f.binding, next.piece, f.security);
	CHECK(context && context->kind == ArchiveRecordKind::StagingPiece && context->rowId.size() == 64);
	CHECK(context && context->rowId != f.binding.source.contentHash.toUtf8());
	const auto final = ArchiveMediaChunkContext(f.state.account, f.security.storeId, QString(64, 'a'), 100, 0);
	CHECK(final && context && final->kind != context->kind && final->rowId != context->rowId);
	CHECK(context && !next.pieceFrame.contains(f.binding.source.contentHash.toUtf8()));
	CHECK(!OpenArchiveEnvelope(*final, f.security.keyId, next.pieceFrame, f.security.crypto));
	const auto progress = SealArchiveStagingRecord(f.binding, next.next, f.security);
	CHECK(progress);
	if (progress) {
		auto address = ArchiveStagingAddress(next.next);
		address.checkpoint++;
		CHECK(!OpenArchiveStagingRecord(f.binding, address, *progress, f.security));
		for (auto i = qsizetype(0); i < progress->size(); i += 37) {
			auto damaged = *progress;
			damaged[i] ^= 1;
			CHECK(!OpenArchiveStagingRecord(f.binding, ArchiveStagingAddress(next.next), damaged, f.security));
		}
	}
	auto invalid = f.pages.at(0);
	invalid.ranges.resize(kArchiveStagingRangeLimit + 1, invalid.ranges[0]);
	CHECK(!EncodeArchiveStagingRecord(invalid));
	invalid = f.pages.at(0);
	invalid.coverage[invalid.coverage.size() - 1] |= char(0x80);
	CHECK(!EncodeArchiveStagingRecord(invalid));
	auto noMac = f.security;
	noMac.hmacSha256 = {};
	CHECK(!ArchiveStagingPieceContext(f.binding, next.piece, noMac));
	CHECK(!SealArchiveStagingRecord(f.binding, f.header, noMac));
	auto boundary = Fixture(std::numeric_limits<uint64_t>::max());
	RoundTripRecord(boundary.binding, boundary.header, boundary.security);
}

void CoverageAndCrashes() {
	Section = "out of order coverage overlap and atomic credit";
	auto f = Fixture(kArchiveStagingWindowBytes + 13);
	const auto initial = ArchiveStagingHeaderHash(f.header);
	const auto first = f.plan(kArchiveStagingWindowBytes + 5, "last");
	CHECK(first && first.page.index == 1 && first.next.header.coveredBytes == 4);
	CHECK(first.pieceFrame.size() <= kArchiveMediaChunkBytes + 4608);
	for (auto cut = 0; cut != 4; ++cut) {
		const auto result = CompleteArchiveStagingPiece(f.header, first,
			f.ack(first, bool(cut & 1), bool(cut & 2)), f.security, f.state, f.epoch);
		CHECK(result.status == (cut == 3 ? ArchiveStagingStatus::Ready : ArchiveStagingStatus::NotCommitted));
		CHECK(cut == 3 || !result.header);
		CHECK(ArchiveStagingHeaderHash(f.header) == initial);
	}
	auto wrongAck = f.ack(first);
	wrongAck.pieceId = QString(64, 'a');
	CHECK(CompleteArchiveStagingPiece(f.header, first, wrongAck, f.security, f.state, f.epoch).status == ArchiveStagingStatus::Stale);
	f.pieces[first.pieceId] = first.pieceFrame;
	CHECK(f.header.coveredBytes == 0 && f.pages.empty());
	CHECK(!FreezeArchiveStagingCheckpoint(f.header, f.state, f.epoch));
	CHECK(f.install(first));
	CHECK(CompleteArchiveStagingPiece(f.header, first, f.ack(first), f.security, f.state, f.epoch).status == ArchiveStagingStatus::AlreadyApplied);
	CHECK(f.plan(kArchiveStagingWindowBytes + 5, "last").status == ArchiveStagingStatus::NoChange);
	const auto before = ArchiveStagingHeaderHash(f.header);
	CHECK(f.plan(kArchiveStagingWindowBytes + 4, "Xbad").status == ArchiveStagingStatus::Conflict);
	CHECK(ArchiveStagingHeaderHash(f.header) == before);
	CHECK(f.put(kArchiveStagingWindowBytes, "12345last6789"));
	CHECK(f.pages.at(1).complete && f.pages.at(1).coverage.isEmpty());
	CHECK(!FreezeArchiveStagingCheckpoint(f.header, f.state, f.epoch));
	CHECK(f.put(7, "abc"));
	CHECK(f.put(0, "0123456abc"));
	const auto corrupt = [&] {
		auto frame = f.frame(0);
		frame.back() ^= 1;
		return PlanArchiveStagingPiece(f.header, f.page(0), f.proof(0), frame, { 10, "next" }, f.security, f.state, f.epoch);
	}();
	CHECK(corrupt.status == ArchiveStagingStatus::IntegrityFailure);
	auto wrongProof = f.proof(0);
	wrongProof.siblings[1] = QString(64, 'a');
	CHECK(PlanArchiveStagingPiece(f.header, f.page(0), wrongProof, f.frame(0), { 10, "next" },
		f.security, f.state, f.epoch).status == ArchiveStagingStatus::IntegrityFailure);
	CHECK(f.plan(kArchiveStagingWindowBytes - 1, "cross").status == ArchiveStagingStatus::TooLarge);
	CHECK(ArchiveStagingInputSize(f.binding, kArchiveStagingWindowBytes - 1, 5) == 1);
	CHECK(ArchiveStagingInputSize(f.binding, kArchiveStagingWindowBytes, 5) == 5);
	CHECK(f.plan(0, QByteArray(kArchiveMediaChunkBytes + 1, 'x')).status == ArchiveStagingStatus::TooLarge);
	const auto fill = QByteArray(kArchiveStagingWindowBytes - 10, 'x');
	const auto stale = f.plan(10, fill);
	CHECK(stale);
	CHECK(f.put(10, "x"));
	CHECK(CompleteArchiveStagingPiece(f.header, stale, f.ack(stale), f.security, f.state, f.epoch).status == ArchiveStagingStatus::Stale);
	CHECK(f.put(10, fill));
	CHECK(f.header.coveredBytes == f.binding.totalBytes);
	const auto frozen = FreezeArchiveStagingCheckpoint(f.header, f.state, f.epoch);
	CHECK(frozen);
	if (frozen) { RoundTripRecord(f.binding, *frozen, f.security); }
}

void SparseAndFences() {
	Section = "uint64 sparse addresses and exact staging fences";
	auto f = Fixture(std::numeric_limits<uint64_t>::max());
	const auto end = f.binding.totalBytes;
	CHECK(f.put(end - 1, "z"));
	CHECK(f.header.coveredBytes == 1 && f.header.pages == 1);
	CHECK(f.pages.begin()->second.coverage.size() <= kArchiveStagingWindowBytes / 8);
	CHECK(f.put(0, "a"));
	CHECK(f.header.coveredBytes == 2 && f.pages.size() == 2);
	CHECK(!ArchiveStagingInputSize(f.binding, end, 1));
	CHECK(f.plan(end - 1, "zz").status == ArchiveStagingStatus::TooLarge);
	const auto valid = f.binding;
	for (auto field = 0; field != 13; ++field) {
		auto binding = valid;
		switch (field) {
		case 0: binding.jobId += u"-other"_q; break;
		case 1: binding.source.version += u"-other"_q; break;
		case 2: binding.source.revision += u"-other"_q; break;
		case 3: binding.source.contentHash = QString(64, 'b'); break;
		case 4: binding.source.mediaIdentity += u"-other"_q; break;
		case 5: binding.source.generation.account++; break;
		case 6: binding.source.generation.chat++; break;
		case 7: binding.source.epoch.generation++; break;
		case 8: binding.source.epoch.storeId += u"-other"_q; break;
		case 9: binding.source.key.account.environment = 1; break;
		case 10: binding.source.key.chat.kind = ArchiveChatKind::Group; break;
		case 11: binding.source.key.representation = u"translated"_q; break;
		case 12: binding.totalBytes--; break;
		}
		CHECK(CheckArchiveStagingBinding(binding, f.state, f.epoch) != ArchiveStagingStatus::Ready);
		CHECK(!BeginArchiveStaging(binding, f.state, f.epoch));
	}
	auto epoch = f.epoch;
	epoch.removalPending = true;
	CHECK(CheckArchiveStagingBinding(valid, f.state, epoch) == ArchiveStagingStatus::Stale);
	CHECK(Apply(f.state, PlanArchivePurge(f.state, Purge(f.state, ArchivePurgeKind::EditHistory))));
	CHECK(f.plan(1, "x").status == ArchiveStagingStatus::Stale);
	auto exhausted = Fixture(1);
	exhausted.header.checkpoint = std::numeric_limits<uint64_t>::max();
	CHECK(exhausted.plan(0, "x").status == ArchiveStagingStatus::Exhausted);
	auto tiny = Fixture(1);
	CHECK(tiny.put(0, "x"));
	CHECK(tiny.pages.at(0).complete);
	auto zero = Fixture(0);
	CHECK(FreezeArchiveStagingCheckpoint(zero.header, zero.state, zero.epoch));
	CHECK(!ArchiveStagingInputSize(zero.binding, 0, 1));
}

ArchiveStagingFinalizeInput Input(Fixture &f, const ArchiveStagingFinalizeStep &step) {
	const auto &request = step.request;
	switch (request.kind) {
	case ArchiveStagingRequestKind::Page: {
		const auto &page = f.pages.at(request.pageIndex);
		const auto frame = SealArchiveStagingRecord(f.binding, page, f.security);
		CHECK(frame);
		return ArchiveStagingPageInput{ request.id, page.checkpoint, frame.value_or(QByteArray()), f.proof(page.index) };
	}
	case ArchiveStagingRequestKind::Piece: return ArchiveStagingPieceInput{ request.id, f.frame(request.pageIndex) };
	case ArchiveStagingRequestKind::Publish:
		CHECK(step.publication);
		return ArchiveStagingPublicationAck(*step.publication, true);
	case ArchiveStagingRequestKind::Complete: return ArchiveStagingFinish{ request.id };
	case ArchiveStagingRequestKind::Finished: return ArchiveStagingFinish{ request.id };
	}
	return ArchiveStagingFinish{ request.id };
}

void Fill(Fixture &f, const QByteArray &bytes) {
	CHECK(uint64_t(bytes.size()) == f.binding.totalBytes);
	for (auto index = ArchiveStagingPageCount(f.binding.totalBytes); index != 0; --index) {
		const auto offset = (index - 1) * kArchiveStagingWindowBytes;
		CHECK(f.put(offset, bytes.mid(offset, kArchiveStagingWindowBytes)));
	}
}

ArchiveStagingFinalizeStart Start(Fixture &f) {
	const auto frozen = FreezeArchiveStagingCheckpoint(f.header, f.state, f.epoch);
	CHECK(frozen);
	return frozen ? BeginArchiveStagingFinalize(f.header, *frozen, f.security, f.state, f.epoch) : ArchiveStagingFinalizeStart();
}

ArchiveStagingFinalizeStep Advance(Fixture &f, ArchiveStagingFinalizer &session,
		const ArchiveStagingFinalizeInput &input) {
	return AdvanceArchiveStagingFinalize(session, input, f.header, f.security, f.state, f.epoch);
}

ArchiveStagingFinalizeStep Run(Fixture &f, ArchiveStagingFinalizeStart &started,
		std::vector<ArchiveStagingPublication> *publications = nullptr) {
	auto step = started.step;
	CHECK(started.status == ArchiveStagingStatus::Ready && started.session);
	if (!started.session) { return step; }
	for (auto count = 0; count != 10000 && step.status == ArchiveStagingStatus::Ready; ++count) {
		CHECK(step.bufferedPlaintextBytes <= kArchiveMediaChunkBytes);
		CHECK(step.request.id == started.session->request().id);
		if (step.publication) {
			CHECK(step.verifiedBytes == f.binding.totalBytes);
			CHECK(step.publication->chunkIndex == step.publishedChunks);
			CHECK(CheckArchiveStagingPublication(*step.publication, f.header, f.security, f.state, f.epoch)
				== ArchiveStagingStatus::Ready);
			if (publications) { publications->push_back(*step.publication); }
		}
		step = Advance(f, *started.session, Input(f, step));
	}
	return step;
}

void FinalizeBoundaries() {
	Section = "verify reseal canonical boundaries and durable completion";
	for (const auto size : { 0, 1, kArchiveStagingWindowBytes - 1, kArchiveStagingWindowBytes,
		kArchiveStagingWindowBytes + 1, kArchiveMediaChunkBytes - 1, kArchiveMediaChunkBytes,
		kArchiveMediaChunkBytes + 1, kArchiveMediaChunkBytes * 2 + 17 }) {
		auto bytes = QByteArray(size, 'x');
		for (auto i = 0; i < bytes.size(); ++i) { bytes[i] = char((i * 17 + i / 71) % 251); }
		auto f = Fixture(size, Hash(bytes));
		Fill(f, bytes);
		auto started = Start(f);
		auto publications = std::vector<ArchiveStagingPublication>();
		const auto finished = Run(f, started, &publications);
		CHECK(finished.status == ArchiveStagingStatus::Done && finished.completion);
		CHECK(finished.verifiedBytes == uint64_t(size) && finished.resealedBytes == uint64_t(size));
		CHECK(publications.size() == size_t(size ? (size - 1) / kArchiveMediaChunkBytes + 1 : 1));
		for (const auto &publication : publications) {
			const auto context = ArchiveMediaChunkContext(f.state.account, f.security.storeId,
				publication.blobToken, size, publication.chunkIndex);
			CHECK(context);
			if (!context) { continue; }
			const auto opened = OpenArchiveEnvelope(*context, f.security.keyId, publication.frame, f.security.crypto);
			CHECK(opened && opened.bytes == bytes.mid(publication.chunkIndex * kArchiveMediaChunkBytes,
				kArchiveMediaChunkBytes));
			CHECK(publication.blobHash == Hash(bytes));
			CHECK(publication.blobToken != publication.blobHash);
		}
		if (finished.completion) {
			CHECK(f.state.jobs[0].state == ArchiveJobState::Staged);
			CHECK(Apply(f.state, *finished.completion));
			CHECK(f.state.jobs[0].state == ArchiveJobState::Complete);
			CHECK(Message(f.state).versions[0].media[0].blobId == Hash(bytes));
			CHECK(CheckArchiveStagingBinding(f.binding, f.state, f.epoch) == ArchiveStagingStatus::Stale);
		}
	}
}

void FinalizeFailuresAndReplay() {
	Section = "finalizer corruption holes replay publication crash cuts";
	const auto bytes = QByteArray(kArchiveMediaChunkBytes + 3, 'q');
	auto f = Fixture(bytes.size(), Hash(bytes));
	Fill(f, bytes);
	const auto frozen = FreezeArchiveStagingCheckpoint(f.header, f.state, f.epoch);
	CHECK(frozen);
	if (!frozen) { return; }
	for (auto corruption = 0; corruption != 7; ++corruption) {
		auto started = Start(f);
		auto input = std::get<ArchiveStagingPageInput>(Input(f, started.step));
		switch (corruption) {
		case 0: input.frame.clear(); break;
		case 1: input.frame.back() ^= 1; break;
		case 2: input.proof.siblings[3] = QString(64, 'a'); break;
		case 3: input.checkpoint++; break;
		case 4: input.proof.siblings.pop_back(); break;
		case 5: input.frame = *SealArchiveStagingRecord(f.binding, f.pages.at(1), f.security); break;
		case 6: input.frame = *SealArchiveStagingRecord(f.binding, f.header, f.security); break;
		}
		const auto refused = Advance(f, *started.session, input);
		CHECK(refused.phase == ArchiveStagingFinalizePhase::Refused && !refused.publication && !refused.completion);
		CHECK(Advance(f, *started.session, Input(f, started.step)).status == refused.status);
	}
	for (auto pass = 0; pass != 2; ++pass) {
		auto started = Start(f);
		auto step = started.step;
		while (step.request.kind != ArchiveStagingRequestKind::Piece
			|| (step.phase == ArchiveStagingFinalizePhase::Verify) != (pass == 0)) {
			step = Advance(f, *started.session, Input(f, step));
			CHECK(step.status == ArchiveStagingStatus::Ready);
			if (step.status != ArchiveStagingStatus::Ready) { break; }
		}
		auto input = std::get<ArchiveStagingPieceInput>(Input(f, step));
		input.frame.back() ^= 1;
		CHECK(Advance(f, *started.session, input).status == ArchiveStagingStatus::IntegrityFailure);
	}
	auto mismatch = Fixture(3, Hash("different"));
	Fill(mismatch, "abc");
	auto wrongHash = Start(mismatch);
	CHECK(Run(mismatch, wrongHash).status == ArchiveStagingStatus::IntegrityFailure);
	auto missing = Fixture(2);
	CHECK(missing.put(0, "a"));
	auto claimed = ArchiveStagingCheckpoint{ missing.header, ArchiveStagingHeaderHash(missing.header) };
	CHECK(BeginArchiveStagingFinalize(missing.header, claimed, missing.security, missing.state, missing.epoch).status
		== ArchiveStagingStatus::Incomplete);
	claimed.header.coveredBytes = 2;
	claimed.previousHeaderHash = ArchiveStagingHeaderHash(claimed.header);
	auto falseComplete = BeginArchiveStagingFinalize(claimed.header, claimed, missing.security, missing.state, missing.epoch);
	CHECK(falseComplete.session);
	if (falseComplete.session) {
		const auto page = missing.pages.at(0);
		const auto frame = SealArchiveStagingRecord(missing.binding, page, missing.security);
		CHECK(AdvanceArchiveStagingFinalize(*falseComplete.session,
			ArchiveStagingPageInput{ 1, page.checkpoint, *frame, missing.proof(0) },
			claimed.header, missing.security, missing.state, missing.epoch).status == ArchiveStagingStatus::Incomplete);
	}
	auto started = Start(f);
	auto step = started.step;
	auto last = ArchiveStagingFinalizeInput(ArchiveStagingFinish{});
	auto durable = std::map<uint64_t, ArchiveStagingPublication>();
	for (auto count = 0; count != 1000 && step.request.kind != ArchiveStagingRequestKind::Complete; ++count) {
		auto input = Input(f, step);
		auto future = input;
		std::visit([](auto &value) { value.requestId++; }, future);
		CHECK(Advance(f, *started.session, future).status == ArchiveStagingStatus::Invalid);
		if (step.request.id > 1) {
			const auto replay = Advance(f, *started.session, last);
			CHECK(replay.status == ArchiveStagingStatus::NoChange && replay.request.id == step.request.id);
		}
		if (step.publication) {
			const auto pendingFrame = step.publication->frame;
			const auto uncommitted = Advance(f, *started.session, ArchiveStagingPublicationAck(*step.publication, false));
			CHECK(uncommitted.status == ArchiveStagingStatus::NotCommitted);
			CHECK(uncommitted.publication && uncommitted.publication->frame == pendingFrame);
			durable[step.publication->chunkIndex] = *step.publication;
		}
		last = input;
		step = Advance(f, *started.session, input);
		CHECK(step.status == ArchiveStagingStatus::Ready);
	}
	CHECK(step.request.kind == ArchiveStagingRequestKind::Complete && !step.completion);
	CHECK(f.state.jobs[0].state == ArchiveJobState::Staged);
	const auto completed = Advance(f, *started.session, Input(f, step));
	CHECK(completed.status == ArchiveStagingStatus::Done && completed.completion);
	CHECK(!Advance(f, *started.session, Input(f, step)).completion);
	for (auto cut = 0; cut != 5; ++cut) {
		auto restart = Start(f);
		auto current = restart.step;
		while (current.status == ArchiveStagingStatus::Ready) {
			auto input = Input(f, current);
			if (current.publication) {
				auto ack = ArchiveStagingPublicationAck(*current.publication, true);
				const auto previous = durable.at(ack.chunkIndex);
				CHECK(previous.frame != current.publication->frame);
				ack.existingFrame = previous.frame;
				ack.frameHash = previous.frameHash;
				if (cut == 1) { ack.frameHash = QString(64, 'a'); }
				if (cut == 2) { ack.existingFrame.back() ^= 1; }
				if (cut == 3) { ack.chunkIndex++; }
				if (cut == 4) { ack.checkpointHash = QString(64, 'a'); }
				input = ack;
			}
			current = Advance(f, *restart.session, input);
		}
		CHECK(current.status == (cut == 0 ? ArchiveStagingStatus::Done
			: cut < 3 ? ArchiveStagingStatus::IntegrityFailure : ArchiveStagingStatus::Stale));
	}
}

void HistoricalAndRaces() {
	Section = "historical exact-version completion and every-phase fences";
	auto original = Fixture(kArchiveStagingWindowBytes + 5);
	Fill(original, QByteArray(original.binding.totalBytes, 'h'));
	for (auto phase = 0; phase != 4; ++phase) {
		for (auto field = 0; field != 10; ++field) {
			auto f = original;
			auto started = Start(f);
			auto step = started.step;
			while ((phase == 1 && step.phase == ArchiveStagingFinalizePhase::Verify)
				|| (phase == 2 && step.request.kind != ArchiveStagingRequestKind::Publish)
				|| (phase == 3 && step.request.kind != ArchiveStagingRequestKind::Complete)) {
				step = Advance(f, *started.session, Input(f, step));
				if (step.status != ArchiveStagingStatus::Ready) { break; }
			}
			const auto input = Input(f, step);
			switch (field) {
			case 0: f.epoch.generation++; break;
			case 1: f.epoch.account.userId++; break;
			case 2: f.epoch.storeId += u"new"_q; break;
			case 3: f.state.accountGeneration++; break;
			case 4: f.state.messages[0].versions[0].revision += u"changed"_q; break;
			case 5: f.state.messages[0].versions[0].content.text = u"changed"_q; break;
			case 6: f.state.jobs[0].state = ArchiveJobState::Failed; break;
			case 7: f.security.keyId += u"changed"_q; break;
			case 8: f.header.checkpoint++; break;
			case 9: f.state.messages[0].versions.clear(); break;
			}
			const auto refused = Advance(f, *started.session, input);
			CHECK(refused.status != ArchiveStagingStatus::Ready && !refused.publication && !refused.completion);
			CHECK(refused.phase == ArchiveStagingFinalizePhase::Refused);
			if (step.publication) {
				CHECK(CheckArchiveStagingPublication(*step.publication, f.header, f.security, f.state, f.epoch)
					!= ArchiveStagingStatus::Ready || field == 7);
			}
		}
	}
	auto f = original;
	auto edit = Event(f.state, u"v2"_q, 2, u"later"_q, ArchiveOrigin::AuthoritativeEdit);
	AddMedia(edit);
	CHECK(Apply(f.state, PlanArchiveEvent(f.state, edit)));
	const auto newer = Message(f.state).versions[1];
	auto started = Start(f);
	const auto finished = Run(f, started);
	CHECK(finished.status == ArchiveStagingStatus::Done && finished.completion);
	if (finished.completion) {
		CHECK(Apply(f.state, *finished.completion));
		CHECK(Message(f.state).versions[0].media[0].state == ArchiveMediaState::Complete);
		CHECK(Message(f.state).versions[1] == newer);
	}
}

QByteArray VirtualWindow(uint64_t index, uint32_t size) {
	auto bytes = QByteArray(size, '\0');
	for (auto i = uint32_t(0); i != size; ++i) { bytes[i] = char((index * 37 + i * 19 + i / 127) % 251); }
	return bytes;
}

void LargeVirtualStream() {
	Section = "128 MiB virtual stream bounded pages and plaintext buffers";
	const auto total = uint64_t(128) * kArchiveMediaChunkBytes + 19;
	auto f = Fixture(total);
	auto whole = QCryptographicHash(QCryptographicHash::Sha256);
	for (auto index = uint64_t(0); index != ArchiveStagingPageCount(total); ++index) {
		const auto size = uint32_t(std::min(uint64_t(kArchiveStagingWindowBytes), total - index * kArchiveStagingWindowBytes));
		const auto bytes = VirtualWindow(index, size);
		whole.addData(bytes);
		CHECK(f.put(index * kArchiveStagingWindowBytes, bytes));
		CHECK(f.pieces.size() == 1);
		f.pieces.clear();
		const auto encoded = EncodeArchiveStagingRecord(f.pages.at(index));
		CHECK(encoded && encoded->size() < kArchiveStagingRecordByteLimit);
	}
	const auto expected = QString::fromLatin1(whole.result().toHex());
	auto started = Start(f);
	auto step = started.step;
	auto published = uint64_t(0);
	while (step.status == ArchiveStagingStatus::Ready) {
		auto input = ArchiveStagingFinalizeInput(ArchiveStagingFinish{});
		CHECK(step.bufferedPlaintextBytes <= kArchiveMediaChunkBytes);
		CHECK(f.pieces.empty());
		if (step.request.kind == ArchiveStagingRequestKind::Piece) {
			const auto &range = step.request.range;
			const auto bytes = VirtualWindow(step.request.pageIndex, range.length);
			CHECK(bytes.size() <= kArchiveStagingWindowBytes && Hash(bytes) == range.pieceHash);
			const auto context = ArchiveStagingPieceContext(f.binding, range, f.security);
			const auto sealed = SealArchiveEnvelope(*context, f.security.keyId, bytes, f.security.crypto);
			CHECK(sealed);
			input = ArchiveStagingPieceInput{ step.request.id, sealed.bytes };
		} else {
			input = Input(f, step);
		}
		if (step.publication) {
			CHECK(step.verifiedBytes == total && step.publication->blobHash == expected);
			CHECK(step.publication->frame.size() <= kArchiveMediaChunkBytes + 4608);
			++published;
		}
		step = Advance(f, *started.session, input);
	}
	CHECK(step.status == ArchiveStagingStatus::Done && step.completion);
	CHECK(published == 129 && step.resealedBytes == total);
}

void EveryFinalizeCrashCut() {
	Section = "restart at every finalizer request and publication boundary";
	auto f = Fixture(kArchiveMediaChunkBytes + 7);
	Fill(f, QByteArray(f.binding.totalBytes, 'r'));
	auto initial = Start(f);
	auto trace = std::vector<ArchiveStagingRequestKind>();
	auto step = initial.step;
	while (step.status == ArchiveStagingStatus::Ready) {
		trace.push_back(step.request.kind);
		step = Advance(f, *initial.session, Input(f, step));
	}
	CHECK(step.status == ArchiveStagingStatus::Done);
	for (auto cut = size_t(0); cut <= trace.size(); ++cut) {
		auto durable = std::map<uint64_t, ArchiveStagingPublication>();
		{
			auto crashed = Start(f);
			auto current = crashed.step;
			for (auto index = size_t(0); index != cut; ++index) {
				CHECK(current.request.kind == trace[index]);
				if (current.publication) { durable[current.publication->chunkIndex] = *current.publication; }
				current = Advance(f, *crashed.session, Input(f, current));
			}
		}
		auto restarted = Start(f);
		auto current = restarted.step;
		while (current.status == ArchiveStagingStatus::Ready) {
			auto input = Input(f, current);
			if (current.publication) {
				const auto found = durable.find(current.publication->chunkIndex);
				if (found != durable.end()) {
					auto ack = ArchiveStagingPublicationAck(*current.publication, true);
					ack.existingFrame = found->second.frame;
					ack.frameHash = found->second.frameHash;
					input = ack;
				}
			}
			current = Advance(f, *restarted.session, input);
		}
		CHECK(current.status == ArchiveStagingStatus::Done && current.completion);
		CHECK(f.state.jobs[0].state == ArchiveJobState::Staged);
		if (current.completion) {
			const auto stopped = CompleteArchiveTransaction(f.state, *current.completion, false);
			CHECK(stopped.status == ArchiveCommitStatus::NotCommitted && stopped.state == f.state);
		}
	}
}

void FragmentedCoverageAndDomains() {
	Section = "bounded fragmented coverage and distinct checkpoint domains";
	auto f = Fixture(4096);
	for (auto parity = 0; parity != 2; ++parity) {
		auto bytes = QByteArray(4096, 'x');
		for (auto offset = parity; offset < 4096; offset += 2) {
			CHECK(f.put(offset, bytes.mid(offset, 1)));
		}
		CHECK(f.header.coveredBytes == uint64_t(parity == 0 ? 2048 : 4096));
		CHECK(f.pages.at(0).ranges.size() == 1);
		CHECK(EncodeArchiveStagingRecord(f.pages.at(0))->size() < kArchiveStagingRecordByteLimit);
		CHECK(bool(FreezeArchiveStagingCheckpoint(f.header, f.state, f.epoch)) == (parity == 1));
	}
	auto tiny = Fixture(1);
	const auto update = tiny.plan(0, "x");
	CHECK(update && tiny.install(update));
	const auto frozen = FreezeArchiveStagingCheckpoint(tiny.header, tiny.state, tiny.epoch);
	CHECK(frozen);
	if (frozen) {
		const auto deltaContext = ArchiveStagingRecordContext(tiny.binding, ArchiveStagingAddress(update.next), tiny.security);
		const auto frozenContext = ArchiveStagingRecordContext(tiny.binding, ArchiveStagingAddress(*frozen), tiny.security);
		CHECK(deltaContext && frozenContext && deltaContext->rowId != frozenContext->rowId);
		const auto encrypted = SealArchiveStagingRecord(tiny.binding, update.next, tiny.security);
		CHECK(encrypted && !OpenArchiveStagingRecord(tiny.binding, ArchiveStagingAddress(*frozen), *encrypted, tiny.security));
	}
	CHECK(int(ArchiveRecordKind::Maintenance) == 12);
	CHECK(int(ArchiveRecordKind::StagingPiece) == 13);
	CHECK(int(ArchiveRecordKind::StagingProgress) == 14);
}

void AcknowledgementIntegrity() {
	Section = "acknowledgement cannot replace captured bytes or invent coverage";
	auto f = Fixture(3);
	CHECK(f.put(0, "a"));
	const auto planned = f.plan(1, "b");
	CHECK(planned);
	auto changed = planned;
	const auto replacement = QByteArray("Xb\0", 3);
	changed.piece.pieceHash = Hash(replacement);
	changed.page.ranges[0] = changed.piece;
	changed.next.pageHash = ArchiveStagingPageHash(changed.page);
	auto root = changed.next.pageHash;
	for (auto level = 0; level != kArchiveStagingProofDepth; ++level) {
		root = Hash(QByteArray("purple-archive/staging/node/v1") + char(level + 1)
			+ QByteArray::fromHex(root.toLatin1()) + QByteArray::fromHex(changed.next.proof.siblings[level].toLatin1()));
	}
	changed.next.header.rootHash = root;
	const auto context = ArchiveStagingPieceContext(f.binding, changed.piece, f.security);
	CHECK(context);
	if (context) {
		changed.pieceId = QString::fromLatin1(context->rowId);
		changed.pieceFrame = SealArchiveEnvelope(*context, f.security.keyId, replacement, f.security.crypto).bytes;
		CHECK(CompleteArchiveStagingPiece(f.header, changed, f.ack(changed), f.security, f.state, f.epoch).status
			== ArchiveStagingStatus::Conflict);
	}
	changed = planned;
	changed.previousPieceFrame.back() ^= 1;
	CHECK(CompleteArchiveStagingPiece(f.header, changed, f.ack(changed), f.security, f.state, f.epoch).status
		== ArchiveStagingStatus::IntegrityFailure);
	changed = planned;
	changed.page.coverage[0] = 2;
	CHECK(!CompleteArchiveStagingPiece(f.header, changed, f.ack(changed), f.security, f.state, f.epoch).header);
	auto zero = Fixture(3);
	const auto first = zero.plan(0, QByteArray(1, '\0'));
	CHECK(first && zero.install(first));
	auto second = zero.plan(1, QByteArray(1, '\0'));
	CHECK(second && first.pieceId == second.pieceId && first.pieceFrame != second.pieceFrame);
	second.pieceFrame = first.pieceFrame;
	CHECK(zero.install(second));
	CHECK(zero.header.coveredBytes == 2 && !FreezeArchiveStagingCheckpoint(zero.header, zero.state, zero.epoch));
}

void CryptoFailures() {
	Section = "staging and finalizer fail closed on crypto provider errors";
	for (auto error = 0; error != 4; ++error) {
		auto f = Fixture(1);
		switch (error) {
		case 0: f.security.crypto.random = [](int) -> std::optional<QByteArray> { return {}; }; break;
		case 1: f.security.crypto.sealAes256Gcm = {}; break;
		case 2: f.security.hmacSha256 = {}; break;
		case 3: f.security.hmacSha256 = [](const QByteArray &) { return std::optional(QByteArray(31, 'x')); }; break;
		}
		CHECK(!f.plan(0, "x"));
	}
	for (auto error = 0; error != 4; ++error) {
		auto f = Fixture(1);
		Fill(f, "x");
		auto started = Start(f);
		auto step = started.step;
		while (step.phase == ArchiveStagingFinalizePhase::Verify || step.request.kind == ArchiveStagingRequestKind::Page) {
			step = Advance(f, *started.session, Input(f, step));
			if (step.status != ArchiveStagingStatus::Ready) { break; }
		}
		const auto input = Input(f, step);
		switch (error) {
		case 0: f.security.crypto.random = {}; break;
		case 1: f.security.crypto.sealAes256Gcm = {}; break;
		case 2: f.security.crypto.openAes256Gcm = {}; break;
		case 3: f.security.hmacSha256 = {}; break;
		}
		const auto refused = Advance(f, *started.session, input);
		CHECK(refused.phase == ArchiveStagingFinalizePhase::Refused && !refused.publication && !refused.completion);
	}
}

}

int main() {
	ContextsAndCodec();
	CoverageAndCrashes();
	SparseAndFences();
	FinalizeBoundaries();
	FinalizeFailuresAndReplay();
	HistoricalAndRaces();
	LargeVirtualStream();
	EveryFinalizeCrashCut();
	FragmentedCoverageAndDomains();
	AcknowledgementIntegrity();
	CryptoFailures();
	return Finish();
}
