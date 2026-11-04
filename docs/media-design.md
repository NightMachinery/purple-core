# Media playback design (F1-F5)

Coordinator decisions for the three open questions at the end (3 Nov 2026): Q1 yes (ship both Android F1 hooks; device confirmation comes later, outside the cloud), Q2 yes (Desktop F1 hover controls as the last Desktop commit), Q3 option A (200% = 480px with the softness note).
Implementation starts now; the upstream merge has landed (Desktop v7.3.3, Android 13.1.1), so re-anchor every hook by class and function: the line numbers below predate the merge.

# Media playback design: round PiP, double-click restart, private-chat voice queue, round sizes

@Opus5.5 [2026-10-31 Sat]. Design only. Nothing was built, run or sent. Evidence comes from reading code at:

- Desktop `the Desktop repository`, branch `dev` at `7916710aa8`, upstream merge base `8e18cb7110` (2026-08-07).
- Android `the Android repository`, upstream base `62b56a07c` (2026-08-25, "update to 12.10.1").

Desktop paths are relative to `Telegram/SourceFiles/`. Android paths are relative to
`TMessagesProj/src/main/java/org/telegram/`.

## Summary

Five features. These terms are used throughout:

- **Round**: a round video message (video note).
- **Voice queue**: the automatic "play the next voice or round message" behaviour.
- **Hook**: an edit to an upstream file, allowed only as one include or import, one guarded call, or one early
  return (AGENTS.md "Keeping upstream merges easy", in both repositories).
- **Churn**: how many upstream commits edited a file, or the hooked function, in the year before the last
  merge base. Desktop has about 1,000 upstream commits a year; Android has 52, each one a whole release.
  A **hot** file is one upstream edits in most releases. Function-level churn is the figure that predicts
  conflicts, and it is low at every hook site below.

The five features:

- **F1, round picture-in-picture.** The owner is right: Desktop already does this (details below). Android
  also already has a floating round (`PipRoundVideoView`) that survives navigation. The real gap is on
  Android: a tap on its PiP cannot take you back to the message, and the chat scrolls you back to the playing
  round whenever something other than your finger moves the list. Desktop work is optional: hover controls.
- **F2, double-click or double-tap restarts.** Applies to rounds and voice messages on both clients.
- **F3, private-chat voice queue** (setting, default ON). The rule and the setting live in purple-core. The
  manual-versus-automatic tracking needs no new upstream line on Desktop.
- **F4, the floating round keeps its in-chat size** (Desktop, device-local setting, default ON).
- **F5, video message size** (Desktop only, device-local setting, default 200%). Android's round is already
  60% of the screen's short side, and it grows to almost full width while it plays.

Upstream totals for all five features, required hooks only:

- Desktop: 5 files, about +23 lines.
- Android: 4 files, about +6 −1 lines.
- Together: **9 upstream files, about 30 changed lines.**
- The optional Desktop F1 hover controls add 1 line in a file that is already counted.

There are **3 open questions**, listed at the end.

Implementation starts only after the upstream merge (owner, 31 Oct). Every hook below is therefore anchored by
class and function as well as by file:line, and the line numbers will move. The local `upstream/dev` ref
equals the merge base (it has not been fetched since), so the churn figures cover the year before the base.
Re-run the two churn commands in "Re-anchoring after the merge" once the merge lands.

## Shared infrastructure

### Settings

- **F3** goes in `settings.toml`, as the brief asks. It is a new app-wide table, parsed in purple-core:

  ```toml
  [playback]
  private_chat_queue_p = true
  ```

  - It follows the pattern of `ReadSuggestions` (`purple_core/purple/purple_settings.cpp:1509-1534`, a bool
    read with `.value_or(default)` and a warning for wrong types).
  - It gets a `Playback playback;` member in `struct Settings` (`purple_settings.h:966-999`).
  - It gets a scope entry `{ u"playback"_q, SettingScope::AppWide }` in the table list
    (`purple_settings.cpp:1855-1882`).
  - It gets a "Playback" label in `purple_config_diff.cpp:583`'s key-to-title switch.
  - App-wide means the answer syncs to the other devices along with `settings.toml`. That suits a listening
    preference.
- **F4 and F5** are device-local, not in `settings.toml`. A size is a fact about one screen. Syncing
  `200%` from a laptop to a phone, or to a small second laptop, would be wrong.
  - Desktop keeps them where the dash preference lives, the generic Purple preference store
    (`purple-replace-dashes`, see `docs/purple/upstream_hooks.md` "Settings UI and instant replaces").
  - The keys are `purple-video-message-scale` (an integer percent) and `purple-video-message-float-keep`
    (a bool).
  - A missing key reads as the default.

### Settings rows

No upstream file changes for any settings row.

- **Desktop:** a "Media playback" group inside the Purple section on the Advanced page. That section is built
  by `Purple::BuildAdvancedSection` (`purple/purple_settings_section.cpp:718-720`), and its existing toggle
  pattern is "Local Premium features" at `:685-701`. The group has three rows: the F3 toggle, the F5 size
  chooser (a single-choice box: 100, 150, 200, 250%), and the F4 toggle.
- **Android:** one check row in `PurpleSettingsActivity`, built the way the existing check row is
  (`ui/PurpleSettingsActivity.java:244-247` for the item, `:495-498` for the click). It takes a fresh id from
  the `0x70750000 + n` range; `0x70750021` and `0x70750026` are taken, so the implementer picks the next free
  one. It writes through the existing TOML splice path.

