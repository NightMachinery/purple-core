/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_store.h"

#include "purple/purple_archive_internal.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <limits>
#include <map>

namespace Purple {
namespace {

using namespace ArchiveInternal;

ArchiveTransaction Refuse(ArchivePlanStatus status) {
	auto result = ArchiveTransaction();
	result.status = status;
	return result;
}

std::optional<ArchiveTransaction> CheckState(const ArchiveState &state) {
	if (state.version > kArchiveSchemaVersion) {
		return Refuse(ArchivePlanStatus::ReadOnly);
	} else if (!IsArchiveState(state)) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	return std::nullopt;
}

ArchiveMessage *Message(ArchiveState &state, const ArchiveMessageKey &key) {
	for (auto &message : state.messages) {
		if (SameArchiveMessage(message.key, key)) {
			return &message;
		}
	}
	return nullptr;
}

ArchiveVersion *Version(ArchiveState &state,
		const ArchiveMessageKey &key, const QString &id) {
	if (const auto message = Message(state, key)) {
		for (auto &version : message->versions) {
			if (version.id == id) {
				return &version;
			}
		}
	}
	return nullptr;
}

ArchiveMediaReference *Reference(ArchiveVersion &version, const QString &identity) {
	for (auto &reference : version.media) {
		if (reference.identity == identity) {
			return &reference;
		}
	}
	return nullptr;
}

ArchiveMissingSlots EventMissingSlots(const ArchiveEvent &event) {
	auto result = event.missing;
	if (result.revision.isEmpty()) {
		result.revision = event.origin == ArchiveOrigin::Hydration ? event.targetRevision : event.id;
	}
	return result;
}

QString EventFingerprint(const ArchiveEvent &event) {
	auto aliases = QJsonArray();
	for (const auto &alias : event.migrationAliases) {
		aliases.push_back(QJsonArray{ int(alias.kind), QString::number(alias.id) });
	}
	auto source = QJsonValue(QJsonValue::Null);
	if (event.source) {
		source = QJsonObject{
			{ u"kind"_q, int(event.source->kind) },
			{ u"hash"_q, event.source->contentHash },
			{ u"revision"_q, event.source->revision },
			{ u"representation"_q, event.source->representation },
			{ u"bytes"_q, QString::fromLatin1(event.source->bytes.toBase64()) },
		};
		if (event.source->binding) {
			auto object = source.toObject();
			object.insert(u"binding"_q, SourceBindingObject(*event.source->binding));
			source = object;
		}
	}
	const auto object = QJsonObject{
		{ u"order"_q, QString::number(event.order) },
		{ u"origin"_q, int(event.origin) },
		{ u"content"_q, event.content
			? QJsonValue(ContentObject(*event.content)) : QJsonValue(QJsonValue::Null) },
		{ u"completeness"_q, int(event.completeness) },
		{ u"missing"_q, MissingSlotsObject(EventMissingSlots(event)) },
		{ u"representation"_q, event.observation.representation },
		{ u"target"_q, event.targetVersion },
		{ u"targetRevision"_q, event.targetRevision },
		{ u"expectedHash"_q, event.expectedContentHash },
		{ u"aliases"_q, aliases },
		{ u"source"_q, source },
	};
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return canonical ? QString::fromLatin1(QCryptographicHash::hash(
		canonical.canonical, QCryptographicHash::Sha256).toHex()) : QString();
}

ArchiveVersion MakeVersion(const ArchiveEvent &event, bool missingPreimage) {
	auto version = ArchiveVersion();
	version.id = event.id;
	version.revision = event.id;
	version.order = event.order;
	version.origin = event.origin;
	version.firstObservedAt = event.observation.observedAt;
	version.content = *event.content;
	version.missing = EventMissingSlots(event);
	version.completeness = event.completeness;
	version.observation = event.observation;
	version.source = event.source;
	version.missingPreimage = missingPreimage;
	for (const auto &media : version.content.media) {
		version.media.push_back({ media.identity });
	}
	return version;
}

bool PreservesCapturedJson(const QJsonValue &before, const QJsonValue &after,
		bool open, bool placeholder = false) {
	if (placeholder && (before.isUndefined() || before.isNull()
		|| (before.isString() && before.toString().isEmpty()))) {
		return !after.isUndefined();
	} else if (before.type() != after.type()) {
		return false;
	} else if (before.isObject()) {
		const auto captured = before.toObject();
		const auto enriched = after.toObject();
		if (!open && captured.size() != enriched.size()) {
			return false;
		}
		for (auto field = captured.begin(); field != captured.end(); ++field) {
			if (!enriched.contains(field.key())
				|| !PreservesCapturedJson(field.value(), enriched.value(field.key()), open)) {
				return false;
			}
		}
		return true;
	} else if (open && before.isArray()) {
		const auto captured = before.toArray();
		const auto enriched = after.toArray();
		if (captured.size() > enriched.size()) {
			return false;
		}
		for (auto index = qsizetype(0); index != captured.size(); ++index) {
			if (!PreservesCapturedJson(captured[index], enriched[index], true)) {
				return false;
			}
		}
		return true;
	}
	return before == after;
}

bool HydrationPreservesPath(const QJsonValue &before, const QJsonValue &after,
		std::vector<QString> &path, const std::vector<ArchiveMissingSlot> &declaredSlots) {
	const auto declared = std::any_of(declaredSlots.begin(), declaredSlots.end(),
		[&](const auto &slot) { return slot.path == path; });
	if (declared) {
		return PreservesCapturedJson(before, after, true, true);
	} else if (!before.isObject() || !after.isObject()) {
		return before == after;
	}
	const auto captured = before.toObject();
	const auto enriched = after.toObject();
	for (auto field = captured.begin(); field != captured.end(); ++field) {
		if (!enriched.contains(field.key())) {
			return false;
		}
	}
	for (auto field = enriched.begin(); field != enriched.end(); ++field) {
		path.push_back(field.key());
		const auto preserved = HydrationPreservesPath(captured.value(field.key()), field.value(), path, declaredSlots);
		path.pop_back();
		if (!preserved) {
			return false;
		}
	}
	return true;
}

QJsonValue ContentPath(const ArchiveContent &content, const std::vector<QString> &path) {
	auto value = QJsonValue(ContentObject(content));
	for (const auto &part : path) {
		value = value.toObject().value(part);
	}
	return value;
}

bool IsFilledSlot(const QJsonValue &value) {
	return !value.isUndefined() && !value.isNull()
		&& (!value.isString() || !value.toString().isEmpty())
		&& (!value.isArray() || !value.toArray().isEmpty())
		&& (!value.isObject() || !value.toObject().isEmpty());
}

bool HydrationPreservesKnownContent(const ArchiveVersion &version,
		const ArchiveEvent &event) {
	const auto &before = version.content;
	const auto &after = *event.content;
	const auto remaining = EventMissingSlots(event);
	if (remaining.revision != version.revision
		|| remaining.preserved != version.missing.preserved
		|| before.media.size() != after.media.size()) {
		return false;
	}
	for (const auto &slot : remaining.content) {
		if (std::find(version.missing.content.begin(), version.missing.content.end(), slot)
			== version.missing.content.end()) {
			return false;
		}
	}
	for (const auto &slot : version.missing.content) {
		if (std::find(remaining.content.begin(), remaining.content.end(), slot) == remaining.content.end()
			&& !IsFilledSlot(ContentPath(after, slot.path))) {
			return false;
		}
	}
	for (const auto &identity : remaining.media) {
		if (std::find(version.missing.media.begin(), version.missing.media.end(), identity)
			== version.missing.media.end()) {
			return false;
		}
	}
	auto captured = ContentObject(before);
	auto enriched = ContentObject(after);
	captured.remove(u"media"_q);
	enriched.remove(u"media"_q);
	auto path = std::vector<QString>();
	if (!HydrationPreservesPath(captured, enriched, path, version.missing.content)) {
		return false;
	}
	for (auto index = size_t(0); index != before.media.size(); ++index) {
		const auto &old = before.media[index];
		const auto &current = after.media[index];
		const auto incomplete = std::find(version.missing.media.begin(), version.missing.media.end(), old.identity)
			!= version.missing.media.end();
		if (old.identity != current.identity || old.kind != current.kind
			|| !PreservesCapturedJson(old.attributes, current.attributes, incomplete)
			|| !PreservesCapturedJson(old.preserved, current.preserved, incomplete)
			|| (incomplete && std::find(remaining.media.begin(), remaining.media.end(), old.identity)
				== remaining.media.end() && current.attributes.isEmpty() && current.preserved.isEmpty())) {
			return false;
		}
	}
	return true;
}

void ReplaceBody(ArchiveVersion &version, const ArchiveEvent &event) {
	auto media = std::vector<ArchiveMediaReference>();
	for (const auto &descriptor : event.content->media) {
		const auto old = Reference(version, descriptor.identity);
		media.push_back(old ? *old : ArchiveMediaReference{ descriptor.identity });
	}
	version.content = *event.content;
	version.missing = EventMissingSlots(event);
	version.completeness = event.completeness;
	version.observation = event.observation;
	version.media = std::move(media);
	if (event.source) {
		version.source = event.source;
	} else if (version.source
		&& (version.source->revision != version.revision
			|| version.source->contentHash != ArchiveContentHash(version.content)
			|| version.source->representation != version.observation.representation)) {
		version.source.reset();
	}
}

template <typename Accept>
void ShareKnownMedia(ArchiveState &target, const ArchiveState &source, Accept accept) {
	auto available = std::map<std::pair<QString, QString>, QString>();
	for (const auto &message : source.messages) {
		for (const auto &version : message.versions) {
			for (const auto &ref : version.media) {
				if (ref.state != ArchiveMediaState::Complete) {
					continue;
				}
				for (const auto &descriptor : version.content.media) {
					if (descriptor.identity == ref.identity) {
						available.try_emplace(std::pair(ref.identity, descriptor.kind), ref.blobId);
					}
				}
			}
		}
	}
	for (auto &message : target.messages) {
		for (auto &version : message.versions) {
			for (auto &ref : version.media) {
				if (ref.state == ArchiveMediaState::Complete || !accept(message, version, ref)) {
					continue;
				}
				for (const auto &descriptor : version.content.media) {
					if (descriptor.identity != ref.identity) {
						continue;
					}
					const auto i = available.find({ ref.identity, descriptor.kind });
					if (i != available.end()) {
						ref.state = ArchiveMediaState::Complete;
						ref.blobId = i->second;
					}
				}
			}
		}
	}
}

void Reconcile(ArchiveState &state, std::vector<QString> &cancelled,
		std::vector<QString> &revoked) {
	for (auto &job : state.jobs) {
		if (job.state != ArchiveJobState::Staged) {
			continue;
		}
		const auto version = Version(state, job.key, job.version);
		const auto reference = version ? Reference(*version, job.mediaIdentity) : nullptr;
		if (!reference || reference->state != ArchiveMediaState::Staged
			|| !IsGeneration(state, job.key, job.generation)) {
			job.state = ArchiveJobState::Cancelled;
			cancelled.push_back(job.id);
			if (reference && reference->state == ArchiveMediaState::Staged) {
				reference->state = ArchiveMediaState::Missing;
			}
		}
	}
	for (auto &lease : state.leases) {
		if (lease.revoked) {
			continue;
		}
		const auto version = Version(state, lease.key, lease.version);
		if (!version || std::none_of(version->media.begin(), version->media.end(),
			[&](const auto &ref) { return ref.blobId == lease.blobId; })) {
			lease.revoked = true;
			revoked.push_back(lease.id);
		}
	}
}

ArchiveTransaction Transaction(const ArchiveState &before, ArchiveState after) {
	if (before == after) {
		return Refuse(ArchivePlanStatus::NoChange);
	} else if (before.revision == std::numeric_limits<uint64_t>::max()) {
		return Refuse(ArchivePlanStatus::Exhausted);
	}
	auto result = ArchiveTransaction();
	Reconcile(after, result.releaseStagedJobs, result.revokeReaders);
	after.revision = before.revision + 1;
	if (!IsArchiveState(after)) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	result.status = ArchivePlanStatus::Ready;
	result.expected = before;
	result.next = std::move(after);
	result.writes.push_back({ ArchiveStoreOperation::CompareRevision });
	for (const auto &message : result.next.messages) {
		const auto old = FindArchiveMessage(before, message.key);
		if (!old || *old != message) {
			result.writes.push_back({ ArchiveStoreOperation::PutMessage, message.key });
		}
	}
	for (const auto &blob : result.next.blobs) {
		const auto old = std::find_if(before.blobs.begin(), before.blobs.end(),
			[&](const auto &entry) { return entry.id == blob.id; });
		if (old == before.blobs.end() || *old != blob) {
			result.writes.push_back({ ArchiveStoreOperation::PutBlob, {}, blob.id });
		}
	}
	for (const auto &blob : before.blobs) {
		if (std::none_of(result.next.blobs.begin(), result.next.blobs.end(),
			[&](const auto &entry) { return entry.id == blob.id; })) {
			result.writes.push_back({ ArchiveStoreOperation::EraseBlob, {}, blob.id });
		}
	}
	for (const auto &job : result.next.jobs) {
		const auto old = std::find_if(before.jobs.begin(), before.jobs.end(),
			[&](const auto &entry) { return entry.id == job.id; });
		if (old == before.jobs.end() || *old != job) {
			result.writes.push_back({ ArchiveStoreOperation::PutJob, {}, job.id });
		}
	}
	for (const auto &lease : result.next.leases) {
		const auto old = std::find_if(before.leases.begin(), before.leases.end(),
			[&](const auto &entry) { return entry.id == lease.id; });
		if (old == before.leases.end() || *old != lease) {
			result.writes.push_back({ ArchiveStoreOperation::PutLease, {}, lease.id });
		}
	}
	for (const auto &lease : before.leases) {
		if (std::none_of(result.next.leases.begin(), result.next.leases.end(),
			[&](const auto &entry) { return entry.id == lease.id; })) {
			result.writes.push_back({ ArchiveStoreOperation::EraseLease, {}, lease.id });
		}
	}
	if (before.accountGeneration != result.next.accountGeneration
		|| before.chatGenerations != result.next.chatGenerations) {
		result.writes.push_back({ ArchiveStoreOperation::PutGenerations });
	}
	result.writes.push_back({ ArchiveStoreOperation::PutRevision });
	return result;
}

bool InScope(const ArchivePurgeScope &scope, const ArchiveMessageKey &key) {
	if (scope.account != key.account) {
		return false;
	} else if (!scope.chat || SameArchiveChat(*scope.chat, key.chat)) {
		return true;
	}
	return std::any_of(scope.migrationAliases.begin(), scope.migrationAliases.end(),
		[&](const auto &chat) { return SameArchiveChat(chat, key.chat); });
}

}

ArchiveTransaction PlanArchiveEvent(const ArchiveState &state, const ArchiveEvent &event) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	if (!IsArchiveMessageKey(event.key) || event.key.account != state.account
		|| !IsArchiveToken(event.id) || !IsOrigin(event.origin)
		|| !IsCompleteness(event.completeness) || !IsObservation(event.observation)
		|| event.observation.representation != event.key.representation
		|| !IsMigrationAliases(event.key.chat, event.migrationAliases)
		|| (event.content && (!IsContent(*event.content)
			|| !IsContentCompleteness(*event.content, event.completeness)))
		|| (event.source && !IsSource(*event.source))
		|| ((!event.targetVersion.isEmpty()) && !IsArchiveToken(event.targetVersion))
		|| ((!event.targetRevision.isEmpty()) && !IsArchiveToken(event.targetRevision))
		|| ((!event.expectedContentHash.isEmpty()) && !IsArchiveHash(event.expectedContentHash))) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	const auto bodyEvent = event.origin == ArchiveOrigin::Baseline
		|| event.origin == ArchiveOrigin::AuthoritativeEdit
		|| event.origin == ArchiveOrigin::Hydration;
	if (event.content.has_value() != bodyEvent
		|| (event.source && !bodyEvent && event.origin != ArchiveOrigin::SendMetadata)
		|| ((IsDeletion(event.origin) || event.origin == ArchiveOrigin::AuthoritativeEdit)
			&& !event.order)) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	if (!IsGeneration(state, event.key, event.generation)) {
		return Refuse(ArchivePlanStatus::Stale);
	}
	if (bodyEvent && !IsMissingSlots(EventMissingSlots(event), *event.content,
		event.completeness, event.origin == ArchiveOrigin::Hydration ? event.targetRevision : event.id)) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	const auto fingerprint = EventFingerprint(event);
	if (fingerprint.isEmpty()) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	auto next = state;
	auto message = Message(next, event.key);
	if (message) {
		for (const auto &receipt : message->receipts) {
			if (receipt.id == event.id) {
				return Refuse(receipt.fingerprint == fingerprint
					? ArchivePlanStatus::NoChange : ArchivePlanStatus::Conflict);
			}
		}
		if (bodyEvent && message->payloadPurged) {
			return Refuse(ArchivePlanStatus::Purged);
		}
		if (event.origin != ArchiveOrigin::Hydration && event.origin != ArchiveOrigin::SendMetadata) {
			if (event.order < message->order) {
				return Refuse(ArchivePlanStatus::Stale);
			} else if (event.order == message->order
				&& (IsDeletion(event.origin) || event.origin == ArchiveOrigin::AuthoritativeEdit)) {
				return Refuse(ArchivePlanStatus::Conflict);
			}
		}
	} else {
		if (event.origin == ArchiveOrigin::Hydration || event.origin == ArchiveOrigin::SendMetadata) {
			return Refuse(ArchivePlanStatus::NotFound);
		}
		next.messages.push_back(ArchiveMessage{ event.key });
		message = &next.messages.back();
	}
	if (event.origin == ArchiveOrigin::Hydration) {
		const auto version = Version(next, event.key, event.targetVersion);
		if (!version || event.targetRevision != version->revision
			|| event.expectedContentHash != ArchiveContentHash(version->content)) {
			return Refuse(ArchivePlanStatus::Stale);
		}
		if (!HydrationPreservesKnownContent(*version, event)) {
			return Refuse(ArchivePlanStatus::Conflict);
		}
		if (version->completeness == ArchiveCompleteness::Complete
			&& (event.completeness != ArchiveCompleteness::Complete
				|| !SameArchiveContent(version->content, *event.content))) {
			return Refuse(ArchivePlanStatus::Conflict);
		}
		ReplaceBody(*version, event);
	} else if (event.origin == ArchiveOrigin::SendMetadata) {
		if (message->versions.empty() || event.targetVersion != message->versions.back().id
			|| event.targetRevision != message->versions.back().revision) {
			return Refuse(ArchivePlanStatus::Stale);
		}
		auto &head = message->versions.back();
		if (event.source) {
			if (message->payloadPurged) { return Refuse(ArchivePlanStatus::Purged); }
			const auto &source = *event.source;
			if (!source.binding || !SourceOwnedBy(source, event.key, head.id)
				|| source.contentHash != ArchiveContentHash(head.content)
				|| event.expectedContentHash != source.contentHash
				|| source.revision != head.revision
				|| source.representation != head.observation.representation
				|| head.completeness != ArchiveCompleteness::Complete) {
				return Refuse(ArchivePlanStatus::Stale);
			}
			if (head.source) {
				auto previous = *head.source;
				if (!previous.binding) { previous.binding = source.binding; }
				if (previous != source) { return Refuse(ArchivePlanStatus::Conflict); }
			}
			head.source = source;
		}
		auto &observation = head.observation;
		for (auto i = event.observation.context.begin(); i != event.observation.context.end(); ++i) {
			observation.context.insert(i.key(), i.value());
		}
		for (auto i = event.observation.transport.begin(); i != event.observation.transport.end(); ++i) {
			observation.transport.insert(i.key(), i.value());
		}
	} else if (IsDeletion(event.origin)) {
		message->deleted = true;
		message->deletionOrigin = event.origin;
		message->deletedObservedAt = event.observation.observedAt;
		message->order = event.order;
	} else {
		if (message->versions.empty()) {
			message->versions.push_back(MakeVersion(event,
				event.origin == ArchiveOrigin::AuthoritativeEdit));
		} else if (SameArchiveContent(message->versions.back().content, *event.content)) {
			auto &head = message->versions.back();
			head.observation = event.observation;
			if (event.origin == ArchiveOrigin::AuthoritativeEdit || event.order > message->order) {
				head.revision = event.id;
				head.missing = EventMissingSlots(event);
				if (head.completeness == ArchiveCompleteness::Complete
					&& (!head.missing.content.empty() || !head.missing.media.empty())) {
					return Refuse(ArchivePlanStatus::Conflict);
				}
				head.source = event.source;
			} else if ((!event.missing.content.empty() || !event.missing.media.empty())
				&& EventMissingSlots(event) != head.missing) {
				return Refuse(ArchivePlanStatus::Conflict);
			} else if (event.source) {
				head.source = event.source;
			}
			if (event.completeness == ArchiveCompleteness::Complete) {
				if (!head.missing.content.empty() || !head.missing.media.empty()) {
					return Refuse(ArchivePlanStatus::Conflict);
				}
				head.completeness = event.completeness;
			}
		} else if (event.origin == ArchiveOrigin::AuthoritativeEdit) {
			message->versions.push_back(MakeVersion(event, false));
		} else {
			const auto &head = message->versions.back();
			if (event.targetVersion != head.id
				|| event.targetRevision != head.revision
				|| event.expectedContentHash != ArchiveContentHash(head.content)) {
				return Refuse(ArchivePlanStatus::Stale);
			}
			if (event.order == message->order) {
				return Refuse(ArchivePlanStatus::Conflict);
			}
			message->versions.push_back(MakeVersion(event, false));
		}
		message->order = event.order;
	}
	message->receipts.push_back({ event.id, fingerprint });
	for (const auto &alias : event.migrationAliases) {
		if (std::none_of(message->migrationAliases.begin(), message->migrationAliases.end(),
			[&](const auto &entry) { return SameArchiveChat(alias, entry); })) {
			message->migrationAliases.push_back(alias);
		}
	}
	ShareKnownMedia(next, next, [&](const auto &entry, const auto &, const auto &) {
		return SameArchiveMessage(entry.key, event.key);
	});
	return Transaction(state, std::move(next));
}

