/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_config_flow.h"

#include "purple/purple_config_payload.h"
#include "purple/purple_state.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] QString RecordHash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes,
		QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] SyncConfigReviewStatus InventoryStatus(
		const SyncAccountInventoryResult &inventory) {
	const auto &directory = inventory.directory;
	if (!inventory.scan.complete()
		|| !inventory.read
		|| inventory.read->status == SyncCandidateReadStatus::Incomplete
		|| !directory.complete) {
		return SyncConfigReviewStatus::Incomplete;
	} else if (inventory.status == SyncAccountInventoryStatus::NeedsReview
		|| inventory.read->status == SyncCandidateReadStatus::NeedsReview) {
		return SyncConfigReviewStatus::NeedsReview;
	} else if (inventory.status != SyncAccountInventoryStatus::Complete
		|| !inventory.read->complete()) {
		return SyncConfigReviewStatus::Incomplete;
	} else if (directory.unreadableCandidate
		|| directory.messageIdCollision
		|| (directory.selectedSpace
			&& (!directory.publishableSpace
				|| *directory.selectedSpace != *directory.publishableSpace
				|| !IsSyncSpaceId(*directory.selectedSpace)))
		|| (!directory.selectedSpace && !directory.canCreateSpace)) {
		return SyncConfigReviewStatus::NeedsReview;
	}
	return SyncConfigReviewStatus::Ready;
}

[[nodiscard]] bool AnyNewerSchema(
		const std::vector<SyncConfigHeadRecord> &heads) {
	return std::any_of(heads.begin(), heads.end(), [](const auto &head) {
		return head.newerSchema;
	});
}

[[nodiscard]] SyncConfigReview Refuse(
		SyncConfigReview review,
		SyncConfigReviewStatus status) {
	review.status = status;
	return review;
}

[[nodiscard]] bool SameConfigData(
		const SyncLocalConfigState &a,
		const SyncLocalConfigState &b) {
	return a.base == b.base
		&& a.baseLineage == b.baseLineage
		&& a.equiv == b.equiv
		&& a.pending == b.pending
		&& a.seenSeq == b.seenSeq;
}

[[nodiscard]] bool KeepsSeenSequences(
		const std::map<QString, uint64_t> &current,
		const std::map<QString, uint64_t> &next) {
	return std::all_of(current.begin(), current.end(), [&](const auto &entry) {
		const auto found = next.find(entry.first);
		return (found != next.end()) && (found->second >= entry.second);
	});
}

[[nodiscard]] SyncConfigPublishStatus OwnFailure(
		SyncOwnInventoryStatus status) {
	switch (status) {
	case SyncOwnInventoryStatus::Incomplete:
		return SyncConfigPublishStatus::Incomplete;
	case SyncOwnInventoryStatus::CloneDetected:
		return SyncConfigPublishStatus::CloneDetected;
	default:
		return SyncConfigPublishStatus::NeedsReview;
	}
}

[[nodiscard]] SyncConfigPostPlan FinishPost(SyncConfigPublishStatus status) {
	auto result = SyncConfigPostPlan();
	result.step = SyncConfigPostStep::Finish;
	result.status = status;
	return result;
}

class StampWriter final {
public:
	void add(const char *name, const QByteArray &value) {
		_bytes += name;
		_bytes += '=';
		_bytes += QByteArray::number(qlonglong(value.size()));
		_bytes += ':';
		_bytes += value;
		_bytes += '\n';
	}
	void add(const char *name, const QString &value) {
		add(name, value.toUtf8());
	}
	void add(const char *name, uint64_t value) {
		add(name, QByteArray::number(qulonglong(value)));
	}
	void add(const char *name, int64_t value) {
		add(name, QByteArray::number(qlonglong(value)));
	}
	void add(const char *name, bool value) {
		add(name, QByteArray(value ? "1" : "0"));
	}
	void addKeys(const char *name, const std::vector<ConfigHead> &heads) {
		add(name, uint64_t(heads.size()));
		for (const auto &head : heads) {
			add("key", head.key);
		}
	}
	void addHead(const ConfigHead &head, int32_t messageId) {
		add("message", int64_t(messageId));
		add("space", head.space);
		add("install", head.install);
		add("seq", head.seq);
		add("key", head.key);
		add("lineage", uint64_t(head.lineage.size()));
		for (const auto &key : head.lineage) {
			add("parent", key);
		}
	}
	[[nodiscard]] QString finish() const {
		return RecordHash(_bytes);
	}

private:
	QByteArray _bytes;

};

} // namespace

SyncSettingsFile MakeSyncSettingsFile(
		SyncSettingsFileStatus status,
		const QByteArray &text,
		bool usingLastGood) {
	auto result = SyncSettingsFile();
	result.usingLastGood = usingLastGood;
	switch (status) {
	case SyncSettingsFileStatus::Absent:
		result.status = SyncSettingsFileStatus::Absent;
		result.fingerprint = SettingsFingerprint(QByteArray());
		break;
	case SyncSettingsFileStatus::Present:
		if (text.size() <= kSyncSettingsMaximumBytes) {
			result.status = SyncSettingsFileStatus::Present;
			result.text = text;
			result.fingerprint = SettingsFingerprint(text);
		}
		break;
	case SyncSettingsFileStatus::Invalid:
		break;
	}
	return result;
}

bool SameSyncSettingsFile(
		const SyncSettingsFile &a,
		const SyncSettingsFile &b) {
	return a.status == b.status
		&& a.status != SyncSettingsFileStatus::Invalid
		&& a.fingerprint == b.fingerprint
		&& a.text == b.text
		&& a.usingLastGood == b.usingLastGood;
}