### Code placement

- Desktop: `Telegram/SourceFiles/purple/purple_media_playback.{h,cpp}` for F1, F2, F4 and F5, and
  `purple/purple_voice_queue.{h,cpp}` for F3.
- Desktop: a facade header `purple/hooks/media_playback.h` gathers every declaration that upstream files
  include (convention D5, `docs/purple/upstream_hooks.md:207-240`). Each upstream file gets exactly one
  Purple include, on the line after its own header.
  - Two hook files already carry three older Purple includes each (`history_inner_widget.cpp:9-11`,
    `history_view_list_widget.cpp:10-12`).
  - For those two files, add the declarations to whichever facade the refactor batches settle on there. If
    none exists yet at implementation time, add one include and record it in the hook entry.
- Desktop sources are registered in `Telegram/cmake/purple.cmake`.
- Android: `messenger/purple/PurpleVoiceQueue.java`, `messenger/purple/PurpleMediaRestart.java` and
  `messenger/purple/PurpleRoundPip.java`. ChatActivity hooks go through its per-instance helper
  `PurpleChatHooks purple` (`ui/ChatActivity.java:400`), so ChatActivity imports nothing. Files with one or
  two call sites use fully qualified names (AGENTS.md rules).
- purple-core: `purple/purple_playback.{h,cpp}` holds the F3 rule. It is described under F3.

## F1. Round picture-in-picture

### Desktop: current behaviour (already done)

Desktop already lets you use Telegram normally while a round plays.

- **Ownership.** `Core::Application` owns one floating-player controller (`_floatPlayers`). It rebinds the
  controller to the last active window's delegate (`core/application.cpp:1563-1597`, `Media::Player::FloatController::replaceDelegate`
  at `media/player/media_player_float.cpp:330-342`).
- **Creation.** `FloatController` creates a `Float` whenever the current voice-type track is a round
  (`FloatController::checkCurrent`, `float.cpp:382-428`).
- **Hiding while visible inline.** It hides only while the message itself is on screen
  (`FloatController::checkVisibility`, `float.cpp:446-458`, asking `MainWidget::floatPlayerIsVisible`,
  `mainwidget.cpp:561-563`).
- **Survival.** It survives switching chats, opening Settings or Info (as a section or as a layer, with
  `Info::LayerWidget` acting as the delegate, `info/info_layer_widget.cpp:35,45,81-88`) and switching
  windows. A chat switch does not stop voice playback.
- **Click:** pause or resume (`Float::mouseReleaseEvent`, `float.cpp:169-172`, `Float::pauseResume`
  `:193-201`).
- **Drag:** moves it. The corner and column are saved in Core settings (`float.cpp:139-149`, `:594-635`).
- **Drag mostly off-screen:** closes it and stops playback (`Float::finishDrag`, `float.cpp:637-663`, then
  `MainWidget::floatPlayerClosed` → `stopAndClosePlayer`, `mainwidget.cpp:299-301`, `:565-573`).
- **Double-click:** undoes the first click's pause and jumps to the message (`Float::mouseDoubleClickEvent`,
  `float.cpp:185-191`, then `MainWidget::floatPlayerDoubleClickEvent` → `showMessage`, `mainwidget.cpp:575-578`).
- **Removal.** It detaches when the message is deleted (`float.cpp:118-123`) or the session changes
  (`:125-129`).

### Desktop: the gap and the optional design

The only gap is discoverability. The float shows no close or pause control, closing it means dragging it off
the window, and "double-click to open" is invisible.

**Optional, not required: hover controls.**

- `Purple::DecorateFloat(not_null<Float*>)` adds a child overlay widget. It is transparent to drags and
  appears on hover, with a pause/play glyph, a close cross, and a tooltip "Double-click to open the message".
- Close calls `Media::Player::instance()->stop(AudioMsgId::Type::Voice)`, the same route the drag-out close
  takes.
- The overlay handles clicks only inside its two buttons, so drag and double-click keep working everywhere
  else.
- Hook: one call at the end of the `Float::Float` constructor (`media/player/media_player_float.cpp:95-133`).
  The file is already counted under F4, so this adds +1 line.

### Android: current behaviour

- **Leaving the chat.** The playing round moves to `PipRoundVideoView` when you leave the chat, including
  when you open another screen on top of it.
  - `ActionBarLayout` calls `onRemoveFromParent` on the previous fragment
    (`ui/ActionBar/ActionBarLayout.java:2353, 2589, 2759`).
  - `ChatActivity.onRemoveFromParent` (`ui/ChatActivity.java:13738-13748`) hands the texture to
    `MediaController.setTextureView(..., false)` (`messenger/MediaController.java:3215-3220`).
  - The PiP is created on surface loss (`:3777-3795`).
- **Window.** It is an application-level window over the whole activity (`ui/Components/PipRoundVideoView.java:291-294`),
  so it survives the chat list, other chats and Settings.
- **Tap:** pause or resume (`PipRoundVideoView.java:157-167`).
- **Drag:** moves it. The position is saved in "pipconfig" (`:121-156`, `:276-281`).
- **Fling to an edge:** closes it and calls `cleanupPlayer` (`:448-484`, `:515-524`, `MediaController.java:3195`).
- **Gap 1: no way back from the PiP.** A tap never returns to the message. Only the top player bar does that
  (`ui/Components/FragmentContextView.java:763-791`).