ArchiveTransaction PlanArchivePurge(const ArchiveState &state, const ArchivePurgeRequest &request) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	const auto &scope = request.scope;
	if (scope.account != state.account || !AccountKeyText(scope.account.environment, scope.account.userId)
		|| (request.kind != ArchivePurgeKind::Trashbin && request.kind != ArchivePurgeKind::EditHistory)
		|| (scope.chat ? (!IsArchiveChatKey(*scope.chat)
			|| !IsMigrationAliases(*scope.chat, scope.migrationAliases)) : !scope.migrationAliases.empty())) {
		return Refuse(ArchivePlanStatus::Invalid);
	} else if (request.expectedRevision != state.revision) {
		return Refuse(ArchivePlanStatus::Stale);
	}
	auto next = state;
	ShareKnownMedia(next, state, [&](const auto &message, const auto &version, const auto &) {
		return InScope(scope, message.key)
			&& (request.kind == ArchivePurgeKind::EditHistory
				? version.id == message.versions.back().id : !message.deleted);
	});
	if (!scope.chat) {
		if (next.accountGeneration == std::numeric_limits<uint64_t>::max()) {
			return Refuse(ArchivePlanStatus::Exhausted);
		}
		++next.accountGeneration;
	} else {
		auto chats = scope.migrationAliases;
		chats.push_back(*scope.chat);
		for (const auto &chat : chats) {
			auto i = std::find_if(next.chatGenerations.begin(), next.chatGenerations.end(),
				[&](const auto &entry) { return SameArchiveChat(chat, entry.chat); });
			if (i == next.chatGenerations.end()) {
				next.chatGenerations.push_back({ chat, 1 });
			} else if (i->generation == std::numeric_limits<uint64_t>::max()) {
				return Refuse(ArchivePlanStatus::Exhausted);
			} else {
				++i->generation;
			}
		}
	}
	for (auto &message : next.messages) {
		if (!InScope(scope, message.key)) {
			continue;
		}
		if (request.kind == ArchivePurgeKind::Trashbin && message.deleted) {
			message.versions.clear();
			message.payloadPurged = true;
		} else if (request.kind == ArchivePurgeKind::EditHistory && message.versions.size() > 1) {
			message.versions.erase(message.versions.begin(), message.versions.end() - 1);
		}
	}
	for (auto &lease : next.leases) {
		if (InScope(scope, lease.key)) {
			lease.revoked = true;
		}
	}
	auto result = Transaction(state, std::move(next));
	if (result) {
		for (const auto &lease : state.leases) {
			if (!lease.revoked && InScope(scope, lease.key)
				&& std::find(result.revokeReaders.begin(), result.revokeReaders.end(), lease.id)
					== result.revokeReaders.end()) {
				result.revokeReaders.push_back(lease.id);
			}
		}
	}
	return result;
}

