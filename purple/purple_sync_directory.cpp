/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_directory.h"
#include "purple/purple_types.h"

#include <map>
#include <set>
#include <tuple>

namespace Purple {
namespace {

[[nodiscard]] bool ValidHash(const QString &hash) {
	if (hash.size() != 64) {
		return false;
	}
	for (const auto character : hash) {
		if (!((character >= u'0' && character <= u'9')
			|| (character >= u'a' && character <= u'f'))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool SameCandidate(
		const SyncDirectoryCandidate &a,
		const SyncDirectoryCandidate &b) {
	return a.documentId == b.documentId
		&& a.editDate == b.editDate
		&& a.original == b.original
		&& a.status == b.status
		&& a.recordHash == b.recordHash
		&& a.payloadValidated == b.payloadValidated
		&& ((!a.header && !b.header)
			|| (a.header && b.header
				&& a.header->space == b.header->space
				&& a.header->stream == b.header->stream
				&& a.header->writerInstall == b.header->writerInstall
				&& a.header->writerDevice == b.header->writerDevice
				&& a.header->writerPlatform == b.header->writerPlatform
				&& a.header->writerApp == b.header->writerApp
				&& a.header->seq == b.header->seq
				&& a.header->at == b.header->at));
}

[[nodiscard]] bool HasHeader(SyncEnvelopeStatus status) {
	return status == SyncEnvelopeStatus::Valid
		|| status == SyncEnvelopeStatus::UnsupportedStream
		|| status == SyncEnvelopeStatus::UnsupportedEncoding;
}

} // namespace

SyncDirectory ResolveSyncDirectory(
		const std::vector<SyncDirectoryCandidate> &candidates,
		bool inventoryComplete) {
	auto result = SyncDirectory();
	result.complete = inventoryComplete;
	auto groups = std::map<std::tuple<QString, QString, QString>, size_t>();
	auto messages = std::map<int64_t, SyncDirectoryCandidate>();
	for (const auto &candidate : candidates) {
		if (!candidate.original) {
			continue;
		}
		if (candidate.messageId <= 0 || candidate.documentId == 0) {
			result.unreadableCandidate = true;
			continue;
		}
		const auto [message, inserted] = messages.emplace(
			candidate.messageId, candidate);
		if (!inserted) {
			if (!SameCandidate(message->second, candidate)) {
				result.messageIdCollision = true;
			}
			continue;
		}
		if (!HasHeader(candidate.status)) {
			result.unreadableCandidate = true;
			continue;
		}
		if (!candidate.header
			|| !IsSyncSpaceId(candidate.header->space)
			|| !IsSyncInstallId(candidate.header->writerInstall)
			|| candidate.header->stream.isEmpty()
			|| candidate.header->seq == 0) {
			result.unreadableCandidate = true;
			continue;
		}
		const auto &header = *candidate.header;
		const auto key = std::tuple(
			header.space, header.stream, header.writerInstall);
		const auto [entry, created] = groups.emplace(
			key, result.groups.size());
		if (created) {
			result.groups.push_back({
				header.space, header.stream, header.writerInstall });
		}
		result.groups[entry->second].records.push_back(candidate);
		if (!result.selectedSpace
			|| *CompareSyncSpaceIds(header.space, *result.selectedSpace) < 0) {
			result.selectedSpace = header.space;
		}
	}
	for (auto &group : result.groups) {
		auto highest = uint64_t(0);
		for (const auto &record : group.records) {
			const auto seq = record.header->seq;
			if (seq > highest) {
				highest = seq;
				group.headCandidates.clear();
			}
			if (seq == highest) {
				group.headCandidates.push_back(record);
			}
		}
		auto hashes = std::set<QString>();
		for (const auto &head : group.headCandidates) {
			if (ValidHash(head.recordHash)) {
				hashes.insert(head.recordHash);
			}
		}
		group.ambiguous = hashes.size() > 1;
		group.supportedHead = !group.ambiguous
			&& hashes.size() == 1
			&& (group.stream == u"config"_q
				|| group.stream == u"library"_q);
		for (const auto &head : group.headCandidates) {
			if (head.status != SyncEnvelopeStatus::Valid
				|| !head.payloadValidated
				|| !ValidHash(head.recordHash)) {
				group.supportedHead = false;
			}
		}
	}
	if (inventoryComplete
		&& !result.unreadableCandidate
		&& !result.messageIdCollision) {
		auto ambiguousSelectedSpace = false;
		for (const auto &group : result.groups) {
			if (group.ambiguous
				&& result.selectedSpace == group.space) {
				ambiguousSelectedSpace = true;
			}
		}
		if (!ambiguousSelectedSpace) {
			result.publishableSpace = result.selectedSpace;
		}
		result.canCreateSpace = !result.selectedSpace;
	}
	return result;
}

} // namespace Purple
