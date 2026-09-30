/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/

// Tests for the settings sync flow shared by the clients: review, head
// extraction, the review stamp, apply planning and completion, the commit
// check, the post planners and the describe enums.
//
// A Device below keeps its sync state, staged record and settings.toml in
// memory, and stages, confirms and commits through the same core calls the
// clients' stores make before they write files. A Cloud is one account's
// Saved Messages; its inventory goes through the core's classification and
// FinishSyncAccountInventory, as a real check does. The flows are the pure
// part of the desktop harness that proved the desktop flow before it moved
// here; History, file writes and Telegram posting stay client tests.

#include "purple/purple_config_payload.h"
#include "purple/purple_config_sync.h"
#include "purple/purple_state.h"
#include "purple/purple_sync_config_describe.h"
#include "purple/purple_sync_config_flow.h"
#include "purple/purple_sync_envelope.h"
#include "purple/purple_sync_inventory.h"
#include "purple/purple_sync_local_state.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <cstdio>
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

using namespace Purple;

namespace {

auto Checks = 0;
auto Failures = 0;
const char *Section = "";

void Report(bool ok, const char *what, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("  FAIL  %s:%d  %s\n", Section, line, what);
	}
}

#define CHECK(cond) Report((cond), #cond, __LINE__)

void Begin(const char *name) {
	Section = name;
	if (qEnvironmentVariableIsSet("PURPLE_TEST_TRACE")) {
		std::printf("== %s\n", name);
		std::fflush(stdout);
	}
}

constexpr auto kUserId = uint64_t(777);

const auto T0 = QByteArray("version = 1\n# first\n");
const auto T1 = QByteArray("version = 1\n# local edit\n");
const auto TB = QByteArray("version = 1\n# from b\n");
const auto TB2 = QByteArray("version = 1\n# from b, second\n");
const auto TB3 = QByteArray("version = 1\n# from b, third\n");
const auto TL = QByteArray("version = 1\n# local conflict\n");
const auto TB4 = QByteArray("version = 1\n# from b, fourth\n");
const auto TB5 = QByteArray("version = 1\n# from b, fifth\n");
const auto TC = QByteArray("version = 1\n# from c\n");
const auto TB6 = QByteArray("version = 1\n# from b, sixth\n");
const auto TC2 = QByteArray("version = 1\n# from c, second\n");

struct Cloud {
	std::vector<std::pair<int32_t, QByteArray>> records;
	int32_t nextId = 100;
	bool scanComplete = true;

	int32_t add(const QByteArray &bytes) {
		records.push_back({ nextId, bytes });
		return nextId++;
	}
	[[nodiscard]] SyncAccountInventoryResult inventory() const {
		auto scan = SyncHistoryScanResult();
		scan.status = scanComplete
			? SyncHistoryScanStatus::Complete
			: SyncHistoryScanStatus::RequestFailed;
		auto read = SyncCandidateReadResult();
		for (const auto &[id, bytes] : records) {
			auto record = ClassifySyncCandidate(id, bytes);
			record.documentId = uint64_t(id) + 5000;
			record.editDate = 1700000000;
			scan.candidateIds.push_back(id);
			read.records.push_back(std::move(record));
		}
		scan.scannedCount = records.size();
		read.status = AggregateSyncCandidateRead(read.records);
		return FinishSyncAccountInventory(kUserId, scan, read);
	}
};

struct Remote {
	QString install;
	QString device;
	QString platform;
	uint64_t seq = 0;
};

Remote MakeRemote(char seed, const QString &platform) {
	auto result = Remote();
	result.install = *FormatSyncInstallId(QByteArray(16, seed));
	result.device = platform.toLower() + u":"_q + result.install;
	result.platform = platform;
	return result;
}

ConfigVersion VersionOfRecord(const QByteArray &bytes) {
	return InspectConfigPayload(ParseSyncEnvelope(bytes)).version;
}

QByteArray BuildRemote(
		Remote &remote,
		const QString &space,
		const QByteArray &text,
		const std::vector<ConfigVersion> &parents) {
	const auto built = BuildConfigRecord({
		.text = text,
		.parents = parents,
		.space = space,
		.install = remote.install,
		.device = remote.device,
		.platform = remote.platform,
		.app = u"Harness"_q,
		.seq = ++remote.seq,
		.at = 1700000000 + remote.seq,
	});
	CHECK(bool(built));
	return built.canonical;
}

ConfigVersion Post(
		Cloud &cloud,
		Remote &remote,
		const QString &space,
		const QByteArray &text,
		const std::vector<ConfigVersion> &parents = {}) {
	const auto bytes = BuildRemote(remote, space, text, parents);
	cloud.add(bytes);
	return VersionOfRecord(bytes);
}

std::vector<QString> Keys(const std::vector<ConfigHead> &heads) {
	auto result = std::vector<QString>();
	for (const auto &head : heads) {
		result.push_back(head.key);
	}
	return result;
}

bool SameSet(std::vector<QString> a, std::vector<QString> b) {
	return SameSyncConfigKeySet(std::move(a), std::move(b));
}

SyncConfigWriter DesktopWriter() {
	return { .platform = u"macOS"_q, .app = u"Purple Telegram Desktop"_q };
}

struct Device {
	explicit Device(char seed) : seed(seed) {
	}

	char seed = 'a';
	std::optional<SyncLocalState> state;
	QByteArray staged;
	SyncSettingsFile local = MakeSyncSettingsFile(
		SyncSettingsFileStatus::Absent);
	SyncConfigWriter writer = DesktopWriter();
	SyncConfigSendQueue queue = SyncConfigSendQueue::Empty;
	int64_t now = 1800000000;

	void setLocal(const QByteArray &text) {
		local = MakeSyncSettingsFile(SyncSettingsFileStatus::Present, text);
	}
	void removeLocal() {
		local = MakeSyncSettingsFile(SyncSettingsFileStatus::Absent);
	}
	[[nodiscard]] QByteArray token() const {
		return state ? state->bindingToken.toLatin1() : QByteArray();
	}
	[[nodiscard]] QByteArray stateBytes() const {
		return state ? SerializeSyncLocalState(*state).canonical : QByteArray();
	}
};

SyncConfigReview Review(
		const Device &device,
		const SyncAccountInventoryResult &inventory) {
	return ReviewSyncConfigInventory(
		inventory,
		device.state ? &*device.state : nullptr,
		device.staged,
		device.local);
}

SyncConfigReview Review(const Device &device, const Cloud &cloud) {
	return Review(device, cloud.inventory());
}

bool Join(Device &device, const SyncAccountInventoryResult &inventory) {
	if (device.state) {
		return false;
	}
	const auto &directory = inventory.directory;
	auto state = SyncLocalState();
	state.install = *FormatSyncInstallId(QByteArray(16, device.seed));
	state.createdDevice = u"desktop:"_q + state.install;
	state.space = directory.selectedSpace
		? *directory.selectedSpace
		: *FormatTimeOrderedSyncSpaceId(
			1800000000000ULL + uint8_t(device.seed),
			QByteArray(10, device.seed));
	state.bindingToken = *FormatSyncBindingToken(
		QByteArray(16, device.seed));
	device.state = state;
	return true;
}

bool Stage(
		Device &device,
		const QByteArray &record,
		const SyncLocalConfigState &next) {
	const auto parsed = ParseSyncEnvelope(record);
	const auto hash = parsed.envelope.document.value(
		u"payload_sha256"_q).toString();
	const auto reserved = ReserveSyncSeq(
		*device.state,
		SyncLocalStream::Config,
		hash);
	if (!reserved) {
		return false;
	}
	auto state = reserved.state;
	state.configData = next;
	const auto issued = AppendIssuedConfigRecord(state, record);
	if (!issued) {
		return false;
	}
	device.state = issued.state;
	device.staged = record;
	return true;
}

bool Confirm(Device &device, const QByteArray &bytes, int32_t messageId) {
	if (!device.state
		|| !device.state->config.pendingSeq
		|| bytes != device.staged) {
		return false;
	}
	const auto parsed = ParseSyncEnvelope(bytes);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid) {
		return false;
	}
	const auto &document = parsed.envelope.document;
	const auto observation = OwnRecordObservation{
		OwnRecordObservationKind::Present,
		uint64_t(document.value(u"seq"_q).toDouble()),
		document.value(u"payload_sha256"_q).toString(),
	};
	const auto confirmation = ConfirmConfigReadBack(
		*device.state,
		device.state->createdDevice,
		observation,
		inspected.version);
	if (confirmation.verdict != SyncCloneVerdict::NoClone
		|| !confirmation.changed) {
		return false;
	}
	const auto recorded = RecordConfirmedOwnConfigMessage(
		confirmation.state,
		messageId,
		bytes);
	if (!recorded) {
		return false;
	}
	device.state = recorded.state;
	device.staged.clear();
	return true;
}

enum class PostMode {
	Confirm,
	LoseReceipt,
	Fail,
};

struct PublishRun {
	SyncConfigPublishStatus status = SyncConfigPublishStatus::Incomplete;
	int32_t messageId = 0;
	int posts = 0;
	QByteArray posted;
	ConfigVersion version;
};

PublishRun Publish(
		Device &device,
		Cloud &cloud,
		const SyncAccountInventoryResult &inventory,
		const SyncConfigPublishRequest &request,
		PostMode mode = PostMode::Confirm) {
	using Status = SyncConfigPublishStatus;
	auto run = PublishRun();
	if (!device.state) {
		run.status = Status::AccountUnbound;
		return run;
	}
	const auto post = [&](const QByteArray &bytes) {
		++run.posts;
		run.posted = bytes;
		run.version = VersionOfRecord(bytes);
		if (mode == PostMode::Fail) {
			run.status = Status::OutcomeUnknown;
			return;
		}
		const auto id = cloud.add(bytes);
		if (mode == PostMode::LoseReceipt) {
			run.status = Status::OutcomeUnknown;
			return;
		}
		run.messageId = id;
		run.status = Confirm(device, bytes, id)
			? Status::Confirmed
			: Status::StoreError;
	};
	const auto plan = PlanSyncConfigPost(
		*device.state,
		device.token(),
		device.staged,
		inventory,
		device.local,
		request,
		device.now,
		device.writer,
		device.queue);
	switch (plan.step) {
	case SyncConfigPostStep::Finish:
		run.status = plan.status;
		break;
	case SyncConfigPostStep::ConfirmFound:
		run.messageId = plan.messageId;
		run.version = VersionOfRecord(plan.record);
		run.status = Confirm(device, plan.record, plan.messageId)
			? Status::Confirmed
			: Status::StoreError;
		break;
	case SyncConfigPostStep::Stage: {
		if (!Stage(device, plan.record, plan.nextConfigData)) {
			run.status = Status::StoreError;
			break;
		}
		const auto staged = PlanSyncConfigStagedPost(
			*device.state,
			device.token(),
			plan.own,
			device.staged);
		auto scoped = inventory;
		SelectSyncSpaceIfEmpty(scoped, device.state->space);
		const auto stateless = PlanSyncConfigStagedPost(
			*device.state,
			device.token(),
			ReconcileOwnConfigInventory(
				*device.state,
				scoped,
				scoped.accountUserId,
				device.staged),
			device.staged);
		CHECK(stateless.step == staged.step);
		CHECK(stateless.status == staged.status);
		CHECK(stateless.record == staged.record);
		if (staged.step != SyncConfigPostStep::Post) {
			run.status = staged.status;
			break;
		}
		post(staged.record);
	} break;
	case SyncConfigPostStep::Post:
		post(plan.record);
		break;
	}
	return run;
}

PublishRun Publish(
		Device &device,
		Cloud &cloud,
		const SyncConfigPublishRequest &request,
		PostMode mode = PostMode::Confirm) {
	const auto inventory = cloud.inventory();
	return Publish(device, cloud, inventory, request, mode);
}