bool SyncSettingsTextWritable(const QByteArray &bytes) {
	return QString::fromUtf8(bytes).toUtf8() == bytes;
}

QString SyncConfigKeyFingerprint(const QString &key) {
	const auto parsed = ParseConfigVersionKey(key);
	return parsed ? parsed->fingerprint : QString();
}

ConfigSyncState SyncConfigStateOf(const SyncLocalState &state) {
	const auto &data = state.configData;
	return {
		.space = state.space,
		.install = state.install,
		.base = data.base,
		.baseLineage = data.baseLineage,
		.equiv = data.equiv,
		.pending = data.pending,
		.seenSeq = data.seenSeq,
	};
}

SyncLocalConfigState SyncLocalConfigOf(const ConfigSyncState &state) {
	return {
		.base = state.base,
		.baseLineage = state.baseLineage,
		.equiv = state.equiv,
		.pending = state.pending,
		.seenSeq = state.seenSeq,
	};
}

std::vector<ConfigHead> SyncConfigHeadsOf(
		const std::vector<SyncConfigHeadRecord> &records) {
	auto result = std::vector<ConfigHead>();
	result.reserve(records.size());
	for (const auto &record : records) {
		result.push_back(record.head);
	}
	return result;
}

std::vector<QString> SyncConfigVersionKeys(
		const std::vector<ConfigVersion> &versions) {
	auto result = std::vector<QString>();
	result.reserve(versions.size());
	for (const auto &version : versions) {
		result.push_back(version.key);
	}
	return result;
}

bool SameSyncConfigKeySet(std::vector<QString> a, std::vector<QString> b) {
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	a.erase(std::unique(a.begin(), a.end()), a.end());
	b.erase(std::unique(b.begin(), b.end()), b.end());
	return a == b;
}

void SelectSyncSpaceIfEmpty(
		SyncAccountInventoryResult &inventory,
		const QString &space) {
	auto &directory = inventory.directory;
	if (!directory.selectedSpace
		&& directory.canCreateSpace
		&& directory.groups.empty()) {
		directory.selectedSpace = space;
		directory.publishableSpace = space;
	}
}

std::optional<SyncConfigHeadRecord> ParseSyncConfigHead(
		const SyncCandidateRecord &record) {
	const auto parsed = ParseSyncEnvelope(record.bytes);
	if (!parsed || !parsed.header || parsed.header->stream != u"config"_q) {
		return std::nullopt;
	}
	const auto inspected = InspectConfigPayload(parsed);
	if (inspected.status == ConfigPayloadStatus::Invalid
		|| !IsConfigVersionKey(inspected.version.key)) {
		return std::nullopt;
	}
	const auto &header = *parsed.header;
	return SyncConfigHeadRecord{
		.head = {
			.space = header.space,
			.install = header.writerInstall,
			.seq = header.seq,
			.key = inspected.version.key,
			.lineage = inspected.version.lineage,
		},
		.messageId = record.id,
		.text = inspected.text,
		.device = header.writerDevice,
		.platform = header.writerPlatform,
		.app = header.writerApp,
		.at = header.at,
		.newerSchema = (inspected.status == ConfigPayloadStatus::NewerSchema),
	};
}

SyncConfigHeads ExtractSyncConfigHeads(
		const SyncAccountInventoryResult &inventory,
		const QString &space,
		const QString &ownInstall) {
	auto result = SyncConfigHeads();
	if (!inventory.read || space.isEmpty()) {
		return result;
	}
	const auto &records = inventory.read->records;
	for (const auto &group : inventory.directory.groups) {
		if (group.space != space
			|| group.stream != u"config"_q
			|| (!ownInstall.isEmpty() && group.install == ownInstall)) {
			continue;
		}
		if (group.ambiguous
			|| !group.supportedHead
			|| group.headCandidates.empty()) {
			return result;
		}
		const auto candidate = std::min_element(
			group.headCandidates.begin(),
			group.headCandidates.end(),
			[](const auto &a, const auto &b) {
				return a.messageId < b.messageId;
			});
		const auto record = std::find_if(
			records.begin(),
			records.end(),
			[&](const SyncCandidateRecord &record) {
				return record.id == candidate->messageId;
			});
		if (record == records.end()
			|| record->status != SyncCandidateStatus::Valid
			|| !candidate->header
			|| RecordHash(record->bytes) != candidate->recordHash) {
			return result;
		}
		const auto parsed = ParseSyncConfigHead(*record);
		if (!parsed
			|| parsed->head.space != group.space
			|| parsed->head.install != group.install
			|| parsed->head.seq != candidate->header->seq) {
			return result;
		}
		result.heads.push_back(*parsed);
	}
	result.status = SyncConfigHeadsStatus::Complete;
	return result;
}

SyncConfigOwnHead FindSyncConfigOwnHead(
		const SyncAccountInventoryResult &inventory,
		const SyncOwnInventoryResult &own) {
	if (own.status != SyncOwnInventoryStatus::Absent
		&& own.status != SyncOwnInventoryStatus::Present
		&& own.status != SyncOwnInventoryStatus::PendingFound) {
		return {};
	} else if (own.observation.kind == OwnRecordObservationKind::Absent) {
		return { .valid = true };
	} else if (own.observation.kind != OwnRecordObservationKind::Present
		|| !inventory.read) {
		return {};
	}
	const auto &records = inventory.read->records;
	const auto record = std::find_if(
		records.begin(),
		records.end(),
		[&](const SyncCandidateRecord &record) {
			return record.id == own.head.messageId;
		});
	if (record == records.end()
		|| record->status != SyncCandidateStatus::Valid) {
		return {};
	}
	const auto parsed = ParseSyncConfigHead(*record);
	if (!parsed
		|| parsed->newerSchema
		|| parsed->head.seq != own.observation.seq) {
		return {};
	}
	return { .valid = true, .head = parsed };
}