- **Gap 2: the chat pulls you back.** In `ChatActivity.updateTextureViewPosition` (`ChatActivity.java:15795-15818`),
  when the playing round leaves the visible part of the list:
  - if your finger did not cause the move (`checkTextureViewPosition` is false, which is set at `:6830` and
    cleared at `:6802`),
  - and no PiP exists yet,
  - then it calls `scrollToMessageId` back to the round instead of switching to the PiP.

  So tapping a reply quote, a search result or the scroll-to-bottom button while a round plays yanks the chat
  back. This is inferred from the code and needs a runtime check (open question 1).

### Android: design

- **Double-tap the PiP to open the message.** This mirrors Desktop's float.
  - `PurpleRoundPip.onTap(MessageObject)` remembers the time of the last tap.
  - On a second tap within `ViewConfiguration.getDoubleTapTimeout()`, it undoes the first tap's pause or
    resume (the same trick Desktop's `Float::mouseDoubleClickEvent` uses).
  - It then opens the chat at the message through the same route `FragmentContextView` uses: a `ChatActivity`
    with `chat_id`/`user_id` and `message_id`, or `scrollToMessageId` if that chat is already on top. That
    route is the one users already take, so Work Mode's chat-open policy applies exactly as it does today.
  - A single tap keeps today's pause or resume.
  - Hook: one guarded early return in the `ACTION_UP` branch of the PiP's `onTouchEvent`
    (`PipRoundVideoView.java:157-158`, inside `if (startDragging && !dragging)`), before the toggle.
- **Go to the PiP instead of scrolling back.** In `ChatActivity.updateTextureViewPosition`, the condition
  `if (checkTextureViewPosition || PipRoundVideoView.getInstance() != null)` gains
  `|| purple.roundLeavesToPip()`. That is one changed upstream line. The helper returns true, so a round
  that leaves the list for any reason continues in the PiP, the same as when you scroll it away by hand.
  - This is the one hook that edits an existing line instead of adding one. There is no place for an early
    return that would not change the surrounding branch, and that else-if is the smallest extension point.
  - Record it in the README section.

### Edge cases (both clients)

- **Calls.**
  - Desktop pauses voice-type playback on call start and resumes at call end, unless you paused or played by
    hand in between (`Media::Player::Instance` `pauseOnCall`/`resumeOnCall`, `media_player_instance.cpp:187-200`,
    `:1125-1146`).
  - Android pauses on telephony RINGING and on audio-focus loss from `VoIPService` (`MediaController.java:1451-1474`,
    `:1537-1564`).
  - The PiP and the float stay and show the paused frame. No change.
- **Other audio.**
  - Desktop: a round suppresses music (`media/audio/media_audio.cpp:728-729`). The media viewer pauses both
    (`media/view/media_view_overlay_widget.cpp:5851-5852`).
  - Android: another player starting pauses the round (`MediaController.java:1956-1964`).
  - No change on either client.
- **Message deleted while playing.**
  - Desktop: the float detaches and the player stops (`instance.cpp:326-331`).
  - Android: `messagesDeleted` → `cleanupPlayer` closes the PiP (`MediaController.java:1845-1871`).
  - The Android double-tap checks that the message still exists before opening the chat. If it is gone, it
    does nothing.
- **Global music player.** It is untouched. It is a separate Song player on Desktop and a separate playlist
  on Android.
- **Work Mode.** Opening the message from the PiP uses the stock open route, so a chat the running preset
  hides behaves as it does when you open it from the top player bar today. F1 adds no new bypass.

## F2. Double-click or double-tap restarts from 0:00

Decision: voice messages get the same behaviour as rounds. On both clients a voice message also resumes from
a saved position or from where it was paused. "Start over" is the same need, and the hook sites are shared.

### Desktop: current behaviour

- **Click on a playing or paused round.** The click lands on `_seekl`, a `VoiceSeekClickHandler`
  (`history/view/media/history_view_gif.cpp:238-242`), which derives from `DocumentOpenClickHandler`
  (`data/data_file_click_handler.h:52`). Press and release without a drag toggles pause through
  `playPauseCancelClicked(Voice)` (`Gif::clickHandlerPressedChanged`, `gif.cpp:1640-1663`).
- **Click on a stopped round.** `_openl` → `HistoryInner::elementOpenDocument` → `Data::ResolveDocument` →
  `Instance::playPause(AudioMsgId, ctx)` (`data/data_document_resolver.cpp:259-266`, `instance.cpp:867-877`).
- **Saved position.** `Instance::streamingOptions` resumes from the saved last position and then clears it
  (`instance.cpp:905-925`).
- **Double-click.** `HistoryInner::mouseDoubleClickEvent` (`history/history_inner_widget.cpp:2712-2759`) and
  `ListWidget::mouseDoubleClickEvent` (`history/view/history_view_list_widget.cpp:3363-3387`) call
  `mouseActionStart` again, so a double-click on a round is two toggles. The Reply or React quick action is
  skipped while a click handler is active. This is inferred from code, not observed.
- **Restart from code.** `Instance::finishSeeking(Voice, 0.)` restarts the current track at 0
  (`instance.cpp:1192-1209`).

### Desktop: design

`Purple::RestartOnDoubleClick(const ClickHandlerPtr &active, bool selecting)`:

- **Rejects:**
  - a double-click while selecting messages;
  - an active handler that is not a `DocumentOpenClickHandler` for a voice or video message;
  - TTL (view-once) media;
  - a document that is not the current voice-type track.
- **Otherwise:** calls `finishSeeking(AudioMsgId::Type::Voice, 0.)`. If the track is paused it then calls
  `play(Voice)`. It returns true.
- **Upstream hooks:** one early return at the top of `HistoryInner::mouseDoubleClickEvent` and of
  `ListWidget::mouseDoubleClickEvent`, before `mouseActionStart`. The second release then finds no pressed
  handler, so it toggles nothing.
- **What you see.** The first click of the pair still toggles. A playing round pauses for under one
  double-click interval and then restarts. A stopped one starts and then jumps to 0:00. Delaying every single
  click to wait for a possible second one would avoid the blip, but it would add latency to the common case,
  so this design does not do it.
- **The floating player.** Its double-click keeps opening the message, as it does today.

### Android: current behaviour

- **Round tap.** `ChatMessageCell.didClickedImage` (`ui/Cells/ChatMessageCell.java:6012`, round branch
  `:6045-6054`):
  - while downloading, it calls `didPressButton`;
  - if the round is not playing, or is paused, it calls `needPlayMessage`;
  - otherwise it calls `pauseMessage`.
  - So two taps are pause and resume.
- **Voice play button.** `ChatMessageCell.didPressButton` (`:17858`). `buttonState` 0 plays and 1 pauses.
- **Double-tap to react.** It is fed only when the cell did not consume the touch
  (`ui/Components/RecyclerListView.java:1256-1266`). A tap inside the round circle is consumed
  (`ChatMessageCell.java:4317-4320`), so the reaction double-tap never fires on the circle or on the play
  button.
- **Restart from code.** `MediaController.seekToProgress(msg, 0f)` (`MediaController.java:2611-2642`),
  followed by `playMessage(msg)` if the track is paused (`:3624-3632`).

### Android: design

`PurpleMediaRestart.onTap(MessageObject)`:

- It is true when this is the second tap within `getDoubleTapTimeout()` on the same message, the message is
  a voice or round that is the playing message (`isPlayingMessage`), and it is not a view-once message.
  View-once messages go to `SecretVoicePlayer` (`ChatActivity.java:39413-39434`).
- In that case it seeks to 0, resumes if paused, and the caller returns.
- Otherwise it records the tap and the upstream code runs.
- Hooks: one guarded early return at the top of `ChatMessageCell.didClickedImage` and one at the top of
  `ChatMessageCell.didPressButton`.
- The double-tap reaction outside the circle is unchanged.

### Edge cases

- **Not downloaded yet.** Android `buttonState != -1`, or a Desktop round that is not yet streamed: there
  is nothing to seek, so the hook declines and the upstream click proceeds.
- **Media viewer open, or in a call.** There is no in-chat double-click target. Nothing changes.
- **Forwarded messages, or your own message.** The behaviour is the same for any voice or round.

## F3. Private-chat voice queue (default ON)

The rule, in one sentence: in a one-to-one chat, when your manual play starts on the other person's voice or
round message, automatic "play next" plays only their voice or round messages and skips yours.

- A manual play on your own message, in Saved Messages, in a group or in a channel changes nothing.
- "Their message" means **incoming** (`!out`). In a one-to-one chat the only two senders are you and them,
  so a message they forwarded to you counts as theirs, and one you forwarded counts as yours. Telegram's
  sender, not the original author, decides.
- "One-to-one chat" means:
  - a user chat that is not Saved Messages and not the Replies or Verification Codes service chats;
  - on Android, secret chats too, since they are one-to-one.
  - Bots count as one-to-one.

### Desktop: current behaviour

- **One playlist.** Voice and round messages share one voice-type playlist per history
  (`data/data_media_types.cpp:1120-1134`, `Instance::_voiceData`, `media_player_instance.cpp:161-163`).
- **The window.** The playlist is a `SparseIdsMergedSlice` of ±32 ids around the current track
  (`Instance::validatePlaylist`, `:423-449`).
- **Trigger.** When a track ends, `Instance::emitUpdate` calls `moveInPlaylist(data, 1, true)`, unless the
  experimental `disable-autoplay-next` option is on (`:1336-1344`).
- **The chooser.** `Instance::moveInPlaylist` (`:554-660`) takes the adjacent index and plays it through its
  `jumpByItem` lambda. That lambda fires `_switchToNext`, and the history scrolls to the new item
  (`history/history_widget.cpp:913-919`).
- **Filters.** There is no filter on direction or read state. A manual play is not recorded anywhere. The
  only signal is `autonext`, which is true only from `emitUpdate`.

### Android: current behaviour

- **Building the queue.** `ChatActivity.createVoiceMessagesPlaylist(start, unreadOnly)` (`ChatActivity.java:35004-35021`)
  starts with the tapped message and then adds the later voice and round messages. It skips your own
  messages only in unread mode (raise-to-ear).
- **Where it is set.** `needPlayMessage` (`:39435-39438`), `maybePlayVisibleVideo` (`:30544-30546`) and
  `playFirstUnreadVoiceMessage` (`:12937`) pass it to `MediaController.setVoiceMessagesPlaylist`
  (`MediaController.java:3131-3141`).
- **Live append.** New messages are appended live in the `didReceiveNewMessages` branch of
  `MediaController.didReceivedNotification` (`:1940-1955`), with the same filter.
- **Advancing.** On completion, `MediaController.cleanupPlayer` removes the finished entry and plays the
  next one (`:2548-2570`).
- **Manual plays.** Every queue build is a user action. Automatic advance never rebuilds the queue.

### purple-core

`purple/purple_playback.{h,cpp}`. It is pure and reads only `Settings::playback`:

```cpp
enum class PlaybackChat { OneToOne, Self, Other };
struct VoiceQueueAnchor { bool confined = false; };
[[nodiscard]] VoiceQueueAnchor AnchorForManualPlay(
	const Playback &settings, PlaybackChat chat, bool startOutgoing);
[[nodiscard]] bool QueueKeeps(VoiceQueueAnchor anchor, bool candidateOutgoing);
```

- The setting is read when the anchor is made. So toggling it takes effect from the next manual play on both
  clients, never in the middle of a queue.
- Android reaches the rule through one JNI method, `PurpleCore.voiceQueueKeeps(int chat, boolean startOut,
  boolean candidateOut)`, in `purple_jni/jni_bridge.cpp`. The native side reads `gate.settings.playback`
  the way the existing bridge reads `suggestions`/`lastSeen` (`jni_bridge.cpp:1476-1534`).
  - It is called once per candidate while the queue is built: tens of calls per tap, which is negligible.
  - This keeps the rule in one place instead of restating `!out` in Java.

### Desktop: design

- **Anchor tracking needs no new upstream line.**
  - `purple_voice_queue.cpp` subscribes inside the existing `Purple::OnMediaPlayerStarted()` hook
    (`core/application.cpp:314`, defined at `purple/purple_app_services.cpp:40-42`).
  - It subscribes to `instance()->switchToNextEvents()` (`media_player_instance.h:160`) to remember the item
    an automatic advance is about to play.
  - It subscribes to `trackChanged()` (`:166`) filtered to `Voice`. A voice-type track change whose item is
    not the remembered automatic one is a manual play: a chat click, the player bar's next or previous
    button, or the control socket. A manual play recomputes the anchor (history, confined or not).
  - An automatic advance keeps the anchor.
- **The queue hook.** `Purple::ConfinedVoiceNext(data->type, autonext, data->history, useIndex, step, itemAt)`
  returns `std::optional<HistoryItem*>`:
  - `nullopt` means not confining, so upstream proceeds unchanged;
  - an item means play it;
  - `nullptr` means no eligible item is left.
  - It walks forward from `useIndex` while `itemAt(i)` is outgoing. It stops at the edge of the loaded
    slice: more than about 31 of your own voice messages in a row ends the queue, which is acceptable and
    documented.
- **Upstream hook.** One guarded early return in `Instance::moveInPlaylist`, right after `useIndex` is
  computed (`media_player_instance.cpp:637-646`):

  ```cpp
  if (const auto next = Purple::ConfinedVoiceNext(
  		data->type, autonext, data->history, useIndex,
  		[&](int i) { return itemByIndex(data, i); })) {
  	return *next && jumpByItem(*next);
  }
  ```

  - `jumpByItem` fires `_switchToNext` for the chosen item, so the history scrolls to it.
  - Song playback (type `Song`) and manual next or previous (`autonext == false`) never reach the walk.

### Android: design

- **Queue build.** One guarded `continue` inside the loop of `ChatActivity.createVoiceMessagesPlaylist`:
  `if (purple.skipsInVoicePlaylist(startMessageObject, messageObject)) continue;`
  - It covers all three callers.
  - It records the anchor (account, dialog id, confined) in `PurpleVoiceQueue`.
  - Unread mode already keeps only incoming messages, so it is a no-op there.
- **Live append.** One guarded `continue` in the append loop of `MediaController.didReceivedNotification`'s
  `didReceiveNewMessages` branch, before the filter `if` (`MediaController.java:1949`):
  `if (org.telegram.messenger.purple.PurpleVoiceQueue.skipsAppended(messageObject)) continue;`
  It drops your own new voice messages (for example, a reply you record while listening) only while the
  anchor for that dialog is confined.

### Edge cases

- **Your own message is next.** It is skipped. The next one of theirs plays. If none is left, the queue
  ends the way it does today (Desktop `_tracksFinished` → `stopAndClose`, `mainwidget.cpp:401-416`;
  Android `cleanupPlayer` with no next).
- **First manual play on your own message.** Not confined: today's behaviour.
- **Forwarded messages.** Judged by direction, as described above.
- **Chat switch.** Both queues are bound to the playing chat, not the open chat. The anchor goes with them.
- **Message deleted while playing.** Desktop stops and clears (`instance.cpp:326-331`). Android nulls the
  queue (`MediaController.java:1845-1871`, `:2559-2561`). The next manual play re-anchors, so a stale
  anchor can never confine another chat.
- **Calls and other audio.** Unchanged. Pausing does not touch the anchor.
- **Global music player.** Untouched. On Android, a saved music state restores after the queue ends, as
  today (`:3353-3354`).
- **Work Mode.** Not involved. The queue already plays only within the one chat you started in.
- **Desktop `disable-autoplay-next` option.** Unchanged. There is no advance to confine.

## F4. The floating round keeps its in-chat size (Desktop, default ON)

### Current behaviour

- `Float::Float` sizes the float as `2 * st::mediaPlayerFloatMargin + st::mediaPlayerFloatSize` and then
  calls `resize` (`media/player/media_player_float.cpp:105-107`).
- The constants are 12px and 128px (`media/player/media_player.style:319-320`).
- The in-chat round is up to 240px (F5), so the float is about half its size. The frame is filled at the
  inner size (`Float::paintEvent`, `float.cpp:236-246`), and the shadow is built from the widget size after
  `resize` (`prepareShadow`).

### Design

- **Size rule.** `Purple::FloatPlayerSize(item, size)`:
  - With the setting ON, it returns `2 * margin` plus the in-chat side from F5.
  - It clamps that to half the shorter side of the delegate's window, so the float never covers most of a
    small window.
  - With the setting OFF, it returns `size` unchanged, which restores today's shrink.
- **Hook.** One inserted line after `auto size = ...;` in `Float::Float`: `size = Purple::FloatPlayerSize(_item, size);`.
  Shadow, frame size and progress arc all follow `size` already.
- **Screen edges and drag.** `FloatController` places the float by saved corner and column, and
  `Float::outRatio` closes it when less than half is visible. Both work on the widget's actual size, so a
  bigger float docks and closes with the same rules. Closing by drag needs a longer drag.
- **Known limit.** The size is fixed when the float is created. Shrinking the window afterwards does not
  shrink the float. Re-creating on resize would need another hook in `FloatController`, which this design
  does not add. The next round played gets the new clamp.
- **Video quality.** The same as in-chat at the same size (see F5).

## F5. Video message size (Desktop only, default 200%)

### Current behaviour

- **Desktop.**
  - `Gif::countThumbSize` caps a round at `st::maxVideoMessageSize`, 240px (`history/view/media/history_view_gif.cpp:308-327`,
    `ui/chat/chat.style:167`).
  - It only ever downscales from the video's own size (`DownscaledSize`).
  - The message bubble is limited to `max(st::msgMaxWidth = 430px, media->bubbleWidthLimit())`
    (`history/view/history_view_message.cpp:6432-6439`).
  - `Gif::bubbleWidthLimit` already exists for rounds, but returns 0 except for an ephemeral plate
    (`gif.cpp:2325-2335`).
  - Desktop records rounds at 400×400 (`ui/controls/round_video_recorder.cpp:29`).
- **Android.** A round is 60% of the screen's short side, and the playing round is the short side minus
  28dp (`messenger/AndroidUtilities.java:2796-2801`). Android rounds are already large. **Decision: no F5 on
  Android.** Its default would stay as today in any case.

### Design

- **The size.** `Purple::VideoMessageSide()` is `scale% × 240px`, in style-scaled pixels. Choices are 100,
  150, 200 and 250%, default 200 (480px).
- **Hook 1.** One early return at the top of `Gif::countThumbSize`:
  `if (const auto s = Purple::VideoMessageThumbSize(_data, _parent, inOutWidthMax); !s.isEmpty()) { return s; }`.
  - Purple declines (returns an empty size) for anything that is not a round, for rounds hosted in an
    instant view, and at 100%.
  - Otherwise it returns a square of `min(side, inOutWidthMax)` and lowers `inOutWidthMax` to it. This is
    the clamp: in a chat column narrower than the size, the round takes the available width, as photos do.
  - It upscales on purpose, where upstream never upscales.
- **Hook 2.** One early return at the top of `Gif::bubbleWidthLimit`, returning the Purple width when the
  round is larger than `msgMaxWidth`. Without it, 200% would be clipped to 430px.
  - The upstream ephemeral-plate case is computed after the hook. Purple returns its value only when it
    exceeds 430px, and then it also covers the plate's side room (`maxWidth()` includes the reply and forward
    additions).
  - The implementer checks this case in the GUI (open question 3 covers the alternative of capping at 430).