SyncConfigPublishRequest LocalRequest(
		const Device &device,
		const Cloud &cloud) {
	const auto review = Review(device, cloud);
	auto request = SyncConfigPublishRequest{
		.expectedFingerprint = review.local.fingerprint,
	};
	if (review.status != SyncConfigReviewStatus::Ready) {
		request.expectedParents = std::vector<QString>();
		return request;
	}
	switch (review.plan.verdict) {
	case ConfigSyncVerdict::UpToDate:
		request.expectedParents = std::vector<QString>();
		break;
	case ConfigSyncVerdict::Empty:
	case ConfigSyncVerdict::LocalChanges:
		if (const auto choice = PlanConfigChoice(
				review.state,
				review.plan,
				std::nullopt)) {
			request.expectedParents = SyncConfigVersionKeys(choice->parents);
		}
		break;
	default:
		break;
	}
	return request;
}

PublishRun PublishLocal(Device &device, Cloud &cloud) {
	return Publish(device, cloud, LocalRequest(device, cloud));
}

PublishRun PublishOwn(
		Device &device,
		Cloud &cloud,
		std::optional<QString> expectedFingerprint,
		std::optional<std::vector<QString>> expectedParents) {
	return Publish(device, cloud, SyncConfigPublishRequest{
		.expectedFingerprint = std::move(expectedFingerprint),
		.expectedParents = std::move(expectedParents),
	});
}

SyncConfigPublishGateStatus Gate(
		const Device &device,
		const Cloud &cloud,
		const SyncConfigPublishRequest &request) {
	auto inventory = cloud.inventory();
	SelectSyncSpaceIfEmpty(inventory, device.state->space);
	const auto own = ReconcileOwnConfigInventory(
		*device.state,
		inventory,
		kUserId,
		{});
	return PlanSyncConfigPublishGate(
		*device.state,
		inventory,
		own,
		device.local.fingerprint,
		request).status;
}

struct ApplyRun {
	SyncConfigApplyStatus status = SyncConfigApplyStatus::NeedsReview;
	bool joined = false;
	bool wroteFile = false;
	bool adopted = false;
	bool publishNeeded = false;
	bool otherVersionsRemain = false;
	bool promiseKept = true;
	bool update = false;
	ConfigSyncVerdict nextVerdict = ConfigSyncVerdict::Invalid;
	QString fingerprint;
	QString versionKey;
	std::vector<QString> expectedParents;
	std::optional<SyncConfigHeadRecord> source;
};

SyncConfigApplyStatus StatusOf(SyncConfigApplyPlanStatus status) {
	switch (status) {
	case SyncConfigApplyPlanStatus::Ready:
		return SyncConfigApplyStatus::Applied;
	case SyncConfigApplyPlanStatus::NeedsRecheck:
		return SyncConfigApplyStatus::NeedsRecheck;
	case SyncConfigApplyPlanStatus::NeedsReview:
		return SyncConfigApplyStatus::NeedsReview;
	case SyncConfigApplyPlanStatus::InvalidChoice:
		return SyncConfigApplyStatus::InvalidChoice;
	}
	return SyncConfigApplyStatus::NeedsReview;
}

ApplyRun Apply(
		Device &device,
		const SyncAccountInventoryResult &inventory,
		const SyncConfigReview &shown,
		const std::optional<QString> &key) {
	auto run = ApplyRun();
	const auto checked = CheckSyncConfigApplyChoice(shown, key);
	if (checked != SyncConfigApplyPlanStatus::Ready) {
		run.status = StatusOf(checked);
		return run;
	}
	const auto stamp = SyncConfigReviewStamp(shown);
	if (!shown.bound) {
		const auto before = PlanSyncConfigApply(
			ReviewSyncConfigInventory(inventory, nullptr, {}, device.local),
			stamp,
			key);
		if (before.status != SyncConfigApplyPlanStatus::Ready) {
			run.status = StatusOf(before.status);
			return run;
		} else if (!before.join || !Join(device, inventory)) {
			run.status = SyncConfigApplyStatus::NeedsRecheck;
			return run;
		}
		run.joined = true;
	}
	const auto fresh = Review(device, inventory);
	const auto expected = shown.bound
		? stamp
		: SyncConfigReviewStamp(SyncConfigJoinedReview(shown, *device.state));
	const auto plan = PlanSyncConfigApply(fresh, expected, key);
	if (plan.status != SyncConfigApplyPlanStatus::Ready || plan.join) {
		run.status = plan.join
			? SyncConfigApplyStatus::NeedsRecheck
			: StatusOf(plan.status);
		return run;
	}
	run.update = plan.update;
	run.versionKey = plan.versionKey;
	run.source = plan.source;
	run.otherVersionsRemain = plan.otherVersionsRemain;
	if (plan.choice.writeRemote) {
		if (!SyncSettingsTextWritable(plan.source->text)) {
			run.status = SyncConfigApplyStatus::WriteError;
			return run;
		}
		device.setLocal(plan.source->text);
		run.wroteFile = true;
	}
	const auto completion = CompleteSyncConfigApply(plan, device.local);
	switch (completion.status) {
	case SyncConfigApplyCompletionStatus::Ready:
		break;
	case SyncConfigApplyCompletionStatus::ReadBackMismatch:
		run.status = SyncConfigApplyStatus::WriteError;
		return run;
	case SyncConfigApplyCompletionStatus::InvalidPlan:
	case SyncConfigApplyCompletionStatus::AdoptRefused:
		run.status = SyncConfigApplyStatus::NeedsReview;
		return run;
	}
	if (completion.adopted) {
		const auto commit = CheckSyncConfigDataCommit(
			*device.state,
			SyncLocalConfigOf(*completion.adopted));
		if (commit.status == SyncConfigCommitStatus::Ready) {
			device.state = commit.state;
		} else if (commit.status != SyncConfigCommitStatus::Unchanged) {
			run.status = SyncConfigApplyStatus::StoreError;
			return run;
		}
		run.adopted = true;
	}
	run.fingerprint = completion.fingerprint;
	run.nextVerdict = completion.nextVerdict;
	run.publishNeeded = completion.publishNeeded;
	run.expectedParents = completion.expectedParents;
	run.promiseKept = completion.promiseKept;
	run.status = SyncConfigApplyStatus::Applied;
	return run;
}

ApplyRun Apply(
		Device &device,
		const Cloud &cloud,
		const SyncConfigReview &shown,
		const std::optional<QString> &key) {
	return Apply(device, cloud.inventory(), shown, key);
}

SyncConfigAction ActionOf(const SyncConfigReview &review) {
	return DescribeSyncConfigReview(
		review,
		SyncSettingsPublishable(
			review.local,
			u"desktop:x"_q,
			DesktopWriter())).action;
}

SyncConfigMessage MessageOf(const SyncConfigReview &review) {
	return DescribeSyncConfigReview(
		review,
		SyncSettingsPublishable(
			review.local,
			u"desktop:x"_q,
			DesktopWriter())).message;
}

void TestFlowBasics() {
	Begin("sync flow basics");
	using File = SyncSettingsFileStatus;
	const auto present = MakeSyncSettingsFile(File::Present, T0);
	CHECK(present.status == File::Present);
	CHECK(present.text == T0);
	CHECK(present.fingerprint == SettingsFingerprint(T0));
	const auto absent = MakeSyncSettingsFile(File::Absent, T0);
	CHECK(absent.status == File::Absent);
	CHECK(absent.text.isEmpty());
	CHECK(absent.fingerprint == SettingsFingerprint(QByteArray()));
	const auto empty = MakeSyncSettingsFile(File::Present, QByteArray());
	CHECK(empty.status == File::Present);
	CHECK(empty.fingerprint == absent.fingerprint);
	CHECK(!SameSyncSettingsFile(empty, absent));
	const auto invalid = MakeSyncSettingsFile(File::Invalid, T0);
	CHECK(invalid.status == File::Invalid);
	CHECK(invalid.text.isEmpty() && invalid.fingerprint.isEmpty());
	CHECK(!SameSyncSettingsFile(invalid, invalid));
	CHECK(SameSyncSettingsFile(present, present));
	CHECK(SameSyncSettingsFile(absent, absent));
	CHECK(!SameSyncSettingsFile(
		present,
		MakeSyncSettingsFile(File::Present, T1)));
	const auto largest = QByteArray(kSyncSettingsMaximumBytes, '#');
	CHECK(MakeSyncSettingsFile(File::Present, largest).status
		== File::Present);
	CHECK(MakeSyncSettingsFile(File::Present, largest + '#').status
		== File::Invalid);

	CHECK(SyncSettingsTextWritable(T0));
	CHECK(SyncSettingsTextWritable(QByteArray()));
	CHECK(!SyncSettingsTextWritable(
		QByteArray("version = 1\n# \xff\xfe bad\n")));

	const auto version = MakeConfigVersion(T0, {});
	CHECK(version.has_value());
	CHECK(version && SyncConfigKeyFingerprint(version->key)
		== ParseConfigVersionKey(version->key)->fingerprint);
	CHECK(SyncConfigKeyFingerprint(u"garbage"_q).isEmpty());

	CHECK(SameSyncConfigKeySet({ u"a"_q, u"b"_q }, { u"b"_q, u"a"_q }));
	CHECK(SameSyncConfigKeySet({ u"a"_q, u"a"_q }, { u"a"_q }));
	CHECK(SameSyncConfigKeySet({}, {}));
	CHECK(!SameSyncConfigKeySet({ u"a"_q }, { u"a"_q, u"b"_q }));
	CHECK(SyncConfigVersionKeys({ { .key = u"k"_q } })
		== std::vector<QString>{ u"k"_q });

	auto resumed = SyncHistoryPages(98);
	CHECK(resumed.offset() == 98);
	CHECK(resumed.count() == 0);
	CHECK(resumed.Add({ { 98, true } }) == SyncHistoryPageStatus::Stalled);
	CHECK(resumed.Add({ { 97, true }, { 90, false } })
		== SyncHistoryPageStatus::More);
	CHECK(resumed.offset() == 90);
	CHECK(resumed.candidates() == std::vector<int32_t>{ 97 });
	CHECK(SyncHistoryPages(0).offset() == 0);
	CHECK(SyncHistoryPages(0).Add({ { 5, true } })
		== SyncHistoryPageStatus::More);
}

