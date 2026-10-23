/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_carry.h"

#include "purple/purple_sync_json.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace Purple {
namespace {

constexpr auto kSafeInteger = uint64_t(9007199254740991ULL);

[[nodiscard]] std::optional<PeerToken> Normalized(
		const PeerToken &chat,
		const AccountKey &account) {
	if (!AccountKeyText(account.environment, account.userId)) {
		return std::nullopt;
	}
	const auto text = PeerTokenText(chat, uint64_t(account.userId));
	return text ? ParsePeerToken(*text) : std::nullopt;
}

[[nodiscard]] bool Scope(
		const SyncCarryReference &reference,
		const AccountKey &account,
		const PeerToken &chat) {
	return reference.account == account
		&& Normalized(reference.chat, account) == chat
		&& reference.messageId > 0
		&& IsSyncCarryHash(reference.recordHash);
}

[[nodiscard]] bool SameMessage(
		const SyncCarryMessage &a,
		const SyncCarryMessage &b) {
	return a.reference == b.reference && a.date == b.date && a.bytes == b.bytes
		&& a.family == b.family && a.ownership == b.ownership;
}

[[nodiscard]] bool Carryable(SyncCandidateStatus status) {
	return status == SyncCandidateStatus::Valid
		|| status == SyncCandidateStatus::NewerSchema
		|| status == SyncCandidateStatus::NewerMajor
		|| status == SyncCandidateStatus::UnsupportedStream
		|| status == SyncCandidateStatus::UnsupportedEncoding
		|| status == SyncCandidateStatus::UnsupportedLibrary;
}

[[nodiscard]] QString StateHash(const SyncLocalState &state) {
	const auto written = SerializeSyncLocalState(state);
	return written ? SyncCarryRecordHash(written.canonical) : QString();
}

[[nodiscard]] std::optional<ConfigPayloadInspection> OwnStage(
		const SyncLocalState &state,
		const QByteArray &bytes) {
	if (!state.config.pendingSeq || state.createdDevice.isEmpty()) {
		return std::nullopt;
	}
	const auto parsed = ParseSyncEnvelope(bytes);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid
		|| !parsed.header || parsed.header->stream != u"config"_q
		|| parsed.header->space != state.space
		|| parsed.header->writerInstall != state.install
		|| parsed.header->writerDevice != state.createdDevice
		|| parsed.header->seq != state.config.pendingSeq
		|| parsed.envelope.document.value(u"payload_sha256"_q).toString()
			!= state.config.ownHash
		|| (!state.configData.pending.isEmpty()
			&& state.configData.pending != inspected.version.key)) {
		return std::nullopt;
	}
	const auto appended = AppendIssuedConfigRecord(state, bytes);
	return (appended && !appended.changed)
		? std::optional(inspected) : std::nullopt;
}

[[nodiscard]] bool Issued(
		const std::vector<SyncIssuedRecord> &records,
		uint64_t seq,
		const QString &hash) {
	return std::any_of(records.begin(), records.end(), [&](const auto &record) {
		return record.seq == seq && record.recordHash == hash;
	});
}

[[nodiscard]] bool SameIssued(
		const std::vector<SyncIssuedRecord> &a,
		const std::vector<SyncIssuedRecord> &b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
		[](const auto &x, const auto &y) {
			return x.seq == y.seq && x.recordHash == y.recordHash;
		});
}

[[nodiscard]] bool SameHeader(
		const std::optional<SyncEnvelopeHeader> &a,
		const std::optional<SyncEnvelopeHeader> &b) {
	if (!a || !b) {
		return !a && !b;
	}
	return std::tie(a->space, a->stream, a->writerInstall, a->writerDevice,
		a->writerPlatform, a->writerApp, a->seq, a->at)
		== std::tie(b->space, b->stream, b->writerInstall, b->writerDevice,
			b->writerPlatform, b->writerApp, b->seq, b->at);
}

[[nodiscard]] bool HasHash(
		const std::vector<QString> &hashes,
		const QString &hash) {
	return std::binary_search(hashes.begin(), hashes.end(), hash);
}