- **Layout and grouping.** Rounds are never grouped into albums. A reply, forward or via header sits beside
  the round (`gif.cpp:430-445`). That width comes from `maxWidth()`, so it follows. The info badge and
  transcription button are positioned from the media rect, so they follow too.
- **Changing the setting.**
  - On change, Purple asks each loaded session to re-measure its round items through `Data::Session::requestItemResize`
    (`data/data_session.h:413`).
  - If no public way exists to list the loaded round items, the change applies as each chat is next laid out.
    The implementer prefers whatever path the Chat settings font-size change already uses.
  - The list keeps its scroll anchor through the existing resize path. That is upstream behaviour, and
    nothing new is needed.
- **HiDPI and blur.**
  - At 200% on a 2× screen, a 400px source is drawn across 960 device pixels, a 2.4× upscale, so it looks
    softer than today.
  - Today's 240px on a 2× screen is already 480 device pixels, a 1.2× upscale.
  - On a 1× screen, 480px is a 1.2× upscale.
  - Rounds recorded on phones are around 384px, slightly softer still.
  - The settings row's about text says that larger sizes are softer.
- **The floating player.** With F4 ON, the float uses the same side.
- **The media viewer.** Rounds do not open in the media viewer (a click plays inline), so it is unaffected.
- **Inline frame cost.** The streamed round is rendered at the drawn size. 480px frames are cheap.

