/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_model.h"

#include "purple/purple_archive_internal.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

#include <algorithm>

namespace Purple {
namespace {

bool SafeObject(const QJsonObject &object) {
	return bool(CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

bool IsAccount(const AccountKey &account) {
	return AccountKeyText(account.environment, account.userId).has_value();
}

template <typename T>
bool InRange(T value, T last) {
	return int(value) >= 0 && int(value) <= int(last);
}

bool UniqueChats(const std::vector<ArchiveChatKey> &chats) {
	for (auto i = size_t(0); i != chats.size(); ++i) {
		if (!IsArchiveChatKey(chats[i])) {
			return false;
		}
		for (auto j = size_t(0); j != i; ++j) {
			if (SameArchiveChat(chats[i], chats[j])) {
				return false;
			}
		}
	}
	return true;
}

}

bool IsArchiveToken(const QString &token) {
	if (token.isEmpty() || token.size() > 256) {
		return false;
	}
	for (const auto ch : token) {
		const auto c = ch.unicode();
		if (!((c >= 'a' && c <= 'z')
			|| (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == ':' || c == '.')) {
			return false;
		}
	}
	return true;
}

bool IsArchiveHash(const QString &hash) {
	return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](QChar ch) {
		return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
	});
}

bool IsArchiveChatKey(const ArchiveChatKey &key) {
	return InRange(key.kind, ArchiveChatKind::Secret) && key.id > 0
		&& SafeObject(key.preserved);
}

bool IsArchiveMessageKey(const ArchiveMessageKey &key) {
	return IsAccount(key.account)
		&& SafeObject(key.preserved)
		&& IsArchiveChatKey(key.chat)
		&& InRange(key.collection, ArchiveCollection::QuickReply)
		&& IsArchiveToken(key.representation)
		&& (key.collection == ArchiveCollection::QuickReply
			? key.collectionId > 0 : key.collectionId == 0)
		&& (key.chat.kind == ArchiveChatKind::Secret
			? key.messageId != 0
			: key.messageId > 0);
}

bool SameArchiveChat(const ArchiveChatKey &a, const ArchiveChatKey &b) {
	return a.kind == b.kind && a.id == b.id;
}

bool SameArchiveMessage(const ArchiveMessageKey &a, const ArchiveMessageKey &b) {
	return a.account == b.account
		&& SameArchiveChat(a.chat, b.chat)
		&& a.messageId == b.messageId
		&& a.collection == b.collection
		&& a.collectionId == b.collectionId
		&& a.representation == b.representation;
}

namespace ArchiveInternal {

bool IsMigrationAliases(const ArchiveChatKey &chat,
		const std::vector<ArchiveChatKey> &aliases) {
	if (!UniqueChats(aliases)) {
		return false;
	}
	for (const auto &alias : aliases) {
		if (SameArchiveChat(chat, alias)
			|| (chat.kind != ArchiveChatKind::Group && chat.kind != ArchiveChatKind::Channel)
			|| (alias.kind != ArchiveChatKind::Group && alias.kind != ArchiveChatKind::Channel)) {
			return false;
		}
	}
	return true;
}


QJsonObject ContentObject(const ArchiveContent &content) {
	auto media = QJsonArray();
	for (const auto &entry : content.media) {
		auto object = entry.preserved;
		object.insert(u"identity"_q, entry.identity);
		object.insert(u"kind"_q, entry.kind);
		object.insert(u"attributes"_q, entry.attributes);
		media.push_back(object);
	}
	auto object = content.preserved;
	object.insert(u"text"_q, content.text);
	object.insert(u"caption"_q, content.caption);
	object.insert(u"entities"_q, content.entities);
	object.insert(u"rich"_q, content.rich);
	object.insert(u"media"_q, media);
	object.insert(u"opaqueFormat"_q, content.opaqueFormat);
	object.insert(u"opaque"_q, QString::fromLatin1(content.opaque.toBase64()));
	return object;
}

QJsonObject MissingSlotsObject(const ArchiveMissingSlots &missing) {
	auto object = missing.preserved;
	auto declaredSlots = QJsonArray();
	for (const auto &slot : missing.content) {
		auto entry = slot.preserved;
		auto path = QJsonArray();
		for (const auto &part : slot.path) {
			path.push_back(part);
		}
		entry.insert(u"path"_q, path);
		declaredSlots.push_back(entry);
	}
	auto media = QJsonArray();
	for (const auto &identity : missing.media) {
		media.push_back(identity);
	}
	object.insert(u"revision"_q, missing.revision);
	object.insert(u"content"_q, declaredSlots);
	object.insert(u"media"_q, media);
	return object;
}

bool IsMissingSlots(const ArchiveMissingSlots &missing,
		const ArchiveContent &content, ArchiveCompleteness completeness,
		const QString &revision) {
	if (missing.revision != revision || !SafeObject(missing.preserved)
		|| missing.preserved.contains(u"revision"_q)
		|| missing.preserved.contains(u"content"_q) || missing.preserved.contains(u"media"_q)
		|| missing.content.size() > 256 || missing.media.size() > content.media.size()
		|| (completeness != ArchiveCompleteness::Partial
			&& (!missing.content.empty() || !missing.media.empty()))) {
		return false;
	}
	const auto root = ContentObject(content);
	for (auto index = size_t(0); index != missing.content.size(); ++index) {
		const auto &slot = missing.content[index];
		if (slot.path.empty() || slot.path.size() > 32 || !SafeObject(slot.preserved)
			|| slot.preserved.contains(u"path"_q)
			|| slot.path.front() == u"media"_q) {
			return false;
		}
		auto value = QJsonValue(root);
		for (auto part = size_t(0); part != slot.path.size(); ++part) {
			if (slot.path[part].isEmpty() || slot.path[part].size() > 1024 || !value.isObject()) {
				return false;
			}
			value = value.toObject().value(slot.path[part]);
		}
		for (auto before = size_t(0); before != index; ++before) {
			const auto &other = missing.content[before].path;
			const auto common = std::min(slot.path.size(), other.size());
			if (std::equal(slot.path.begin(), slot.path.begin() + common, other.begin())) {
				return false;
			}
		}
	}
	auto identities = QSet<QString>();
	for (const auto &identity : missing.media) {
		if (identities.contains(identity)
			|| std::none_of(content.media.begin(), content.media.end(),
				[&](const auto &media) { return media.identity == identity; })) {
			return false;
		}
		identities.insert(identity);
	}
	return true;
}

bool IsContent(const ArchiveContent &content) {
	auto identities = QSet<QString>();
	if (content.opaque.isEmpty() != content.opaqueFormat.isEmpty()
		|| (!content.opaqueFormat.isEmpty() && !IsArchiveToken(content.opaqueFormat))) {
		return false;
	}
	for (const auto &media : content.media) {
		if (!IsArchiveToken(media.identity)
			|| !IsArchiveToken(media.kind)
			|| identities.contains(media.identity)) {
			return false;
		}
		identities.insert(media.identity);
	}
	return !CanonicalArchiveContent(content).isEmpty();
}

bool IsObservation(const ArchiveObservation &observation) {
	return SafeObject(observation.context)
		&& SafeObject(observation.transport)
		&& SafeObject(observation.preserved)
		&& observation.observedAt >= 0
		&& observation.serverTime >= 0
		&& observation.topicId >= 0
		&& IsArchiveToken(observation.representation)
		&& (observation.payloadFormat.isEmpty() == observation.payload.isEmpty())
		&& (observation.payloadFormat.isEmpty()
			|| IsArchiveToken(observation.payloadFormat));
}

bool IsSource(const ArchiveSource &source) {
	return SafeObject(source.preserved)
		&& InRange(source.kind, ArchiveSourceKind::ReceivedMarkdownFile)
		&& IsArchiveHash(source.contentHash)
		&& IsArchiveToken(source.revision)
		&& IsArchiveToken(source.representation);
}

bool IsOrigin(ArchiveOrigin origin) {
	return InRange(origin, ArchiveOrigin::SendMetadata);
}

bool IsCompleteness(ArchiveCompleteness completeness) {
	return InRange(completeness, ArchiveCompleteness::Opaque);
}

bool IsContentCompleteness(const ArchiveContent &content,
		ArchiveCompleteness completeness) {
	return IsCompleteness(completeness)
		&& (completeness != ArchiveCompleteness::Opaque
			|| !content.opaque.isEmpty() || !content.rich.isNull());
}

bool IsDeletion(ArchiveOrigin origin) {
	return origin == ArchiveOrigin::AuthoritativeDelete
		|| origin == ArchiveOrigin::LocalDelete
		|| origin == ArchiveOrigin::MediaExpiry
		|| origin == ArchiveOrigin::MediaConsumption;
}

bool IsGeneration(const ArchiveState &state,
		const ArchiveMessageKey &key, const ArchiveGeneration &generation) {
	return key.account == state.account
		&& generation == ArchiveGenerationFor(state, key.chat);
}

}

QByteArray CanonicalArchiveContent(const ArchiveContent &content) {
	const auto result = CanonicalizeSyncJson(QJsonDocument(
		ArchiveInternal::ContentObject(content)).toJson(QJsonDocument::Compact));
	return result ? result.canonical : QByteArray();
}

QString ArchiveContentHash(const ArchiveContent &content) {
	const auto bytes = CanonicalArchiveContent(content);
	return bytes.isEmpty() ? QString() : QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool SameArchiveContent(const ArchiveContent &a, const ArchiveContent &b) {
	const auto bytes = CanonicalArchiveContent(a);
	return !bytes.isEmpty() && bytes == CanonicalArchiveContent(b);
}

ArchiveGeneration ArchiveGenerationFor(
		const ArchiveState &state,
		const ArchiveChatKey &chat) {
	for (const auto &entry : state.chatGenerations) {
		if (SameArchiveChat(entry.chat, chat)) {
			return { state.accountGeneration, entry.generation };
		}
	}
	return { state.accountGeneration, 0 };
}

const ArchiveMessage *FindArchiveMessage(
		const ArchiveState &state,
		const ArchiveMessageKey &key) {
	const auto i = std::find_if(state.messages.begin(), state.messages.end(), [&](const auto &m) {
		return SameArchiveMessage(m.key, key);
	});
	return i == state.messages.end() ? nullptr : &*i;
}

const ArchiveVersion *FindArchiveVersion(
		const ArchiveState &state,
		const ArchiveMessageKey &key,
		const QString &version) {
	const auto message = FindArchiveMessage(state, key);
	if (message) {
		for (const auto &entry : message->versions) {
			if (entry.id == version) {
				return &entry;
			}
		}
	}
	return nullptr;
}

bool ArchiveSourceMatches(const ArchiveVersion &version) {
	return IsArchiveToken(version.id)
		&& ArchiveInternal::IsContent(version.content)
		&& version.source
		&& ArchiveInternal::IsSource(*version.source)
		&& version.completeness == ArchiveCompleteness::Complete
		&& version.source->contentHash == ArchiveContentHash(version.content)
		&& version.source->revision == version.revision
		&& version.source->representation == version.observation.representation;
}

bool IsArchiveState(const ArchiveState &state) {
	using namespace ArchiveInternal;
	if (state.version != kArchiveSchemaVersion || !IsAccount(state.account)
		|| !SafeObject(state.preserved)) {
		return false;
	}
	auto chats = std::vector<ArchiveChatKey>();
	for (const auto &generation : state.chatGenerations) {
		if (!SafeObject(generation.preserved)) { return false; }
		chats.push_back(generation.chat);
	}
	if (!UniqueChats(chats)) {
		return false;
	}
	auto blobs = QSet<QString>();
	for (const auto &blob : state.blobs) {
		if (!SafeObject(blob.preserved) || !IsArchiveHash(blob.id)
			|| !InRange(blob.state, ArchiveBlobState::GarbagePending)
			|| blobs.contains(blob.id)) {
			return false;
		}
		blobs.insert(blob.id);
	}
	for (auto index = size_t(0); index != state.messages.size(); ++index) {
		const auto &message = state.messages[index];
		if (!IsArchiveMessageKey(message.key)
			|| message.key.account != state.account
			|| !IsMigrationAliases(message.key.chat, message.migrationAliases)
			|| !SafeObject(message.preserved)
			|| message.deletedObservedAt < 0
			|| (message.payloadPurged && (!message.versions.empty() || !message.deleted))
			|| message.deleted != message.deletionOrigin.has_value()
			|| (message.deletionOrigin && !IsDeletion(*message.deletionOrigin))) {
			return false;
		}
		for (auto before = size_t(0); before != index; ++before) {
			if (SameArchiveMessage(message.key, state.messages[before].key)) {
				return false;
			}
		}
		auto versions = QSet<QString>();
		auto previous = uint64_t(0);
		for (const auto &version : message.versions) {
			if (!IsArchiveToken(version.id) || !IsArchiveToken(version.revision)
				|| versions.contains(version.id)
				|| !SafeObject(version.preserved)
				|| version.firstObservedAt < 0
				|| (!versions.empty() && version.order <= previous)
				|| version.order > message.order
				|| (version.origin != ArchiveOrigin::Baseline
					&& version.origin != ArchiveOrigin::AuthoritativeEdit)
				|| !IsContentCompleteness(version.content, version.completeness)
				|| !IsContent(version.content)
				|| !IsMissingSlots(version.missing, version.content, version.completeness, version.revision)
				|| !IsObservation(version.observation)
				|| version.observation.representation != message.key.representation
				|| (version.source && !IsSource(*version.source))) {
				return false;
			}
			previous = version.order;
			versions.insert(version.id);
			auto media = QSet<QString>();
			for (const auto &ref : version.media) {
				const auto descriptor = std::find_if(version.content.media.begin(),
					version.content.media.end(), [&](const auto &entry) {
						return entry.identity == ref.identity;
					});
				const auto blob = std::find_if(state.blobs.begin(), state.blobs.end(),
					[&](const auto &entry) { return entry.id == ref.blobId; });
				if (!SafeObject(ref.preserved)
					|| descriptor == version.content.media.end()
					|| media.contains(ref.identity)
					|| !InRange(ref.state, ArchiveMediaState::Failed)
					|| (ref.state == ArchiveMediaState::Complete
						? (blob == state.blobs.end() || blob->state != ArchiveBlobState::Complete)
						: !ref.blobId.isEmpty())) {
					return false;
				}
				if (ref.state == ArchiveMediaState::Staged) {
					const auto jobs = std::count_if(state.jobs.begin(), state.jobs.end(), [&](const auto &job) {
						return job.state == ArchiveJobState::Staged
							&& SameArchiveMessage(job.key, message.key)
							&& job.version == version.id && job.mediaIdentity == ref.identity;
					});
					if (jobs != 1) { return false; }
				}
				media.insert(ref.identity);
			}
			if (media.size() != qsizetype(version.content.media.size())) {
				return false;
			}
		}
		auto receipts = QSet<QString>();
		for (const auto &receipt : message.receipts) {
			if (!SafeObject(receipt.preserved)
				|| !IsArchiveToken(receipt.id) || !IsArchiveHash(receipt.fingerprint)
				|| receipts.contains(receipt.id)) {
				return false;
			}
			receipts.insert(receipt.id);
		}
		for (const auto &version : message.versions) {
			if (!receipts.contains(version.id) || !receipts.contains(version.revision)) {
				return false;
			}
		}
	}
	auto jobs = QSet<QString>();
	for (const auto &job : state.jobs) {
		if (!SafeObject(job.preserved) || !IsArchiveToken(job.id) || jobs.contains(job.id)
			|| !IsArchiveMessageKey(job.key) || job.key.account != state.account
			|| !IsArchiveToken(job.version) || !IsArchiveToken(job.mediaIdentity)
			|| (!job.expectedHash.isEmpty() && !IsArchiveHash(job.expectedHash))
			|| !InRange(job.state, ArchiveJobState::Cancelled)
			|| (!job.blobId.isEmpty() && !IsArchiveHash(job.blobId))
			|| (job.state == ArchiveJobState::Staged && !job.blobId.isEmpty())
			|| (job.state == ArchiveJobState::Complete && job.blobId.isEmpty())
			|| (job.state != ArchiveJobState::Complete && (!job.blobId.isEmpty() || job.completedSize))
			|| (job.state == ArchiveJobState::Failed
				? !IsArchiveToken(job.failure) : !job.failure.isEmpty())) {
			return false;
		}
		if (job.state == ArchiveJobState::Complete) {
			const auto blob = std::find_if(state.blobs.begin(), state.blobs.end(),
				[&](const auto &entry) { return entry.id == job.blobId; });
			if ((job.expectedSize && *job.expectedSize != job.completedSize)
				|| (!job.expectedHash.isEmpty() && job.expectedHash != job.blobId)
				|| (blob != state.blobs.end() && blob->size != job.completedSize)) {
				return false;
			}
		}
		if (job.state == ArchiveJobState::Staged) {
			const auto version = FindArchiveVersion(state, job.key, job.version);
			if (!IsGeneration(state, job.key, job.generation) || !version) {
				return false;
			}
			const auto count = std::count_if(version->media.begin(), version->media.end(),
				[&](const auto &ref) {
					return ref.identity == job.mediaIdentity && ref.state == ArchiveMediaState::Staged;
				});
			if (count != 1) {
				return false;
			}
		}
		jobs.insert(job.id);
	}
	auto leases = QSet<QString>();
	for (const auto &lease : state.leases) {
		const auto blob = std::find_if(state.blobs.begin(), state.blobs.end(),
			[&](const auto &entry) { return entry.id == lease.blobId; });
		if (!SafeObject(lease.preserved) || !IsArchiveToken(lease.id) || leases.contains(lease.id)
			|| !IsArchiveMessageKey(lease.key) || lease.key.account != state.account
			|| !IsArchiveToken(lease.version)
			|| blob == state.blobs.end() || blob->state != ArchiveBlobState::Complete) {
			return false;
		}
		if (!lease.revoked) {
			if (!IsGeneration(state, lease.key, lease.generation)) { return false; }
			const auto version = FindArchiveVersion(state, lease.key, lease.version);
			if (!version || std::none_of(version->media.begin(), version->media.end(),
				[&](const auto &ref) { return ref.blobId == lease.blobId; })) {
				return false;
			}
		}
		leases.insert(lease.id);
	}
	return IsCodecFields(state);
}

}
