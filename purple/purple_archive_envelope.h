/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_model.h"

#include <functional>

namespace Purple {

inline constexpr auto kArchiveEnvelopeVersion = 1;
inline constexpr auto kArchiveEnvelopeByteLimit = 4 * 1024 * 1024;
inline constexpr auto kArchiveDataKeyBytes = 32;
inline constexpr auto kArchiveNonceBytes = 12;
inline constexpr auto kArchiveTagBytes = 16;
inline constexpr auto kArchiveMediaChunkBytes = 1024 * 1024;

enum class ArchiveRecordKind {
	Account = 1,
	Message = 2,
	Version = 3,
	Event = 4,
	Blob = 5,
	Job = 6,
	Lease = 7,
	Generation = 8,
	MediaChunk = 9,
	WrappedKey = 10,
	CaptureGap = 11,
	Maintenance = 12,
};

struct ArchiveEnvelopeContext {
	AccountKey account;
	QString storeId;
	ArchiveRecordKind kind = ArchiveRecordKind::Account;
	QByteArray rowId;
	friend bool operator==(const ArchiveEnvelopeContext &,
		const ArchiveEnvelopeContext &) = default;
};

struct ArchiveSealedBytes {
	QByteArray ciphertext;
	QByteArray tag;
};

struct ArchiveCrypto {
	std::function<std::optional<QByteArray>(int)> random;
	std::function<std::optional<ArchiveSealedBytes>(const QString &keyId,
		const QByteArray &nonce, const QByteArray &plaintext,
		const QByteArray &aad)> sealAes256Gcm;
	std::function<std::optional<QByteArray>(const QString &keyId,
		const QByteArray &nonce, const ArchiveSealedBytes &sealed,
		const QByteArray &aad)> openAes256Gcm;
};

enum class ArchiveEnvelopeStatus {
	Valid,
	Invalid,
	NewerVersion,
	UnsupportedSuite,
	WrongContext,
	WrongKey,
	CryptoFailure,
};

struct ArchiveEnvelopeResult {
	ArchiveEnvelopeStatus status = ArchiveEnvelopeStatus::Invalid;
	QByteArray bytes;
	[[nodiscard]] explicit operator bool() const {
		return status == ArchiveEnvelopeStatus::Valid;
	}
};

[[nodiscard]] bool IsArchiveEnvelopeContext(const ArchiveEnvelopeContext &context);
[[nodiscard]] ArchiveEnvelopeResult SealArchiveEnvelope(
	const ArchiveEnvelopeContext &context,
	const QString &keyId,
	const QByteArray &plaintext,
	const ArchiveCrypto &crypto);
[[nodiscard]] ArchiveEnvelopeResult OpenArchiveEnvelope(
	const ArchiveEnvelopeContext &context,
	const QString &keyId,
	const QByteArray &frame,
	const ArchiveCrypto &crypto);
[[nodiscard]] ArchiveEnvelopeResult WrapArchiveDataKey(
	const AccountKey &account,
	const QString &storeId,
	const QString &dataKeyId,
	const QString &platformKeyId,
	const QByteArray &dataKey,
	const ArchiveCrypto &crypto);
[[nodiscard]] ArchiveEnvelopeResult UnwrapArchiveDataKey(
	const AccountKey &account,
	const QString &storeId,
	const QString &dataKeyId,
	const QString &platformKeyId,
	const QByteArray &frame,
	const ArchiveCrypto &crypto);

[[nodiscard]] std::optional<ArchiveEnvelopeContext> ArchiveMediaChunkContext(
	const AccountKey &account,
	const QString &storeId,
	const QString &blobToken,
	uint64_t totalBytes,
	uint64_t chunkIndex);
[[nodiscard]] std::optional<int> ArchiveMediaChunkSize(uint64_t totalBytes, uint64_t chunkIndex);

enum class ArchiveKeyPurpose { DesktopWrapping, IndexTokens };

struct ArchiveKeyDerivation {
	QByteArray salt;
	QByteArray info;
	int outputBytes = 32;
};

[[nodiscard]] std::optional<ArchiveKeyDerivation> ArchiveKeyDerivationFor(
	const AccountKey &account, const QString &storeId, const QString &profileId,
	ArchiveKeyPurpose purpose);

enum class ArchiveKeyAction { Create, Unlock, Unreadable, ReadOnly, FinishRemoval, UnsupportedPlatform, Invalid };
enum class ArchiveKeyPlatform { Unknown, Desktop, Android };

struct ArchiveKeyCapability {
	ArchiveKeyPlatform platform = ArchiveKeyPlatform::Unknown;
	int androidApiLevel = 0;
	bool wrappingAvailable = false;
};

struct ArchiveKeyFacts {
	bool archiveExists = false;
	bool wrappedKeyExists = false;
	bool platformKeyAvailable = false;
	bool removalPending = false;
	int64 storageVersion = 1;
	int64 envelopeVersion = 1;
	ArchiveKeyCapability capability;
};

[[nodiscard]] ArchiveKeyAction PlanArchiveKeyOpen(const ArchiveKeyFacts &facts);

}