ArchiveTransaction PlanArchiveMediaStage(const ArchiveState &state, const ArchiveMediaJob &job) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	if (!IsArchiveToken(job.id) || !IsArchiveMessageKey(job.key) || job.key.account != state.account
		|| !IsArchiveToken(job.version) || !IsArchiveToken(job.mediaIdentity)
		|| (!job.expectedHash.isEmpty() && !IsArchiveHash(job.expectedHash))
		|| job.state != ArchiveJobState::Staged || !job.blobId.isEmpty()
		|| job.completedSize != 0 || !job.failure.isEmpty()) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	for (const auto &old : state.jobs) {
		if (old.id == job.id) {
			auto declaration = old;
			declaration.state = ArchiveJobState::Staged;
			declaration.blobId.clear();
			declaration.completedSize = 0;
			declaration.failure.clear();
			if (declaration != job) {
				return Refuse(ArchivePlanStatus::Conflict);
			}
			return Refuse(old.state == ArchiveJobState::Cancelled
				? ArchivePlanStatus::Stale : ArchivePlanStatus::NoChange);
		}
	}
	if (!IsGeneration(state, job.key, job.generation)) {
		return Refuse(ArchivePlanStatus::Stale);
	}
	auto next = state;
	ShareKnownMedia(next, state, [&](const auto &message, const auto &version, const auto &ref) {
		return SameArchiveMessage(message.key, job.key)
			&& version.id == job.version && ref.identity == job.mediaIdentity;
	});
	const auto version = Version(next, job.key, job.version);
	const auto reference = version ? Reference(*version, job.mediaIdentity) : nullptr;
	if (!reference) {
		return Refuse(ArchivePlanStatus::NotFound);
	} else if (reference->state == ArchiveMediaState::Complete) {
		return Transaction(state, std::move(next));
	} else if (reference->state == ArchiveMediaState::Staged) {
		return Refuse(ArchivePlanStatus::Conflict);
	}
	reference->state = ArchiveMediaState::Staged;
	next.jobs.push_back(job);
	return Transaction(state, std::move(next));
}

