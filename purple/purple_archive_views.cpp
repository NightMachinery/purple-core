/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_views.h"

#include "purple/purple_archive_sql_internal.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>

#include <algorithm>

namespace Purple {
namespace {

std::optional<ArchivePurgeScope> Normalize(ArchivePurgeScope scope) {
	if (!AccountKeyText(scope.account.environment, scope.account.userId)
		|| scope.migrationAliases.size() > 31
		|| (!scope.chat && !scope.migrationAliases.empty())
		|| (scope.chat && !IsArchiveChatKey(*scope.chat))) { return {}; }
	for (auto i = size_t(0); i != scope.migrationAliases.size(); ++i) {
		const auto &chat = scope.migrationAliases[i];
		if (!IsArchiveChatKey(chat) || SameArchiveChat(chat, *scope.chat)) { return {}; }
		for (auto j = size_t(0); j != i; ++j) {
			if (SameArchiveChat(chat, scope.migrationAliases[j])) { return {}; }
		}
	}
	std::sort(scope.migrationAliases.begin(), scope.migrationAliases.end(), [](const auto &a, const auto &b) {
		return a.kind != b.kind ? int(a.kind) < int(b.kind) : a.id < b.id;
	});
	return scope;
}

QJsonArray Chat(const ArchiveChatKey &chat) {
	return { int(chat.kind), QString::number(chat.id) };
}

QString ConfirmationToken(const ArchivePurgeConfirmation &confirmation) {
	const auto scope = Normalize(confirmation.request.scope);
	if (!scope || !CanRunArchiveWork(confirmation.epoch, confirmation.epoch)
		|| scope->account != confirmation.epoch.account
		|| (confirmation.request.kind != ArchivePurgeKind::Trashbin
			&& confirmation.request.kind != ArchivePurgeKind::EditHistory)) { return {}; }
	auto aliases = QJsonArray();
	for (const auto &alias : scope->migrationAliases) { aliases.push_back(Chat(alias)); }
	const auto object = QJsonObject{
		{ u"format"_q, u"purple-archive-purge-confirmation-v1"_q },
		{ u"account"_q, *AccountKeyText(scope->account.environment, scope->account.userId) },
		{ u"store"_q, confirmation.epoch.storeId },
		{ u"epoch"_q, QString::number(confirmation.epoch.generation) },
		{ u"revision"_q, QString::number(confirmation.request.expectedRevision) },
		{ u"generation"_q, QString::number(confirmation.accountGeneration) },
		{ u"kind"_q, int(confirmation.request.kind) },
		{ u"chat"_q, scope->chat ? QJsonValue(Chat(*scope->chat)) : QJsonValue(QJsonValue::Null) },
		{ u"aliases"_q, aliases },
	};
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return canonical ? QString::fromLatin1(QCryptographicHash::hash(canonical.canonical,
		QCryptographicHash::Sha256).toHex()) : QString();
}

struct Predicate {
	QString sql;
	std::vector<ArchiveSqlValue> values;
};

std::optional<Predicate> Filter(const ArchiveViewScope &input, const ArchiveSqlSecurity &security) {
	const auto scope = Normalize(input.messages);
	if (!scope || (input.collection && (int(*input.collection) < 0
		|| int(*input.collection) > int(ArchiveCollection::QuickReply)))
		|| (input.representation && !IsArchiveToken(*input.representation))) { return {}; }
	auto result = Predicate{ u"m.purged=0"_q };
	if (scope->chat) {
		auto chats = scope->migrationAliases;
		chats.insert(chats.begin(), *scope->chat);
		result.sql += u" AND m.chat_token IN ("_q;
		for (auto i = size_t(0); i != chats.size(); ++i) {
			const auto token = ArchiveSqlChatId(chats[i], scope->account, security);
			if (token.isEmpty()) { return {}; }
			if (i) { result.sql += ','; }
			result.sql += '?';
			result.values.push_back(token);
		}
		result.sql += ')';
	}
	if (input.topic) {
		if (!scope->chat || input.topic->id < 0 || !IsArchiveChatKey(input.topic->chat)
			|| (!SameArchiveChat(*scope->chat, input.topic->chat)
				&& std::none_of(scope->migrationAliases.begin(), scope->migrationAliases.end(),
					[&](const auto &alias) { return SameArchiveChat(alias, input.topic->chat); }))) { return {}; }
		const auto token = ArchiveSqlTopicId(input.topic->chat, scope->account, input.topic->id, security);
		if (token.isEmpty()) { return {}; }
		result.sql += u" AND m.topic_token=?"_q;
		result.values.push_back(token);
	}
	if (input.collection) {
		result.sql += u" AND m.collection=?"_q;
		result.values.push_back(int64(*input.collection));
	}
	if (input.representation) {
		const auto token = ArchiveSqlToken(scope->account, ArchiveIndexKind::Representation,
			input.representation->toUtf8(), security);
		if (token.isEmpty()) { return {}; }
		result.sql += u" AND m.representation_token=?"_q;
		result.values.push_back(token);
	}
	return result;
}

QString Meta() {
	return ArchiveSqlSchemaGuard() + u" AND account_token=? AND store_id=? AND maintenance_id=''"_q;
}

bool BindMeta(ArchiveSqlStatement &statement, const ArchiveViewScope &scope,
		const ArchiveSqlSecurity &security) {
	const auto token = ArchiveSqlToken(scope.messages.account, ArchiveIndexKind::Account, "account", security);
	if (token.isEmpty()) { return false; }
	statement.binds.insert(statement.binds.end(), { token, security.storeId });
	return true;
}

}

bool ArchiveDeletedViewMatches(const ArchiveViewScope &input,
		const ArchiveMessage &message, const ArchiveVersionSnapshot *head) {
	const auto scope = Normalize(input.messages);
	if (!scope || !IsArchiveMessageKey(message.key) || message.key.account != scope->account
		|| !message.deleted || message.payloadPurged || !message.deletionOrigin
		|| (*message.deletionOrigin != ArchiveOrigin::AuthoritativeDelete
			&& *message.deletionOrigin != ArchiveOrigin::LocalDelete
			&& *message.deletionOrigin != ArchiveOrigin::MediaExpiry
			&& *message.deletionOrigin != ArchiveOrigin::MediaConsumption)
		|| (scope->chat && !SameArchiveChat(*scope->chat, message.key.chat)
			&& std::none_of(scope->migrationAliases.begin(), scope->migrationAliases.end(),
				[&](const auto &alias) { return SameArchiveChat(alias, message.key.chat); }))
		|| (input.collection && message.key.collection != *input.collection)
		|| (input.representation && message.key.representation != *input.representation)) { return false; }
	if (head && (!SameArchiveMessage(head->key, message.key) || head->version.order > message.order
		|| (!message.versions.empty() && message.versions.back() != head->version))) { return false; }
	if (input.topic && (!scope->chat || !head || input.topic->id < 0 || !IsArchiveChatKey(input.topic->chat)
		|| !SameArchiveChat(input.topic->chat, message.key.chat)
		|| head->version.observation.topicId != input.topic->id)) { return false; }
	return true;
}

ArchiveDeletedStatus ArchiveMessageDeletedStatus(const ArchiveMessage &message) {
	if (!message.deleted) { return ArchiveDeletedStatus::NotDeleted; }
	return message.deletionOrigin == ArchiveOrigin::MediaExpiry ? ArchiveDeletedStatus::Expired
		: message.deletionOrigin == ArchiveOrigin::MediaConsumption ? ArchiveDeletedStatus::Consumed
		: ArchiveDeletedStatus::Deleted;
}

std::optional<ArchivePurgeConfirmation> MakeArchivePurgeConfirmation(const ArchiveState &state,
		const ArchiveStoreEpoch &epoch, ArchivePurgeKind kind, const ArchivePurgeScope &scope) {
	const auto normalized = Normalize(scope);
	if (!normalized || !IsArchiveState(state) || state.account != epoch.account
		|| state.account != scope.account || !CanRunArchiveWork(epoch, epoch)) { return {}; }
	auto confirmation = ArchivePurgeConfirmation{ epoch, { kind, *normalized, state.revision }, state.accountGeneration };
	confirmation.token = ConfirmationToken(confirmation);
	return confirmation.token.isEmpty() ? std::nullopt : std::optional(confirmation);
}

std::optional<ArchivePurgeRequest> ConfirmArchivePurge(const ArchiveState &current,
		const ArchiveStoreEpoch &epoch, ArchivePurgeKind currentKind,
		const ArchivePurgeScope &currentScope, const ArchivePurgeConfirmation &confirmation) {
	const auto fresh = MakeArchivePurgeConfirmation(current, epoch, currentKind, currentScope);
	if (!fresh || !CanRunArchiveWork(confirmation.epoch, epoch)
		|| fresh->token != confirmation.token || ConfirmationToken(confirmation) != confirmation.token) { return {}; }
	return fresh->request;
}

ArchiveSqlStatement ArchiveSqlDeletedView(const ArchiveViewScope &scope, int limit,
		const ArchiveSqlSecurity &security, const std::optional<ArchiveTranscriptCursor> &cursor,
		ArchiveTranscriptOrder order) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto predicate = Filter(scope, security);
		if (!predicate || !scope.messages.chat || limit < 1 || limit > kArchiveSqlPageLimit
			|| (order != ArchiveTranscriptOrder::Chronological && order != ArchiveTranscriptOrder::ReverseChronological)
			|| (cursor && (cursor->date < 0 || !cursor->messageId || !IsArchiveHash(cursor->messageKey)))) { return {}; }
		auto statement = ArchiveSqlStatement{ u"SELECT m.message_key,m.transcript_date,m.message_id,m.envelope,m.head_version,v.envelope "
			"FROM messages m LEFT JOIN versions v ON v.message_key=m.message_key AND v.version_id=m.head_version WHERE "_q
			+ predicate->sql + u" AND m.deleted=1 AND EXISTS(SELECT 1 FROM archive_meta WHERE "_q + Meta() + ')',
			predicate->values, ArchiveSqlExpectation::Rows };
		if (!BindMeta(statement, scope, security)) { return {}; }
		const auto ascending = order == ArchiveTranscriptOrder::Chronological;
		if (cursor) {
			statement.sql += ascending ? u" AND (m.transcript_date,m.message_id,m.message_key)>(?,?,?)"_q
				: u" AND (m.transcript_date,m.message_id,m.message_key)<(?,?,?)"_q;
			statement.binds.insert(statement.binds.end(), { cursor->date, cursor->messageId, cursor->messageKey });
		}
		statement.sql += ascending ? u" ORDER BY m.transcript_date,m.message_id,m.message_key LIMIT ?"_q
			: u" ORDER BY m.transcript_date DESC,m.message_id DESC,m.message_key DESC LIMIT ?"_q;
		statement.binds.push_back(int64(limit));
		statement.rows.maximum = limit;
		return statement;
	});
}