const SyncConfigHeadRecord *FindSyncConfigHeadRecord(
		const SyncConfigReview &review,
		const ConfigHead &head) {
	const auto found = std::find_if(
		review.heads.begin(),
		review.heads.end(),
		[&](const SyncConfigHeadRecord &record) {
			return record.head.install == head.install
				&& record.head.seq == head.seq
				&& record.head.key == head.key;
		});
	return (found != review.heads.end()) ? &*found : nullptr;
}

SyncConfigReview ReviewSyncConfigInventory(
		const SyncAccountInventoryResult &inventory,
		const SyncLocalState *state,
		const QByteArray &stagedRecord,
		const SyncSettingsFile &local) {
	auto result = SyncConfigReview();
	result.accountUserId = inventory.accountUserId;
	result.bound = (state != nullptr);
	result.local = local;
	if (state) {
		result.space = state->space;
		result.state = SyncConfigStateOf(*state);
	}
	if (local.usingLastGood) {
		auto running = local;
		running.usingLastGood = false;
		const auto usual = ReviewSyncConfigInventory(
			inventory,
			state,
			stagedRecord,
			running);
		if (usual.status == SyncConfigReviewStatus::Ready
			&& usual.plan.verdict == ConfigSyncVerdict::Pending) {
			result.plan.verdict = ConfigSyncVerdict::Pending;
		}
		return Refuse(
			std::move(result),
			SyncConfigReviewStatus::UsingLastGood);
	} else if (local.status == SyncSettingsFileStatus::Invalid) {
		return Refuse(
			std::move(result),
			SyncConfigReviewStatus::InvalidSettings);
	}
	const auto inventoryStatus = InventoryStatus(inventory);
	if (inventoryStatus != SyncConfigReviewStatus::Ready) {
		return Refuse(std::move(result), inventoryStatus);
	}
	if (!state) {
		const auto &selected = inventory.directory.selectedSpace;
		if (!selected) {
			result.plan.verdict = ConfigSyncVerdict::Empty;
			result.status = SyncConfigReviewStatus::Ready;
			return result;
		}
		const auto heads = ExtractSyncConfigHeads(
			inventory,
			*selected,
			QString());
		if (heads.status != SyncConfigHeadsStatus::Complete
			|| AnyNewerSchema(heads.heads)) {
			return Refuse(
				std::move(result),
				SyncConfigReviewStatus::NeedsReview);
		}
		result.space = *selected;
		result.state.space = *selected;
		result.heads = heads.heads;
		result.plan = PlanConfigSync(
			local.fingerprint,
			result.state,
			SyncConfigHeadsOf(result.heads));
		result.status = SyncConfigReviewStatus::Ready;
		return result;
	}
	auto scoped = inventory;
	SelectSyncSpaceIfEmpty(scoped, state->space);
	if (!scoped.directory.selectedSpace
		|| *scoped.directory.selectedSpace != state->space) {
		return Refuse(std::move(result), SyncConfigReviewStatus::NeedsReview);
	}
	const auto own = ReconcileOwnConfigInventory(
		*state,
		scoped,
		scoped.accountUserId,
		stagedRecord);
	switch (own.status) {
	case SyncOwnInventoryStatus::Incomplete:
		return Refuse(std::move(result), SyncConfigReviewStatus::Incomplete);
	case SyncOwnInventoryStatus::CloneDetected:
		return Refuse(
			std::move(result),
			SyncConfigReviewStatus::CloneDetected);
	case SyncOwnInventoryStatus::NeedsReview:
		return Refuse(std::move(result), SyncConfigReviewStatus::NeedsReview);
	case SyncOwnInventoryStatus::Absent:
	case SyncOwnInventoryStatus::Present:
	case SyncOwnInventoryStatus::PendingFound:
		break;
	}
	const auto ownHead = FindSyncConfigOwnHead(scoped, own);
	const auto heads = ExtractSyncConfigHeads(
		scoped,
		state->space,
		state->install);
	if (!ownHead.valid
		|| heads.status != SyncConfigHeadsStatus::Complete
		|| AnyNewerSchema(heads.heads)) {
		return Refuse(std::move(result), SyncConfigReviewStatus::NeedsReview);
	}
	result.heads = heads.heads;
	result.ownHead = ownHead.head;
	result.plan = PlanConfigSync(
		local.fingerprint,
		result.state,
		SyncConfigHeadsOf(result.heads),
		ownHead.head
			? std::make_optional(ownHead.head->head)
			: std::nullopt);
	result.status = SyncConfigReviewStatus::Ready;
	return result;
}