## Upstream-merge plan

Line counts are added lines unless stated. "Fn churn" is the number of upstream commits in the year before
the base that touched the hooked function.

### Desktop (5 files, about +23)

1. `media/player/media_player_instance.cpp`, `Media::Player::Instance::moveInPlaylist`, after `useIndex`
   (`:637-646`).
   - F3: one include and one guarded early return, +7.
   - File churn 7 per year, fn churn 1.
2. `history/history_inner_widget.cpp`, `HistoryInner::mouseDoubleClickEvent`, first statement
   (`:2712-2714`).
   - F2: one early return, +3. Add one include only if no facade covers it yet.
   - **Hot file**: 146 commits per year. Fn churn 2. Hooking the first lines of the function keeps the hook
     clear of the quick-action block, which is where upstream edits.
3. `history/view/history_view_list_widget.cpp`, `HistoryView::ListWidget::mouseDoubleClickEvent`, first
   statement (`:3363-3365`).
   - F2: same as above, +3.
   - **Hot file**: 91 commits per year. Fn churn 2.
4. `history/view/media/history_view_gif.cpp`, `HistoryView::Gif::countThumbSize` (`:308`) and
   `Gif::bubbleWidthLimit` (`:2325`).
   - F5: one include and two early returns, +7.
   - File churn 16 per year, fn churn 1 for `countThumbSize`.
