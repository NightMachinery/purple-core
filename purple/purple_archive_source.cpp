/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_source.h"

#include "purple/purple_archive_internal.h"

namespace Purple {

ArchiveCopySourceChoice ChooseCopySource(
		const ArchiveState &state,
		const ArchiveCopyRequest &request) {
	auto result = ArchiveCopySourceChoice();
	if (!IsArchiveState(state) || !IsArchiveMessageKey(request.key)
		|| request.key.account != state.account
		|| !IsArchiveToken(request.version) || !IsArchiveToken(request.revision)
		|| !IsArchiveHash(request.contentHash)
		|| request.displayedRepresentation != request.key.representation
		|| !ArchiveInternal::IsGeneration(state, request.key, request.generation)
		|| (request.extent != ArchiveCopyExtent::FullBody
			&& request.extent != ArchiveCopyExtent::Selection
			&& request.extent != ArchiveCopyExtent::Fragment)
		|| (request.target != ArchiveCopyTarget::CurrentMessage
			&& request.target != ArchiveCopyTarget::ArchivedVersion)) {
		return result;
	}
	const auto message = FindArchiveMessage(state, request.key);
	const auto version = FindArchiveVersion(state, request.key, request.version);
	if (!message || !version || message->payloadPurged
		|| version->revision != request.revision
		|| ArchiveContentHash(version->content) != request.contentHash
		|| version->observation.representation != request.displayedRepresentation
		|| (request.target == ArchiveCopyTarget::CurrentMessage
			&& (message->deleted || version != &message->versions.back()))) {
		return result;
	}
	result.validTarget = true;
	result.partial = version->completeness != ArchiveCompleteness::Complete;
	if (request.extent != ArchiveCopyExtent::FullBody
		|| request.displayedRepresentation != u"original"_q
		|| !ArchiveSourceMatches(*version) || !version->source->binding
		|| !ArchiveInternal::SourceOwnedBy(*version->source, request.key, request.version)) {
		return result;
	}
	result.kind = ArchiveCopySourceKind::GenuineSource;
	result.bytes = version->source->bytes;
	return result;
}

}