[[nodiscard]] SyncCarryPendingResult PendingBase(
		const AccountKey &account,
		const SyncLocalState &state,
		const PeerToken &source) {
	auto result = SyncCarryPendingResult();
	result.account = account;
	result.chat = source;
	result.state = state;
	result.beforeHash = StateHash(state);
	if (result.beforeHash.isEmpty() || !Normalized(source, account)) {
		return result;
	}
	result.chat = *Normalized(source, account);
	if (state.chat != result.chat) {
		result.status = SyncCarryPendingStatus::WrongChat;
	} else if (state.library.pendingSeq) {
		result.status = SyncCarryPendingStatus::LibraryPending;
	} else if (!state.config.pendingSeq) {
		result.status = state.configData.pending.isEmpty()
			? SyncCarryPendingStatus::NoPending
			: SyncCarryPendingStatus::InvalidStage;
	} else {
		result.status = SyncCarryPendingStatus::Ready;
		result.stageSeq = state.config.pendingSeq;
	}
	return result;
}

}

QString SyncCarryRecordHash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes, QCryptographicHash::Sha256).toHex());
}

bool IsSyncCarryHash(const QString &hash) {
	if (hash.size() != 64) {
		return false;
	}
	for (const auto ch : hash) {
		if (!((ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f'))) {
			return false;
		}
	}
	return true;
}

std::optional<SyncCarryMessage> MakeSyncCarryMessage(
		const AccountKey &account,
		const PeerToken &chat,
		int32_t messageId,
		int64_t date,
		QByteArray bytes,
		SyncChatMessageOwnership ownership,
		SyncCarryFamily family) {
	const auto normalized = Normalized(chat, account);
	if (!normalized || messageId <= 0 || date <= 0
		|| uint64_t(date) > kSafeInteger || bytes.size() > kSyncRecordMaximumBytes
		|| ownership < SyncChatMessageOwnership::Unknown
		|| ownership > SyncChatMessageOwnership::Other
		|| family < SyncCarryFamily::Settings || family > SyncCarryFamily::Playlists) {
		return std::nullopt;
	}
	const auto hash = SyncCarryRecordHash(bytes);
	return SyncCarryMessage{
		{ account, *normalized, messageId, hash }, date, std::move(bytes), family, ownership,
	};
}

SyncCarrySelection SelectSyncCarryHeads(const SyncCarryInventory &inventory) {
	auto result = SyncCarrySelection();
	const auto chat = Normalized(inventory.chat, inventory.account);
	if (!chat) {
		return result;
	}
	if (!inventory.complete) {
		result.status = SyncCarryStatus::ReadDestination;
		return result;
	}
	auto messages = std::map<int32_t, SyncCarryMessage>();
	for (const auto &message : inventory.messages) {
		auto normalized = MakeSyncCarryMessage(inventory.account,
			message.reference.chat, message.reference.messageId, message.date,
			message.bytes, message.ownership, message.family);
		if (!normalized || !Scope(message.reference, inventory.account, *chat)
			|| normalized->reference.recordHash != message.reference.recordHash) {
			result.status = SyncCarryStatus::WrongScope;
			return result;
		}
		const auto [i, inserted] = messages.emplace(
			normalized->reference.messageId, *normalized);
		if (!inserted && !SameMessage(i->second, *normalized)) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
	}
	auto candidates = std::vector<SyncDirectoryCandidate>();
	auto selected = std::map<int32_t, SyncCarrySelected>();
	auto hashes = std::set<QString>();
	auto stampRecords = QJsonArray();
	for (const auto &[id, message] : messages) {
		if (message.ownership == SyncChatMessageOwnership::Other) {
			continue;
		} else if (message.ownership != SyncChatMessageOwnership::Own) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
		stampRecords.append(QJsonArray{
			id, QJsonValue(qint64(message.date)), int(message.family),
			message.reference.recordHash,
		});
		if (message.family == SyncCarryFamily::Playlists) {
			result.playlists = true;
			continue;
		}
		const auto parsed = ClassifySyncCandidate(id, message.bytes);
		if (parsed.header && parsed.header->stream == u"library"_q) {
			result.playlists = true;
			continue;
		}
		if (!Carryable(parsed.status)) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
		const auto record = SyncCarrySelected{
			message.reference, message.date, parsed.status, parsed.header,
		};
		result.records.push_back(record);
		hashes.insert(message.reference.recordHash);
		selected.emplace(id, record);
		if (parsed.status == SyncCandidateStatus::NewerMajor) {
			result.heads.push_back(record);
			continue;
		}
		auto candidate = SyncDirectoryCandidateOf(parsed);
		candidate.documentId = uint64_t(id);
		candidate.recordHash = message.reference.recordHash;
		if (parsed.status == SyncCandidateStatus::NewerSchema) {
			candidate.status = SyncEnvelopeStatus::Valid;
		}
		candidates.push_back(std::move(candidate));
	}
	const auto directory = ResolveSyncDirectory(candidates, true);
	if (directory.unreadableCandidate || directory.messageIdCollision) {
		result.status = SyncCarryStatus::NeedsReview;
		return result;
	}
	for (const auto &group : directory.groups) {
		for (const auto &head : group.headCandidates) {
			result.heads.push_back(selected.at(int32_t(head.messageId)));
		}
	}
	std::sort(result.heads.begin(), result.heads.end(), [](const auto &a, const auto &b) {
		return std::tie(a.reference.recordHash, a.reference.messageId)
			< std::tie(b.reference.recordHash, b.reference.messageId);
	});
	result.heads.erase(std::unique(result.heads.begin(), result.heads.end(),
		[](const auto &a, const auto &b) {
			return a.reference.recordHash == b.reference.recordHash;
		}), result.heads.end());
	result.hashes.assign(hashes.begin(), hashes.end());
	const auto stamp = QJsonObject{
		{ u"account"_q, *AccountKeyText(inventory.account.environment, inventory.account.userId) },
		{ u"chat"_q, *PeerTokenText(*chat) },
		{ u"records"_q, stampRecords },
	};
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(stamp).toJson());
	if (!canonical) {
		return {};
	}
	result.stamp = SyncCarryRecordHash(canonical.canonical);
	result.status = SyncCarryStatus::Ready;
	return result;
}

SyncCarryPlanResult PlanCarry(const SyncCarryInput &input) {
	auto result = SyncCarryPlanResult();
	const auto source = Normalized(input.source, input.account);
	const auto target = Normalized(input.target, input.account);
	if (!source || !target || source == target
		|| input.mode < SyncCarryMode::Switch || input.mode > SyncCarryMode::Fill
		|| input.access < SyncCarrySourceAccess::Unknown
		|| input.access > SyncCarrySourceAccess::ChatGone
		|| input.cleanup < SyncCarryCleanup::None
		|| input.cleanup > SyncCarryCleanup::AllSettings
		|| (input.cleanup != SyncCarryCleanup::None && !input.cleanupAuthorized)
		|| (input.cleanup == SyncCarryCleanup::AllSettings
			&& input.mode != SyncCarryMode::Switch)
		|| (input.cleanup == SyncCarryCleanup::OwnLatePosts
			&& input.mode != SyncCarryMode::Follow)) {
		return result;
	}
	if (!input.targetEligibility) {
		result.status = SyncCarryStatus::NeedsReview;
		return result;
	} else if (input.targetEligibility.readers.warningRequired
		&& !input.readerConsentAccepted) {
		result.status = SyncCarryStatus::ConsentRequired;
		return result;
	}
	if (input.winner
		&& (Normalized(input.winner->chat, input.account) != target
			|| input.winner->markId <= 0 || input.winner->markDate <= 0)) {
		return result;
	}
	if (input.mode != SyncCarryMode::Switch && !input.winner) {
		return result;
	}
	if (input.state) {
		const auto &state = *input.state;
		if (CheckSyncAccountBinding(state, input.preferenceToken)
				!= SyncAccountBindingVerdict::Bound
			|| Normalized(state.chat, input.account) != source
			|| state.createdDevice.isEmpty()) {
			result.status = SyncCarryStatus::WrongScope;
			return result;
		}
		if (state.library.pendingSeq || input.otherStagesPresent
			|| (!state.config.pendingSeq && !state.configData.pending.isEmpty())) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
		if (state.config.pendingSeq) {
			result.status = (input.access == SyncCarrySourceAccess::ChatGone)
				? SyncCarryStatus::AbandonPending
				: (input.access == SyncCarrySourceAccess::Readable
					|| input.access == SyncCarrySourceAccess::Protected)
				? SyncCarryStatus::FinishPendingOld : SyncCarryStatus::UnknownSource;
			return result;
		}
	} else if (input.otherStagesPresent) {
		result.status = SyncCarryStatus::NeedsReview;
		return result;
	}
	if (input.access == SyncCarrySourceAccess::Unknown) {
		result.status = SyncCarryStatus::UnknownSource;
		return result;
	}
	if (input.newInventory.account != input.account
		|| Normalized(input.newInventory.chat, input.account) != target) {
		result.status = SyncCarryStatus::WrongScope;
		return result;
	}
	const auto destination = SelectSyncCarryHeads(input.newInventory);
	if (destination.status != SyncCarryStatus::Ready) {
		result.status = destination.status;
		return result;
	}
	auto old = SyncCarrySelection();
	if (input.access == SyncCarrySourceAccess::Readable) {
		if (input.oldInventory.account != input.account
			|| Normalized(input.oldInventory.chat, input.account) != source) {
			result.status = SyncCarryStatus::WrongScope;
			return result;
		}
		old = SelectSyncCarryHeads(input.oldInventory);
		if (old.status != SyncCarryStatus::Ready) {
			result.status = (old.status == SyncCarryStatus::ReadDestination)
				? SyncCarryStatus::ReadSource : old.status;
			return result;
		}
	}
	auto &plan = result.plan;
	plan.account = input.account;
	plan.source = *source;
	plan.target = *target;
	plan.mode = input.mode;
	plan.access = input.access;
	plan.sourceStamp = old.stamp;
	plan.winner = input.winner;
	if (plan.winner) {
		plan.winner->chat = *target;
	}
	plan.cleanup = input.cleanup;
	plan.cleanupAuthorized = input.cleanupAuthorized;
	plan.selected = old.heads;
	plan.cleanupCandidates = old.records;
	plan.playlistRequired = input.playlistsInUse || old.playlists || destination.playlists;
	auto required = std::set<QString>();
	for (const auto &record : old.heads) {
		required.insert(record.reference.recordHash);
		if (!HasHash(destination.hashes, record.reference.recordHash)) {
			plan.forwards.push_back(record.reference);
		}
	}
	if (input.state) {
		const auto &state = *input.state;
		plan.stateHash = StateHash(state);
		plan.ownerInstall = state.install;
		plan.ownerDevice = state.createdDevice;
		plan.confirmedAtCommit = state.config.confirmedSeq;
		plan.issued = state.config.issuedRecords;
		auto ownPresent = false;
		for (const auto selection : std::array<const SyncCarrySelection*, 2>{ &old, &destination }) {
			for (const auto &record : selection->records) {
				if (!record.header || record.header->space != state.space
					|| record.header->stream != u"config"_q
					|| record.header->writerInstall != state.install) {
					continue;
				}
				const auto currentUnissued = record.header->seq == state.config.seq
					&& !Issued(state.config.issuedRecords, record.header->seq,
						record.reference.recordHash);
				auto legacyPayloadMatches = false;
				if (currentUnissued && record.status == SyncCandidateStatus::Valid
					&& std::none_of(state.config.issuedRecords.begin(), state.config.issuedRecords.end(),
						[&](const auto &issued) { return issued.seq == record.header->seq; })) {
					const auto &inventory = selection == &old ? input.oldInventory : input.newInventory;
					const auto message = std::find_if(inventory.messages.begin(), inventory.messages.end(),
						[&](const auto &entry) { return entry.reference == record.reference; });
					if (message != inventory.messages.end()) {
						const auto parsed = ParseSyncEnvelope(message->bytes);
						legacyPayloadMatches = parsed && parsed.envelope.document
							.value(u"payload_sha256"_q).toString() == state.config.ownHash;
					}
				}
				if (record.header->writerDevice != state.createdDevice
					|| record.header->seq > state.config.seq
					|| (currentUnissued && !legacyPayloadMatches)) {
					result.status = SyncCarryStatus::NeedsReview;
					return result;
				}
				if (record.header->seq == state.config.confirmedSeq
					&& Issued(state.config.issuedRecords, record.header->seq,
						record.reference.recordHash)) {
					const auto carried = selection == &destination
						|| required.contains(record.reference.recordHash);
					if (carried) {
						ownPresent = true;
						required.insert(record.reference.recordHash);
					}
				}
			}
		}
		const auto ownNeeded = (!state.configData.base.isEmpty()
				|| state.config.confirmedSeq != 0)
			&& (!ownPresent || state.config.abandonedSeq != 0);
		if (ownNeeded) {
			if (!input.ownVersion || state.configData.base.isEmpty()
				|| input.ownVersion->version.key != state.configData.base
				|| input.ownVersion->version.lineage != state.configData.baseLineage
				|| state.config.seq == kSafeInteger) {
				result.status = SyncCarryStatus::MissingLocalVersion;
				return result;
			}
			const auto &own = *input.ownVersion;
			const auto record = BuildConfigRecord({
				.text = own.text,
				.version = own.version,
				.space = state.space,
				.install = state.install,
				.device = state.createdDevice,
				.platform = own.platform,
				.app = own.app,
				.seq = state.config.seq + 1,
				.at = own.at,
			});
			if (!record) {
				result.status = SyncCarryStatus::MissingLocalVersion;
				return result;
			}
			auto reservation = ReserveSyncSeq(state, SyncLocalStream::Config, record.payloadHash);
			if (!reservation) {
				result.status = SyncCarryStatus::NeedsReview;
				return result;
			}
			reservation.state.configData.pending = record.version.key;
			const auto issued = AppendIssuedConfigRecord(reservation.state, record.canonical);
			if (!issued) {
				result.status = SyncCarryStatus::NeedsReview;
				return result;
			}
			result.reservedState = issued.state;
			plan.reservedStateHash = StateHash(issued.state);
			plan.ownRecord = record.canonical;
			plan.ownRecordHash = SyncCarryRecordHash(record.canonical);
			plan.confirmedAtCommit = reservation.seq;
			plan.issued = issued.state.config.issuedRecords;
			required.insert(plan.ownRecordHash);
		}
	}
	plan.requiredHashes.assign(required.begin(), required.end());
	result.status = SyncCarryStatus::Ready;
	return result;
}

SyncCarryReadback VerifySyncCarryReadback(
		const SyncCarryPlan &plan,
		const SyncCarryInventory &destination,
		const SyncPlaylistCarryReceipt &playlist) {
	auto result = SyncCarryReadback();
	if (!ParseSyncCarryPlan(SyncCarryPlanJson(plan))) {
		return result;
	}
	if (destination.account != plan.account
		|| Normalized(destination.chat, plan.account) != plan.target) {
		result.status = SyncCarryStatus::WrongScope;
		return result;
	}
	const auto selected = SelectSyncCarryHeads(destination);
	if (selected.status != SyncCarryStatus::Ready) {
		result.status = selected.status;
		return result;
	}
	result.destinationStamp = selected.stamp;
	for (const auto &hash : plan.requiredHashes) {
		if (!HasHash(selected.hashes, hash)) {
			result.missingHashes.push_back(hash);
		}
	}
	if (!result.missingHashes.empty()) {
		result.status = SyncCarryStatus::MissingRecords;
		return result;
	}
	if (plan.playlistRequired || selected.playlists) {
		if (!plan.playlistRequired || playlist.status != SyncPlaylistCarryStatus::Verified
			|| playlist.account != plan.account || playlist.source != plan.source
			|| playlist.target != plan.target
			|| playlist.planStamp != SyncCarryPlanStamp(plan)
			|| playlist.destinationStamp != selected.stamp) {
			result.status = SyncCarryStatus::PlaylistRequired;
			return result;
		}
	}
	result.status = SyncCarryStatus::Ready;
	return result;
}

std::optional<SyncLocalState> PrepareSyncCarryStage(
		const SyncCarryPlan &plan,
		const SyncLocalState &state) {
	if (!ParseSyncCarryPlan(SyncCarryPlanJson(plan)) || plan.ownRecord.isEmpty()
		|| state.chat != plan.source || state.install != plan.ownerInstall
		|| state.createdDevice != plan.ownerDevice) {
		return std::nullopt;
	}
	if (StateHash(state) == plan.reservedStateHash) {
		return OwnStage(state, plan.ownRecord) ? std::optional(state) : std::nullopt;
	}
	if (StateHash(state) != plan.stateHash || state.config.pendingSeq
		|| state.library.pendingSeq || !state.configData.pending.isEmpty()) {
		return std::nullopt;
	}
	const auto parsed = ParseSyncEnvelope(plan.ownRecord);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed.header || inspected.status != ConfigPayloadStatus::Valid
		|| inspected.version.key != state.configData.base
		|| parsed.header->seq != state.config.seq + 1) {
		return std::nullopt;
	}
	auto reserved = ReserveSyncSeq(state, SyncLocalStream::Config,
		parsed.envelope.document.value(u"payload_sha256"_q).toString());
	if (!reserved) {
		return std::nullopt;
	}
	reserved.state.configData.pending = inspected.version.key;
	const auto issued = AppendIssuedConfigRecord(reserved.state, plan.ownRecord);
	return issued && StateHash(issued.state) == plan.reservedStateHash
		? std::optional(issued.state) : std::nullopt;
}

