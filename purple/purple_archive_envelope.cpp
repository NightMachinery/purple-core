/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_envelope.h"

#include <algorithm>

namespace Purple {
namespace {

const auto Magic = QByteArray("PARCHIVE");

void Number(QByteArray &out, uint64_t value, int bytes) {
	for (auto i = bytes - 1; i >= 0; --i) {
		out.append(char(value >> (i * 8)));
	}
}

void Field(QByteArray &out, const QByteArray &bytes) {
	Number(out, bytes.size(), 4);
	out.append(bytes);
}

struct Reader {
	const QByteArray &bytes;
	qsizetype offset = 0;
	bool valid = true;

	uint64_t number(int size) {
		if (!valid || size > bytes.size() - offset) {
			valid = false;
			return 0;
		}
		auto result = uint64_t(0);
		for (auto i = 0; i != size; ++i) {
			result = (result << 8) | static_cast<unsigned char>(bytes[offset++]);
		}
		return result;
	}

	QByteArray take(qsizetype size) {
		if (!valid || size < 0 || size > bytes.size() - offset) {
			valid = false;
			return {};
		}
		const auto result = bytes.mid(offset, size);
		offset += size;
		return result;
	}

	QByteArray field(qsizetype limit) {
		const auto size = number(4);
		if (size > uint64_t(limit)) {
			valid = false;
			return {};
		}
		return take(qsizetype(size));
	}
};

bool KeyId(const QString &id) {
	return IsArchiveToken(id) && id.toUtf8().size() <= 128;
}

QByteArray Header(const ArchiveEnvelopeContext &context, const QString &keyId,
		const QByteArray &nonce, qsizetype size) {
	auto header = Magic;
	Number(header, kArchiveEnvelopeVersion, 2);
	Number(header, 1, 2);
	Field(header, context.storeId.toUtf8());
	Number(header, int(context.kind), 2);
	Field(header, context.rowId);
	Field(header, keyId.toUtf8());
	header.append(nonce);
	Number(header, size, 4);
	return header;
}

QByteArray AssociatedData(QByteArray header, const AccountKey &account) {
	Field(header, AccountKeyText(account.environment, account.userId)->toUtf8());
	return header;
}

ArchiveEnvelopeResult Refuse(ArchiveEnvelopeStatus status) {
	return { status, {} };
}

}

bool IsArchiveEnvelopeContext(const ArchiveEnvelopeContext &context) {
	return AccountKeyText(context.account.environment, context.account.userId).has_value()
		&& KeyId(context.storeId) && !context.rowId.isEmpty()
		&& context.rowId.size() <= 4096
		&& int(context.kind) >= int(ArchiveRecordKind::Account)
		&& int(context.kind) <= int(ArchiveRecordKind::Maintenance);
}

ArchiveEnvelopeResult SealArchiveEnvelope(const ArchiveEnvelopeContext &context,
		const QString &keyId, const QByteArray &plaintext, const ArchiveCrypto &crypto) {
	if (!IsArchiveEnvelopeContext(context) || !KeyId(keyId)
		|| plaintext.size() > kArchiveEnvelopeByteLimit
		|| !crypto.random || !crypto.sealAes256Gcm) {
		return {};
	}
	const auto nonce = crypto.random(kArchiveNonceBytes);
	if (!nonce || nonce->size() != kArchiveNonceBytes) {
		return Refuse(ArchiveEnvelopeStatus::CryptoFailure);
	}
	const auto header = Header(context, keyId, *nonce, plaintext.size());
	const auto aad = AssociatedData(header, context.account);
	const auto sealed = crypto.sealAes256Gcm(keyId, *nonce, plaintext, aad);
	if (!sealed || sealed->tag.size() != kArchiveTagBytes
		|| sealed->ciphertext.size() != plaintext.size()) {
		return Refuse(ArchiveEnvelopeStatus::CryptoFailure);
	}
	return { ArchiveEnvelopeStatus::Valid, header + sealed->ciphertext + sealed->tag };
}

ArchiveEnvelopeResult OpenArchiveEnvelope(const ArchiveEnvelopeContext &context,
		const QString &keyId, const QByteArray &frame, const ArchiveCrypto &crypto) {
	if (!IsArchiveEnvelopeContext(context) || !KeyId(keyId)
		|| frame.size() > kArchiveEnvelopeByteLimit + 4608) {
		return {};
	}
	auto reader = Reader{ frame };
	if (reader.take(Magic.size()) != Magic) { return {}; }
	const auto version = reader.number(2);
	if (!reader.valid || version == 0) { return {}; }
	if (version > kArchiveEnvelopeVersion) {
		return Refuse(ArchiveEnvelopeStatus::NewerVersion);
	}
	const auto suite = reader.number(2);
	if (!reader.valid) { return {}; }
	if (suite != 1) { return Refuse(ArchiveEnvelopeStatus::UnsupportedSuite); }
	const auto store = reader.field(128);
	const auto kind = reader.number(2);
	const auto row = reader.field(4096);
	const auto storedKey = reader.field(128);
	const auto nonce = reader.take(kArchiveNonceBytes);
	const auto size = reader.number(4);
	const auto aad = AssociatedData(frame.left(reader.offset), context.account);
	if (!reader.valid || size > kArchiveEnvelopeByteLimit
		|| uint64_t(frame.size() - reader.offset) != size + kArchiveTagBytes) {
		return {};
	}
	if (store != context.storeId.toUtf8() || kind != uint64_t(context.kind)
		|| row != context.rowId) {
		return Refuse(ArchiveEnvelopeStatus::WrongContext);
	}
	if (storedKey != keyId.toUtf8()) { return Refuse(ArchiveEnvelopeStatus::WrongKey); }
	if (!crypto.openAes256Gcm) { return Refuse(ArchiveEnvelopeStatus::CryptoFailure); }
	const auto sealed = ArchiveSealedBytes{ reader.take(qsizetype(size)), reader.take(kArchiveTagBytes) };
	const auto plaintext = crypto.openAes256Gcm(keyId, nonce, sealed, aad);
	if (!plaintext || plaintext->size() != qsizetype(size)) {
		return Refuse(ArchiveEnvelopeStatus::CryptoFailure);
	}
	return { ArchiveEnvelopeStatus::Valid, *plaintext };
}

ArchiveEnvelopeResult WrapArchiveDataKey(const AccountKey &account,
		const QString &storeId, const QString &dataKeyId,
		const QString &platformKeyId, const QByteArray &dataKey,
		const ArchiveCrypto &crypto) {
	if (!KeyId(dataKeyId) || dataKey.size() != kArchiveDataKeyBytes) { return {}; }
	return SealArchiveEnvelope({ account, storeId, ArchiveRecordKind::WrappedKey,
		dataKeyId.toUtf8() }, platformKeyId, dataKey, crypto);
}

ArchiveEnvelopeResult UnwrapArchiveDataKey(const AccountKey &account,
		const QString &storeId, const QString &dataKeyId,
		const QString &platformKeyId, const QByteArray &frame,
		const ArchiveCrypto &crypto) {
	if (!KeyId(dataKeyId)) { return {}; }
	auto result = OpenArchiveEnvelope({ account, storeId, ArchiveRecordKind::WrappedKey,
		dataKeyId.toUtf8() }, platformKeyId, frame, crypto);
	if (result && result.bytes.size() != kArchiveDataKeyBytes) { return {}; }
	return result;
}

std::optional<int> ArchiveMediaChunkSize(uint64_t totalBytes, uint64_t chunkIndex) {
	const auto chunks = totalBytes == 0 ? uint64_t(1)
		: (totalBytes - 1) / kArchiveMediaChunkBytes + 1;
	if (chunkIndex >= chunks) { return {}; }
	return int(std::min(uint64_t(kArchiveMediaChunkBytes), totalBytes - chunkIndex * kArchiveMediaChunkBytes));
}

std::optional<ArchiveEnvelopeContext> ArchiveMediaChunkContext(const AccountKey &account,
		const QString &storeId, const QString &blobToken, uint64_t totalBytes, uint64_t chunkIndex) {
	if (!IsArchiveHash(blobToken) || !ArchiveMediaChunkSize(totalBytes, chunkIndex)) { return {}; }
	auto context = ArchiveEnvelopeContext{ account, storeId, ArchiveRecordKind::MediaChunk,
		(blobToken + '/' + QString::number(totalBytes) + '/' + QString::number(chunkIndex)).toUtf8() };
	return IsArchiveEnvelopeContext(context) ? std::optional(std::move(context)) : std::nullopt;
}

std::optional<ArchiveKeyDerivation> ArchiveKeyDerivationFor(const AccountKey &account,
		const QString &storeId, const QString &profileId, ArchiveKeyPurpose purpose) {
	const auto accountId = AccountKeyText(account.environment, account.userId);
	if (!accountId || !KeyId(storeId) || (!profileId.isEmpty() && !KeyId(profileId))
		|| (purpose == ArchiveKeyPurpose::DesktopWrapping && profileId.isEmpty())
		|| (purpose != ArchiveKeyPurpose::DesktopWrapping && purpose != ArchiveKeyPurpose::IndexTokens)) { return {}; }
	auto result = ArchiveKeyDerivation();
	result.salt = purpose == ArchiveKeyPurpose::DesktopWrapping
		? QByteArray("purple-archive/desktop-wrap/v1") : QByteArray("purple-archive/index/v1");
	result.info = QByteArray("purple-archive/key-context/v1");
	Field(result.info, accountId->toUtf8());
	Field(result.info, storeId.toUtf8());
	Field(result.info, profileId.toUtf8());
	return result;
}

ArchiveKeyAction PlanArchiveKeyOpen(const ArchiveKeyFacts &facts) {
	if (facts.removalPending) { return ArchiveKeyAction::FinishRemoval; }
	if (facts.capability.platform == ArchiveKeyPlatform::Unknown) { return ArchiveKeyAction::UnsupportedPlatform; }
	if ((facts.capability.platform != ArchiveKeyPlatform::Desktop
		&& facts.capability.platform != ArchiveKeyPlatform::Android)
		|| (facts.capability.platform == ArchiveKeyPlatform::Android && facts.capability.androidApiLevel < 0)) {
		return ArchiveKeyAction::Invalid;
	}
	if (!facts.capability.wrappingAvailable
		|| (facts.capability.platform == ArchiveKeyPlatform::Android && facts.capability.androidApiLevel < 23)) {
		return ArchiveKeyAction::UnsupportedPlatform;
	}
	if (facts.storageVersion < 1 || facts.envelopeVersion < 1) { return ArchiveKeyAction::Invalid; }
	if (facts.storageVersion > 1 || facts.envelopeVersion > kArchiveEnvelopeVersion) {
		return ArchiveKeyAction::ReadOnly;
	}
	if (!facts.archiveExists && !facts.wrappedKeyExists) { return ArchiveKeyAction::Create; }
	if (!facts.archiveExists || !facts.wrappedKeyExists || !facts.platformKeyAvailable) {
		return ArchiveKeyAction::Unreadable;
	}
	return ArchiveKeyAction::Unlock;
}

}