SyncConfigRelinkReview ReviewSyncConfigRelink(
		const SyncAccountInventoryResult &inventory,
		const SyncLocalState &state,
		uint64_t expectedAccountUserId,
		const QByteArray &stagedRecord,
		const SyncSettingsFile &local) {
	auto result = SyncConfigRelinkReview();
	if (!expectedAccountUserId
		|| inventory.accountUserId != expectedAccountUserId) {
		result.review.status = SyncConfigReviewStatus::AccountUnavailable;
		return result;
	}
	result.review = ReviewSyncConfigInventory(
		inventory, &state, stagedRecord, local);
	result.review.bound = false;
	if (result.review.status != SyncConfigReviewStatus::Ready) {
		return result;
	}
	auto scoped = inventory;
	SelectSyncSpaceIfEmpty(scoped, state.space);
	const auto own = ReconcileOwnConfigInventory(
		state, scoped, expectedAccountUserId, stagedRecord);
	result.ownStatus = own.status;
	result.proven = own.status == SyncOwnInventoryStatus::Present
		|| own.status == SyncOwnInventoryStatus::PendingFound
		|| (own.status == SyncOwnInventoryStatus::Absent
			&& state.config.confirmedSeq == 0
			&& state.library.confirmedSeq == 0);
	return result;
}

namespace {

[[nodiscard]] bool RecoveryStorePresent(const SyncRecoveryStore &store) {
	return store.statePresent || store.pendingPresent;
}

[[nodiscard]] QByteArray RecoveryStagedConfig(
		const SyncRecoveryStore &store,
		const SyncLocalState &state) {
	const auto name = u"config-%1.json"_q.arg(state.config.pendingSeq);
	const auto found = store.pendingFiles.find(name);
	return (store.pendingPresent && found != store.pendingFiles.end())
		? found->second : QByteArray();
}

[[nodiscard]] bool SafePendingName(const QString &name) {
	return !name.isEmpty() && name != u"."_q && name != u".."_q
		&& std::none_of(name.begin(), name.end(), [](QChar ch) {
			return ch == u'/' || ch == u'\\' || ch.unicode() < 0x20
				|| ch.unicode() == 0x7f;
		});
}

} // namespace

SyncAccountRecoveryPlan PlanSyncAccountRecovery(
		const SyncAccountRecoveryInput &input) {
	auto result = SyncAccountRecoveryPlan();
	result.files = ResolveAccountFiles(input.account, input.startupOwner, input.layout);
	if (!result.files) {
		return result;
	} else if (!input.signedIn) {
		result.status = SyncAccountRecoveryStatus::WaitingForSignIn;
		return result;
	} else if (!input.preferenceToken.isEmpty()
		&& !IsSyncBindingToken(input.preferenceToken)) {
		return result;
	} else if (!input.discoveryComplete) {
		result.status = SyncAccountRecoveryStatus::Incomplete;
		return result;
	}
	auto preferences = std::set<QString>{
		*AccountKeyText(input.account.environment, input.account.userId) };
	auto preferenceTokens = std::set<QByteArray>();
	if (!input.preferenceToken.isEmpty()) {
		preferenceTokens.insert(input.preferenceToken);
	}
	for (const auto &preference : input.otherPreferences) {
		const auto key = AccountKeyText(
			preference.account.environment, preference.account.userId);
		if (!key || (!preference.token.isEmpty()
			&& !IsSyncBindingToken(preference.token))) {
			return result;
		} else if (!preferences.insert(*key).second
			|| (!preference.token.isEmpty()
				&& !preferenceTokens.insert(preference.token).second)) {
			result.status = SyncAccountRecoveryStatus::Ambiguous;
			return result;
		}
	}
	auto states = std::vector<std::optional<SyncLocalState>>();
	auto directories = std::set<QString>();
	for (auto i = size_t(0); i != input.stores.size(); ++i) {
		const auto &store = input.stores[i];
		const auto key = store.directoryAccount
			? AccountKeyText(store.directoryAccount->environment,
				store.directoryAccount->userId)
			: std::make_optional(QString());
		if (!key || (!store.statePresent && !store.state.isEmpty())
			|| (!store.pendingPresent && !store.pendingFiles.empty())
			|| std::any_of(store.pendingFiles.begin(), store.pendingFiles.end(),
				[](const auto &file) { return !SafePendingName(file.first); })) {
			return result;
		} else if (!directories.insert(*key).second) {
			result.status = SyncAccountRecoveryStatus::Ambiguous;
			return result;
		}
		const auto parsed = store.statePresent
			? ParseSyncLocalState(store.state) : SyncLocalParseResult();
		states.push_back(parsed ? std::make_optional(parsed.state) : std::nullopt);
		const auto candidate = store.directoryAccount
			? *store.directoryAccount == input.account
			: input.account == *input.startupOwner;
		if (candidate && RecoveryStorePresent(store)) {
			if (result.store || (store.directoryAccount
				&& input.account == *input.startupOwner)) {
				result.status = SyncAccountRecoveryStatus::Ambiguous;
				result.store.reset();
				return result;
			}
			result.store = i;
		}
	}
	for (auto i = size_t(0); i != states.size(); ++i) {
		if (states[i] && !input.preferenceToken.isEmpty()
			&& states[i]->bindingToken.toLatin1() == input.preferenceToken
			&& (!result.store || *result.store != i)) {
			result.status = SyncAccountRecoveryStatus::Ambiguous;
			return result;
		}
	}
	if (!result.store) {
		result.status = SyncAccountRecoveryStatus::Setup;
		return result;
	}
	const auto &state = states[*result.store];
	if (!state || state->bindingToken.isEmpty()) {
		result.status = SyncAccountRecoveryStatus::StartAgain;
		result.offerStartAgain = true;
		return result;
	}
	for (const auto &preference : input.otherPreferences) {
		if (preference.token == state->bindingToken.toLatin1()) {
			result.status = SyncAccountRecoveryStatus::Ambiguous;
			return result;
		}
	}
	for (auto i = size_t(0); i != states.size(); ++i) {
		if (i != *result.store && states[i]
			&& states[i]->bindingToken == state->bindingToken) {
			result.status = SyncAccountRecoveryStatus::Ambiguous;
			return result;
		}
	}
	if (CheckSyncAccountBinding(*state, input.preferenceToken)
		== SyncAccountBindingVerdict::Bound) {
		result.status = SyncAccountRecoveryStatus::Bound;
		return result;
	}
	if (input.stores[*result.store].directoryAccount) {
		result.status = SyncAccountRecoveryStatus::AutoRelink;
		result.restoreToken = state->bindingToken;
		return result;
	} else if (!input.cloud || !input.cloud->storeSpaceComplete) {
		result.status = SyncAccountRecoveryStatus::Incomplete;
		return result;
	} else if (input.cloud->account != input.account
		|| input.cloud->inventory.accountUserId != uint64_t(input.account.userId)) {
		result.status = SyncAccountRecoveryStatus::Ambiguous;
		return result;
	}
	const auto cloudStatus = InventoryStatus(input.cloud->inventory);
	if (cloudStatus != SyncConfigReviewStatus::Ready) {
		result.status = cloudStatus == SyncConfigReviewStatus::Incomplete
			? SyncAccountRecoveryStatus::Incomplete
			: SyncAccountRecoveryStatus::NeedsReview;
		return result;
	}
	result.proof = ReviewSyncConfigRelink(input.cloud->inventory, *state,
		uint64_t(input.account.userId),
		RecoveryStagedConfig(input.stores[*result.store], *state), input.local);
	if (result.proof->proven) {
		result.status = SyncAccountRecoveryStatus::RootRelink;
		result.restoreToken = state->bindingToken;
	} else if (result.proof->review.status == SyncConfigReviewStatus::UsingLastGood
		|| result.proof->review.status == SyncConfigReviewStatus::InvalidSettings) {
		result.status = SyncAccountRecoveryStatus::NeedsReview;
	} else {
		result.status = SyncAccountRecoveryStatus::StartAgain;
		result.offerStartAgain = true;
	}
	return result;
}