ArchiveSqlStatement ArchiveSqlScopeCounts(const ArchiveViewScope &scope, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto predicate = Filter(scope, security);
		if (!predicate) { return {}; }
		const auto selections = std::vector<QString>{
			u"COUNT(*) FROM messages m WHERE "_q,
			u"COUNT(*) FROM messages m WHERE m.deleted=1 AND "_q,
			u"COUNT(*) FROM versions v JOIN messages m ON m.message_key=v.message_key WHERE "_q,
			u"COUNT(*) FROM versions v JOIN messages m ON m.message_key=v.message_key WHERE v.version_id<>m.head_version AND "_q,
			u"COUNT(*) FROM media_refs r JOIN messages m ON m.message_key=r.message_key WHERE "_q,
			u"COUNT(*) FROM media_refs r JOIN messages m ON m.message_key=r.message_key WHERE r.state=2 AND "_q,
			u"COUNT(DISTINCT r.blob_id) FROM media_refs r JOIN messages m ON m.message_key=r.message_key WHERE r.blob_id IS NOT NULL AND "_q,
		};
		auto statement = ArchiveSqlStatement{ u"SELECT "_q, {}, ArchiveSqlExpectation::OneRow };
		for (auto i = size_t(0); i != selections.size(); ++i) {
			if (i) { statement.sql += ','; }
			statement.sql += u"(SELECT "_q + selections[i] + predicate->sql + ')';
			statement.binds.insert(statement.binds.end(), predicate->values.begin(), predicate->values.end());
		}
		statement.sql += u" FROM archive_meta WHERE singleton=1 AND "_q + Meta();
		if (!BindMeta(statement, scope, security)) { return {}; }
		return statement;
	});
}

ArchiveSqlStatement ArchiveSqlScopeBlobs(const ArchiveViewScope &scope, int limit,
		const ArchiveSqlSecurity &security, const QString &after) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto predicate = Filter(scope, security);
		if (!predicate || limit < 1 || limit > kArchiveSqlPageLimit || (!after.isEmpty() && !IsArchiveHash(after))) { return {}; }
		auto statement = ArchiveSqlStatement{ u"SELECT b.blob_id,b.bytes,b.envelope FROM blobs b WHERE b.blob_id>? AND "
			"EXISTS(SELECT 1 FROM media_refs r JOIN messages m ON m.message_key=r.message_key WHERE r.blob_id=b.blob_id AND "_q
			+ predicate->sql + u") AND EXISTS(SELECT 1 FROM archive_meta WHERE "_q + Meta() + u") ORDER BY b.blob_id LIMIT ?"_q,
			{ after }, ArchiveSqlExpectation::Rows };
		statement.binds.insert(statement.binds.end(), predicate->values.begin(), predicate->values.end());
		if (!BindMeta(statement, scope, security)) { return {}; }
		statement.binds.push_back(int64(limit));
		statement.rows.maximum = limit;
		return statement;
	});
}

}
