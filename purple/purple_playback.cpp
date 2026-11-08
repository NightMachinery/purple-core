/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playback.h"

namespace Purple {

VoiceQueueAnchor AnchorForManualPlay(
		const Playback &settings,
		PlaybackChat chat,
		bool startOutgoing) {
	return {
		.confined = settings.privateChatQueue
			&& chat == PlaybackChat::OneToOne
			&& !startOutgoing,
	};
}

bool QueueKeeps(VoiceQueueAnchor anchor, bool candidateOutgoing) {
	return !anchor.confined || !candidateOutgoing;
}

VoiceQueueDecision ContinuationDecision(
		VoiceContinuation mode,
		bool outgoing,
		bool unheard,
		bool keptByOwnership) {
	switch (mode) {
	case VoiceContinuation::ResumeMusic:
		return VoiceQueueDecision::Stop;
	case VoiceContinuation::Unheard:
		return outgoing ? VoiceQueueDecision::Skip
			: unheard ? VoiceQueueDecision::Play : VoiceQueueDecision::Stop;
	case VoiceContinuation::AllFollowing:
		return keptByOwnership ? VoiceQueueDecision::Play : VoiceQueueDecision::Skip;
	}
	return VoiceQueueDecision::Stop;
}

} // namespace Purple