SyncStartAgainPlan PlanSyncStartAgain(const SyncStartAgainInput &input) {
	auto result = SyncStartAgainPlan();
	const auto recovery = PlanSyncAccountRecovery(input.recovery);
	if (!recovery.offerStartAgain || !recovery.store || !recovery.files) {
		return result;
	} else if (input.operationInFlight
		|| input.queue != SyncConfigSendQueue::Empty) {
		result.status = SyncStartAgainStatus::Busy;
		return result;
	} else if (!input.confirmed) {
		result.status = SyncStartAgainStatus::ConfirmationRequired;
		return result;
	}
	const auto &store = input.recovery.stores[*recovery.store];
	if (store.pendingPresent && !store.pendingComplete) {
		result.status = SyncStartAgainStatus::Incomplete;
		return result;
	} else if (!input.archiveAvailable) {
		result.status = SyncStartAgainStatus::ArchiveUnavailable;
		return result;
	}
	const auto format = u"yyyyMMdd-HHmmss"_q;
	const auto date = QDateTime::fromString(input.archiveDate, format);
	if (!date.isValid() || date.toString(format) != input.archiveDate) {
		result.status = SyncStartAgainStatus::InvalidDate;
		return result;
	}
	result.status = SyncStartAgainStatus::Ready;
	result.account = input.recovery.account;
	result.archiveDirectory = recovery.files->syncDirectory
		+ u"/orphaned-"_q + input.archiveDate;
	result.history = recovery.files->history;
	result.expected = store;
	result.preferenceToken = input.recovery.preferenceToken;
	if (store.pendingPresent) {
		result.moves.push_back({ SyncOrphanMoveKind::PendingDirectory,
			recovery.files->syncDirectory + u"/pending"_q,
			result.archiveDirectory + u"/pending"_q });
	}
	if (store.statePresent) {
		result.moves.push_back({ SyncOrphanMoveKind::StateFile,
			recovery.files->syncStore, result.archiveDirectory + u"/state.json"_q });
	}
	return result;
}

SyncStartAgainCompletion CompleteSyncStartAgain(
		const SyncStartAgainPlan &plan,
		const SyncStartAgainObservation &observation,
		const QByteArray &newTokenEntropy) {
	auto result = SyncStartAgainCompletion();
	if (plan.status != SyncStartAgainStatus::Ready || plan.moves.empty()
		|| !AccountKeyText(plan.account.environment, plan.account.userId)) {
		return result;
	} else if (!observation.complete) {
		result.status = SyncStartAgainCompletionStatus::Incomplete;
		return result;
	} else if (observation.account != plan.account
		|| observation.preferenceToken != plan.preferenceToken
		|| observation.originalStatePresent || observation.originalPendingPresent
		|| observation.archivedState.has_value() != plan.expected.statePresent
		|| observation.archivedPending.has_value() != plan.expected.pendingPresent
		|| (observation.archivedState && *observation.archivedState != plan.expected.state)
		|| (observation.archivedPending
			&& *observation.archivedPending != plan.expected.pendingFiles)) {
		result.status = SyncStartAgainCompletionStatus::NeedsRecheck;
		return result;
	}
	result.status = SyncStartAgainCompletionStatus::SetupReady;
	result.binding = PlanSyncSetupBindingToken(
		observation.preferenceToken, newTokenEntropy);
	result.clearAccountUndo = true;
	return result;
}