ArchiveTransaction PlanArchiveMediaCompletion(const ArchiveState &state,
		const QString &jobId, const ArchiveMediaResult &result) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	auto next = state;
	const auto i = std::find_if(next.jobs.begin(), next.jobs.end(),
		[&](const auto &job) { return job.id == jobId; });
	if (i == next.jobs.end()) {
		return Refuse(ArchivePlanStatus::NotFound);
	} else if (i->state == ArchiveJobState::Cancelled || !IsGeneration(state, i->key, i->generation)) {
		return Refuse(ArchivePlanStatus::Stale);
	} else if (i->state != ArchiveJobState::Staged) {
		const auto matches = result.success
			? (i->state == ArchiveJobState::Complete && i->blobId == result.blobId
				&& result.verifiedHash == result.blobId
				&& i->completedSize == result.size && result.failure.isEmpty())
			: (i->state == ArchiveJobState::Failed && i->failure == result.failure
				&& result.blobId.isEmpty() && result.verifiedHash.isEmpty() && !result.size);
		return Refuse(matches ? ArchivePlanStatus::NoChange : ArchivePlanStatus::Conflict);
	}
	const auto version = Version(next, i->key, i->version);
	const auto reference = version ? Reference(*version, i->mediaIdentity) : nullptr;
	if (!reference || reference->state != ArchiveMediaState::Staged) {
		return Refuse(ArchivePlanStatus::Stale);
	}
	if (!result.success) {
		if (!IsArchiveToken(result.failure) || !result.blobId.isEmpty()
			|| !result.verifiedHash.isEmpty() || result.size) {
			return Refuse(ArchivePlanStatus::Invalid);
		}
		i->state = ArchiveJobState::Failed;
		i->failure = result.failure;
		reference->state = ArchiveMediaState::Failed;
	} else {
		if (!IsArchiveHash(result.blobId) || result.verifiedHash != result.blobId
			|| (!i->expectedHash.isEmpty() && result.blobId != i->expectedHash)
			|| (i->expectedSize && *i->expectedSize != result.size)
			|| (!result.size && (!i->expectedSize || *i->expectedSize))
			|| !result.failure.isEmpty()) {
			return Refuse(ArchivePlanStatus::Invalid);
		}
		const auto blob = std::find_if(next.blobs.begin(), next.blobs.end(),
			[&](const auto &entry) { return entry.id == result.blobId; });
		if (blob != next.blobs.end()) {
			if (blob->state != ArchiveBlobState::Complete || blob->size != result.size) {
				return Refuse(ArchivePlanStatus::Conflict);
			}
		} else {
			next.blobs.push_back({ result.blobId, result.size });
		}
		i->state = ArchiveJobState::Complete;
		i->blobId = result.blobId;
		i->completedSize = result.size;
		reference->state = ArchiveMediaState::Complete;
		reference->blobId = result.blobId;
	}
	ShareKnownMedia(next, next, [&](const auto &, const auto &, const auto &ref) {
		return ref.identity == i->mediaIdentity;
	});
	return Transaction(state, std::move(next));
}