bool SyncCarryPlanUsesState(
		const SyncCarryPlan &plan,
		const std::optional<SyncLocalState> &state) {
	if (!ParseSyncCarryPlan(SyncCarryPlanJson(plan))) {
		return false;
	}
	if (plan.stateHash.isEmpty()) {
		return !state;
	}
	if (!state || state->chat != plan.source || state->install != plan.ownerInstall
		|| state->createdDevice != plan.ownerDevice) {
		return false;
	}
	if (plan.ownRecord.isEmpty()) {
		return StateHash(*state) == plan.stateHash
			&& plan.confirmedAtCommit == state->config.confirmedSeq
			&& SameIssued(plan.issued, state->config.issuedRecords);
	}
	const auto reserved = PrepareSyncCarryStage(plan, *state);
	return reserved && plan.confirmedAtCommit == reserved->config.pendingSeq
		&& SameIssued(plan.issued, reserved->config.issuedRecords);
}

SyncCarryCommit PrepareSyncCarryCommit(
		const SyncCarryPlan &plan,
		const std::optional<SyncLocalState> &state,
		const SyncCarryInventory &destination,
		const SyncPlaylistCarryReceipt &playlist) {
	auto result = SyncCarryCommit();
	const auto verified = VerifySyncCarryReadback(plan, destination, playlist);
	if (!verified) {
		result.status = verified.status;
		return result;
	}
	if (plan.stateHash.isEmpty()) {
		result.status = state ? SyncCarryStatus::WrongScope : SyncCarryStatus::Ready;
		return result;
	}
	if (!SyncCarryPlanUsesState(plan, state) || !state
		|| StateHash(*state) != (plan.ownRecord.isEmpty()
		? plan.stateHash : plan.reservedStateHash)
		|| state->chat != plan.source || state->install != plan.ownerInstall
		|| state->createdDevice != plan.ownerDevice || state->library.pendingSeq) {
		result.status = SyncCarryStatus::WrongScope;
		return result;
	}
	auto next = *state;
	if (!plan.ownRecord.isEmpty()) {
		const auto stage = OwnStage(next, plan.ownRecord);
		const auto parsed = ParseSyncEnvelope(plan.ownRecord);
		if (!stage || !parsed.header) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
		const auto confirmed = ConfirmConfigReadBack(next, next.createdDevice,
			{ OwnRecordObservationKind::Present, parsed.header->seq,
				parsed.envelope.document.value(u"payload_sha256"_q).toString() },
			stage->version);
		if (!confirmed.changed || confirmed.verdict != SyncCloneVerdict::NoClone) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
		next = confirmed.state;
	}
	const auto moved = MoveSyncChat(next, plan.target, false);
	if (!moved) {
		result.status = SyncCarryStatus::NeedsReview;
		return result;
	}
	next = moved.state;
	auto messages = destination.messages;
	std::sort(messages.begin(), messages.end(), [](const auto &a, const auto &b) {
		return a.reference.messageId < b.reference.messageId;
	});
	auto adoptedHashes = std::set<QString>();
	for (const auto &message : messages) {
		if (message.family != SyncCarryFamily::Settings
			|| message.ownership != SyncChatMessageOwnership::Own) {
			continue;
		}
		const auto record = ClassifySyncCandidate(message.reference.messageId, message.bytes);
		if (!record.header || record.status != SyncCandidateStatus::Valid
			|| record.header->writerInstall != next.install
			|| record.header->writerDevice != next.createdDevice
			|| record.header->seq > next.config.confirmedSeq
			|| !Issued(next.config.issuedRecords, record.header->seq,
				message.reference.recordHash)) {
			continue;
		}
		if (!adoptedHashes.insert(message.reference.recordHash).second) {
			continue;
		}
		const auto adopted = AdoptIssuedOwnConfigMessage(
			next, message.reference.messageId, message.bytes);
		if (!adopted) {
			result.status = SyncCarryStatus::NeedsReview;
			return result;
		}
		next = adopted.state;
	}
	const auto written = SerializeSyncLocalState(next);
	if (!written) {
		result.status = SyncCarryStatus::NeedsReview;
		return result;
	}
	result.status = SyncCarryStatus::Ready;
	result.state = std::move(next);
	result.canonical = written.canonical;
	result.stateHash = SyncCarryRecordHash(written.canonical);
	return result;
}

