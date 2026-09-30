/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_inventory.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <map>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] QString RecordHash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool OwnCanonicalConfigRecord(
		const QByteArray &bytes,
		const SyncLocalState &state,
		std::optional<uint64_t> expectedSeq = std::nullopt) {
	const auto parsed = ParseSyncEnvelope(bytes);
	if (!parsed || !parsed.header
		|| InspectConfigPayload(parsed).status != ConfigPayloadStatus::Valid) {
		return false;
	}
	const auto written = SerializeSyncEnvelope(parsed.envelope);
	const auto &header = *parsed.header;
	return written && written.canonical == bytes
		&& header.stream == u"config"_q
		&& header.writerInstall == state.install
		&& header.writerDevice == state.createdDevice
		&& header.space == state.space
		&& (!expectedSeq || header.seq == *expectedSeq);
}

[[nodiscard]] bool OpaqueWithHeader(const SyncCandidateRecord &record) {
	return (record.status == SyncCandidateStatus::UnsupportedStream
		|| record.status == SyncCandidateStatus::UnsupportedEncoding
		|| record.status == SyncCandidateStatus::UnsupportedLibrary)
		&& record.header;
}

} // namespace

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

SyncCandidateRecord ClassifySyncCandidate(int32_t id, QByteArray bytes) {
	if (bytes.size() > kSyncRecordMaximumBytes) {
		return { .id = id, .status = SyncCandidateStatus::Oversized };
	}
	auto record = SyncCandidateRecord{ .id = id, .bytes = std::move(bytes) };
	const auto parsed = ParseSyncEnvelope(record.bytes);
	record.header = parsed.header;
	record.envelopeError = parsed.error;
	switch (parsed.status) {
	case SyncEnvelopeStatus::NewerMajor:
		record.status = SyncCandidateStatus::NewerMajor;
		return record;
	case SyncEnvelopeStatus::UnsupportedStream:
		record.status = SyncCandidateStatus::UnsupportedStream;
		return record;
	case SyncEnvelopeStatus::UnsupportedEncoding:
		record.status = SyncCandidateStatus::UnsupportedEncoding;
		return record;
	case SyncEnvelopeStatus::Invalid:
		record.status = SyncCandidateStatus::Invalid;
		return record;
	case SyncEnvelopeStatus::Valid:
		break;
	}
	if (parsed.envelope.document.value(u"stream"_q).toString()
			== u"config"_q) {
		const auto inspected = InspectConfigPayload(parsed);
		record.configError = inspected.error;
		record.status = (inspected.status == ConfigPayloadStatus::Valid)
			? SyncCandidateStatus::Valid
			: (inspected.status == ConfigPayloadStatus::NewerSchema)
			? SyncCandidateStatus::NewerSchema
			: SyncCandidateStatus::Invalid;
	} else {
		record.status = SyncCandidateStatus::UnsupportedLibrary;
	}
	return record;
}

SyncCandidateReadStatus AggregateSyncCandidateRead(
		const std::vector<SyncCandidateRecord> &records) {
	auto result = SyncCandidateReadStatus::Complete;
	for (const auto &record : records) {
		if (record.status == SyncCandidateStatus::RequestFailed
			|| record.status == SyncCandidateStatus::Inaccessible
			|| record.status == SyncCandidateStatus::Cancelled) {
			result = SyncCandidateReadStatus::Incomplete;
		} else if (record.status != SyncCandidateStatus::Valid
			&& !OpaqueWithHeader(record)
			&& result != SyncCandidateReadStatus::Incomplete) {
			result = SyncCandidateReadStatus::NeedsReview;
		}
	}
	return result;
}