void TestFlowHeads() {
	Begin("sync flow heads");
	const auto space = *FormatSyncSpaceId(QByteArray(16, 's'));
	auto cloud = Cloud();
	auto b = MakeRemote('b', u"Android"_q);
	auto c = MakeRemote('c', u"Windows"_q);
	const auto b1 = Post(cloud, b, space, TB);
	const auto b2 = Post(cloud, b, space, TB2, { b1 });
	const auto c1 = Post(cloud, c, space, TC);
	const auto inventory = cloud.inventory();
	CHECK(inventory.status == SyncAccountInventoryStatus::Complete);
	const auto all = ExtractSyncConfigHeads(inventory, space, QString());
	CHECK(all.status == SyncConfigHeadsStatus::Complete);
	CHECK(all.heads.size() == 2);
	auto foundB = false;
	auto foundC = false;
	for (const auto &head : all.heads) {
		if (head.head.install == b.install) {
			foundB = true;
			CHECK(head.head.seq == 2);
			CHECK(head.head.key == b2.key);
			CHECK(head.head.lineage == b2.lineage);
			CHECK(head.text == TB2);
			CHECK(head.device == b.device);
			CHECK(head.platform == u"Android"_q);
			CHECK(head.app == u"Harness"_q);
			CHECK(head.at == 1700000002);
			CHECK(!head.newerSchema);
			CHECK(head.messageId == 101);
		} else if (head.head.install == c.install) {
			foundC = true;
			CHECK(head.head.key == c1.key);
		}
	}
	CHECK(foundB && foundC);
	const auto skipped = ExtractSyncConfigHeads(inventory, space, b.install);
	CHECK(skipped.status == SyncConfigHeadsStatus::Complete);
	CHECK(skipped.heads.size() == 1);
	CHECK(!skipped.heads.empty()
		&& skipped.heads[0].head.install == c.install);
	const auto otherSpace = ExtractSyncConfigHeads(
		inventory,
		*FormatSyncSpaceId(QByteArray(16, 't')),
		QString());
	CHECK(otherSpace.status == SyncConfigHeadsStatus::Complete);
	CHECK(otherSpace.heads.empty());
	CHECK(ExtractSyncConfigHeads(inventory, QString(), QString()).status
		== SyncConfigHeadsStatus::NeedsReview);

	auto missing = inventory;
	missing.read->records.erase(missing.read->records.begin() + 1);
	CHECK(ExtractSyncConfigHeads(missing, space, QString()).status
		== SyncConfigHeadsStatus::NeedsReview);
	auto unread = inventory;
	unread.read.reset();
	CHECK(ExtractSyncConfigHeads(unread, space, QString()).status
		== SyncConfigHeadsStatus::NeedsReview);

	auto ambiguousCloud = cloud;
	auto forked = b;
	forked.seq = 1;
	ambiguousCloud.add(BuildRemote(forked, space, TB3, { b1 }));
	const auto ambiguous = ambiguousCloud.inventory();
	CHECK(ExtractSyncConfigHeads(ambiguous, space, QString()).status
		== SyncConfigHeadsStatus::NeedsReview);
	CHECK(ambiguous.status == SyncAccountInventoryStatus::NeedsReview);

	auto tampered = inventory;
	for (auto &record : tampered.read->records) {
		if (record.id == 101) {
			record.bytes = BuildRemote(forked, space, TB3, { b1 });
		}
	}
	CHECK(ExtractSyncConfigHeads(tampered, space, QString()).status
		== SyncConfigHeadsStatus::NeedsReview);

	CHECK(ParseSyncConfigHead(
		ClassifySyncCandidate(900, cloud.records[0].second)).has_value());
	CHECK(!ParseSyncConfigHead(
		ClassifySyncCandidate(901, QByteArray("{\"not\":\"a record\"}")))
			.has_value());

	auto device = Device('a');
	device.setLocal(T0);
	const auto review = Review(device, cloud);
	CHECK(review.status == SyncConfigReviewStatus::Ready);
	CHECK(!review.bound);
	CHECK(review.accountUserId == kUserId);
	CHECK(review.space == space);
	CHECK(review.state.space == space);
	CHECK(review.state.install.isEmpty());
	CHECK(review.plan.verdict == ConfigSyncVerdict::Conflict);
	CHECK(review.plan.offered.size() == 2);
	CHECK(review.local.status == SyncSettingsFileStatus::Present);
	CHECK(review.local.fingerprint == SettingsFingerprint(T0));
	CHECK(review.heads.size() == 2);
	CHECK(!review.ownHead.has_value());
	for (const auto &head : review.plan.offered) {
		const auto record = FindSyncConfigHeadRecord(review, head);
		CHECK(record && record->head.key == head.key);
	}
	CHECK(!FindSyncConfigHeadRecord(review, ConfigHead{ .key = b1.key }));

	CHECK(Review(device, ambiguous).status
		== SyncConfigReviewStatus::NeedsReview);
	device.local = MakeSyncSettingsFile(SyncSettingsFileStatus::Invalid);
	CHECK(Review(device, cloud).status
		== SyncConfigReviewStatus::InvalidSettings);
	CHECK(Review(device, cloud).local.status
		== SyncSettingsFileStatus::Invalid);
	device.removeLocal();
	const auto absent = Review(device, cloud);
	CHECK(absent.status == SyncConfigReviewStatus::Ready);
	CHECK(absent.local.status == SyncSettingsFileStatus::Absent);
	CHECK(absent.local.fingerprint == SettingsFingerprint(QByteArray()));
}

void TestFlowEmptyJoinAndLocalChanges() {
	Begin("sync flow empty, join and local changes");
	using Status = SyncConfigPublishStatus;
	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	const auto review = Review(device, cloud);
	CHECK(review.status == SyncConfigReviewStatus::Ready);
	CHECK(!review.bound);
	CHECK(review.space.isEmpty());
	CHECK(review.plan.verdict == ConfigSyncVerdict::Empty);
	CHECK(MessageOf(review) == SyncConfigMessage::EmptyUnbound);
	CHECK(ActionOf(review) == SyncConfigAction::Publish);
	CHECK(Apply(device, cloud, review, u"1.x"_q).status
		== SyncConfigApplyStatus::InvalidChoice);
	CHECK(!device.state);

	const auto applied = Apply(device, cloud, review, std::nullopt);
	CHECK(applied.status == SyncConfigApplyStatus::Applied);
	CHECK(applied.joined);
	CHECK(!applied.wroteFile);
	CHECK(!applied.adopted);
	CHECK(applied.publishNeeded);
	CHECK(applied.expectedParents.empty());
	CHECK(applied.nextVerdict == ConfigSyncVerdict::Empty);
	CHECK(applied.fingerprint == SettingsFingerprint(T0));
	CHECK(applied.promiseKept);
	CHECK(device.state && !device.state->install.isEmpty());
	CHECK(device.state && device.state->configData.base.isEmpty());
	const auto joinedEmpty = Review(device, cloud);
	CHECK(joinedEmpty.bound);
	CHECK(MessageOf(joinedEmpty) == SyncConfigMessage::EmptyBound);

	CHECK(PublishOwn(
		device,
		cloud,
		std::nullopt,
		std::vector<QString>{ u"1.x"_q }).status == Status::NeedsReview);
	CHECK(PublishOwn(
		device,
		cloud,
		applied.fingerprint,
		std::vector<QString>{ u"1.x"_q }).status == Status::NeedsReview);
	CHECK(PublishOwn(device, cloud, applied.fingerprint, std::nullopt)
		.status == Status::NeedsReview);
	CHECK(cloud.records.empty());
	const auto first = PublishOwn(
		device,
		cloud,
		applied.fingerprint,
		applied.expectedParents);
	CHECK(first.status == Status::Confirmed);
	CHECK(first.posts == 1);
	CHECK(first.version.parents.empty());
	CHECK(first.version.key == MakeConfigVersion(T0, {})->key);
	CHECK(device.state->configData.base == first.version.key);
	CHECK(device.state->config.pendingSeq == 0);
	CHECK(device.staged.isEmpty());

	const auto upToDate = Review(device, cloud);
	CHECK(upToDate.status == SyncConfigReviewStatus::Ready);
	CHECK(upToDate.bound);
	CHECK(upToDate.plan.verdict == ConfigSyncVerdict::UpToDate);
	CHECK(upToDate.ownHead.has_value());
	CHECK(upToDate.ownHead
		&& upToDate.ownHead->head.key == first.version.key);
	CHECK(upToDate.ownHead
		&& upToDate.ownHead->messageId == first.messageId);
	CHECK(MessageOf(upToDate) == SyncConfigMessage::UpToDateAlone);
	CHECK(PublishLocal(device, cloud).status == Status::AlreadySynced);
	CHECK(Apply(device, cloud, upToDate, std::nullopt).status
		== SyncConfigApplyStatus::InvalidChoice);

	device.setLocal(T1);
	const auto changed = Review(device, cloud);
	CHECK(changed.plan.verdict == ConfigSyncVerdict::LocalChanges);
	CHECK(!changed.plan.ownStale);
	CHECK(MessageOf(changed) == SyncConfigMessage::LocalChangesEdited);
	CHECK(ActionOf(changed) == SyncConfigAction::PublishChanges);
	CHECK(PublishOwn(
		device,
		cloud,
		SettingsFingerprint(T0),
		std::vector<QString>{ first.version.key }).status
			== Status::NeedsReview);
	CHECK(PublishOwn(device, cloud, std::nullopt, std::vector<QString>{})
		.status == Status::NeedsReview);
	CHECK(PublishOwn(
		device,
		cloud,
		SettingsFingerprint(T1),
		std::vector<QString>{}).status == Status::NeedsReview);
	CHECK(Publish(device, cloud, SyncConfigPublishRequest()).status
		== Status::NeedsReview);
	const auto noop = Apply(device, cloud, changed, std::nullopt);
	CHECK(noop.status == SyncConfigApplyStatus::Applied);
	CHECK(!noop.joined && !noop.wroteFile && !noop.adopted);
	CHECK(noop.publishNeeded);
	CHECK(noop.expectedParents
		== std::vector<QString>{ first.version.key });
	const auto second = PublishOwn(
		device,
		cloud,
		noop.fingerprint,
		noop.expectedParents);
	CHECK(second.status == Status::Confirmed);
	CHECK(second.version.parents
		== std::vector<QString>{ first.version.key });
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	auto oldCloud = cloud;
	auto early = MakeRemote('e', u"Linux"_q);
	const auto olderSpace = *FormatTimeOrderedSyncSpaceId(
		1,
		QByteArray(10, 'x'));
	Post(oldCloud, early, olderSpace, TB);
	CHECK(Review(device, oldCloud).status
		== SyncConfigReviewStatus::NeedsReview);

	auto cloneCloud = cloud;
	auto clone = Remote{
		.install = device.state->install,
		.device = u"desktop:someone-else"_q,
		.platform = u"macOS"_q,
		.seq = 2,
	};
	Post(cloneCloud, clone, device.state->space, TB, { second.version });
	CHECK(Review(device, cloneCloud).status
		== SyncConfigReviewStatus::CloneDetected);
	CHECK(PublishLocal(device, cloneCloud).status == Status::CloneDetected);

	auto incomplete = cloud;
	incomplete.scanComplete = false;
	CHECK(Review(device, incomplete).status
		== SyncConfigReviewStatus::Incomplete);
	CHECK(PublishLocal(device, incomplete).status == Status::Incomplete);
}