std::vector<SyncCarryReference> SyncCarryCleanupCandidates(
		const SyncCarryPlan &plan,
		const SyncCarryInventory &destination,
		const SyncPlaylistCarryReceipt &playlist) {
	if (!plan.cleanupAuthorized || plan.cleanup == SyncCarryCleanup::None
		|| !VerifySyncCarryReadback(plan, destination, playlist)) {
		return {};
	}
	auto result = std::vector<SyncCarryReference>();
	const auto selection = SelectSyncCarryHeads(destination);
	for (const auto &record : plan.cleanupCandidates) {
		if (plan.cleanup == SyncCarryCleanup::AllSettings
			&& plan.mode == SyncCarryMode::Switch) {
			result.push_back(record.reference);
		} else if (plan.cleanup == SyncCarryCleanup::OwnLatePosts
			&& plan.mode == SyncCarryMode::Follow && plan.source != PeerToken()
			&& plan.winner && record.date > plan.winner->markDate
			&& record.header && record.header->stream == u"config"_q
			&& record.header->writerInstall == plan.ownerInstall
			&& record.header->writerDevice == plan.ownerDevice
			&& record.header->seq <= plan.confirmedAtCommit
			&& Issued(plan.issued, record.header->seq, record.reference.recordHash)
			&& HasHash(selection.hashes, record.reference.recordHash)) {
			result.push_back(record.reference);
		}
	}
	return result;
}

