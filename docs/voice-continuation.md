# After a voice message

App-wide playback settings shared with clients through settings.toml:

- `after_voice = "unheard"` (default): skip outgoing voice/round messages;
  play incoming messages until the first with the client's listened flag set.
- `after_voice = "resume_music"`: stop after the tapped message.
- `after_voice = "all_following"`: use the existing private_chat_queue_p
  ownership rule and play every following eligible voice/round message.
- `resume_music_after_voice_p = true` (default): resume interrupted music
  playing after the selected queue ends. False restores the track paused.

ContinuationDecision is independent of media kind: clients pass only eligible
voice and round messages and stop scanning after Stop. They snapshot settings
on a manual play. The core does not infer unheard from playback position;
Android passes isContentUnread(), a server-synced flag that can be changed by
another client. Desktop may adopt these keys later without renaming them.