QString SyncConfigReviewStamp(const SyncConfigReview &review) {
	auto writer = StampWriter();
	writer.add("stamp", QByteArray("purple-config-review-1"));
	writer.add("status", int64_t(review.status));
	writer.add("account", review.accountUserId);
	writer.add("bound", review.bound);
	writer.add("space", review.space);
	writer.add("state-space", review.state.space);
	writer.add("state-install", review.state.install);
	writer.add("local-status", int64_t(review.local.status));
	writer.add("local-fingerprint", review.local.fingerprint);
	writer.add("verdict", int64_t(review.plan.verdict));
	writer.add("own-stale", review.plan.ownStale);
	writer.addKeys("offered", review.plan.offered);
	writer.addKeys("same", review.plan.same);
	writer.add("heads", uint64_t(review.heads.size()));
	for (const auto &record : review.heads) {
		writer.addHead(record.head, record.messageId);
		writer.add("text", RecordHash(record.text));
	}
	writer.add("own", review.ownHead.has_value());
	if (review.ownHead) {
		writer.addHead(review.ownHead->head, review.ownHead->messageId);
	}
	return writer.finish();
}

SyncConfigReview SyncConfigJoinedReview(
		const SyncConfigReview &shown,
		const SyncLocalState &joined) {
	auto result = shown;
	result.bound = true;
	result.state.install = joined.install;
	if (result.space.isEmpty()) {
		result.space = joined.space;
		result.state.space = joined.space;
	}
	return result;
}

SyncConfigApplyPlanStatus CheckSyncConfigApplyChoice(
		const SyncConfigReview &shown,
		const std::optional<QString> &chosenRemoteKey) {
	if (shown.status != SyncConfigReviewStatus::Ready
		|| shown.local.status == SyncSettingsFileStatus::Invalid
		|| (!shown.bound && !shown.state.install.isEmpty())) {
		return SyncConfigApplyPlanStatus::NeedsReview;
	} else if (!PlanConfigChoice(shown.state, shown.plan, chosenRemoteKey)) {
		return SyncConfigApplyPlanStatus::InvalidChoice;
	}
	return SyncConfigApplyPlanStatus::Ready;
}

SyncConfigApplyPlan PlanSyncConfigApply(
		const SyncConfigReview &fresh,
		const QString &expectedStamp,
		const std::optional<QString> &chosenRemoteKey) {
	auto result = SyncConfigApplyPlan();
	if (SyncConfigReviewStamp(fresh) != expectedStamp) {
		result.status = SyncConfigApplyPlanStatus::NeedsRecheck;
		return result;
	}
	const auto checked = CheckSyncConfigApplyChoice(fresh, chosenRemoteKey);
	if (checked != SyncConfigApplyPlanStatus::Ready) {
		result.status = checked;
		return result;
	}
	const auto choice = PlanConfigChoice(
		fresh.state,
		fresh.plan,
		chosenRemoteKey);
	result.join = !fresh.bound;
	result.state = fresh.state;
	result.heads = SyncConfigHeadsOf(fresh.heads);
	result.ownHead = fresh.ownHead
		? std::make_optional(fresh.ownHead->head)
		: std::nullopt;
	result.verdict = fresh.plan.verdict;
	result.choice = *choice;
	result.localFingerprint = fresh.local.fingerprint;
	if (choice->writeRemote) {
		const auto record = std::find_if(
			fresh.heads.begin(),
			fresh.heads.end(),
			[&](const SyncConfigHeadRecord &record) {
				return record.head.key == choice->write.key
					&& record.head.install == choice->write.install
					&& record.head.seq == choice->write.seq;
			});
		if (record == fresh.heads.end()) {
			result.status = SyncConfigApplyPlanStatus::NeedsReview;
			return result;
		}
		const auto &state = fresh.state;
		const auto baseKnown = !state.base.isEmpty()
			&& (SyncConfigKeyFingerprint(state.base)
				== fresh.local.fingerprint);
		const auto written = SyncConfigKeyFingerprint(record->head.key);
		result.source = *record;
		result.writeFingerprint = written;
		result.versionKey = baseKnown ? state.base : QString();
		result.update = (fresh.plan.verdict == ConfigSyncVerdict::UpdateReady);
		result.otherVersionsRemain = std::any_of(
			fresh.plan.offered.begin(),
			fresh.plan.offered.end(),
			[&](const ConfigHead &head) {
				return SyncConfigKeyFingerprint(head.key) != written;
			});
	}
	result.status = SyncConfigApplyPlanStatus::Ready;
	return result;
}