5. `media/player/media_player_float.cpp`, `Media::Player::Float::Float` (`:95-133`).
   - F4: one include and one line, +2.
   - Optional F1 hover controls: +1 in the same constructor.
   - File churn 1 per year, fn churn 1.

`docs/purple/upstream_hooks.md` gets one new section, `## Media playback (round PiP, restart, voice queue,
round size)`, with a bold entry per file above, in the style of `:1159-1166`. Each entry names the function,
the Purple function behind it, and the feature doc `docs/purple/media_playback.md`.

Measure with `purple/upstream_report.sh --worktree --near`. The one `core/application.cpp` hook the feature
uses already exists (`Purple::OnMediaPlayerStarted`) and is unchanged.

### Android (4 files, about +6 −1)

1. `ui/ChatActivity.java`:
   - `createVoiceMessagesPlaylist` loop, before the filter `if` (`:35016`), F3: +1, through `purple`.
   - `updateTextureViewPosition` condition (`:15807`), F1: +1 −1.
   - **Hot file**: 34 of 52 upstream commits in the year. Fn churn 0 for both.
2. `messenger/MediaController.java`, `didReceivedNotification`, the `didReceiveNewMessages` append loop
   (before the filter `if` at `:1949`).
   - F3: +1, fully qualified.
   - File churn 7, region churn 1.
   - First Purple edit in this file. AGENTS.md names `MediaController` as a file to keep small, and this is a
     single `continue`.
