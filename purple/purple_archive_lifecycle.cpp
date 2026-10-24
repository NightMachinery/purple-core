/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_lifecycle.h"

#include "purple/purple_archive_sql_internal.h"

#include "purple/purple_sync_json.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>

#include <limits>

namespace Purple {
namespace {

bool IsEpoch(const ArchiveStoreEpoch &epoch) {
	return IsArchiveEnvelopeContext({ epoch.account, epoch.storeId,
		ArchiveRecordKind::Account, "account" });
}

bool IsRemoval(const ArchiveRemoval &removal) {
	return IsEpoch(removal.epoch) && removal.epoch.removalPending
		&& removal.epoch.generation > 0 && IsArchiveToken(removal.operationId)
		&& int(removal.action) >= int(ArchiveRemovalAction::WriteTombstone)
		&& int(removal.action) <= int(ArchiveRemovalAction::Done);
}

}

bool CanRunArchiveWork(const ArchiveStoreEpoch &captured, const ArchiveStoreEpoch &current) {
	return IsEpoch(captured) && IsEpoch(current) && !captured.removalPending
		&& !current.removalPending && captured == current;
}

std::optional<ArchiveRemoval> BeginArchiveRemoval(const ArchiveStoreEpoch &current,
		const QString &operationId) {
	if (!IsEpoch(current) || current.removalPending || !IsArchiveToken(operationId)
		|| current.generation == std::numeric_limits<uint64_t>::max()) { return {}; }
	auto removal = ArchiveRemoval{ current, operationId };
	++removal.epoch.generation;
	removal.epoch.removalPending = true;
	return removal;
}

std::optional<QByteArray> EncodeArchiveRemovalTombstone(const ArchiveRemoval &removal) {
	if (!IsRemoval(removal)) { return {}; }
	const auto object = QJsonObject{
		{ u"format"_q, u"purple-archive-removal"_q },
		{ u"version"_q, 1 },
		{ u"account"_q, *AccountKeyText(removal.epoch.account.environment, removal.epoch.account.userId) },
		{ u"store"_q, removal.epoch.storeId },
		{ u"operation"_q, removal.operationId },
		{ u"generation"_q, QString::number(removal.epoch.generation) },
	};
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return canonical ? std::optional(canonical.canonical) : std::nullopt;
}

std::optional<ArchiveRemoval> ResumeArchiveRemoval(const QByteArray &durableTombstone) {
	if (durableTombstone.size() > 4096) { return {}; }
	const auto canonical = CanonicalizeSyncJson(durableTombstone);
	if (!canonical) { return {}; }
	auto object = QJsonDocument::fromJson(canonical.canonical).object();
	if (object.take(u"format"_q) != QJsonValue(u"purple-archive-removal"_q)
		|| object.take(u"version"_q) != QJsonValue(1)) { return {}; }
	const auto accountValue = object.take(u"account"_q);
	const auto store = object.take(u"store"_q);
	const auto operation = object.take(u"operation"_q);
	const auto generationValue = object.take(u"generation"_q);
	if (!accountValue.isString() || !store.isString() || !operation.isString()
		|| !generationValue.isString() || !object.isEmpty()) { return {}; }
	const auto account = ParseAccountKey(accountValue.toString());
	auto valid = false;
	const auto generation = generationValue.toString().toULongLong(&valid);
	if (!account || !valid || QString::number(generation) != generationValue.toString()) { return {}; }
	auto removal = ArchiveRemoval{ { *account, store.toString(), generation, true },
		operation.toString(), ArchiveRemovalAction::FenceWork };
	return IsRemoval(removal) ? std::optional(std::move(removal)) : std::nullopt;
}

ArchiveRemovalResult AdvanceArchiveRemoval(const ArchiveRemoval &removal,
		const ArchiveRemovalAck &ack) {
	auto result = ArchiveRemovalResult{ ArchiveRemovalStatus::Invalid, removal };
	if (!IsRemoval(removal) || ack.account != removal.epoch.account
		|| ack.storeId != removal.epoch.storeId || ack.operationId != removal.operationId
		|| ack.generation != removal.epoch.generation || ack.action != removal.action
		|| removal.action == ArchiveRemovalAction::Done) { return result; }
	result.status = ArchiveRemovalStatus::Wait;
	if (!ack.success) { return result; }
	if (removal.action == ArchiveRemovalAction::DrainReadersAndClose) {
		if (ack.readers || ack.writers || !ack.databaseClosed) { return result; }
	} else if (!ack.durable) {
		return result;
	}
	result.state.action = ArchiveRemovalAction(int(removal.action) + 1);
	result.status = ArchiveRemovalStatus::Advanced;
	return result;
}

std::optional<uint64_t> TotalArchiveUsage(const ArchiveUsage &usage) {
	auto total = uint64_t(0);
	for (const auto bytes : { usage.databaseBytes, usage.walBytes, usage.shmBytes,
		usage.blobBytes, usage.stagingBytes }) {
		if (bytes > std::numeric_limits<uint64_t>::max() - total) { return {}; }
		total += bytes;
	}
	return total;
}

std::optional<ArchiveUsageScan> ContinueArchiveUsage(const ArchiveUsageScan &scan,
		const ArchiveStoreEpoch &current, const std::vector<ArchiveUsageEntry> &page, bool complete) {
	if (scan.complete || !CanRunArchiveWork(scan.epoch, current) || page.size() > kArchiveSqlPageLimit) { return {}; }
	auto next = scan;
	for (const auto &entry : page) {
		if (entry.token.isEmpty() || entry.token.size() > 1024 || entry.token <= next.cursor
			|| entry.bytes > std::numeric_limits<uint64_t>::max() - next.bytes) { return {}; }
		next.cursor = entry.token;
		next.bytes += entry.bytes;
	}
	next.complete = complete;
	return next;
}

ArchiveSqlProgram PlanArchiveCaptureGap(const ArchiveCaptureGap &gap,
		const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlProgram {
		if (gap.storeId != security.storeId || !IsArchiveToken(gap.id)
			|| !IsArchiveToken(gap.reason) || gap.observedAt < 0
			|| !IsArchiveEnvelopeContext({ gap.account, gap.storeId, ArchiveRecordKind::CaptureGap, gap.id.toUtf8() })) {
			return {};
		}
		const auto object = QJsonObject{
			{ u"format"_q, u"purple-archive-gap"_q }, { u"version"_q, 1 },
			{ u"id"_q, gap.id }, { u"observedAt"_q, QString::number(gap.observedAt) },
			{ u"reason"_q, gap.reason }, { u"details"_q, gap.details },
		};
		const auto canonical = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
		if (!canonical) { return {}; }
		const auto gapToken = ArchiveSqlToken(gap.account, ArchiveIndexKind::Maintenance, (u"gap/"_q + gap.id).toUtf8(), security);
		const auto encrypted = SealArchiveEnvelope({ gap.account, gap.storeId,
			ArchiveRecordKind::CaptureGap, gapToken.toUtf8() }, security.keyId, canonical.canonical, security.crypto);
		if (!encrypted) { return { ArchiveSqlStatus::EncodingFailed }; }
		const auto account = ArchiveSqlToken(gap.account, ArchiveIndexKind::Account, "account", security);
		return { ArchiveSqlStatus::Ready, {
			{ u"BEGIN IMMEDIATE"_q },
			{ u"SELECT 1 FROM archive_meta WHERE singleton=1 AND schema_version=? AND schema_fingerprint=? AND account_token=? AND store_id=? AND maintenance_id=''"_q,
				{ int64(kArchiveSqlSchemaVersion), ArchiveSqlExpectedSchemaFingerprint(), account, gap.storeId }, ArchiveSqlExpectation::OneRow },
			{ u"INSERT INTO capture_gaps VALUES(?,?,?,?,?) ON CONFLICT(gap_id) DO UPDATE SET gap_id=excluded.gap_id "
				"WHERE capture_gaps.fingerprint=excluded.fingerprint"_q,
				{ gapToken, gap.observedAt, gap.reason,
					ArchiveSqlToken(gap.account, ArchiveIndexKind::Maintenance, QByteArray("gap-fingerprint/") + canonical.canonical, security),
					encrypted.bytes }, ArchiveSqlExpectation::OneChange },
			{ u"COMMIT"_q },
		} };
	});
}

ArchiveSqlStatement ArchiveSqlReadCaptureGaps(int limit) {
	if (limit < 1 || limit > kArchiveSqlPageLimit) { return {}; }
	return { u"SELECT gap_id,observed_at,reason,envelope FROM capture_gaps ORDER BY observed_at DESC,gap_id DESC LIMIT ?"_q,
		{ int64(limit) }, ArchiveSqlExpectation::Rows };
}

}