SyncConfigApplyCompletion CompleteSyncConfigApply(
		const SyncConfigApplyPlan &plan,
		const SyncSettingsFile &current) {
	auto result = SyncConfigApplyCompletion();
	if (plan.status != SyncConfigApplyPlanStatus::Ready
		|| plan.join
		|| (plan.choice.writeRemote && !plan.source)) {
		return result;
	}
	if (plan.choice.writeRemote
		? (current.status != SyncSettingsFileStatus::Present
			|| current.text != plan.source->text
			|| current.fingerprint != plan.writeFingerprint)
		: (current.status == SyncSettingsFileStatus::Invalid
			|| current.fingerprint != plan.localFingerprint)) {
		result.status = SyncConfigApplyCompletionStatus::ReadBackMismatch;
		return result;
	}
	result.fingerprint = current.fingerprint;
	auto next = plan.state;
	if (!plan.choice.adopt.empty()) {
		const auto adopted = AdoptConfigHeads(
			plan.state,
			result.fingerprint,
			plan.choice.adopt);
		if (!adopted) {
			result.status = SyncConfigApplyCompletionStatus::AdoptRefused;
			return result;
		}
		next = *adopted;
	}
	for (const auto &head : plan.choice.seen) {
		auto &seen = next.seenSeq[head.install];
		seen = std::max(seen, head.seq);
	}
	if (!plan.choice.adopt.empty() || !plan.choice.seen.empty()) {
		result.adopted = next;
	}
	const auto fresh = PlanConfigSync(
		result.fingerprint,
		next,
		plan.heads,
		plan.ownHead);
	result.nextVerdict = fresh.verdict;
	if (fresh.verdict == ConfigSyncVerdict::Empty
		|| fresh.verdict == ConfigSyncVerdict::LocalChanges
		|| fresh.verdict == ConfigSyncVerdict::Choose
		|| fresh.verdict == ConfigSyncVerdict::Conflict) {
		const auto proposal = PlanConfigChoice(next, fresh, std::nullopt);
		if (proposal && proposal->publish && proposal->adopt.empty()) {
			result.publishNeeded = true;
			result.expectedParents = SyncConfigVersionKeys(proposal->parents);
		}
	}
	result.promiseKept = (plan.verdict == ConfigSyncVerdict::UpdateReady)
		? (fresh.verdict == ConfigSyncVerdict::UpToDate
			|| fresh.verdict == ConfigSyncVerdict::LocalChanges)
		: (!plan.choice.writeRemote
			|| (plan.choice.publish == result.publishNeeded
				&& SameSyncConfigKeySet(
					SyncConfigVersionKeys(plan.choice.parents),
					result.expectedParents)));
	result.status = SyncConfigApplyCompletionStatus::Ready;
	return result;
}

SyncConfigCommitCheck CheckSyncConfigDataCommit(
		const SyncLocalState &current,
		const SyncLocalConfigState &next) {
	auto result = SyncConfigCommitCheck();
	if (current.config.pendingSeq != 0
		|| !current.configData.pending.isEmpty()
		|| !next.pending.isEmpty()
		|| !KeepsSeenSequences(current.configData.seenSeq, next.seenSeq)) {
		result.status = SyncConfigCommitStatus::InvalidTransition;
		return result;
	}
	auto updated = current;
	updated.configData = next;
	const auto serialized = SerializeSyncLocalState(updated);
	if (!serialized) {
		result.status = SyncConfigCommitStatus::InvalidState;
		result.error = serialized.error;
		return result;
	}
	const auto parsed = ParseSyncLocalState(serialized.canonical);
	if (!parsed || !SameConfigData(parsed.state.configData, next)) {
		result.status = SyncConfigCommitStatus::InvalidState;
		result.error = parsed.error;
		return result;
	}
	result.status = SameConfigData(current.configData, next)
		? SyncConfigCommitStatus::Unchanged
		: SyncConfigCommitStatus::Ready;
	result.state = std::move(updated);
	result.canonical = serialized.canonical;
	return result;
}

SyncConfigPublishEntry PlanSyncConfigPublishEntry(
		const SyncConfigPublishRequest &request,
		bool staged) {
	const auto expects = request.expectedFingerprint.has_value()
		|| request.expectedParents.has_value();
	if (request.pendingOnly) {
		return (staged && !expects)
			? SyncConfigPublishEntry::FinishStaged
			: SyncConfigPublishEntry::Refuse;
	}
	return (!staged
			&& request.expectedFingerprint
			&& request.expectedParents)
		? SyncConfigPublishEntry::NewContent
		: SyncConfigPublishEntry::Refuse;
}

SyncConfigPublishGate PlanSyncConfigPublishGate(
		const SyncLocalState &state,
		const SyncAccountInventoryResult &inventory,
		const SyncOwnInventoryResult &own,
		const QString &localFingerprint,
		const SyncConfigPublishRequest &request) {
	auto result = SyncConfigPublishGate();
	const auto entry = PlanSyncConfigPublishEntry(
		request,
		state.config.pendingSeq != 0);
	if (entry != SyncConfigPublishEntry::NewContent
		|| *request.expectedFingerprint != localFingerprint) {
		return result;
	}
	const auto heads = ExtractSyncConfigHeads(
		inventory,
		state.space,
		state.install);
	const auto ownHead = FindSyncConfigOwnHead(inventory, own);
	if (heads.status != SyncConfigHeadsStatus::Complete
		|| AnyNewerSchema(heads.heads)
		|| !ownHead.valid) {
		return result;
	}
	const auto config = SyncConfigStateOf(state);
	result.plan = PlanConfigSync(
		localFingerprint,
		config,
		SyncConfigHeadsOf(heads.heads),
		ownHead.head
			? std::make_optional(ownHead.head->head)
			: std::nullopt);
	switch (result.plan.verdict) {
	case ConfigSyncVerdict::UpToDate:
		result.status = SyncConfigPublishGateStatus::AlreadySynced;
		return result;
	case ConfigSyncVerdict::Empty:
	case ConfigSyncVerdict::LocalChanges:
	case ConfigSyncVerdict::Choose:
	case ConfigSyncVerdict::Conflict:
		break;
	default:
		return result;
	}
	const auto choice = PlanConfigChoice(config, result.plan, std::nullopt);
	if (!choice
		|| !choice->publish
		|| !choice->adopt.empty()
		|| !SameSyncConfigKeySet(
			SyncConfigVersionKeys(choice->parents),
			*request.expectedParents)) {
		return result;
	}
	result.status = SyncConfigPublishGateStatus::Proceed;
	result.parents = choice->parents;
	return result;
}

