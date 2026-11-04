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

} // namespace Purple
