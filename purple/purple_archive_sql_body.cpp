/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_sql_internal.h"

#include <QtCore/QCryptographicHash>

#include <limits>

namespace Purple {
namespace {

ArchiveVersionSnapshot Projection(ArchiveVersionSnapshot snapshot) {
	snapshot.preserved = {};
	auto &version = snapshot.version;
	auto descriptors = std::move(version.content.media);
	version.content = {};
	for (const auto &descriptor : descriptors) {
		version.content.media.push_back({ descriptor.identity, descriptor.kind });
	}
	version.source.reset();
	version.observation = {};
	version.observation.representation = snapshot.key.representation;
	version.missing = {};
	version.missing.revision = version.revision;
	version.completeness = ArchiveCompleteness::Complete;
	version.preserved = {};
	for (auto &ref : version.media) { ref.preserved = {}; }
	return snapshot;
}

QString BodyToken(const ArchiveRow &row, const ArchiveSqlSecurity &security) {
	if (!row.body || ArchiveRowKind(row) != ArchiveRecordKind::Version) { return {}; }
	const auto &snapshot = std::get<ArchiveVersionSnapshot>(row.value);
	return ArchiveSqlToken(snapshot.key.account, ArchiveIndexKind::Blob,
		QByteArray("version-body/") + row.body->hash.toLatin1(), security);
}

}

std::optional<ArchiveRow> PrepareArchiveSqlVersion(const ArchiveVersionSnapshot &snapshot) {
	if (snapshot.version.media.size() > kArchiveSqlPurgeMediaLimit) { return {}; }
	const auto bytes = EncodeArchiveVersionSnapshot(snapshot);
	if (!bytes || bytes->size() > kArchiveSqlBodyByteLimit) { return {}; }
	if (bytes->size() <= kArchiveSqlInlineBodyLimit) { return ArchiveRow{ snapshot }; }
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex());
	auto result = ArchiveRow{ Projection(snapshot), {}, ArchiveVersionBody{ hash, bytes->size() } };
	const auto compact = EncodeArchiveRow(result);
	return compact && compact->size() <= kArchiveSqlPurgeEnvelopeLimit - kArchiveSqlPurgeMediaLimit * 64 - 2048 ? std::optional(result) : std::nullopt;
}

std::optional<QByteArray> SealArchiveSqlStoredRow(const AccountKey &account, const QString &id,
		const ArchiveRow &row, const ArchiveSqlSecurity &security) {
	if (id.isEmpty()) { return {}; }
	const auto plaintext = EncodeArchiveRow(row);
	if (!plaintext) { return {}; }
	if (ArchiveRowKind(row) == ArchiveRecordKind::Account) {
		auto worst = std::get<ArchiveState>(row.value);
		worst.revision = std::numeric_limits<uint64_t>::max();
		worst.accountGeneration = std::numeric_limits<uint64_t>::max();
		const auto worstBytes = EncodeArchiveRow({ worst });
		if (!worstBytes || worstBytes->size() > kArchiveSqlPurgeEnvelopeLimit - 1024) { return {}; }
	}
	const auto sealed = SealArchiveEnvelope({ account, security.storeId, ArchiveRowKind(row), id.toUtf8() },
		security.keyId, *plaintext, security.crypto);
	const auto limit = ArchiveRowKind(row) == ArchiveRecordKind::Blob
		? kArchiveSqlPurgeBlobEnvelopeLimit : kArchiveSqlPurgeEnvelopeLimit;
	return sealed && sealed.bytes.size() <= limit ? std::optional(sealed.bytes) : std::nullopt;
}

