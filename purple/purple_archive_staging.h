/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_media_lifecycle.h"

#include <memory>

namespace Purple {

inline constexpr auto kArchiveStagingVersion = 1;
inline constexpr auto kArchiveStagingWindowBytes = 256 * 1024;
inline constexpr auto kArchiveStagingRecordByteLimit = 64 * 1024;
inline constexpr auto kArchiveStagingRangeLimit = 128;
inline constexpr auto kArchiveStagingProofDepth = 46;

enum class ArchiveStagingStatus {
	Ready, NoChange, Invalid, NewerFormat, Unsupported, TooLarge,
	ReadOnly, Stale, Conflict, Incomplete, IntegrityFailure,
	NotCommitted, AlreadyApplied, Exhausted, Done,
};

struct ArchiveStagingBinding {
	ArchiveMediaSource source;
	QString jobId;
	uint64_t totalBytes = 0;
	std::optional<QString> expectedBlobHash;
};

struct ArchiveStagingHeader {
	ArchiveStagingBinding binding;
	uint64_t checkpoint = 0;
	QString rootHash;
	uint64_t coveredBytes = 0;
	uint64_t pages = 0;
};

struct ArchiveStagingRange {
	uint64_t offset = 0;
	uint32_t length = 0;
	QString pieceHash;
	friend bool operator==(const ArchiveStagingRange &, const ArchiveStagingRange &) = default;
};

struct ArchiveStagingCoveragePage {
	QString bindingId;
	uint64_t index = 0;
	uint64_t checkpoint = 0;
	std::vector<ArchiveStagingRange> ranges;
	bool complete = false;
	QByteArray coverage;
	friend bool operator==(const ArchiveStagingCoveragePage &, const ArchiveStagingCoveragePage &) = default;
};

struct ArchiveStagingProof {
	std::vector<QString> siblings;
	friend bool operator==(const ArchiveStagingProof &, const ArchiveStagingProof &) = default;
};

struct ArchiveStagingNode {
	int level = 0;
	uint64_t index = 0;
	QString hash;
	friend bool operator==(const ArchiveStagingNode &, const ArchiveStagingNode &) = default;
};

struct ArchiveStagingCheckpoint {
	ArchiveStagingHeader header;
	QString previousHeaderHash;
	std::optional<uint64_t> pageIndex;
	QString pageHash;
	ArchiveStagingProof proof;
};

using ArchiveStagingRecord = std::variant<ArchiveStagingHeader,
	ArchiveStagingCoveragePage, ArchiveStagingCheckpoint>;

enum class ArchiveStagingRecordKind { Header, CoveragePage, Checkpoint };

struct ArchiveStagingRecordAddress {
	ArchiveStagingRecordKind kind = ArchiveStagingRecordKind::Header;
	uint64_t position = 0;
	uint64_t checkpoint = 0;
};

struct ArchiveStagingDecodeResult {
	ArchiveStagingStatus status = ArchiveStagingStatus::Invalid;
	std::optional<ArchiveStagingRecord> record;
	[[nodiscard]] explicit operator bool() const {
		return status == ArchiveStagingStatus::Ready;
	}
};

[[nodiscard]] ArchiveStagingStatus CheckArchiveStagingBinding(
	const ArchiveStagingBinding &binding, const ArchiveState &current,
	const ArchiveStoreEpoch &epoch);
[[nodiscard]] QString ArchiveStagingBindingId(const ArchiveStagingBinding &binding,
	const ArchiveSqlSecurity &security);
[[nodiscard]] std::optional<uint32_t> ArchiveStagingInputSize(
	const ArchiveStagingBinding &binding, uint64_t offset, uint64_t available);
[[nodiscard]] uint64_t ArchiveStagingPageCount(uint64_t totalBytes);
[[nodiscard]] QString ArchiveStagingEmptyRoot();
[[nodiscard]] ArchiveStagingProof ArchiveStagingEmptyProof();
[[nodiscard]] std::optional<ArchiveStagingHeader> BeginArchiveStaging(
	const ArchiveStagingBinding &binding, const ArchiveState &current,
	const ArchiveStoreEpoch &epoch);
[[nodiscard]] QString ArchiveStagingHeaderHash(const ArchiveStagingHeader &header);
[[nodiscard]] QString ArchiveStagingPageHash(const ArchiveStagingCoveragePage &page);
[[nodiscard]] std::optional<std::vector<ArchiveStagingNode>> ArchiveStagingNodes(
	const ArchiveStagingCheckpoint &checkpoint);
[[nodiscard]] std::optional<QByteArray> EncodeArchiveStagingRecord(const ArchiveStagingRecord &record);
[[nodiscard]] ArchiveStagingDecodeResult DecodeArchiveStagingRecord(const QByteArray &bytes);
[[nodiscard]] ArchiveStagingRecordAddress ArchiveStagingAddress(const ArchiveStagingRecord &record);
[[nodiscard]] std::optional<ArchiveEnvelopeContext> ArchiveStagingPieceContext(
	const ArchiveStagingBinding &binding, const ArchiveStagingRange &range,
	const ArchiveSqlSecurity &security);
[[nodiscard]] std::optional<ArchiveEnvelopeContext> ArchiveStagingRecordContext(
	const ArchiveStagingBinding &binding, const ArchiveStagingRecordAddress &address,
	const ArchiveSqlSecurity &security);
[[nodiscard]] std::optional<QByteArray> SealArchiveStagingRecord(
	const ArchiveStagingBinding &binding, const ArchiveStagingRecord &record,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveStagingDecodeResult OpenArchiveStagingRecord(
	const ArchiveStagingBinding &binding, const ArchiveStagingRecordAddress &address,
	const QByteArray &frame, const ArchiveSqlSecurity &security);

struct ArchiveStagingPieceFact {
	uint64_t offset = 0;
	QByteArray bytes;
};

struct ArchiveStagingUpdate {
	ArchiveStagingStatus status = ArchiveStagingStatus::Invalid;
	ArchiveStagingHeader expected;
	ArchiveStagingCheckpoint next;
	ArchiveStagingCoveragePage page;
	ArchiveStagingRange piece;
	QString pieceId;
	QByteArray pieceFrame;
	std::optional<ArchiveStagingCoveragePage> previousPage;
	QByteArray previousPieceFrame;
	[[nodiscard]] explicit operator bool() const {
		return status == ArchiveStagingStatus::Ready;
	}
};

[[nodiscard]] ArchiveStagingUpdate PlanArchiveStagingPiece(
	const ArchiveStagingHeader &header,
	const std::optional<ArchiveStagingCoveragePage> &page,
	const ArchiveStagingProof &proof, const QByteArray &oldPieceFrame,
	const ArchiveStagingPieceFact &fact, const ArchiveSqlSecurity &security,
	const ArchiveState &current, const ArchiveStoreEpoch &epoch);

struct ArchiveStagingDurability {
	bool pieceDurable = false;
	bool checkpointCasDurable = false;
	QString bindingId;
	QString pieceId;
	QString expectedHeaderHash;
	QString nextHeaderHash;
};

struct ArchiveStagingCommit {
	ArchiveStagingStatus status = ArchiveStagingStatus::Invalid;
	std::optional<ArchiveStagingHeader> header;
};

[[nodiscard]] ArchiveStagingCommit CompleteArchiveStagingPiece(
	const ArchiveStagingHeader &currentHeader, const ArchiveStagingUpdate &update,
	const ArchiveStagingDurability &ack, const ArchiveSqlSecurity &security,
	const ArchiveState &current, const ArchiveStoreEpoch &epoch);
[[nodiscard]] std::optional<ArchiveStagingCheckpoint> FreezeArchiveStagingCheckpoint(
	const ArchiveStagingHeader &header, const ArchiveState &current,
	const ArchiveStoreEpoch &epoch);

enum class ArchiveStagingFinalizePhase { Verify, Reseal, AwaitPublication, AwaitCompletion, Finished, Refused };
enum class ArchiveStagingRequestKind { Page, Piece, Publish, Complete, Finished };

struct ArchiveStagingRequest {
	ArchiveStagingRequestKind kind = ArchiveStagingRequestKind::Page;
	uint64_t id = 0;
	uint64_t pageIndex = 0;
	uint64_t chunkIndex = 0;
	ArchiveStagingRange range;
};

struct ArchiveStagingPublication {
	ArchiveStagingBinding binding;
	QString bindingId;
	QString checkpointHash;
	QString blobHash;
	QString blobToken;
	uint64_t requestId = 0;
	uint64_t chunkIndex = 0;
	uint32_t plaintextBytes = 0;
	QString plaintextHash;
	QString frameHash;
	QByteArray frame;
};

struct ArchiveStagingPageInput {
	uint64_t requestId = 0;
	uint64_t checkpoint = 0;
	QByteArray frame;
	ArchiveStagingProof proof;
};

struct ArchiveStagingPieceInput {
	uint64_t requestId = 0;
	QByteArray frame;
};

struct ArchiveStagingPublishAck {
	uint64_t requestId = 0;
	QString bindingId;
	QString checkpointHash;
	QString blobToken;
	uint64_t chunkIndex = 0;
	QString frameHash;
	bool durable = false;
	QByteArray existingFrame;
};

struct ArchiveStagingFinish {
	uint64_t requestId = 0;
};

using ArchiveStagingFinalizeInput = std::variant<ArchiveStagingPageInput,
	ArchiveStagingPieceInput, ArchiveStagingPublishAck, ArchiveStagingFinish>;

struct ArchiveStagingFinalizeStep {
	ArchiveStagingStatus status = ArchiveStagingStatus::Invalid;
	ArchiveStagingFinalizePhase phase = ArchiveStagingFinalizePhase::Verify;
	ArchiveStagingRequest request;
	std::optional<ArchiveStagingPublication> publication;
	std::optional<ArchiveTransaction> completion;
	uint64_t verifiedBytes = 0;
	uint64_t resealedBytes = 0;
	uint64_t publishedChunks = 0;
	uint32_t bufferedPlaintextBytes = 0;
};

class ArchiveStagingFinalizer;
struct ArchiveStagingFinalizeStart;

[[nodiscard]] ArchiveStagingFinalizeStart BeginArchiveStagingFinalize(
	const ArchiveStagingHeader &currentHeader, const ArchiveStagingCheckpoint &checkpoint,
	const ArchiveSqlSecurity &security, const ArchiveState &current, const ArchiveStoreEpoch &epoch);
[[nodiscard]] ArchiveStagingFinalizeStep AdvanceArchiveStagingFinalize(
	ArchiveStagingFinalizer &session, const ArchiveStagingFinalizeInput &input,
	const ArchiveStagingHeader &currentHeader, const ArchiveSqlSecurity &security,
	const ArchiveState &current, const ArchiveStoreEpoch &epoch);
[[nodiscard]] ArchiveStagingStatus CheckArchiveStagingPublication(
	const ArchiveStagingPublication &publication, const ArchiveStagingHeader &currentHeader,
	const ArchiveSqlSecurity &security, const ArchiveState &current, const ArchiveStoreEpoch &epoch);
[[nodiscard]] ArchiveStagingPublishAck ArchiveStagingPublicationAck(
	const ArchiveStagingPublication &publication, bool durable);

class ArchiveStagingFinalizer {
public:
	struct Data;
	~ArchiveStagingFinalizer();
	ArchiveStagingFinalizer(ArchiveStagingFinalizer &&) noexcept;
	ArchiveStagingFinalizer &operator=(ArchiveStagingFinalizer &&) noexcept;
	ArchiveStagingFinalizer(const ArchiveStagingFinalizer &) = delete;
	ArchiveStagingFinalizer &operator=(const ArchiveStagingFinalizer &) = delete;
	[[nodiscard]] ArchiveStagingRequest request() const;

private:
	explicit ArchiveStagingFinalizer(std::unique_ptr<Data> data);
	std::unique_ptr<Data> _data;
	friend ArchiveStagingFinalizeStart BeginArchiveStagingFinalize(
		const ArchiveStagingHeader &, const ArchiveStagingCheckpoint &,
		const ArchiveSqlSecurity &, const ArchiveState &, const ArchiveStoreEpoch &);
	friend ArchiveStagingFinalizeStep AdvanceArchiveStagingFinalize(
		ArchiveStagingFinalizer &, const ArchiveStagingFinalizeInput &,
		const ArchiveStagingHeader &, const ArchiveSqlSecurity &,
		const ArchiveState &, const ArchiveStoreEpoch &);
};

struct ArchiveStagingFinalizeStart {
	ArchiveStagingStatus status = ArchiveStagingStatus::Invalid;
	std::unique_ptr<ArchiveStagingFinalizer> session;
	ArchiveStagingFinalizeStep step;
};

}