ArchiveTransaction PlanArchiveReaderAcquire(const ArchiveState &state, const ArchiveReaderLease &lease) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	if (!IsArchiveToken(lease.id) || lease.revoked || lease.key.account != state.account) {
		return Refuse(ArchivePlanStatus::Invalid);
	}
	if (!IsGeneration(state, lease.key, lease.generation)) {
		return Refuse(ArchivePlanStatus::Stale);
	}
	for (const auto &old : state.leases) {
		if (old.id == lease.id) {
			return Refuse(old == lease ? ArchivePlanStatus::NoChange : ArchivePlanStatus::Conflict);
		}
	}
	const auto version = FindArchiveVersion(state, lease.key, lease.version);
	if (!version || std::none_of(version->media.begin(), version->media.end(),
		[&](const auto &ref) {
			return ref.state == ArchiveMediaState::Complete && ref.blobId == lease.blobId;
		})) {
		return Refuse(ArchivePlanStatus::NotFound);
	}
	auto next = state;
	next.leases.push_back(lease);
	return Transaction(state, std::move(next));
}

ArchiveTransaction PlanArchiveReaderRelease(const ArchiveState &state, const QString &leaseId) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	auto next = state;
	const auto removed = std::erase_if(next.leases, [&](const auto &lease) { return lease.id == leaseId; });
	return removed ? Transaction(state, std::move(next)) : Refuse(ArchivePlanStatus::NoChange);
}