ArchiveSqlProgram StoreArchiveSqlVersionBody(const ArchiveRow &row,
		const ArchiveVersionSnapshot &snapshot, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlProgram {
		auto result = ArchiveSqlProgram{ ArchiveSqlStatus::Ready };
		const auto id = ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security);
		const auto expected = PrepareArchiveSqlVersion(snapshot);
		if (id.isEmpty() || !expected || expected->value != row.value || expected->body != row.body
			|| !row.preserved.isEmpty()) { return {}; }
		result.statements.push_back({ u"DELETE FROM body_refs WHERE message_key=? AND version_id=?"_q,
			{ id.left(64), id.mid(65) }, ArchiveSqlExpectation::AnyChanges });
		if (!row.body) { return result; }
		const auto bytes = EncodeArchiveVersionSnapshot(snapshot);
		const auto token = BodyToken(row, security);
		if (!bytes || token.isEmpty()) { return {}; }
		const auto count = (row.body->bytes + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes;
		result.statements.push_back({ u"INSERT INTO body_refs VALUES(?,?,?,?,?)"_q,
			{ id.left(64), id.mid(65), token, row.body->bytes, count }, ArchiveSqlExpectation::OneChange });
		for (auto index = int64(0); index != count; ++index) {
			const auto context = ArchiveMediaChunkContext(snapshot.key.account, security.storeId, token, row.body->bytes, index);
			if (!context) { return {}; }
			const auto chunk = SealArchiveEnvelope(*context, security.keyId,
			 bytes->mid(index * kArchiveMediaChunkBytes, kArchiveMediaChunkBytes), security.crypto);
			if (!chunk) { return {}; }
			result.statements.push_back({ u"INSERT INTO body_chunks VALUES(?,?,?,?)"_q,
			 { id.left(64), id.mid(65), index, chunk.bytes }, ArchiveSqlExpectation::OneChange });
		}
		if (!IsBoundedArchiveSqlProgram(result)) { return {}; }
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlReadBodyChunks(const ArchiveRow &row, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		if (!row.body || ArchiveRowKind(row) != ArchiveRecordKind::Version
			|| !IsArchiveHash(row.body->hash) || row.body->bytes < 1 || row.body->bytes > kArchiveSqlBodyByteLimit) { return {}; }
		const auto &snapshot = std::get<ArchiveVersionSnapshot>(row.value);
		const auto id = ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security);
		const auto token = BodyToken(row, security);
		if (id.isEmpty() || token.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT c.envelope FROM body_chunks c JOIN body_refs r USING(message_key,version_id) "
			"WHERE c.message_key=? AND c.version_id=? AND r.blob_id=? AND r.bytes=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') ORDER BY c.chunk_index LIMIT ?"_q,
			{ id.left(64), id.mid(65), token, row.body->bytes,
			 int64((kArchiveSqlBodyByteLimit + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes + 1) }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = (kArchiveSqlBodyByteLimit + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes;
		return result;
	});
}

std::optional<ArchiveRow> RestoreArchiveSqlVersionBody(const ArchiveRow &row,
		const std::vector<QByteArray> &chunks, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> std::optional<ArchiveRow> {
		if (!row.body) { return chunks.empty() ? std::optional(row) : std::nullopt; }
		if (ArchiveRowKind(row) != ArchiveRecordKind::Version || row.body->bytes < 1
			|| row.body->bytes > kArchiveSqlBodyByteLimit
			|| chunks.size() != size_t((row.body->bytes + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes)) { return {}; }
		const auto &projection = std::get<ArchiveVersionSnapshot>(row.value);
		const auto token = BodyToken(row, security);
		if (token.isEmpty()) { return {}; }
		auto bytes = QByteArray();
		for (auto index = size_t(0); index != chunks.size(); ++index) {
			const auto context = ArchiveMediaChunkContext(projection.key.account, security.storeId, token, row.body->bytes, index);
			const auto size = ArchiveMediaChunkSize(row.body->bytes, index);
			if (!context || !size) { return {}; }
			const auto chunk = OpenArchiveEnvelope(*context, security.keyId, chunks[index], security.crypto);
			if (!chunk || chunk.bytes.size() != *size) { return {}; }
			bytes += chunk.bytes;
		}
		if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != row.body->hash) { return {}; }
		const auto decoded = DecodeArchiveVersionSnapshot(bytes);
		if (!decoded.snapshot) { return {}; }
		auto snapshot = *decoded.snapshot;
		auto expected = Projection(snapshot);
		if (expected.version.media.size() != projection.version.media.size()) { return {}; }
		for (auto index = size_t(0); index != expected.version.media.size(); ++index) {
			expected.version.media[index].state = projection.version.media[index].state;
			expected.version.media[index].blobId = projection.version.media[index].blobId;
		}
		if (expected != projection) { return {}; }
		for (auto index = size_t(0); index != snapshot.version.media.size(); ++index) {
			snapshot.version.media[index].state = projection.version.media[index].state;
			snapshot.version.media[index].blobId = projection.version.media[index].blobId;
		}
		return ArchiveRow{ snapshot };
	});
}

static ArchiveSqlCapturePlan PlanArchiveSqlCaptureImpl(const ArchiveState &slice, const ArchiveEvent &event,
		const ArchiveSqlSecurity &security, const ArchiveSqlSlice *restored) {
	auto result = ArchiveSqlCapturePlan();
	auto rows = size_t(1) + slice.messages.size() + slice.blobs.size() + slice.jobs.size()
		+ slice.leases.size() + slice.chatGenerations.size();
	if (slice.messages.size() <= 1) {
		for (const auto &message : slice.messages) { rows += message.versions.size() + message.receipts.size(); }
	}
	if (slice.messages.size() > 1 || rows > kArchiveSqlSliceRowLimit) {
		result.program.status = ArchiveSqlStatus::RetentionGap;
		result.program.retentionGapReason = u"archive-slice-limit"_q;
		return result;
	}
	if (event.source && event.source->bytes.size() > kArchiveSqlBodyByteLimit) {
		result.program.status = ArchiveSqlStatus::RetentionGap;
		result.program.retentionGapReason = u"archive-source-limit"_q;
		return result;
	}
	result.transaction = PlanArchiveEvent(slice, event);
	if (result.transaction.status == ArchivePlanStatus::NoChange) {
		result.program.status = ArchiveSqlStatus::Ready;
		return result;
	}
	if (result.transaction.status == ArchivePlanStatus::ReadOnly) {
		result.program.status = ArchiveSqlStatus::ReadOnly;
		return result;
	}
	if (result.transaction) {
		result.program = restored ? TranslateArchiveSql(result.transaction, *restored, security)
			: TranslateArchiveSql(result.transaction, security);
	}
	if (!result.program) {
		result.program.statements.clear();
		result.program.status = ArchiveSqlStatus::RetentionGap;
		result.program.retentionGapReason = u"archive-capture-refused"_q;
	}
	return result;
}

ArchiveSqlCapturePlan PlanArchiveSqlCapture(const ArchiveState &slice, const ArchiveEvent &event,
		const ArchiveSqlSecurity &security) {
	return PlanArchiveSqlCaptureImpl(slice, event, security, nullptr);
}

ArchiveSqlCapturePlan PlanArchiveSqlCapture(const ArchiveSqlSlice &slice, const ArchiveEvent &event,
		const ArchiveSqlSecurity &security) {
	return PlanArchiveSqlCaptureImpl(slice.state, event, security, &slice);
}

}