3. `ui/Cells/ChatMessageCell.java`, tops of `didClickedImage` (`:6012`) and `didPressButton` (`:17858`).
   - F2: +2, fully qualified.
   - **Hot file**: 28 commits. Fn churn 1 and 0.
4. `ui/Components/PipRoundVideoView.java`, the `ACTION_UP` branch of the inner view's `onTouchEvent`
   (`:157-158`, inside `if (startDragging && !dragging)`).
   - F1: +1, fully qualified.
   - File churn 0.

`docs/upstream-hooks.baseline` changes, regenerated by `purple/upstream-hooks.sh baseline` in the commit that
adds each hook:

- `file .../ui/ChatActivity.java 65 23 40` → about `67 24 42`;
- `file .../ui/Cells/ChatMessageCell.java 19 1 5` → about `21 1 7`;
- new `file .../messenger/MediaController.java 1 0 1`;
- new `file .../ui/Components/PipRoundVideoView.java 1 0 1`.

Run `purple/upstream-hooks.sh check` before each commit. The rationale for the one changed line (the
`updateTextureViewPosition` condition) goes in a new README section, "Media playback", next to "Pinned music".
Javadoc on each Purple method says why its hook sits where it does.

### Places where a pure insertion was impossible

- **Android `updateTextureViewPosition`.** The PiP-or-scroll choice is a single if/else-if. Making "go to the
  PiP" win without restructuring means widening the existing condition by one term. That is +1 −1, with no
  moved code.
- **Desktop F2 hooks.** These are early returns at function tops, so there is no conflict-prone context. The
  hot-file rating comes from edits elsewhere in those files.

### Re-anchoring after the merge

Re-find each site by its function name, then re-run:

```sh
git log --since=<base-1y> --format='C %h' -L ':<Class>\:\:<function>:<file>' <base> | grep -c '^C '
```

On Android, use `/opt/homebrew/bin/git` with `GIT_NO_LAZY_FETCH=1`, and use `-L '/<java signature
fragment>/,+N:<file>'`, because Java has no funcname driver here. Any site whose function count jumps after
the merge should be re-read before hooking.

## Strings

Desktop `Telegram/Resources/langs/purple.strings`:

```
"lng_purple_media_playback" = "Media playback";
"lng_purple_voice_queue_private" = "Keep the voice queue to the other person";
"lng_purple_voice_queue_private_about" = "In a private chat, when you play the other person's voice or video message, only their next messages play automatically.";
"lng_purple_video_message_size" = "Video message size";
"lng_purple_video_message_size_percent" = "{percent}%";
"lng_purple_video_message_size_about" = "Larger sizes look softer, because video messages are recorded at about 400 pixels.";
"lng_purple_video_message_float_keep" = "Keep the size when it floats";
"lng_purple_round_float_close" = "Close";
"lng_purple_round_float_open_hint" = "Double-click to open the message";
```