bool SyncCarrySourceMatchesCleanup(
		const SyncCarryPlan &plan,
		const SyncCarryMessage &freshSource,
		const SyncCarryInventory &destination,
		const SyncPlaylistCarryReceipt &playlist) {
	if (!Scope(freshSource.reference, plan.account, plan.source)
		|| freshSource.ownership != SyncChatMessageOwnership::Own
		|| freshSource.family != SyncCarryFamily::Settings
		|| SyncCarryRecordHash(freshSource.bytes) != freshSource.reference.recordHash) {
		return false;
	}
	const auto metadata = std::find_if(plan.cleanupCandidates.begin(), plan.cleanupCandidates.end(),
		[&](const auto &record) {
			return record.reference == freshSource.reference && record.date == freshSource.date;
		});
	const auto classified = ClassifySyncCandidate(freshSource.reference.messageId, freshSource.bytes);
	if (metadata == plan.cleanupCandidates.end() || metadata->status != classified.status
		|| !SameHeader(metadata->header, classified.header)) {
		return false;
	}
	const auto candidates = SyncCarryCleanupCandidates(plan, destination, playlist);
	return std::find(candidates.begin(), candidates.end(), freshSource.reference)
		!= candidates.end();
}

SyncCarryPendingResult AbandonStageForLostChat(
		const AccountKey &account,
		const SyncLocalState &state,
		const PeerToken &source,
		SyncChatDirectRead outcome,
		const QByteArray &stagedRecord,
		const QByteArray &retainedLocalText) {
	auto result = PendingBase(account, state, source);
	if (result.status != SyncCarryPendingStatus::Ready) {
		return result;
	}
	if (outcome != SyncChatDirectRead::ChatGone) {
		result.status = SyncCarryPendingStatus::NotChatGone;
		return result;
	}
	const auto stage = OwnStage(state, stagedRecord);
	if (!stage || state.version != 2) {
		result.status = SyncCarryPendingStatus::InvalidStage;
		return result;
	}
	if (stage->text != retainedLocalText) {
		result.status = SyncCarryPendingStatus::LocalCopyRequired;
		return result;
	}
	result.stageHash = SyncCarryRecordHash(stagedRecord);
	result.state.config.abandonedSeq = state.config.pendingSeq;
	result.state.config.pendingSeq = 0;
	result.state.configData.pending.clear();
	if (!SerializeSyncLocalState(result.state)) {
		result.state = state;
		result.status = SyncCarryPendingStatus::Invalid;
	}
	return result;
}