ArchiveBlobReferences CountArchiveBlobReferences(const ArchiveState &state, const QString &blobId) {
	auto result = ArchiveBlobReferences();
	for (const auto &message : state.messages) {
		for (const auto &version : message.versions) {
			for (const auto &ref : version.media) {
				if (ref.state == ArchiveMediaState::Complete && ref.blobId == blobId) {
					++result.versions;
				}
			}
		}
	}
	for (const auto &lease : state.leases) {
		if (lease.blobId == blobId) {
			++result.readers;
		}
	}
	return result;
}

ArchiveTransaction PlanArchiveGarbageCollection(const ArchiveState &state) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	auto next = state;
	auto ids = std::vector<QString>();
	for (auto &blob : next.blobs) {
		const auto refs = CountArchiveBlobReferences(state, blob.id);
		if (!refs.versions && !refs.readers) {
			blob.state = ArchiveBlobState::GarbagePending;
			ids.push_back(blob.id);
		}
	}
	if (ids.empty()) {
		return Refuse(ArchivePlanStatus::NoChange);
	}
	auto result = Transaction(state, std::move(next));
	if (result.status == ArchivePlanStatus::NoChange) {
		result.status = ArchivePlanStatus::Ready;
		result.expected = state;
		result.next = state;
	}
	if (result) {
		result.deleteOwnedBlobs = std::move(ids);
	}
	return result;
}