SyncDirectoryCandidate SyncDirectoryCandidateOf(
		const SyncCandidateRecord &record) {
	auto status = SyncEnvelopeStatus::Invalid;
	auto payloadValidated = false;
	switch (record.status) {
	case SyncCandidateStatus::Valid:
		status = SyncEnvelopeStatus::Valid;
		payloadValidated = true;
		break;
	case SyncCandidateStatus::UnsupportedLibrary:
		status = SyncEnvelopeStatus::Valid;
		break;
	case SyncCandidateStatus::UnsupportedStream:
		status = SyncEnvelopeStatus::UnsupportedStream;
		break;
	case SyncCandidateStatus::UnsupportedEncoding:
		status = SyncEnvelopeStatus::UnsupportedEncoding;
		break;
	case SyncCandidateStatus::NewerMajor:
		status = SyncEnvelopeStatus::NewerMajor;
		break;
	default:
		break;
	}
	return {
		.messageId = record.id,
		.documentId = record.documentId,
		.editDate = record.editDate,
		.original = true,
		.status = status,
		.header = record.header,
		.recordHash = record.header && !record.bytes.isEmpty()
			? RecordHash(record.bytes)
			: QString(),
		.payloadValidated = payloadValidated,
	};
}

SyncAccountInventoryResult FinishSyncAccountInventory(
		uint64_t accountUserId,
		SyncHistoryScanResult scan,
		std::optional<SyncCandidateReadResult> read) {
	auto result = SyncAccountInventoryResult{
		.accountUserId = accountUserId,
		.scan = std::move(scan),
		.read = std::move(read),
	};
	if (!result.read) {
		return result;
	}
	auto candidates = std::vector<SyncDirectoryCandidate>();
	candidates.reserve(result.read->records.size());
	for (const auto &record : result.read->records) {
		candidates.push_back(SyncDirectoryCandidateOf(record));
	}
	result.directory = ResolveSyncDirectory(
		candidates,
		result.scan.complete()
			&& result.read->status != SyncCandidateReadStatus::Incomplete);
	const auto directoryNeedsReview = result.directory.unreadableCandidate
		|| result.directory.messageIdCollision
		|| (result.directory.selectedSpace
			&& !result.directory.publishableSpace);
	if (result.scan.complete()
		&& result.read->status != SyncCandidateReadStatus::Incomplete) {
		result.status = (result.read->status == SyncCandidateReadStatus::Complete
			&& !directoryNeedsReview)
			? SyncAccountInventoryStatus::Complete
			: SyncAccountInventoryStatus::NeedsReview;
	}
	return result;
}

