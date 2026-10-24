/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "tests/archive_test_support.h"
#include "purple/purple_archive_envelope.h"
#include "purple/purple_archive_lifecycle.h"
#include "purple/purple_archive_rows.h"
#include "purple/purple_archive_sql.h"
#include "purple/purple_archive_sql_maintenance.h"

#include <QtCore/QCryptographicHash>

#include <memory>

namespace ArchiveStorageTest {

inline QByteArray TestTag(const QString &key, const QByteArray &nonce,
		const QByteArray &ciphertext, const QByteArray &aad) {
	return QCryptographicHash::hash(key.toUtf8() + nonce + aad + ciphertext,
		QCryptographicHash::Sha256).left(kArchiveTagBytes);
}

inline ArchiveCrypto FakeCrypto() {
	auto crypto = ArchiveCrypto();
	auto counter = std::make_shared<uint64_t>(0);
	crypto.random = [counter](int size) -> std::optional<QByteArray> {
		auto nonce = QByteArray(size, '\0');
		const auto value = ++*counter;
		for (auto i = 0; i != 8 && i != size; ++i) { nonce[i] = char(value >> (8 * i)); }
		return nonce;
	};
	crypto.sealAes256Gcm = [](const QString &key, const QByteArray &nonce,
			const QByteArray &plain, const QByteArray &aad) -> std::optional<ArchiveSealedBytes> {
		auto ciphertext = plain;
		for (auto &byte : ciphertext) { byte ^= 0x5a; }
		return ArchiveSealedBytes{ ciphertext, TestTag(key, nonce, ciphertext, aad) };
	};
	crypto.openAes256Gcm = [](const QString &key, const QByteArray &nonce,
			const ArchiveSealedBytes &sealed, const QByteArray &aad) -> std::optional<QByteArray> {
		if (TestTag(key, nonce, sealed.ciphertext, aad) != sealed.tag) { return {}; }
		auto plaintext = sealed.ciphertext;
		for (auto &byte : plaintext) { byte ^= 0x5a; }
		return plaintext;
	};
	return crypto;
}

inline ArchiveSqlSecurity Security() {
	return { u"test-store-incarnation"_q, u"test-data-key"_q, FakeCrypto(),
		[](const QByteArray &bytes) { return std::optional(QCryptographicHash::hash(
			QByteArray("fixture-index-key") + bytes, QCryptographicHash::Sha256)); } };
}

inline ArchiveRemovalAck Ack(const ArchiveRemoval &removal) {
	return { removal.epoch.account, removal.epoch.storeId, removal.operationId,
		removal.epoch.generation, removal.action, true, true, 0, 0, true };
}

}
