/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"

// The private-chat voice queue rule ([playback] private_chat_queue_p).
//
// In a one-to-one chat, when your manual play starts on the other person's
// voice or round message, automatic "play next" plays only their voice or
// round messages and skips yours. A manual play on your own message, in Saved
// Messages, in a group or in a channel changes nothing.
//
// Pure: it reads only Settings::playback and the facts the client passes in.
// The clients own the tracking of manual versus automatic plays and the walk
// over their playlists; both ask this file which candidates a queue keeps, so
// the rule is stated once. See docs/media-design.md, F3.
namespace Purple {

// The chat the queue plays in, as the rule sees it.
//
// OneToOne is a user chat that is not Saved Messages and not a service chat
// (Replies, Verification Codes); bots and, on Android, secret chats count.
// Self is Saved Messages. Other is everything else: groups, channels and the
// service chats.
enum class PlaybackChat : uchar {
	OneToOne,
	Self,
	Other,
};

// What a manual play decided for the queue it starts. Made once per manual
// play and kept by every automatic advance, so a settings change takes effect
// from the next manual play and never in the middle of a queue.
struct VoiceQueueAnchor {
	// Whether the queue keeps only the other person's messages.
	bool confined = false;
};

// The anchor for a manual play that starts on a message in `chat`.
// `startOutgoing` is the started message's direction: Telegram's sender
// decides, so a message they forwarded to you is incoming and one you
// forwarded is outgoing.
[[nodiscard]] VoiceQueueAnchor AnchorForManualPlay(
	const Playback &settings,
	PlaybackChat chat,
	bool startOutgoing);

// Whether a queue started under `anchor` may play a candidate message whose
// direction is `candidateOutgoing`. An unconfined queue keeps everything.
[[nodiscard]] bool QueueKeeps(
	VoiceQueueAnchor anchor,
	bool candidateOutgoing);

} // namespace Purple