void TestFlowChooseUpdateAdoptConflict() {
	Begin("sync flow choose, update, adopt and conflict");
	using Status = SyncConfigPublishStatus;
	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	CHECK(Apply(device, cloud, Review(device, cloud), std::nullopt).status
		== SyncConfigApplyStatus::Applied);
	const auto v1 = PublishLocal(device, cloud);
	CHECK(v1.status == Status::Confirmed);
	device.setLocal(T1);
	const auto v2 = PublishLocal(device, cloud);
	CHECK(v2.status == Status::Confirmed);
	const auto space = device.state->space;

	auto b = MakeRemote('b', u"Android"_q);
	const auto b1 = Post(cloud, b, space, TB);
	const auto choose = Review(device, cloud);
	CHECK(choose.plan.verdict == ConfigSyncVerdict::Choose);
	CHECK(Keys(choose.plan.offered) == std::vector<QString>{ b1.key });
	CHECK(MessageOf(choose) == SyncConfigMessage::ChooseBound);
	CHECK(ActionOf(choose) == SyncConfigAction::Choose);
	CHECK(PublishLocal(device, cloud).status == Status::NeedsReview);
	CHECK(Apply(device, cloud, choose, u"1.0:x"_q).status
		== SyncConfigApplyStatus::InvalidChoice);
	const auto choice = PlanConfigChoice(choose.state, choose.plan, b1.key);
	CHECK(choice && choice->writeRemote && choice->publish);
	const auto picked = Apply(device, cloud, choose, b1.key);
	CHECK(picked.status == SyncConfigApplyStatus::Applied);
	CHECK(picked.wroteFile && picked.adopted && !picked.joined);
	CHECK(device.local.text == TB);
	CHECK(picked.fingerprint == SettingsFingerprint(TB));
	CHECK(!picked.update);
	CHECK(picked.versionKey == v2.version.key);
	CHECK(picked.source && picked.source->platform == u"Android"_q);
	CHECK(picked.source && picked.source->head.install == b.install);
	CHECK(!picked.otherVersionsRemain);
	CHECK(device.state->configData.base == b1.key);
	CHECK(device.state->configData.seenSeq.at(b.install) == 1);
	CHECK(picked.nextVerdict == ConfigSyncVerdict::LocalChanges);
	CHECK(picked.publishNeeded);
	CHECK(picked.promiseKept);
	CHECK(choice && SameSet(
		picked.expectedParents,
		SyncConfigVersionKeys(choice->parents)));
	CHECK(SameSet(picked.expectedParents, { b1.key, v2.version.key }));
	const auto repick = PublishOwn(
		device,
		cloud,
		picked.fingerprint,
		picked.expectedParents);
	CHECK(repick.status == Status::Confirmed);
	CHECK(SameSet(repick.version.parents, { b1.key, v2.version.key }));
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);
	CHECK(MessageOf(Review(device, cloud))
		== SyncConfigMessage::UpToDateWith);

	const auto b2 = Post(cloud, b, space, TB2, { repick.version });
	const auto update = Review(device, cloud);
	CHECK(update.plan.verdict == ConfigSyncVerdict::UpdateReady);
	CHECK(Keys(update.plan.offered) == std::vector<QString>{ b2.key });
	CHECK(MessageOf(update) == SyncConfigMessage::UpdateReady);
	CHECK(ActionOf(update) == SyncConfigAction::ReviewUpdate);
	CHECK(PublishLocal(device, cloud).status == Status::NeedsReview);
	CHECK(Apply(device, cloud, update, std::nullopt).status
		== SyncConfigApplyStatus::InvalidChoice);
	const auto applied = Apply(device, cloud, update, b2.key);
	CHECK(applied.status == SyncConfigApplyStatus::Applied);
	CHECK(applied.wroteFile && applied.adopted);
	CHECK(device.local.text == TB2);
	CHECK(applied.update);
	CHECK(applied.versionKey == repick.version.key);
	CHECK(applied.nextVerdict == ConfigSyncVerdict::UpToDate);
	CHECK(!applied.publishNeeded);
	CHECK(applied.expectedParents.empty());
	CHECK(applied.promiseKept);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.setLocal(TB);
	const auto afterUndo = Review(device, cloud);
	CHECK(afterUndo.plan.verdict == ConfigSyncVerdict::LocalChanges);
	device.setLocal(TB2);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	const auto b3 = Post(cloud, b, space, TB3, { b2 });
	const auto beforeCrash = device.stateBytes();
	device.setLocal(TB3);
	CHECK(device.stateBytes() == beforeCrash);
	const auto crashed = Review(device, cloud);
	CHECK(crashed.plan.verdict == ConfigSyncVerdict::Adopt);
	CHECK(MessageOf(crashed) == SyncConfigMessage::AdoptBound);
	CHECK(ActionOf(crashed) == SyncConfigAction::None);
	CHECK(PublishLocal(device, cloud).status == Status::NeedsReview);
	CHECK(Keys(crashed.plan.same) == std::vector<QString>{ b3.key });
	const auto adopted = Apply(device, cloud, crashed, std::nullopt);
	CHECK(adopted.status == SyncConfigApplyStatus::Applied);
	CHECK(adopted.adopted && !adopted.wroteFile);
	CHECK(adopted.nextVerdict == ConfigSyncVerdict::UpToDate);
	CHECK(!adopted.publishNeeded);
	CHECK(device.state->configData.base == b3.key);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.setLocal(TL);
	const auto b4 = Post(cloud, b, space, TB4, { b3 });
	const auto conflict = Review(device, cloud);
	CHECK(conflict.plan.verdict == ConfigSyncVerdict::Conflict);
	CHECK(Keys(conflict.plan.offered) == std::vector<QString>{ b4.key });
	CHECK(MessageOf(conflict) == SyncConfigMessage::ConflictConcurrent);
	CHECK(PublishLocal(device, cloud).status == Status::NeedsReview);
	const auto kept = Apply(device, cloud, conflict, std::nullopt);
	CHECK(kept.status == SyncConfigApplyStatus::Applied);
	CHECK(!kept.wroteFile && !kept.adopted);
	CHECK(kept.publishNeeded);
	CHECK(kept.expectedParents == std::vector<QString>{ b4.key });
	CHECK(device.local.text == TL);
	CHECK(PublishOwn(
		device,
		cloud,
		kept.fingerprint,
		std::vector<QString>{ b4.key, b3.key }).status
			== Status::NeedsReview);
	const auto keptPost = PublishOwn(
		device,
		cloud,
		kept.fingerprint,
		kept.expectedParents);
	CHECK(keptPost.status == Status::Confirmed);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	auto c = MakeRemote('c', u"Windows"_q);
	const auto b5 = Post(cloud, b, space, TB5, { keptPost.version });
	const auto c1 = Post(cloud, c, space, TC);
	const auto split = Review(device, cloud);
	CHECK(split.plan.verdict == ConfigSyncVerdict::Conflict);
	CHECK(split.plan.offered.size() == 2);
	const auto promise = PlanConfigChoice(split.state, split.plan, c1.key);
	CHECK(promise && promise->publish);
	CHECK(promise && SameSet(
		SyncConfigVersionKeys(promise->parents),
		{ c1.key, b5.key }));
	device.setLocal(TC);
	const auto afterCrash = Review(device, cloud);
	CHECK(afterCrash.plan.verdict == ConfigSyncVerdict::Conflict);
	CHECK(Keys(afterCrash.plan.offered) == std::vector<QString>{ b5.key });
	CHECK(Keys(afterCrash.plan.same) == std::vector<QString>{ c1.key });
	const auto recovered = Apply(device, cloud, afterCrash, std::nullopt);
	CHECK(recovered.status == SyncConfigApplyStatus::Applied);
	CHECK(recovered.adopted && !recovered.wroteFile);
	CHECK(recovered.publishNeeded);
	CHECK(promise && SameSet(
		recovered.expectedParents,
		SyncConfigVersionKeys(promise->parents)));
	const auto recoveredPost = PublishOwn(
		device,
		cloud,
		recovered.fingerprint,
		recovered.expectedParents);
	CHECK(recoveredPost.status == Status::Confirmed);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	const auto b6 = Post(cloud, b, space, TB6, { recoveredPost.version });
	const auto c2 = Post(cloud, c, space, TC2, { recoveredPost.version });
	const auto twoAhead = Review(device, cloud);
	CHECK(twoAhead.plan.verdict == ConfigSyncVerdict::Conflict);
	CHECK(twoAhead.plan.offered.size() == 2);
	CHECK(MessageOf(twoAhead) == SyncConfigMessage::ConflictSplitBound);
	const auto pickPromise = PlanConfigChoice(
		twoAhead.state,
		twoAhead.plan,
		b6.key);
	const auto pickB = Apply(device, cloud, twoAhead, b6.key);
	CHECK(pickB.status == SyncConfigApplyStatus::Applied);
	CHECK(device.local.text == TB6);
	CHECK(pickB.publishNeeded);
	CHECK(pickB.otherVersionsRemain);
	CHECK(!pickB.update);
	CHECK(pickB.promiseKept);
	CHECK(pickPromise && pickPromise->publish);
	CHECK(SameSet(pickB.expectedParents, { b6.key, c2.key }));
	CHECK(pickPromise && SameSet(
		pickB.expectedParents,
		SyncConfigVersionKeys(pickPromise->parents)));
	const auto staleClick = Review(device, cloud);
	CHECK(staleClick.plan.verdict == ConfigSyncVerdict::Choose);
	auto newer = cloud;
	Post(newer, c, space, TC, { c2 });
	CHECK(PublishOwn(
		device,
		newer,
		pickB.fingerprint,
		pickB.expectedParents).status == Status::NeedsReview);
	CHECK(device.state->config.pendingSeq == 0);
	const auto resolved = PublishOwn(
		device,
		cloud,
		pickB.fingerprint,
		pickB.expectedParents);
	CHECK(resolved.status == Status::Confirmed);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);
}

void TestFlowRechecks() {
	Begin("sync flow rechecks");
	using Status = SyncConfigPublishStatus;
	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	CHECK(Apply(device, cloud, Review(device, cloud), std::nullopt).status
		== SyncConfigApplyStatus::Applied);
	const auto v1 = PublishLocal(device, cloud);
	CHECK(v1.status == Status::Confirmed);
	const auto space = device.state->space;
	auto b = MakeRemote('b', u"Android"_q);
	const auto b1 = Post(cloud, b, space, TB, { v1.version });

	const auto review = Review(device, cloud);
	CHECK(review.plan.verdict == ConfigSyncVerdict::UpdateReady);
	const auto stateBefore = device.stateBytes();
	device.setLocal(T1);
	CHECK(Apply(device, cloud, review, b1.key).status
		== SyncConfigApplyStatus::NeedsRecheck);
	CHECK(device.local.text == T1);
	CHECK(device.stateBytes() == stateBefore);
	device.removeLocal();
	CHECK(Apply(device, cloud, review, b1.key).status
		== SyncConfigApplyStatus::NeedsRecheck);
	device.local = MakeSyncSettingsFile(SyncSettingsFileStatus::Invalid);
	CHECK(Apply(device, cloud, review, b1.key).status
		== SyncConfigApplyStatus::NeedsRecheck);
	device.setLocal(T0);

	auto moved = cloud;
	auto b2Writer = b;
	Post(moved, b2Writer, space, TB2, { b1 });
	CHECK(Apply(device, moved.inventory(), review, b1.key).status
		== SyncConfigApplyStatus::NeedsRecheck);
	CHECK(device.local.text == T0);

	auto tampered = review;
	tampered.plan.verdict = ConfigSyncVerdict::Choose;
	CHECK(Apply(device, cloud, tampered, b1.key).status
		!= SyncConfigApplyStatus::Applied);
	CHECK(device.local.text == T0);
	CHECK(device.stateBytes() == stateBefore);

	auto notReady = review;
	notReady.status = SyncConfigReviewStatus::NeedsReview;
	CHECK(Apply(device, cloud, notReady, b1.key).status
		== SyncConfigApplyStatus::NeedsReview);
	auto invalidLocal = review;
	invalidLocal.local = MakeSyncSettingsFile(SyncSettingsFileStatus::Invalid);
	CHECK(Apply(device, cloud, invalidLocal, b1.key).status
		== SyncConfigApplyStatus::NeedsReview);
	auto unboundWithInstall = review;
	unboundWithInstall.bound = false;
	CHECK(Apply(device, cloud, unboundWithInstall, b1.key).status
		== SyncConfigApplyStatus::NeedsReview);
	CHECK(device.stateBytes() == stateBefore);

	device.setLocal(TB);
	const auto next = Review(device, cloud);
	CHECK(next.status == SyncConfigReviewStatus::Ready);
	CHECK(next.plan.verdict == ConfigSyncVerdict::Adopt);
	const auto silent = Apply(device, cloud, next, std::nullopt);
	CHECK(silent.status == SyncConfigApplyStatus::Applied);
	CHECK(silent.adopted && !silent.wroteFile);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.setLocal(T1);
	CHECK(PublishLocal(device, cloud).status == Status::Confirmed);
	device.setLocal(T0);
	const auto stagedRun = Publish(
		device,
		cloud,
		LocalRequest(device, cloud),
		PostMode::Fail);
	CHECK(stagedRun.status == Status::OutcomeUnknown);
	CHECK(stagedRun.posts == 1);
	CHECK(device.state->config.pendingSeq != 0);
	const auto pendingReview = Review(device, cloud);
	CHECK(pendingReview.status == SyncConfigReviewStatus::Ready);
	CHECK(pendingReview.plan.verdict == ConfigSyncVerdict::Pending);
	CHECK(MessageOf(pendingReview) == SyncConfigMessage::Pending);
	CHECK(ActionOf(pendingReview) == SyncConfigAction::FinishSending);
	CHECK(Apply(device, cloud, pendingReview, std::nullopt).status
		== SyncConfigApplyStatus::InvalidChoice);
	cloud.add(device.staged);
	const auto foundPending = Review(device, cloud);
	CHECK(foundPending.status == SyncConfigReviewStatus::Ready);
	CHECK(foundPending.plan.verdict == ConfigSyncVerdict::Pending);
	CHECK(foundPending.ownHead.has_value());
	auto otherSpace = cloud;
	auto early = MakeRemote('e', u"Linux"_q);
	Post(
		otherSpace,
		early,
		*FormatTimeOrderedSyncSpaceId(1, QByteArray(10, 'x')),
		TB);
	const auto paused = Review(device, otherSpace);
	CHECK(paused.status == SyncConfigReviewStatus::NeedsReview);
	CHECK(MessageOf(paused) == SyncConfigMessage::NeedsReviewWithPending);
}

