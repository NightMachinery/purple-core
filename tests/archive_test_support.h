/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_codec.h"
#include "purple/purple_archive_store.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <cstdio>
#include <limits>

using namespace Purple;

namespace ArchiveTest {

inline auto Checks = 0;
inline auto Failures = 0;
inline const char *Section = "";

inline void Report(bool ok, const char *expression, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL %s:%d %s\n", Section, line, expression);
	}
}

#define CHECK(condition) ArchiveTest::Report(bool(condition), #condition, __LINE__)

inline ArchiveState Empty(AccountKey account = { 0, 123 }) {
	auto state = ArchiveState();
	state.account = account;
	return state;
}

inline ArchiveMessageKey Key(AccountKey account = { 0, 123 }, int64 chatId = 45,
		int64 messageId = 67, ArchiveChatKind kind = ArchiveChatKind::Channel) {
	auto key = ArchiveMessageKey();
	key.account = account;
	key.chat = { kind, chatId };
	key.messageId = messageId;
	return key;
}

inline ArchiveEvent Event(const ArchiveState &state, QString id, uint64_t order,
		QString text = u"A"_q, ArchiveOrigin origin = ArchiveOrigin::Baseline,
		ArchiveMessageKey key = Key()) {
	auto event = ArchiveEvent();
	event.key = std::move(key);
	event.id = std::move(id);
	event.order = order;
	event.origin = origin;
	event.generation = ArchiveGenerationFor(state, event.key.chat);
	event.observation.observedAt = 100;
	event.observation.serverTime = 90;
	event.observation.representation = event.key.representation;
	event.completeness = ArchiveCompleteness::Complete;
	if (origin == ArchiveOrigin::Baseline || origin == ArchiveOrigin::AuthoritativeEdit
		|| origin == ArchiveOrigin::Hydration) {
		event.content = ArchiveContent();
		event.content->text = std::move(text);
	}
	return event;
}

inline ArchivePurgeRequest Purge(const ArchiveState &state, ArchivePurgeKind kind,
		std::optional<ArchiveChatKey> chat = std::nullopt) {
	auto request = ArchivePurgeRequest();
	request.kind = kind;
	request.scope.account = state.account;
	request.scope.chat = std::move(chat);
	request.expectedRevision = state.revision;
	return request;
}

inline void RoundTrip(const ArchiveState &state) {
	CHECK(IsArchiveState(state));
	const auto bytes = EncodeArchiveSnapshot(state);
	CHECK(bytes.has_value());
	if (bytes) {
		const auto decoded = DecodeArchiveSnapshot(*bytes);
		CHECK(decoded);
		CHECK(decoded.state && *decoded.state == state);
		CHECK(decoded.state && EncodeArchiveSnapshot(*decoded.state) == bytes);
	}
}

inline bool Apply(ArchiveState &state, const ArchiveTransaction &transaction) {
	CHECK(transaction);
	if (!transaction) { return false; }
	CHECK(transaction.expected == state);
	const auto beforeCommit = CompleteArchiveTransaction(state, transaction, false);
	CHECK(beforeCommit.status == ArchiveCommitStatus::NotCommitted);
	CHECK(beforeCommit.state == state);
	const auto result = CompleteArchiveTransaction(state, transaction, true);
	CHECK(result.status == ArchiveCommitStatus::Applied
		|| result.status == ArchiveCommitStatus::AlreadyApplied);
	state = result.state;
	const auto afterCommit = CompleteArchiveTransaction(state, transaction, true);
	CHECK(afterCommit.status == ArchiveCommitStatus::AlreadyApplied);
	CHECK(afterCommit.state == state);
	RoundTrip(state);
	return true;
}

inline const ArchiveMessage &Message(const ArchiveState &state, const ArchiveMessageKey &key = Key()) {
	const auto result = FindArchiveMessage(state, key);
	CHECK(result);
	static const auto EmptyMessage = ArchiveMessage();
	return result ? *result : EmptyMessage;
}

inline ArchiveMediaJob Job(const ArchiveState &state, QString id, QString version,
		ArchiveMessageKey key = Key()) {
	auto job = ArchiveMediaJob();
	job.id = std::move(id);
	job.key = std::move(key);
	job.version = std::move(version);
	job.mediaIdentity = u"document:555"_q;
	job.generation = ArchiveGenerationFor(state, job.key.chat);
	job.expectedSize = 7;
	return job;
}

inline const auto BlobId = QString(64, u'a');

inline ArchiveMediaResult MediaSuccess() {
	auto result = ArchiveMediaResult();
	result.success = true;
	result.blobId = BlobId;
	result.verifiedHash = BlobId;
	result.size = 7;
	return result;
}

inline void AddMedia(ArchiveEvent &event) {
	event.content->media.push_back({ u"document:555"_q, u"document"_q });
}

inline int Finish() {
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}

}