SyncCarryPendingResult FinishPendingStageForCarry(
		const AccountKey &account,
		const SyncLocalState &state,
		const QByteArray &stagedRecord,
		const SyncCarryMessage &freshReadback) {
	auto result = PendingBase(account, state, state.chat);
	if (result.status != SyncCarryPendingStatus::Ready) {
		return result;
	}
	if (!Scope(freshReadback.reference, account, state.chat)) {
		result.status = SyncCarryPendingStatus::WrongChat;
		return result;
	}
	const auto stage = OwnStage(state, stagedRecord);
	if (!stage || freshReadback.family != SyncCarryFamily::Settings
		|| freshReadback.ownership != SyncChatMessageOwnership::Own
		|| freshReadback.bytes != stagedRecord
		|| SyncCarryRecordHash(freshReadback.bytes) != freshReadback.reference.recordHash) {
		result.status = SyncCarryPendingStatus::InvalidStage;
		return result;
	}
	const auto confirmed = ConfirmConfigReadBack(state, state.createdDevice,
		{ OwnRecordObservationKind::Present, state.config.pendingSeq, state.config.ownHash },
		stage->version);
	if (!confirmed.changed || confirmed.verdict != SyncCloneVerdict::NoClone) {
		result.status = SyncCarryPendingStatus::InvalidStage;
		return result;
	}
	const auto recorded = RecordConfirmedOwnConfigMessage(
		confirmed.state, freshReadback.reference.messageId, freshReadback.bytes);
	if (!recorded) {
		result.status = SyncCarryPendingStatus::InvalidStage;
		return result;
	}
	result.stageHash = freshReadback.reference.recordHash;
	result.state = recorded.state;
	return result;
}

}