void TestFlowJoinVariants() {
	Begin("sync flow join variants");
	using Status = SyncConfigPublishStatus;
	auto cloud = Cloud();
	const auto space = *FormatSyncSpaceId(QByteArray(16, 'j'));
	auto b = MakeRemote('b', u"Android"_q);
	const auto b1 = Post(cloud, b, space, TB);

	auto absent = Device('f');
	absent.removeLocal();
	const auto review = Review(absent, cloud);
	CHECK(review.status == SyncConfigReviewStatus::Ready);
	CHECK(review.plan.verdict == ConfigSyncVerdict::Choose);
	CHECK(MessageOf(review) == SyncConfigMessage::ChooseUnbound);
	const auto joined = Apply(absent, cloud, review, b1.key);
	CHECK(joined.status == SyncConfigApplyStatus::Applied);
	CHECK(joined.joined && joined.wroteFile && joined.adopted);
	CHECK(!joined.publishNeeded);
	CHECK(joined.versionKey.isEmpty());
	CHECK(joined.nextVerdict == ConfigSyncVerdict::UpToDate);
	CHECK(absent.local.text == TB);
	CHECK(absent.state && absent.state->space == space);
	CHECK(absent.state && absent.state->configData.base == b1.key);
	CHECK(Review(absent, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);
	CHECK(PublishLocal(absent, cloud).status == Status::AlreadySynced);

	auto same = Device('g');
	same.setLocal(TB);
	const auto sameReview = Review(same, cloud);
	CHECK(sameReview.plan.verdict == ConfigSyncVerdict::Adopt);
	CHECK(MessageOf(sameReview) == SyncConfigMessage::AdoptUnbound);
	CHECK(ActionOf(sameReview) == SyncConfigAction::Join);
	const auto adopted = Apply(same, cloud, sameReview, std::nullopt);
	CHECK(adopted.status == SyncConfigApplyStatus::Applied);
	CHECK(adopted.joined && adopted.adopted && !adopted.wroteFile);
	CHECK(!adopted.publishNeeded);
	CHECK(Review(same, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	auto c = MakeRemote('c', u"Windows"_q);
	auto both = cloud;
	const auto c1 = Post(both, c, space, TC);
	auto keep = Device('h');
	keep.setLocal(T0);
	const auto keepReview = Review(keep, both);
	CHECK(keepReview.plan.verdict == ConfigSyncVerdict::Conflict);
	CHECK(keepReview.plan.offered.size() == 2);
	CHECK(MessageOf(keepReview) == SyncConfigMessage::ConflictSplitUnbound);
	const auto keepChoice = PlanConfigChoice(
		keepReview.state,
		keepReview.plan,
		std::nullopt);
	const auto kept = Apply(keep, both, keepReview, std::nullopt);
	CHECK(kept.status == SyncConfigApplyStatus::Applied);
	CHECK(kept.joined && !kept.wroteFile && !kept.adopted);
	CHECK(kept.publishNeeded);
	CHECK(keepChoice && SameSet(
		kept.expectedParents,
		SyncConfigVersionKeys(keepChoice->parents)));
	CHECK(SameSet(kept.expectedParents, { b1.key, c1.key }));
	CHECK(PublishLocal(keep, both).status == Status::NeedsReview);
	const auto keptPost = PublishOwn(
		keep,
		both,
		kept.fingerprint,
		kept.expectedParents);
	CHECK(keptPost.status == Status::Confirmed);
	CHECK(Review(keep, both).plan.verdict == ConfigSyncVerdict::UpToDate);

	auto d = MakeRemote('d', u"Linux"_q);
	auto trio = cloud;
	Post(trio, c, space, TC);
	const auto d1 = Post(trio, d, space, T0);
	auto keepSame = Device('i');
	keepSame.setLocal(T0);
	const auto keepSameReview = Review(keepSame, trio);
	CHECK(keepSameReview.plan.verdict == ConfigSyncVerdict::Choose);
	CHECK(keepSameReview.plan.offered.size() == 2);
	CHECK(Keys(keepSameReview.plan.same) == std::vector<QString>{ d1.key });
	const auto keptSame = Apply(
		keepSame,
		trio,
		keepSameReview,
		std::nullopt);
	CHECK(keptSame.status == SyncConfigApplyStatus::Applied);
	CHECK(keptSame.adopted && !keptSame.wroteFile);
	CHECK(keptSame.publishNeeded);
	CHECK(SameSet(keptSame.expectedParents, { b1.key, c1.key }));
	const auto keptSamePost = PublishOwn(
		keepSame,
		trio,
		keptSame.fingerprint,
		keptSame.expectedParents);
	CHECK(keptSamePost.status == Status::Confirmed);
	CHECK(Review(keepSame, trio).plan.verdict
		== ConfigSyncVerdict::UpToDate);

	auto stale = Device('k');
	stale.setLocal(T0);
	const auto first = Review(stale, cloud);
	auto grown = cloud;
	auto bLater = b;
	Post(grown, bLater, space, TB2, { b1 });
	CHECK(Apply(stale, grown, first, b1.key).status
		== SyncConfigApplyStatus::NeedsRecheck);
	CHECK(!stale.state);
	CHECK(stale.local.text == T0);
	stale.setLocal(T1);
	CHECK(Apply(stale, cloud, first, b1.key).status
		== SyncConfigApplyStatus::NeedsRecheck);
	CHECK(!stale.state);

	auto empty = Device('l');
	empty.setLocal(T0);
	const auto emptyReview = Review(empty, Cloud());
	auto taken = Cloud();
	auto early = MakeRemote('e', u"Linux"_q);
	Post(taken, early, space, TB);
	CHECK(Apply(empty, taken, emptyReview, std::nullopt).status
		== SyncConfigApplyStatus::NeedsRecheck);
	CHECK(!empty.state);
}

void TestFlowPublisher() {
	Begin("sync flow publisher");
	using Entry = SyncConfigPublishEntry;
	using Status = SyncConfigPublishStatus;
	using GateStatus = SyncConfigPublishGateStatus;
	const auto full = SyncConfigPublishRequest{
		.expectedFingerprint = u"f"_q,
		.expectedParents = std::vector<QString>(),
	};
	auto pendingWith = full;
	pendingWith.pendingOnly = true;
	CHECK(PlanSyncConfigPublishEntry({}, false) == Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry({}, true) == Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry(full, false) == Entry::NewContent);
	CHECK(PlanSyncConfigPublishEntry(full, true) == Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry(
		{ .expectedFingerprint = u"f"_q },
		false) == Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry(
		{ .expectedParents = std::vector<QString>() },
		false) == Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry({ .pendingOnly = true }, true)
		== Entry::FinishStaged);
	CHECK(PlanSyncConfigPublishEntry({ .pendingOnly = true }, false)
		== Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry(pendingWith, true) == Entry::Refuse);
	CHECK(PlanSyncConfigPublishEntry(pendingWith, false) == Entry::Refuse);
	auto pendingFingerprint = SyncConfigPublishRequest{
		.pendingOnly = true,
		.expectedFingerprint = u"f"_q,
	};
	CHECK(PlanSyncConfigPublishEntry(pendingFingerprint, true)
		== Entry::Refuse);

	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	const auto joined = Apply(
		device,
		cloud,
		Review(device, cloud),
		std::nullopt);
	CHECK(joined.status == SyncConfigApplyStatus::Applied);
	CHECK(joined.publishNeeded);
	auto run = Publish(device, cloud, SyncConfigPublishRequest());
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	run = Publish(device, cloud, { .pendingOnly = true });
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	CHECK(Gate(device, cloud, {}) == GateStatus::NeedsReview);
	CHECK(Gate(device, cloud, { .expectedFingerprint = joined.fingerprint })
		== GateStatus::NeedsReview);
	CHECK(Gate(device, cloud, {
		.expectedFingerprint = joined.fingerprint,
		.expectedParents = joined.expectedParents,
	}) == GateStatus::Proceed);
	CHECK(device.state->config.pendingSeq == 0);
	CHECK(cloud.records.empty());

	auto stopped = device;
	stopped.now = 0;
	CHECK(Publish(stopped, cloud, {
		.expectedFingerprint = joined.fingerprint,
		.expectedParents = joined.expectedParents,
	}).status == Status::NeedsReview);
	CHECK(stopped.state->config.pendingSeq == 0);
	auto unbound = device;
	unbound.state->bindingToken = *FormatSyncBindingToken(
		QByteArray(16, 'z'));
	const auto wrongToken = PlanSyncConfigPost(
		*unbound.state,
		device.token(),
		{},
		cloud.inventory(),
		unbound.local,
		{
			.expectedFingerprint = joined.fingerprint,
			.expectedParents = joined.expectedParents,
		},
		unbound.now,
		unbound.writer,
		SyncConfigSendQueue::HoldsSyncRecord);
	CHECK(wrongToken.step == SyncConfigPostStep::Finish);
	CHECK(wrongToken.status == Status::NeedsReview);

	device.writer.platform = u"Windows"_q;
	const auto first = Publish(device, cloud, {
		.expectedFingerprint = joined.fingerprint,
		.expectedParents = joined.expectedParents,
	});
	device.writer = DesktopWriter();
	CHECK(first.status == Status::Confirmed && first.posts == 1);
	const auto firstHeader = ParseSyncEnvelope(first.posted).header;
	CHECK(firstHeader && firstHeader->writerPlatform == u"Windows"_q);
	CHECK(firstHeader
		&& firstHeader->writerApp == u"Purple Telegram Desktop"_q);
	CHECK(firstHeader && firstHeader->at == uint64_t(device.now));
	CHECK(firstHeader && firstHeader->seq == 1);
	CHECK(firstHeader
		&& firstHeader->writerDevice == device.state->createdDevice);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.setLocal(T1);
	CHECK(Review(device, cloud).plan.verdict
		== ConfigSyncVerdict::LocalChanges);
	CHECK(Gate(device, cloud, {}) == GateStatus::NeedsReview);
	CHECK(Gate(
		device,
		cloud,
		{ .expectedFingerprint = SettingsFingerprint(T1) })
			== GateStatus::NeedsReview);
	run = Publish(device, cloud, SyncConfigPublishRequest());
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	run = Publish(device, cloud, { .pendingOnly = true });
	CHECK(run.status == Status::NeedsReview && run.posts == 0);

	const auto changes = LocalRequest(device, cloud);
	CHECK(changes.expectedParents.has_value());
	CHECK(Gate(device, cloud, changes) == GateStatus::Proceed);
	const auto lost = Publish(device, cloud, changes, PostMode::LoseReceipt);
	CHECK(lost.status == Status::OutcomeUnknown && lost.posts == 1);
	CHECK(device.state->config.pendingSeq != 0);
	const auto snapshot = cloud.inventory();
	const auto pending = Review(device, snapshot);
	CHECK(pending.status == SyncConfigReviewStatus::Ready);
	CHECK(pending.plan.verdict == ConfigSyncVerdict::Pending);
	CHECK(ActionOf(pending) == SyncConfigAction::FinishSending);

	run = Publish(device, cloud, snapshot, changes);
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	run = Publish(device, cloud, snapshot, SyncConfigPublishRequest());
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	auto pendingChanges = changes;
	pendingChanges.pendingOnly = true;
	run = Publish(device, cloud, snapshot, pendingChanges);
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	CHECK(device.state->config.pendingSeq != 0);

	const auto found = PlanSyncConfigPost(
		*device.state,
		device.token(),
		device.staged,
		snapshot,
		device.local,
		{ .pendingOnly = true },
		device.now,
		device.writer,
		SyncConfigSendQueue::HoldsSyncRecord);
	CHECK(found.step == SyncConfigPostStep::ConfirmFound);
	CHECK(found.messageId == lost.messageId || found.messageId > 0);
	CHECK(found.record == device.staged);
	CHECK(found.own.status == SyncOwnInventoryStatus::PendingFound);

	const auto boxB = Publish(device, cloud, snapshot, { .pendingOnly = true });
	CHECK(boxB.status == Status::Confirmed && boxB.posts == 0);
	CHECK(device.state->config.pendingSeq == 0);
	device.setLocal(TL);
	const auto recordsBefore = cloud.records.size();
	const auto stateBefore = device.stateBytes();
	const auto boxA = Publish(device, cloud, snapshot, { .pendingOnly = true });
	CHECK(boxA.status == Status::NeedsReview && boxA.posts == 0);
	CHECK(cloud.records.size() == recordsBefore);
	CHECK(device.stateBytes() == stateBefore);
	const auto afterEdit = Review(device, cloud);
	CHECK(afterEdit.plan.verdict == ConfigSyncVerdict::LocalChanges);
	CHECK(ActionOf(afterEdit) == SyncConfigAction::PublishChanges);

	const auto edit = LocalRequest(device, cloud);
	const auto failed = Publish(device, cloud, edit, PostMode::Fail);
	CHECK(failed.status == Status::OutcomeUnknown && failed.posts == 1);
	CHECK(cloud.records.size() == recordsBefore);
	run = Publish(device, cloud, edit);
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	const auto staged = PlanSyncConfigPost(
		*device.state,
		device.token(),
		device.staged,
		cloud.inventory(),
		MakeSyncSettingsFile(SyncSettingsFileStatus::Invalid),
		{ .pendingOnly = true },
		device.now,
		device.writer,
		SyncConfigSendQueue::Empty);
	CHECK(staged.step == SyncConfigPostStep::Post);
	CHECK(staged.record == device.staged);
	const auto held = PlanSyncConfigPost(
		*device.state,
		device.token(),
		device.staged,
		cloud.inventory(),
		MakeSyncSettingsFile(SyncSettingsFileStatus::Invalid),
		{ .pendingOnly = true },
		device.now,
		device.writer,
		SyncConfigSendQueue::HoldsSyncRecord);
	CHECK(held.step == SyncConfigPostStep::Finish);
	CHECK(held.status == Status::StillSending);
	CHECK(held.record.isEmpty());
	const auto emptyStage = PlanSyncConfigPost(
		*device.state,
		device.token(),
		QByteArray(),
		cloud.inventory(),
		device.local,
		{ .pendingOnly = true },
		device.now,
		device.writer,
		SyncConfigSendQueue::HoldsSyncRecord);
	CHECK(emptyStage.step == SyncConfigPostStep::Finish);
	CHECK(emptyStage.status == Status::NeedsReview);
	const auto finished = Publish(device, cloud, { .pendingOnly = true });
	CHECK(finished.status == Status::Confirmed && finished.posts == 1);
	CHECK(finished.posted == failed.posted);
	CHECK(device.state->config.pendingSeq == 0);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	const auto late = cloud.add(failed.posted);
	CHECK(late > finished.messageId);
	const auto duplicated = Review(device, cloud);
	CHECK(duplicated.status == SyncConfigReviewStatus::Ready);
	CHECK(duplicated.plan.verdict == ConfigSyncVerdict::UpToDate);
	CHECK(duplicated.ownHead
		&& duplicated.ownHead->messageId == finished.messageId);
	auto duplicatedInventory = cloud.inventory();
	SelectSyncSpaceIfEmpty(duplicatedInventory, device.state->space);
	const auto duplicates = ReconcileOwnConfigInventory(
		*device.state,
		duplicatedInventory,
		kUserId,
		{});
	CHECK(duplicates.status == SyncOwnInventoryStatus::Present);
	CHECK(duplicates.duplicateHeadMessageIds
		== std::vector<int32_t>{ late });
	device.setLocal(T1);
	const auto afterDuplicate = PublishLocal(device, cloud);
	CHECK(afterDuplicate.status == Status::Confirmed);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.setLocal(TB);
	const auto queued = Publish(
		device,
		cloud,
		LocalRequest(device, cloud),
		PostMode::Fail);
	CHECK(queued.status == Status::OutcomeUnknown && queued.posts == 1);
	const auto beforeQueue = cloud.inventory();
	const auto drained = cloud.add(queued.posted);
	const auto again = Publish(
		device,
		cloud,
		beforeQueue,
		{ .pendingOnly = true });
	CHECK(again.status == Status::Confirmed && again.posts == 1);
	CHECK(again.posted == queued.posted);
	CHECK(again.messageId > drained);
	const auto twice = Review(device, cloud);
	CHECK(twice.status == SyncConfigReviewStatus::Ready);
	CHECK(twice.plan.verdict == ConfigSyncVerdict::UpToDate);
	CHECK(twice.ownHead && twice.ownHead->messageId == drained);
	device.setLocal(T0);
	CHECK(PublishLocal(device, cloud).status == Status::Confirmed);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.removeLocal();
	CHECK(Publish(device, cloud, LocalRequest(device, cloud)).status
		== Status::InvalidSettings);
	device.setLocal(QByteArray());
	CHECK(Publish(device, cloud, LocalRequest(device, cloud)).status
		== Status::InvalidSettings);
	device.setLocal(QByteArray("not [valid"));
	CHECK(Publish(device, cloud, LocalRequest(device, cloud)).status
		== Status::InvalidSettings);
	auto big = QByteArray("version = 1\n");
	while (big.size() < 250 * 1024) {
		big += "# \"quoted\" padding line to grow the record\n";
	}
	device.setLocal(big);
	CHECK(Publish(device, cloud, LocalRequest(device, cloud)).status
		== Status::InvalidSettings);
	CHECK(device.state->config.pendingSeq == 0);
}

void TestFlowSendQueue() {
	Begin("sync flow send queue");
	using Status = SyncConfigPublishStatus;
	using Queue = SyncConfigSendQueue;
	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	CHECK(Apply(device, cloud, Review(device, cloud), std::nullopt).status
		== SyncConfigApplyStatus::Applied);

	device.queue = Queue::HoldsSyncRecord;
	const auto first = PublishLocal(device, cloud);
	CHECK(first.status == Status::Confirmed && first.posts == 1);
	CHECK(PublishLocal(device, cloud).status == Status::AlreadySynced);
	auto run = Publish(device, cloud, SyncConfigPublishRequest());
	CHECK(run.status == Status::NeedsReview && run.posts == 0);
	run = Publish(device, cloud, { .pendingOnly = true });
	CHECK(run.status == Status::NeedsReview && run.posts == 0);

	device.queue = Queue::Empty;
	device.setLocal(T1);
	const auto queued = Publish(
		device,
		cloud,
		LocalRequest(device, cloud),
		PostMode::Fail);
	CHECK(queued.status == Status::OutcomeUnknown && queued.posts == 1);
	CHECK(device.state->config.pendingSeq != 0);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::Pending);

	device.queue = Queue::HoldsSyncRecord;
	const auto recordsBefore = cloud.records.size();
	const auto stateBefore = device.stateBytes();
	const auto stagedBefore = device.staged;
	const auto held = Publish(device, cloud, { .pendingOnly = true });
	CHECK(held.status == Status::StillSending);
	CHECK(held.posts == 0);
	CHECK(cloud.records.size() == recordsBefore);
	CHECK(device.stateBytes() == stateBefore);
	CHECK(device.staged == stagedBefore);
	CHECK(Publish(device, cloud, LocalRequest(device, cloud)).status
		== Status::NeedsReview);
	auto incomplete = cloud;
	incomplete.scanComplete = false;
	CHECK(Publish(device, incomplete, { .pendingOnly = true }).status
		== Status::Incomplete);
	auto elsewhere = cloud;
	auto early = MakeRemote('e', u"Linux"_q);
	Post(
		elsewhere,
		early,
		*FormatTimeOrderedSyncSpaceId(1, QByteArray(10, 'x')),
		TB);
	CHECK(Publish(device, elsewhere, { .pendingOnly = true }).status
		== Status::NeedsReview);
	CHECK(device.stateBytes() == stateBefore);

	const auto arrived = cloud.add(queued.posted);
	const auto confirmed = Publish(device, cloud, { .pendingOnly = true });
	CHECK(confirmed.status == Status::Confirmed);
	CHECK(confirmed.posts == 0);
	CHECK(confirmed.messageId == arrived);
	CHECK(device.state->config.pendingSeq == 0);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);

	device.queue = Queue::Empty;
	device.setLocal(TB);
	const auto failed = Publish(
		device,
		cloud,
		LocalRequest(device, cloud),
		PostMode::Fail);
	CHECK(failed.status == Status::OutcomeUnknown && failed.posts == 1);
	device.queue = Queue::HoldsSyncRecord;
	CHECK(Publish(device, cloud, { .pendingOnly = true }).status
		== Status::StillSending);
	device.queue = Queue::Empty;
	const auto resent = Publish(device, cloud, { .pendingOnly = true });
	CHECK(resent.status == Status::Confirmed && resent.posts == 1);
	CHECK(resent.posted == failed.posted);
	CHECK(device.state->config.pendingSeq == 0);
	CHECK(Review(device, cloud).plan.verdict == ConfigSyncVerdict::UpToDate);
}

void TestFlowStamp() {
	Begin("sync flow review stamp");
	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	CHECK(Apply(device, cloud, Review(device, cloud), std::nullopt).status
		== SyncConfigApplyStatus::Applied);
	CHECK(PublishLocal(device, cloud).status
		== SyncConfigPublishStatus::Confirmed);
	auto b = MakeRemote('b', u"Android"_q);
	auto c = MakeRemote('c', u"Windows"_q);
	const auto own = device.state->configData.base;
	Post(cloud, b, device.state->space, TB, { VersionOfRecord(
		cloud.records.front().second) });
	Post(cloud, c, device.state->space, TC);
	device.setLocal(T1);
	const auto review = Review(device, cloud);
	CHECK(review.status == SyncConfigReviewStatus::Ready);
	CHECK(review.ownHead.has_value());
	CHECK(review.heads.size() == 2);
	CHECK(!review.plan.offered.empty());
	const auto stamp = SyncConfigReviewStamp(review);
	CHECK(stamp.size() == 64);
	CHECK(stamp == SyncConfigReviewStamp(review));
	CHECK(stamp == SyncConfigReviewStamp(Review(device, cloud)));

	const auto differs = [&](auto change) {
		auto copy = review;
		change(copy);
		return SyncConfigReviewStamp(copy) != stamp;
	};
	CHECK(differs([](auto &r) {
		r.status = SyncConfigReviewStatus::NeedsReview;
	}));
	CHECK(differs([](auto &r) { r.accountUserId = kUserId + 1; }));
	CHECK(differs([](auto &r) { r.bound = false; }));
	CHECK(differs([](auto &r) { r.space = QString(); }));
	CHECK(differs([](auto &r) { r.state.space = QString(); }));
	CHECK(differs([](auto &r) { r.state.install = QString(); }));
	CHECK(differs([](auto &r) {
		r.local.status = SyncSettingsFileStatus::Absent;
	}));
	CHECK(differs([](auto &r) {
		r.local.fingerprint = SettingsFingerprint(T0);
	}));
	CHECK(differs([](auto &r) {
		r.plan.verdict = ConfigSyncVerdict::Choose;
	}));
	CHECK(differs([](auto &r) { r.plan.ownStale = !r.plan.ownStale; }));
	CHECK(differs([](auto &r) { r.plan.offered.pop_back(); }));
	CHECK(differs([](auto &r) {
		r.plan.same.push_back(r.plan.offered.front());
	}));
	CHECK(differs([](auto &r) { r.heads.front().messageId += 1; }));
	CHECK(differs([](auto &r) { r.heads.front().text += '\n'; }));
	CHECK(differs([](auto &r) { r.heads.front().head.seq += 1; }));
	CHECK(differs([](auto &r) { r.heads.front().head.key = u"x"_q; }));
	CHECK(differs([](auto &r) {
		r.heads.front().head.lineage.push_back(u"x"_q);
	}));
	CHECK(differs([](auto &r) {
		r.heads.front().head.install = QString();
	}));
	CHECK(differs([](auto &r) { std::swap(r.heads[0], r.heads[1]); }));
	CHECK(differs([](auto &r) { r.heads.pop_back(); }));
	CHECK(differs([](auto &r) { r.ownHead.reset(); }));
	CHECK(differs([](auto &r) { r.ownHead->messageId += 1; }));
	CHECK(differs([](auto &r) { r.ownHead->head.seq += 1; }));
	CHECK(differs([](auto &r) { r.ownHead->head.key = u"x"_q; }));
	CHECK(!differs([](auto &r) { r.state.base = QString(); }));
	CHECK(!differs([](auto &r) { r.state.seenSeq.clear(); }));
	CHECK(!differs([](auto &r) { r.heads.front().platform = u"x"_q; }));
	CHECK(!differs([](auto &r) { r.plan.classification = {}; }));
	CHECK(!own.isEmpty());

	auto fresh = Device('m');
	fresh.setLocal(T0);
	const auto unbound = Review(fresh, cloud);
	CHECK(!unbound.bound);
	CHECK(unbound.status == SyncConfigReviewStatus::Ready);
	CHECK(Join(fresh, cloud.inventory()));
	const auto bound = Review(fresh, cloud);
	CHECK(SyncConfigReviewStamp(bound) != SyncConfigReviewStamp(unbound));
	const auto joined = SyncConfigJoinedReview(unbound, *fresh.state);
	CHECK(joined.bound);
	CHECK(joined.state.install == fresh.state->install);
	CHECK(joined.space == unbound.space);
	CHECK(SyncConfigReviewStamp(bound) == SyncConfigReviewStamp(joined));
	const auto preJoin = ReviewSyncConfigInventory(
		cloud.inventory(),
		nullptr,
		{},
		fresh.local);
	CHECK(SyncConfigReviewStamp(preJoin) == SyncConfigReviewStamp(unbound));
	CHECK(SyncConfigReviewStamp(SyncConfigJoinedReview(preJoin, *fresh.state))
		== SyncConfigReviewStamp(joined));

	auto lonely = Device('n');
	lonely.setLocal(T0);
	const auto nothing = Review(lonely, Cloud());
	CHECK(nothing.space.isEmpty());
	CHECK(Join(lonely, Cloud().inventory()));
	const auto created = SyncConfigJoinedReview(nothing, *lonely.state);
	CHECK(created.space == lonely.state->space);
	CHECK(created.state.space == lonely.state->space);
	CHECK(SyncConfigReviewStamp(Review(lonely, Cloud()))
		== SyncConfigReviewStamp(created));

	const auto key = review.plan.offered.front().key;
	auto wrong = PlanSyncConfigApply(review, u"stale"_q, key);
	CHECK(wrong.status == SyncConfigApplyPlanStatus::NeedsRecheck);
	auto refused = review;
	refused.status = SyncConfigReviewStatus::NeedsReview;
	CHECK(PlanSyncConfigApply(
		refused,
		SyncConfigReviewStamp(refused),
		key).status == SyncConfigApplyPlanStatus::NeedsReview);
	CHECK(PlanSyncConfigApply(review, stamp, u"1.0:x"_q).status
		== SyncConfigApplyPlanStatus::InvalidChoice);
	auto headless = review;
	headless.heads.clear();
	CHECK(PlanSyncConfigApply(
		headless,
		SyncConfigReviewStamp(headless),
		key).status == SyncConfigApplyPlanStatus::NeedsReview);
	const auto plan = PlanSyncConfigApply(review, stamp, key);
	CHECK(plan.status == SyncConfigApplyPlanStatus::Ready);
	CHECK(!plan.join);
	CHECK(plan.choice.writeRemote);
	CHECK(plan.source && plan.source->head.key == key);
	CHECK(plan.writeFingerprint == SyncConfigKeyFingerprint(key));
	CHECK(plan.localFingerprint == SettingsFingerprint(T1));
	CHECK(plan.versionKey.isEmpty());
	CHECK(plan.verdict == review.plan.verdict);
	CHECK(plan.ownHead.has_value());
	CHECK(plan.heads.size() == 2);

	CHECK(CompleteSyncConfigApply(wrong, device.local).status
		== SyncConfigApplyCompletionStatus::InvalidPlan);
	auto joinPlan = plan;
	joinPlan.join = true;
	CHECK(CompleteSyncConfigApply(joinPlan, device.local).status
		== SyncConfigApplyCompletionStatus::InvalidPlan);
	CHECK(CompleteSyncConfigApply(plan, device.local).status
		== SyncConfigApplyCompletionStatus::ReadBackMismatch);
	CHECK(CompleteSyncConfigApply(
		plan,
		MakeSyncSettingsFile(SyncSettingsFileStatus::Absent)).status
			== SyncConfigApplyCompletionStatus::ReadBackMismatch);
	const auto written = MakeSyncSettingsFile(
		SyncSettingsFileStatus::Present,
		plan.source->text);
	const auto completed = CompleteSyncConfigApply(plan, written);
	CHECK(completed.status == SyncConfigApplyCompletionStatus::Ready);
	CHECK(completed.fingerprint == written.fingerprint);
	CHECK(completed.promiseKept);
	auto blocked = plan;
	blocked.state.pending = own;
	CHECK(!blocked.choice.adopt.empty());
	CHECK(CompleteSyncConfigApply(blocked, written).status
		== SyncConfigApplyCompletionStatus::AdoptRefused);
	auto broken = plan;
	broken.choice.publish = !broken.choice.publish;
	CHECK(!CompleteSyncConfigApply(broken, written).promiseKept);
	auto keepLocal = PlanSyncConfigApply(review, stamp, std::nullopt);
	CHECK(keepLocal.status == SyncConfigApplyPlanStatus::Ready);
	CHECK(!keepLocal.choice.writeRemote);
	CHECK(!keepLocal.source);
	CHECK(CompleteSyncConfigApply(keepLocal, written).status
		== SyncConfigApplyCompletionStatus::ReadBackMismatch);
	const auto kept = CompleteSyncConfigApply(keepLocal, device.local);
	CHECK(kept.status == SyncConfigApplyCompletionStatus::Ready);
	CHECK(kept.publishNeeded);
	CHECK(kept.promiseKept);
	CHECK(SameSet(
		kept.expectedParents,
		SyncConfigVersionKeys(keepLocal.choice.parents)));
}

void TestFlowCommitCheck() {
	Begin("sync flow commit check");
	using Commit = SyncConfigCommitStatus;
	auto cloud = Cloud();
	auto device = Device('a');
	device.setLocal(T0);
	CHECK(Apply(device, cloud, Review(device, cloud), std::nullopt).status
		== SyncConfigApplyStatus::Applied);
	CHECK(PublishLocal(device, cloud).status
		== SyncConfigPublishStatus::Confirmed);
	auto b = MakeRemote('b', u"Android"_q);
	const auto b1 = Post(cloud, b, device.state->space, TB);
	const auto &state = *device.state;
	const auto current = state.configData;

	const auto same = CheckSyncConfigDataCommit(state, current);
	CHECK(same.status == Commit::Unchanged);

	auto next = current;
	next.seenSeq[b.install] = 1;
	next.equiv.push_back(b1.key);
	const auto ready = CheckSyncConfigDataCommit(state, next);
	CHECK(ready.status == Commit::Ready);
	CHECK(ready.state.configData.seenSeq == next.seenSeq);
	CHECK(ready.state.configData.equiv == next.equiv);
	CHECK(ready.state.install == state.install);
	CHECK(ready.canonical == SerializeSyncLocalState(ready.state).canonical);
	const auto parsed = ParseSyncLocalState(ready.canonical);
	CHECK(bool(parsed));
	CHECK(parsed && parsed.state.configData.equiv == next.equiv);

	auto withPending = next;
	withPending.pending = b1.key;
	CHECK(CheckSyncConfigDataCommit(state, withPending).status
		== Commit::InvalidTransition);
	auto pendingState = state;
	pendingState.configData.pending = b1.key;
	CHECK(CheckSyncConfigDataCommit(pendingState, next).status
		== Commit::InvalidTransition);
	auto stagedState = state;
	stagedState.config.pendingSeq = stagedState.config.seq + 1;
	CHECK(CheckSyncConfigDataCommit(stagedState, next).status
		== Commit::InvalidTransition);

	auto seen = ready.state;
	auto lower = seen.configData;
	lower.seenSeq[b.install] = 0;
	CHECK(CheckSyncConfigDataCommit(seen, lower).status
		== Commit::InvalidTransition);
	auto dropped = seen.configData;
	dropped.seenSeq.erase(b.install);
	CHECK(CheckSyncConfigDataCommit(seen, dropped).status
		== Commit::InvalidTransition);
	auto higher = seen.configData;
	higher.seenSeq[b.install] = 5;
	CHECK(CheckSyncConfigDataCommit(seen, higher).status == Commit::Ready);

	auto invalid = next;
	invalid.base = u"not a key"_q;
	const auto refused = CheckSyncConfigDataCommit(state, invalid);
	CHECK(refused.status == Commit::InvalidState);
	CHECK(refused.canonical.isEmpty());
}

void TestFlowDescribe() {
	Begin("sync flow describe");
	using Message = SyncConfigMessage;
	using Action = SyncConfigAction;
	const auto parts = [](const QString &platform, const QString &install) {
		return SyncDeviceNameOf(platform, install);
	};
	CHECK(parts(u"Android"_q, u"in-9c1dxyz"_q)
		== (SyncDeviceNameParts{ u"Android"_q, u"9c1d"_q }));
	CHECK(parts(u"macOS"_q, u"3f2a77"_q)
		== (SyncDeviceNameParts{ u"macOS"_q, u"3f2a"_q }));
	CHECK(parts(QString(), u"in-ab"_q)
		== (SyncDeviceNameParts{ QString(), u"ab"_q }));
	CHECK(parts(u"  Linux  box "_q, u"in-abcd"_q)
		== (SyncDeviceNameParts{ u"Linux box"_q, u"abcd"_q }));
	CHECK(parts(QString(40, u'x'), u"in-abcd"_q)
		== (SyncDeviceNameParts{ QString(32, u'x'), u"abcd"_q }));
	CHECK(parts(u"Windows"_q, QString())
		== (SyncDeviceNameParts{ u"Windows"_q, QString() }));

	const auto writer = DesktopWriter();
	const auto device = u"desktop:x"_q;
	using File = SyncSettingsFileStatus;
	CHECK(SyncSettingsPublishable(
		MakeSyncSettingsFile(File::Present, T0),
		device,
		writer));
	CHECK(!SyncSettingsPublishable(
		MakeSyncSettingsFile(File::Present, QByteArray("not [valid")),
		device,
		writer));
	CHECK(!SyncSettingsPublishable(
		MakeSyncSettingsFile(File::Present, QByteArray()),
		device,
		writer));
	CHECK(!SyncSettingsPublishable(
		MakeSyncSettingsFile(File::Absent),
		device,
		writer));
	CHECK(!SyncSettingsPublishable(
		MakeSyncSettingsFile(File::Invalid),
		device,
		writer));
	auto big = QByteArray("version = 1\n");
	while (big.size() < 250 * 1024) {
		big += "# \"quoted\" padding line to grow the record\n";
	}
	CHECK(!SyncSettingsPublishable(
		MakeSyncSettingsFile(File::Present, big),
		device,
		writer));

	auto failed = SyncConfigReview();
	using Failure = std::pair<SyncConfigReviewStatus, Message>;
	const auto failures = std::vector<Failure>{
		{ SyncConfigReviewStatus::NeedsReview, Message::NeedsReview },
		{ SyncConfigReviewStatus::Incomplete, Message::Incomplete },
		{ SyncConfigReviewStatus::CloneDetected, Message::CloneDetected },
		{
			SyncConfigReviewStatus::AccountUnavailable,
			Message::AccountUnavailable,
		},
		{ SyncConfigReviewStatus::AccountUnbound, Message::AccountUnbound },
		{ SyncConfigReviewStatus::StoreError, Message::StoreError },
		{ SyncConfigReviewStatus::InvalidSettings, Message::InvalidSettings },
	};
	for (const auto &[status, message] : failures) {
		failed.status = status;
		const auto described = DescribeSyncConfigReview(failed, true);
		CHECK(described.message == message);
		CHECK(described.action == Action::None);
	}
	failed.status = SyncConfigReviewStatus::NeedsReview;
	failed.state.pending = u"1.x"_q;
	CHECK(DescribeSyncConfigReview(failed, true).message
		== Message::NeedsReview);
	failed.bound = true;
	CHECK(DescribeSyncConfigReview(failed, true).message
		== Message::NeedsReviewWithPending);
	failed.status = SyncConfigReviewStatus::Incomplete;
	CHECK(DescribeSyncConfigReview(failed, true).message
		== Message::Incomplete);

	auto crafted = SyncConfigReview{ .status = SyncConfigReviewStatus::Ready };
	CHECK(DescribeSyncConfigReview(crafted, true).message
		== Message::InvalidRecords);
	CHECK(DescribeSyncConfigReview(crafted, true).action == Action::None);
	crafted.plan.verdict = ConfigSyncVerdict::UpdateReady;
	CHECK(DescribeSyncConfigReview(crafted, true).message
		== Message::UpdateMissing);
	crafted.plan.offered.push_back({ .install = u"in-ffff"_q, .key = u"k"_q });
	CHECK(DescribeSyncConfigReview(crafted, true).message
		== Message::UpdateMissing);
	crafted.plan.verdict = ConfigSyncVerdict::Empty;
	crafted.local = MakeSyncSettingsFile(File::Absent);
	CHECK(DescribeSyncConfigReview(crafted, false).message
		== Message::NotPublishableAbsent);
	CHECK(DescribeSyncConfigReview(crafted, false).action == Action::None);
	crafted.local = MakeSyncSettingsFile(File::Present, QByteArray("x ["));
	CHECK(DescribeSyncConfigReview(crafted, false).message
		== Message::NotPublishableInvalid);
	crafted.plan.verdict = ConfigSyncVerdict::LocalChanges;
	CHECK(DescribeSyncConfigReview(crafted, false).message
		== Message::NotPublishableInvalid);
	crafted.local = MakeSyncSettingsFile(File::Present, T0);
	const auto t0Key = MakeConfigVersion(T0, {})->key;
	crafted.state.base = t0Key;
	CHECK(DescribeSyncConfigReview(crafted, true).message
		== Message::LocalChangesOwnStale);
	CHECK(DescribeSyncConfigReview(crafted, true).action
		== Action::PublishChanges);
	crafted.local = MakeSyncSettingsFile(File::Present, T1);
	CHECK(DescribeSyncConfigReview(crafted, true).message
		== Message::LocalChangesEdited);

	const auto space = *FormatSyncSpaceId(QByteArray(16, 'q'));
	auto cloud = Cloud();
	auto b = MakeRemote('b', u"Android"_q);
	auto c = MakeRemote('c', u"Windows"_q);
	const auto b1 = Post(cloud, b, space, T0);
	auto split = cloud;
	Post(split, c, space, TC);
	auto viewer = Device('a');
	viewer.setLocal(T0);
	const auto unboundChoose = Review(viewer, split);
	CHECK(unboundChoose.plan.verdict == ConfigSyncVerdict::Choose);
	const auto chooseText = DescribeSyncConfigReview(unboundChoose, true);
	CHECK(chooseText.message == Message::ChooseUnbound);
	CHECK(chooseText.action == Action::Choose);
	CHECK(chooseText.devices
		== std::vector<SyncDeviceNameParts>{
			SyncDeviceNameOf(u"Windows"_q, c.install) });
	CHECK(SyncConfigChooseMessage(unboundChoose) == Message::ChooseUnbound);
	CHECK(SyncChoicePublishes(unboundChoose, std::nullopt));
	CHECK(SyncChoicePublishes(
		unboundChoose,
		unboundChoose.plan.offered.front().key));
	const auto unboundChoices = SyncConfigChoices(unboundChoose, true);
	CHECK(unboundChoices.size() == 2);
	CHECK(unboundChoices.size() == 2
		&& unboundChoices[0].key == unboundChoose.plan.offered.front().key
		&& unboundChoices[0].head >= 0
		&& unboundChoose.heads[unboundChoices[0].head].head.install
			== c.install
		&& unboundChoices[0].publishes);
	CHECK(unboundChoices.size() == 2
		&& !unboundChoices[1].key
		&& unboundChoices[1].head == -1
		&& unboundChoices[1].publishes);
	CHECK(SyncConfigChoices(unboundChoose, false).size() == 1);

	const auto adopt = Review(viewer, cloud);
	CHECK(adopt.plan.verdict == ConfigSyncVerdict::Adopt);
	const auto adoptText = DescribeSyncConfigReview(adopt, true);
	CHECK(adoptText.message == Message::AdoptUnbound);
	CHECK(adoptText.action == Action::Join);
	CHECK(adoptText.devices
		== std::vector<SyncDeviceNameParts>{
			SyncDeviceNameOf(u"Android"_q, b.install) });
	const auto adoptChoices = SyncConfigChoices(adopt, true);
	CHECK(adoptChoices.size() == 1);
	CHECK(adoptChoices.size() == 1
		&& !adoptChoices[0].key
		&& !adoptChoices[0].publishes);

	viewer.setLocal(TB);
	const auto choose = Review(viewer, cloud);
	CHECK(choose.plan.verdict == ConfigSyncVerdict::Choose);
	CHECK(SyncChoicePublishes(choose, std::nullopt));
	CHECK(!SyncChoicePublishes(choose, b1.key));
	const auto chooseChoices = SyncConfigChoices(choose, true);
	CHECK(chooseChoices.size() == 2);
	CHECK(chooseChoices.size() == 2 && !chooseChoices[0].publishes);
	CHECK(chooseChoices.size() == 2 && chooseChoices[1].publishes);

	viewer.setLocal(T0);
	CHECK(Apply(viewer, cloud, Review(viewer, cloud), std::nullopt).status
		== SyncConfigApplyStatus::Applied);
	const auto upToDate = DescribeSyncConfigReview(
		Review(viewer, cloud),
		true);
	CHECK(upToDate.message == Message::UpToDateWith);
	CHECK(upToDate.others == 1);
	const auto b2 = Post(cloud, b, space, TB, { b1 });
	const auto update = Review(viewer, cloud);
	CHECK(update.plan.verdict == ConfigSyncVerdict::UpdateReady);
	const auto updateText = DescribeSyncConfigReview(update, true);
	CHECK(updateText.message == Message::UpdateReady);
	CHECK(updateText.action == Action::ReviewUpdate);
	CHECK(updateText.at == 1700000002);
	CHECK(updateText.devices
		== std::vector<SyncDeviceNameParts>{
			SyncDeviceNameOf(u"Android"_q, b.install) });
	const auto updateChoices = SyncConfigChoices(update, true);
	CHECK(updateChoices.size() == 1);
	CHECK(updateChoices.size() == 1
		&& updateChoices[0].key == b2.key
		&& !updateChoices[0].publishes);
	CHECK(!SyncChoicePublishes(update, b2.key));

	using Kind = SyncConfigApplyFailureKind;
	using Apply = SyncConfigApplyStatus;
	const auto kind = [](SyncConfigApplyOutcome outcome) {
		return DescribeSyncConfigApplyFailure(outcome).kind;
	};
	CHECK(kind({ .status = Apply::Applied }) == Kind::None);
	CHECK(kind({ .status = Apply::Applied, .joined = true }) == Kind::None);
	CHECK(kind({ .status = Apply::NeedsRecheck }) == Kind::NothingDone);
	CHECK(kind({ .status = Apply::NeedsRecheck, .joined = true })
		== Kind::JoinedNotWritten);
	CHECK(kind({ .status = Apply::WriteError, .joined = true })
		== Kind::JoinedNotWritten);
	CHECK(kind({ .status = Apply::StoreError, .wroteFile = true })
		== Kind::WrittenStateNotSaved);
	CHECK(kind({
		.status = Apply::StoreError,
		.joined = true,
		.wroteFile = true,
	}) == Kind::WrittenStateNotSaved);
	CHECK(kind({ .status = Apply::WriteError, .wroteFile = true })
		== Kind::WrittenNotReadBack);
	CHECK(kind({ .status = Apply::NeedsReview, .wroteFile = true })
		== Kind::WrittenNotReadBack);
	const auto described = DescribeSyncConfigApplyFailure({
		.status = Apply::StoreError,
		.joined = true,
		.wroteFile = true,
		.historyKept = true,
		.undoAvailable = true,
		.otherVersionsRemain = true,
	});
	CHECK(described.status == Apply::StoreError);
	CHECK(described.joined);
	CHECK(described.historyKept);
	CHECK(described.undoAvailable);
	CHECK(described.otherVersionsRemain);

	for (const auto status : {
		SyncConfigRestoreStatus::InvalidSettings,
		SyncConfigRestoreStatus::HistoryError,
		SyncConfigRestoreStatus::WriteError,
	}) {
		CHECK(!SyncConfigUndoFinished(status));
	}
	for (const auto status : {
		SyncConfigRestoreStatus::Restored,
		SyncConfigRestoreStatus::Unchanged,
		SyncConfigRestoreStatus::NotFound,
		SyncConfigRestoreStatus::FileDidNotExist,
		SyncConfigRestoreStatus::NotText,
		SyncConfigRestoreStatus::InvalidReason,
	}) {
		CHECK(SyncConfigUndoFinished(status));
	}
}

void TestFlowReviewStatuses() {
	Begin("sync flow review statuses");
	const auto space = *FormatSyncSpaceId(QByteArray(16, 'r'));
	auto cloud = Cloud();
	auto b = MakeRemote('b', u"Android"_q);
	const auto b1 = Post(cloud, b, space, TB);
	auto device = Device('a');
	device.setLocal(T0);

	auto incomplete = cloud;
	incomplete.scanComplete = false;
	CHECK(Review(device, incomplete).status
		== SyncConfigReviewStatus::Incomplete);
	auto unread = cloud.inventory();
	unread.read.reset();
	CHECK(Review(device, unread).status == SyncConfigReviewStatus::Incomplete);
	auto broken = cloud;
	broken.add(QByteArray("{\"not\":\"a record\"}"));
	CHECK(Review(device, broken).status == SyncConfigReviewStatus::NeedsReview);

	const auto newerText = QByteArray("version = 2\n");
	const auto newerVersion = *MakeConfigVersion(newerText, {});
	auto n = MakeRemote('n', u"Linux"_q);
	const auto newerRecord = SerializeSyncEnvelope(SyncEnvelope{
		QJsonObject{
			{ u"purple_sync"_q, 1 },
			{ u"stream"_q, u"config"_q },
			{ u"space"_q, space },
			{ u"writer"_q, QJsonObject{
				{ u"install"_q, n.install },
				{ u"device"_q, n.device },
				{ u"platform"_q, n.platform },
				{ u"app"_q, u"Harness"_q },
			} },
			{ u"seq"_q, 1 },
			{ u"at"_q, 0 },
			{ u"payload"_q, QJsonObject{
				{ u"schema"_q, 2 },
				{ u"key"_q, newerVersion.key },
				{ u"parents"_q, QJsonArray() },
				{ u"lineage"_q, QJsonArray() },
				{ u"warnings"_q, 0 },
				{ u"text"_q, QString::fromUtf8(newerText) },
			} },
		},
	}).canonical;
	CHECK(ClassifySyncCandidate(1, newerRecord).status
		== SyncCandidateStatus::NewerSchema);
	auto newer = cloud;
	newer.add(newerRecord);
	CHECK(Review(device, newer).status == SyncConfigReviewStatus::NeedsReview);
	auto opaque = newer.inventory();
	opaque.status = SyncAccountInventoryStatus::Complete;
	opaque.read->status = SyncCandidateReadStatus::Complete;
	CHECK(Review(device, opaque).status == SyncConfigReviewStatus::NeedsReview);

	CHECK(Apply(device, cloud, Review(device, cloud), b1.key).status
		== SyncConfigApplyStatus::Applied);
	CHECK(device.local.text == TB);
	CHECK(Review(device, cloud).status == SyncConfigReviewStatus::Ready);
	auto elsewhere = device;
	elsewhere.state->space = *FormatSyncSpaceId(QByteArray(16, 'w'));
	CHECK(Review(elsewhere, cloud).status
		== SyncConfigReviewStatus::NeedsReview);
	CHECK(Review(device, incomplete).status
		== SyncConfigReviewStatus::Incomplete);
	const auto invalidState = [&] {
		auto copy = device;
		copy.state->configData.baseLineage.push_back(u"not a key"_q);
		return Review(copy, cloud);
	}();
	CHECK(invalidState.status == SyncConfigReviewStatus::NeedsReview);
}

} // namespace

int main() {
	TestFlowBasics();
	TestFlowHeads();
	TestFlowEmptyJoinAndLocalChanges();
	TestFlowChooseUpdateAdoptConflict();
	TestFlowRechecks();
	TestFlowJoinVariants();
	TestFlowPublisher();
	TestFlowSendQueue();
	TestFlowStamp();
	TestFlowCommitCheck();
	TestFlowDescribe();
	TestFlowReviewStatuses();

	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