ArchiveTransaction CompleteArchiveGarbageCollection(const ArchiveState &state,
		const std::vector<QString> &removedBlobIds) {
	if (const auto error = CheckState(state)) {
		return *error;
	}
	auto next = state;
	for (const auto &id : removedBlobIds) {
		const auto i = std::find_if(next.blobs.begin(), next.blobs.end(),
			[&](const auto &blob) { return blob.id == id; });
		if (i == next.blobs.end()) {
			continue;
		}
		const auto refs = CountArchiveBlobReferences(next, id);
		if (i->state != ArchiveBlobState::GarbagePending || refs.versions || refs.readers) {
			return Refuse(ArchivePlanStatus::Conflict);
		}
		next.blobs.erase(i);
	}
	return Transaction(state, std::move(next));
}

ArchiveCompletion CompleteArchiveTransaction(const ArchiveState &current,
		const ArchiveTransaction &transaction, bool committed) {
	auto result = ArchiveCompletion();
	result.state = current;
	if (!transaction || !IsArchiveState(transaction.expected) || !IsArchiveState(transaction.next)
		|| transaction.expected.account != transaction.next.account
		|| (transaction.expected.revision == std::numeric_limits<uint64_t>::max()
			? transaction.next != transaction.expected
			: (transaction.next.revision != transaction.expected.revision + 1
				&& transaction.next != transaction.expected))) {
		return result;
	} else if (!committed) {
		result.status = ArchiveCommitStatus::NotCommitted;
	} else if (current == transaction.next) {
		result.status = ArchiveCommitStatus::AlreadyApplied;
	} else if (current != transaction.expected) {
		result.status = ArchiveCommitStatus::Stale;
	} else {
		result.status = ArchiveCommitStatus::Applied;
		result.state = transaction.next;
	}
	return result;
}

}