The last two strings are only needed for the optional hover controls.

Android `res/values/purple_strings.xml`: `PurpleMediaPlayback`, `PurpleVoiceQueuePrivate` and
`PurpleVoiceQueuePrivateInfo`, with the same text.

## Tests

- **purple-core `tests/test_config.cpp`.** `[playback]` parsing:
  - missing table → true;
  - true and false;
  - wrong type → warning naming `private_chat_queue_p`;
  - non-table `playback` → warning;
  - the scope registry reports AppWide;
  - the config-diff label is "Playback".
- **purple-core `tests/test_playback.cpp`, new.** The full matrix of `AnchorForManualPlay` by `QueueKeeps`:
  setting on or off × OneToOne, Self or Other × start incoming or outgoing × candidate incoming or outgoing.
  Register it in `tests/run.sh`.
- **Desktop `purple/tests/voice_queue_cases.cpp`.** Through `purple/test_cases.py`, registered in
  `purple/host_gates.json`. `ConfinedVoiceNext` over a fake index-to-item table:
  - skips own messages;
  - stops at the slice edge;
  - declines for `Song`, for `autonext == false` and for an unconfined anchor;
  - the manual and automatic classification from a scripted event sequence (switchToNext then trackChanged,
    trackChanged alone, next or previous).
- **Desktop `purple/tests/media_size_cases.cpp`.**
  - side from percent;
  - clamp to width;
  - declines for non-rounds and at 100%;
  - the F4 float size with the setting ON and OFF and with the window clamp;
  - stored preference parsing (missing, out of range → default).
- **Desktop hook pins.** A case in the same driver reads the five upstream files and asserts that each named
  function contains its one `Purple::` call. This guards against a merge that silently drops a hook.
- **Android host suite `voice-queue`.** `TMessagesProj/jni/purple_jni/tests/voice_queue/`, plus its entry
  in `purple/host_suites.json`.
  - `PurpleVoiceQueue` anchor and append logic with a stub for the JNI rule.
  - A `host-library` case calling the real `voiceQueueKeeps`.
  - `PurpleMediaRestart` and `PurpleRoundPip` tap timing with an injected clock.
- **Android hook pins.** `purple/upstream-hooks.sh check` and the ratchet.
- **GUI checks after build** (out of this design's scope):
  - Desktop: double-click restart, the queue skipping your own message, 200% layout in a narrow window, the
    F4 float size.
  - Android: PiP double-tap, and a reply-quote jump while a round plays.

## Atomic commit groups

purple-core repository, first:

1. "Read the playback table with the private-chat voice queue key", with its tests.
2. "Decide which voice messages a confined queue keeps", with its tests.

Desktop, after the upstream merge:

1. Bump purple-core.
2. F3: `purple_voice_queue.*`, the `moveInPlaylist` hook, the settings row, strings, `docs/purple/media_playback.md`,
   the hooks entry and the cases.
3. F2: the two double-click hooks, `purple_media_playback.*` restart, cases, docs and hooks entries.
4. F5: the two `Gif` hooks, the size preference and chooser row, strings, cases, docs and hooks entry.
5. F4: the `Float::Float` hook, the toggle row, cases, docs and hooks entry.
6. Optional F1: hover controls (same constructor), strings and docs.

Android, after the upstream merge:

1. Bump purple-core, and add the `voiceQueueKeeps` JNI bridge with its host-library case.
2. F3: the `ChatActivity` and `MediaController` hooks, `PurpleVoiceQueue`, the settings row, strings, the
   suite, the README section and the ratchet.
3. F2: the two `ChatMessageCell` hooks, `PurpleMediaRestart`, the suite and the ratchet.
4. F1: the `PipRoundVideoView` hook, the `updateTextureViewPosition` condition, `PurpleRoundPip`, the suite,
   the README and the ratchet.

## Effort

Agent-days including host tests, excluding build and GUI time.

- **F1:** Desktop 0 (optional hover controls 0.5). Android 1.
- **F2:** Desktop 0.5. Android 0.5.
- **F3:** purple-core 0.5. Desktop 1. Android 1.
- **F4:** Desktop 0.5.
- **F5:** Desktop 1, most of it the layout clamp and the relayout path. Android none.
- **Total:** about 6.5 agent-days, plus one GUI verification pass per client.

## Open questions (3)

1. **The Android F1 gap.** Is the intended gap the pull-back to the playing round (tapping a reply or search
   result while a round plays jumps back to it), plus the missing way back from the PiP?
   - Recommended: yes. Ship both small hooks, then confirm the pull-back on a device before the commit
     lands.
   - If the owner meant something else, for example a system picture-in-picture outside the app, that is a
     much larger change (an Android PiP activity) and needs its own design.
2. **Desktop F1 hover controls.** Should the float get pause and close glyphs and a hint on hover?
   - Recommended: yes, as the last Desktop commit. It costs one line in a file F4 already touches.
   - Skipping it leaves today's drag-to-close and invisible double-click.
3. **F5 above 430px.** Reaching a true 2× (480px) needs the `Gif::bubbleWidthLimit` hook and accepts a
   visibly softer round on Retina screens (2.4× upscale of a 400px source).
   - Option A (recommended): as designed, 200% = 480px with the softness note.
   - Option B: cap at the bubble limit of 430px (about 1.8×). This drops one hook and looks slightly
     sharper, but it does not honour "twice as big".