SyncOwnInventoryResult ReconcileOwnConfigInventory(
		const SyncLocalState &state,
		const SyncAccountInventoryResult &inventory,
		uint64_t expectedAccountUserId,
		const QByteArray &stagedCanonicalRecord) {
	auto result = SyncOwnInventoryResult();
	if (!expectedAccountUserId
		|| inventory.accountUserId != expectedAccountUserId) {
		result.status = SyncOwnInventoryStatus::NeedsReview;
		return result;
	}
	if (!inventory.scan.complete()
		|| !inventory.read
		|| inventory.read->status == SyncCandidateReadStatus::Incomplete
		|| !inventory.directory.complete
		|| inventory.status == SyncAccountInventoryStatus::Incomplete) {
		return result;
	}
	if (inventory.status != SyncAccountInventoryStatus::Complete
		|| !inventory.read->complete()
		|| inventory.directory.unreadableCandidate
		|| inventory.directory.messageIdCollision
		|| !inventory.directory.selectedSpace
		|| !inventory.directory.publishableSpace
		|| *inventory.directory.selectedSpace != state.space
		|| *inventory.directory.publishableSpace != state.space
		|| !SerializeSyncLocalState(state)) {
		result.status = SyncOwnInventoryStatus::NeedsReview;
		return result;
	}
	if (state.config.pendingSeq != 0) {
		if (stagedCanonicalRecord.isEmpty()
			|| !OwnCanonicalConfigRecord(stagedCanonicalRecord,
				state, state.config.pendingSeq)) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto staged = ParseSyncEnvelope(stagedCanonicalRecord);
		const auto issued = std::find_if(
			state.config.issuedRecords.begin(),
			state.config.issuedRecords.end(),
			[&](const auto &record) {
				return record.seq == state.config.pendingSeq;
			});
		if (staged.envelope.document.value(u"payload_sha256"_q).toString()
			!= state.config.ownHash
			|| issued == state.config.issuedRecords.end()
			|| issued->recordHash != RecordHash(stagedCanonicalRecord)) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
	}

	auto recordsBySeq = std::map<uint64_t, QByteArray>();
	auto headId = int32_t(0);
	for (const auto &record : inventory.read->records) {
		if (!record.header
			|| record.header->stream != u"config"_q
			|| record.header->writerInstall != state.install
			|| record.header->space != state.space) {
			continue;
		}
		if (record.id <= 0
			|| record.status != SyncCandidateStatus::Valid
			|| !OwnCanonicalConfigRecord(record.bytes, state)) {
			if (record.header->writerDevice != state.createdDevice) {
				result.status = SyncOwnInventoryStatus::CloneDetected;
				result.cloneVerdict = SyncCloneVerdict::DeviceMismatch;
				return result;
			}
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto parsed = ParseSyncEnvelope(record.bytes);
		if (parsed.header->seq != record.header->seq
			|| parsed.header->space != record.header->space
			|| parsed.header->stream != record.header->stream
			|| parsed.header->writerInstall != record.header->writerInstall
			|| parsed.header->writerDevice != record.header->writerDevice) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto seq = parsed.header->seq;
		if (state.config.pendingSeq == seq
			&& record.bytes != stagedCanonicalRecord) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		const auto [position, inserted] = recordsBySeq.try_emplace(
			seq, record.bytes);
		if (!inserted && position->second != record.bytes) {
			result.status = SyncOwnInventoryStatus::NeedsReview;
			return result;
		}
		if (state.config.pendingSeq == seq
			&& record.bytes == stagedCanonicalRecord
			&& (!result.pendingMessageId
				|| record.id < *result.pendingMessageId)) {
			result.pendingMessageId = record.id;
		}
		if (seq == recordsBySeq.rbegin()->first) {
			if (!headId || seq > result.observation.seq) {
				result.duplicateHeadMessageIds.clear();
				headId = record.id;
			} else if (record.id < headId) {
				result.duplicateHeadMessageIds.push_back(headId);
				headId = record.id;
			} else {
				result.duplicateHeadMessageIds.push_back(record.id);
			}
			result.observation.seq = seq;
		}
	}
	if (recordsBySeq.empty()) {
		result.observation.kind = OwnRecordObservationKind::Absent;
		result.cloneVerdict = CheckSyncClone(
			state, state.createdDevice, SyncLocalStream::Config,
			result.observation);
		result.status = (result.cloneVerdict == SyncCloneVerdict::NoClone)
			? SyncOwnInventoryStatus::Absent
			: SyncOwnInventoryStatus::CloneDetected;
		return result;
	}

	const auto &headBytes = recordsBySeq.rbegin()->second;
	const auto head = ParseSyncEnvelope(headBytes);
	result.observation.kind = OwnRecordObservationKind::Present;
	result.observation.seq = head.header->seq;
	result.observation.payloadHash = head.envelope.document.value(
		u"payload_sha256"_q).toString();
	result.head = { headId, RecordHash(headBytes), true };
	std::sort(result.duplicateHeadMessageIds.begin(),
		result.duplicateHeadMessageIds.end());
	result.cloneVerdict = CheckSyncClone(
		state, state.createdDevice, SyncLocalStream::Config,
		result.observation);
	result.status = (result.cloneVerdict != SyncCloneVerdict::NoClone)
		? SyncOwnInventoryStatus::CloneDetected
		: result.pendingMessageId
		? SyncOwnInventoryStatus::PendingFound
		: SyncOwnInventoryStatus::Present;
	return result;
}

} // namespace Purple