SyncConfigPostPlan PlanSyncConfigPost(
		const SyncLocalState &state,
		const QByteArray &bindingToken,
		const QByteArray &stagedRecord,
		const SyncAccountInventoryResult &inventory,
		const SyncSettingsFile &local,
		const SyncConfigPublishRequest &request,
		int64_t now,
		const SyncConfigWriter &writer,
		SyncConfigSendQueue queue) {
	const auto entry = PlanSyncConfigPublishEntry(
		request,
		state.config.pendingSeq != 0);
	if (entry == SyncConfigPublishEntry::Refuse) {
		return FinishPost(SyncConfigPublishStatus::NeedsReview);
	}
	auto scoped = inventory;
	SelectSyncSpaceIfEmpty(scoped, state.space);
	const auto own = ReconcileOwnConfigInventory(
		state,
		scoped,
		scoped.accountUserId,
		stagedRecord);
	if (own.status == SyncOwnInventoryStatus::PendingFound) {
		if (!own.pendingMessageId || !inventory.read) {
			return FinishPost(SyncConfigPublishStatus::NeedsReview);
		}
		const auto &records = inventory.read->records;
		const auto found = std::find_if(
			records.begin(),
			records.end(),
			[&](const SyncCandidateRecord &record) {
				return record.id == *own.pendingMessageId
					&& record.status == SyncCandidateStatus::Valid
					&& record.bytes == stagedRecord;
			});
		if (found == records.end()) {
			return FinishPost(SyncConfigPublishStatus::NeedsReview);
		}
		auto result = SyncConfigPostPlan();
		result.step = SyncConfigPostStep::ConfirmFound;
		result.messageId = found->id;
		result.record = found->bytes;
		result.own = own;
		return result;
	}
	if (own.status != SyncOwnInventoryStatus::Absent
		&& own.status != SyncOwnInventoryStatus::Present) {
		return FinishPost(OwnFailure(own.status));
	}
	if (entry == SyncConfigPublishEntry::FinishStaged) {
		return stagedRecord.isEmpty()
			? FinishPost(SyncConfigPublishStatus::NeedsReview)
			: (queue == SyncConfigSendQueue::HoldsSyncRecord)
			? FinishPost(SyncConfigPublishStatus::StillSending)
			: PlanSyncConfigStagedPost(state, bindingToken, own, stagedRecord);
	}
	if (local.status != SyncSettingsFileStatus::Present
		|| local.text.isEmpty()
		|| local.usingLastGood) {
		return FinishPost(SyncConfigPublishStatus::InvalidSettings);
	}
	const auto gate = PlanSyncConfigPublishGate(
		state,
		scoped,
		own,
		local.fingerprint,
		request);
	if (gate.status == SyncConfigPublishGateStatus::AlreadySynced) {
		return FinishPost(SyncConfigPublishStatus::AlreadySynced);
	} else if (gate.status != SyncConfigPublishGateStatus::Proceed) {
		return FinishPost(SyncConfigPublishStatus::NeedsReview);
	}
	if (state.config.seq == std::numeric_limits<uint64_t>::max()
		|| now <= 0) {
		return FinishPost(SyncConfigPublishStatus::NeedsReview);
	}
	const auto built = BuildConfigRecord({
		.text = local.text,
		.parents = gate.parents,
		.space = state.space,
		.install = state.install,
		.device = state.createdDevice,
		.platform = writer.platform,
		.app = writer.app,
		.seq = state.config.seq + 1,
		.at = uint64_t(now),
	});
	if (!built || built.canonical.size() > kSyncConfigRecordMaximumBytes) {
		return FinishPost(SyncConfigPublishStatus::InvalidSettings);
	}
	auto policy = SyncPublishPolicy();
	policy.enabled = true;
	auto observations = SyncPublishObservations();
	observations.ready = true;
	observations.discoveryComplete = true;
	observations.ownRecord = own.observation;
	observations.ownHead = own.head;
	const auto plan = PlanSyncPublish(
		state,
		bindingToken,
		state.createdDevice,
		SyncLocalStream::Config,
		built.payloadHash,
		policy,
		observations);
	if (plan.action != SyncPublishAction::ReserveAndStage) {
		return FinishPost(SyncConfigPublishStatus::NeedsReview);
	}
	auto result = SyncConfigPostPlan();
	result.step = SyncConfigPostStep::Stage;
	result.record = built.canonical;
	result.nextConfigData = state.configData;
	result.nextConfigData.pending = built.version.key;
	result.own = own;
	return result;
}

SyncConfigPostPlan PlanSyncConfigStagedPost(
		const SyncLocalState &state,
		const QByteArray &bindingToken,
		const SyncOwnInventoryResult &own,
		const QByteArray &stagedRecord) {
	auto policy = SyncPublishPolicy();
	policy.enabled = true;
	policy.editEnabled = false;
	auto observations = SyncPublishObservations();
	observations.discoveryComplete = true;
	observations.ready = true;
	observations.attempt = SyncPublishAttempt::ReconciledAbsent;
	observations.stagedRecordMatches = true;
	observations.stagedRecordHash = RecordHash(stagedRecord);
	observations.ownRecord = own.observation;
	observations.ownHead = own.head;
	const auto plan = PlanSyncPublish(
		state,
		bindingToken,
		state.createdDevice,
		SyncLocalStream::Config,
		state.config.ownHash,
		policy,
		observations);
	if (plan.action != SyncPublishAction::Post) {
		return FinishPost(SyncConfigPublishStatus::NeedsReview);
	}
	auto result = SyncConfigPostPlan();
	result.step = SyncConfigPostStep::Post;
	result.record = stagedRecord;
	result.own = own;
	return result;
}

} // namespace Purple
