<!-- Owner answers to the six open questions (3 Nov): all Option A. 1 discarded recordings go to the bin as "Discarded" items (keep_discarded_p, default on); 2 journal Off still writes the codec config header once; 3 F11 logs relative pc for the app's own libraries plus build id once per crash; 4 Desktop recordings encrypted at rest (AES-CTR from the local key); 5 Android screens without the strip rely on the notification; 6 ParseDuration gains a d suffix. Earlier answers: the dock is a bar on both clients plus a camera bubble for rounds; the journal interval is configurable (Off, 1, 2, 5 s; default 1 s) and repair after the fact also exists. -->

# Media recording design (F6-F11)

Design pass, 3 Nov 2026. Design only: nothing was built, run or sent. It continues `docs/media-design.md` (F1-F5),
which is being implemented on the `media` branch, and uses the same terms and rules.

Evidence comes from reading code at:

- Android: this repository at `0bd0336b` (upstream base `3de37c8c`, "update to 13.1.1"), plus the `media` branch
  at `3be897da` for the F1-F5 classes it adds, and purple-core at `6d0bd81b` (its `media` branch, the gitlink the
  `media` branch pins).
- Desktop: the Desktop repository, branch `dev` at `38857d1`.

Android paths are relative to `TMessagesProj/src/main/java/org/telegram/` unless they start with `TMessagesProj/`.
Desktop paths are relative to `Telegram/SourceFiles/`. Line numbers are from those commits; every hook is also named
by class and function, so it can be re-anchored after the next upstream merge.

Owner decisions are binding and are not re-argued here. They include three relayed during this pass: round video
crash safety uses the sidecar journal (option (a)); the journal interval is a setting with Off; and the
after-the-fact repair is designed in full as the fallback whenever the journal is Off, missing or stale.

## Terms

- **Round**: a round video message (video note). **Voice**: a voice message.
- **Recording**: one voice or round capture from its start to Send, Discard or a crash, including any pauses.
- **Target**: the chat (and forum topic, reply and send-as peer) a recording was started in. Send always goes to
  the target unless the user picks "Send to another chat".
- **Dock**: the app-wide owner of a recording that has left its chat screen. On both clients it is shown as a
  **strip**: a bar in the top context strip (Android `FragmentContextView`, where the music player shows; Desktop
  the bar under the top bar, where the music player shows). A round also gets a **bubble**: a small floating camera.
- **Adopt**: the moment the dock takes a recording over from the chat's own recording UI without stopping it.
- **Recording bin**, or **bin**: the Settings screen and store of partial and unsent recordings.
- **Journal**: the sidecar index file written next to a recording round video (F10 option (a)).
- **Config header**: the first record of a journal: track formats and codec configuration (H.264 SPS/PPS, AAC
  AudioSpecificConfig). It is written once at the start even when the journal setting is Off.
- **Repair**: rebuilding a round's index after the fact by scanning its media data (the untrunc approach).
- **Legacy recorder** and **V2 recorder**: Android 13.1.1 ships two of each kind; see "Android has two recorders
  of each kind" below.
- **Hook**: an edit to an upstream file in the shape AGENTS.md allows: one import or include, one guarded call into
  a Purple class, one early return, or one menu row. A hook carries at most the comment
  `// Purple: see PurpleX#method`.
- **Extension point**: the smallest upstream addition that makes a hook possible where no call site exists
  (AGENTS.md: "add the smallest extension point that makes one").

## Summary

Six features and six extras:

- **F6, round picture-in-picture recording** (default on). Tap the dimmed background to shrink the camera into a
  draggable bubble that keeps recording. A spike first decides whether the bubble can leave the chat. The
  owner-approved fallback, a bubble kept within the chat, is designed in full and needs no spike.
- **F7, Android voice keeps recording in the background and with the phone locked** (default on). A microphone
  foreground service with a notification, and one hook that skips upstream's pause on chat pause.
- **F8, a recording survives switching chats** (default on). The dock adopts it; tapping the message box adopts it
  too; the message box's send button never sends it.
- **F9, seek back and forward** by a step of 5, 10, 15 or 30 s (default 10): buttons in the player strip on both
  clients, on the Desktop float's hover controls, Desktop shortcut commands, and headset rewind and fast-forward
  keys on Android.
- **F10, crash-safe recordings and the Recording bin.** Voice is already written incrementally on Android; Desktop
  streams it to a file. Round video gets the journal (exact) with repair as the fallback, and the sent file keeps
  upstream's format. The bin lists every partial or unsent recording.
- **F11, crash capture into Purple's diagnostic log on Android**, from `ApplicationExitInfo` and Java uncaught
  exceptions, scrubbed to class and method frames and codes.
- **Extras**: (1) send to another chat from the dock, (3) calls pause the recording and offer Resume, (4) headset
  button control while recording, (5) a nudge at 30 min and every 30 min after, (6) preview and trim from the dock.
  Desktop maps media play/pause to pause/resume while recording.

Upstream totals, required hooks only (details in "Upstream hooks"):

- Android: **12 files, about +30 −1 lines.** Two of those lines (in two of the files) exist only if the F6 spike
  succeeds and cross-chat bubbles ship.
- Desktop: **11 files, about +30 −1 lines**, plus an optional +2 in a twelfth file to list the new shortcuts on
  the Shortcuts settings page.
- purple-core: no upstream files. New pure code for the settings, the rules, the Ogg finaliser and the MP4 repair
  scanner.

Effort is about **40 agent-days** (Android 21, Desktop 15, purple-core 3.5), plus about 3 more on Android if
cross-chat round bubbles ship after the spike. There are **6 open questions** at the end.

## Current behaviour

### Android has two recorders of each kind

Upstream 13.1.1 picks between an older and a newer recorder for both voice and round. Everything below has to work
with all four.

- **Voice.** `MediaController.shouldUseAudioRecorderV2` (`messenger/MediaController.java:4488-4492`) picks V2 unless
  the local `SharedSettings.audioRecorderV2Enabled` or the server's `android_audio_recorder_v2_disabled`
  (`messenger/AppGlobalConfig.java:63`) says otherwise. So V2 is the default.
  - Legacy: `recordRunnable` (`MediaController.java:1136-1220`) reads `AudioRecord` and calls the native
    `writeFrame` into an Ogg/Opus file named `<time>_<attach name>.ogg` in `MEDIA_DIR_AUDIO`
    (`:5252`).
  - V2: `startRecordingV2` (`:4494-4560`) drives `AudioRecorderV2Controller` (`messenger/AudioRecorderV2Controller.java`),
    which wraps `AudioRecordingSession` (`utils/recorder/audio/AudioRecordingSession.java`). The session records,
    pauses into a playable preview, trims and resumes, and "incrementally exposes a Telegram-compatible Ogg/Opus
    voice file" (class Javadoc, `:25-33`). Its files are temporary, named
    `audio_recording_<role>_<time>_<n>.ogg` in `MEDIA_DIR_AUDIO` (`:1061-1069`), and deleted on release
    (`:1118-1127`).
- **Round.** `InstantCameraViewBase.create` (`ui/Components/InstantCameraViewBase.java:47-60`) picks
  `InstantCameraView2` when `SharedSettings.roundVideoCamera2Enabled` or the server's `roundVideoRecorder2Allowed`
  says so, and the older `InstantCameraView` otherwise. `ChatActivity.checkInstantCameraView`
  (`ui/ChatActivity.java:10792-10800`) creates it and adds it full-screen to the chat's `contentView` (`:10835`).
  - Legacy: `InstantCameraView` renders the camera through its own GL thread (`CameraGLThread`,
    `ui/Components/InstantCameraView.java:1518`, EGL window surface on the `TextureView`'s `SurfaceTexture` at
    `:1652`) and muxes with `MP4Builder` in its `VideoRecorder` (`:2177`, `createMovie` at `:3301-3306`). The file
    is `<time>_<local id>.mp4` in `MEDIA_DIR_DOCUMENT` (`:765`).
  - V2: `InstantCameraView2` (`ui/Components/InstantCameraView2.java`) drives `RoundVideoSession`
    (`utils/recorder/round/RoundVideoSession.java`), which writes `round_video_*.mp4` temporary files under
    `files/cache` (`:1279`, `:1470-1480`, output directory set at `InstantCameraView2.java:383`) through
    `RoundVideoMp4Writer`, also on `MP4Builder` (`utils/recorder/round/RoundVideoMp4Writer.java:153`).
- Rounds stop at 60 s: `ChatActivityEnterView`'s timer sends state 3 at 59.5 s
  (`ui/Components/ChatActivityEnterView.java:14366-14372`). Desktop has the same cap (`kMaxDuration`,
  `ui/controls/round_video_recorder.cpp:35`).

### Who owns a recording today

- **Android.** `MediaController` is the owner of a voice recording, not the chat screen: it keeps the target
  itself (`recordDialogId`, `recordReplyingMsg`, `recordTopicId` and the send arguments, `MediaController.java:1094-1124`,
  set in `startRecordingV2` at `:4509-4520`), and `stopRecording(1, ...)` sends to that target from anywhere. The
  chat's `ChatActivityEnterView` only draws the UI and listens to `recordStarted`, `recordPaused`, `recordResumed`,
  `recordStopped`, `recordProgressChanged` and friends (`ChatActivityEnterView.java:2619-2632`).
  - `recordStarted` and `recordStopped` carry the chat's class guid, but `recordPaused` is posted with no guid at
    all (`MediaController.java:4906`, `:5005`, `:5143`). So every enter view of the account reacts to a pause.
    That is harmless today because only one chat can have a recording; it matters once one can leave its chat.
  - A round belongs to the chat screen: the `InstantCameraView` is a child of that chat's `contentView`, sends
    through its `Delegate` (the `ChatActivity`, `InstantCameraView.java:3918-3933`, `:1025`) and listens for its
    own upload while recording only while attached (`:571-590`).
- **Desktop.** `Media::Capture::Instance` (`media/audio/media_audio_capture.cpp`) is a global capture, but the
  recording's state lives in the chat's `HistoryView::Controls::VoiceRecordBar`: pause segments, the listen
  (preview) state with trim, TTL, and the send request (`requestToSendWithOptions`,
  `history/view/controls/history_view_voice_record_bar.cpp:3101-3118`). A round's `Ui::RoundVideoRecorder` is
  created by the bar with the chat's outer container as parent (`createVideoRecorder`, `:3341-3387`).

### What happens when you leave the chat today

- **Android, any chat pause** (opening another chat on top, going back, the app going to the background, the screen
  locking): `ChatActivity.onPause` (`ui/ChatActivity.java:30293-30303`) calls
  `MediaController.stopRaiseToEarSensors(this, true, true)`, which pauses a running recording with
  `toggleRecordingPause(false)` and otherwise stops it as a draft (`MediaController.java:2439-2450`). A paused
  voice recording becomes the chat's voice draft (`pushDraftVoiceMessage`, `MediaController.java:5130`;
  `MediaDataController.java:10153`), and reopening the chat shows the paused panel again (`prepareResumedRecording`,
  `:4911`).
- **Android, a locked recording and a back press or a tap on the chat**: `ChatActivity.checkRecordLocked`
  (`ui/ChatActivity.java:34926-34950`) shows "Discard voice message?" with Discard and Continue. It is reached from
  the back button (`:3764`), from `onBackPressed` (`:34983`) and from the transparent `overlayView` that covers the
  chat while recording (`:7855-7863`).
- **Desktop**: `MainWidget::preventsCloseSection` (`mainwidget.cpp:2273-2280`) asks the section, and
  `HistoryWidget::preventsClose` (`history/history_widget.cpp:7360-7366`) or `ComposeControls::preventsClose`
  (`history/view/controls/history_view_compose_controls.cpp:6380-6386`) shows the same discard box
  (`VoiceRecordBar::showDiscardBox`, `voice_record_bar.cpp:3307-3339`). Destroying the bar cancels
  (`~VoiceRecordBar`, `:2140-2144`).

### Background and lock

- **Android voice.** The pause on chat pause above also runs when the activity pauses, so locking the phone pauses
  the recording today. The manifest already declares `FOREGROUND_SERVICE_MICROPHONE`, `RECORD_AUDIO` and
  `WAKE_LOCK` (`TMessagesProj/src/main/AndroidManifest.xml:31`, `:32`, `:36`), and the only service with a
  microphone type is `VoIPService` (`:538-543`).
- **Android round.** Nothing in upstream pauses a round on background; the camera simply loses access when the app
  leaves the foreground (Android grants camera access to foreground apps only, unless a camera-type foreground
  service runs).
- **Desktop.** Capture continues when the window is inactive. Nothing to do (owner decision).

### Calls

- **Android.** On a ringing phone call, the `PhoneStateListener` in `MediaController` stops a voice recording as a
  draft (`stopRecording(2, ...)`, `MediaController.java:1478-1490`). A Telegram call does nothing to a recording.
- **Desktop.** Nothing.

### Crash safety today

- **Android legacy voice**: frames go to the file as they are encoded (`max_ogg_delay = 0`, one Ogg page per packet,
  `TMessagesProj/jni/audio.c:229`, written with `fwrite` at `:220-221`). There is no `fflush`, so a crash loses the
  stdio buffer: a few KB, about one second.
- **Android V2 voice**: the encoder flushes to disk every 100 ms (`OUTPUT_FLUSH_INTERVAL_MS`,
  `utils/recorder/audio/AudioCapture.java:48`, `:274-281`; native `fflush` at `TMessagesProj/jni/audio.c:1613-1619`).
  But the file is a temporary file that a clean release deletes, and nothing finds it after a crash.
- **Android round (both recorders)**: `MP4Builder` writes `ftyp`, then an `mdat` whose size is patched only at
  flush, then sample data as it arrives (`messenger/video/MP4Builder.java:75-183`). The sample table lives only in
  memory (`Mp4Movie.addSample`, `messenger/video/Mp4Movie.java:67-73`) and `moov` is written at the end
  (`finishMovie`, `MP4Builder.java:193-216`). A crash leaves a file with media data and no index: unplayable.
- **Desktop voice**: the encoder writes into a `QByteArray` through a custom AVIO context (`Private::WriteData`,
  `media/audio/media_audio_capture.cpp:259-271`). Nothing reaches the disk.
- **Desktop round**: the same, into memory (`WriteBytesWrap`, `ui/controls/round_video_recorder.cpp:314-330`), with
  `moov` at the trailer (`finishEncoding`, `:486-510`).

### Seeking

- **Android**: the expanded player (`ui/Components/AudioPlayerAlert.java:223-273`) has long-press rewinding on
  previous and next. The strip (`FragmentContextView`) has play, title, speed and close, no seek. The media session
  (`messenger/MusicPlayerService.java:115-170`) offers play, pause, seek-to, repeat and shuffle, but not rewind or
  fast-forward (`:478-499`, `:727-740`).
- **Desktop**: the player bar (`media/player/media_player_widget.cpp`) has play, previous, next, volume, repeat,
  order and speed. System media controls skip by `kSkipMs` for video only (`media/system_media_controls_manager.cpp:391-404`).
  There is no seek shortcut command (`core/shortcuts.h:17-30`).

### Crash reporting today (Android)

- A default uncaught exception handler is installed only when Telegram's own logs are on, and it writes to
  `FileLog` (`messenger/BuildVars.java:46-58`). Nothing reads `ApplicationExitInfo`.
- Purple's diagnostic log (`messenger/purple/PurpleDiagLog.java`, `PurpleDiagLogStore.java`) is always on and
  bounded (256 KB plus one rotated file). Lines are at most 512 characters and are built only from typed fields: a
  slot, clamped counts, versions, booleans, words of a fixed vocabulary, and a scrubbed exception message
  (`PurpleDiagLogStore.java:25-42`, `:58`, `:119-160`, `sanitize` at `:207`).

## The recording dock

### The model (both clients)

One app-wide object owns at most one recording at a time:

- target: account (Android slot index, Desktop session), chat, topic, reply, send-as peer, and the view-once flag;
- kind (voice or round), start time, recorded duration, paused state, and why it paused (user, call, background);
- the waveform so far (the last 64 amplitude buckets for the strip, the full waveform comes from the recorder);
- where it is shown: in its chat (the chat's own UI owns the controls) or docked.

Rules:

- A recording is docked when its chat screen is no longer the visible screen, or when the user moves it (tap on
  the message box, F8). It returns to its chat when the user opens the target chat and taps the strip, or uses
  Preview (below).
- Send always targets the target, except "Send to another chat" (extra 1).
- Only one recording exists at a time, as today. Starting a new recording while one is docked shows the existing
  upstream discard prompt for the docked one first.

### The strip

Content, left to right: target avatar and name, a red dot and the timer, a live waveform, then Pause/Resume,
Preview, Send and Discard. Tapping the avatar, name or waveform opens the target chat at the bottom. Discard asks
for confirmation (the upstream discard strings). On narrow screens Preview moves into a long-press menu on the
timer.

Work Mode: if the running preset hides the target chat, the strip shows "Recording" with a neutral icon instead of
the avatar and name, and tapping it opens the chat through the stock open route, so the preset's chat-open policy
applies as everywhere else. The strip never reveals a hidden chat's name.

### Android design

Code: `messenger/purple/PurpleRecordingDock.java` (the model, the NotificationCenter observers and the actions),
`ui/Components/PurpleRecordingStrip.java` (the per-`FragmentContextView` helper, in that package because it needs
package access to the strip's views), `ui/PurpleRecordingStripView.java` (the bar).

- **State source.** The dock observes the recording account's `recordStarted`, `recordProgressChanged`,
  `recordPaused`, `recordResumed`, `recordStopped`, `audioDidSent` and `audioRecordTooShort`. The target comes from
  the starting chat: `PurpleChatHooks` (`ui/PurpleChatHooks.java`, the chat's per-instance helper) sees
  `recordStarted` with its own class guid and registers the chat, topic, reply and kind. No new hook is needed for
  that; `PurpleChatHooks` already receives the chat's lifecycle.
- **Actions** map to `MediaController`'s public API, which already targets the recording's chat:
  `toggleRecordingPause(voiceOnce)` for Pause/Resume, `stopRecording(1, notify, 0, voiceOnce, payStars)` for Send,
  `stopRecording(0, ...)` for Discard. A docked round sends and cancels through its owning chat's helper, which
  calls the chat's own `needStartRecordVideo(1 or 5, ...)` path (`ChatActivity.java:2460-2467`).
- **Paid messages and slow mode.** Before Send, the dock calls `AlertsCreator.ensurePaidMessageConfirmation` for
  the target, as the enter view does (`ChatActivityEnterView.java:3010`), and refuses with the upstream slow-mode
  alert when the target is in slow mode.
- **Showing the strip.** `FragmentContextView` decides its own visibility and animates the fragment's padding
  through a private delegate (`ui/Components/FragmentContextView.java:213`, `checkPlayer` at `:1821-1960`). Rather
  than copy that machinery, the strip rides on it:
  - The hide condition in `checkPlayer` (`if (messageObject == null || messageObject.getId() == 0 || messageObject.isVideo())`)
    gains `&& !purple.docked()`, so a docked recording takes the existing show path, which sets the player style,
    the height and the padding animation.
  - One early return after `visible = true; setVisibility(VISIBLE);` stops the method before it reads the (absent)
    playing message.
  - The helper adds `PurpleRecordingStripView` as a child of the `FragmentContextView`, above the player row, and
    shows it while docked. The music row underneath is covered; the recording pauses music anyway
    (`SharedConfig.pauseMusicOnRecord`).
  - When the dock changes, the helper calls the `checkPlayer(false)` it was handed as a lambda in its field
    initializer, so hiding uses upstream's own hide animation and falls back to the music player if music is
    loaded.
- **Screens without a strip.** `FragmentContextView` exists in chats, the chat list, topics, calls, shared media and
  the audio picker. Settings and profiles have none; there the notification (F7) is the dock's only surface. See
  open question 5.

### Desktop design

Code: `purple/purple_recording_dock.{h,cpp}` (model and actions), `purple/purple_recording_strip.{h,cpp}` (the bar
widget), `purple/purple_recording_adopt.{h,cpp}` (taking a recording from a `VoiceRecordBar`), declared for hooks in
a facade header `purple/hooks/recording.h` (convention D5 of the Desktop hooks doc).

- **Adoption.** `Media::Capture::Instance` keeps the whole stream across pause and resume (`Inner::pause` returns a
  snapshot and leaves the data in place, `media_audio_capture.cpp:464-480`), so the dock can take over by
  subscribing to `instance()->updated()` itself and remembering the target. The bar is then hidden with its public
  `hideFast()` (`voice_record_bar.cpp:2852-2858`), whose `finish()` only drops the bar's own subscriptions and state
  (`:2829-2850`) and does not stop the capture.
  - The hook passes the send context the section already holds: `HistoryWidget` passes `prepareSendAction({})`
    (private to it, so the hook is the only place that can); `ComposeControls` passes its history and its
    `FullReplyTo`.
  - Refused (the upstream discard box shows instead): a listen state with a trimmed resume prefix
    (`_resumePrefixData`, `:2860-2880`), whose merge logic is private to the bar; and a round (cross-chat rounds
    follow the Android spike, see F6).
- **Send** stops the capture with `instance()->stop(callback)` and calls `ApiWrap::sendVoiceMessage(bytes,
  waveform, duration, false, action)` (`apiwrap.cpp:4263`) with the adopted action.
- **The strip.** A Purple widget placed under the call, export and player bars. `MainWidget` stacks those three by
  hand (`getMainSectionTop`, `mainwidget.cpp:2546-2548`; placement in both layout branches of
  `updateControlsGeometry`, `:2715-2726` and `:2777-2789`). The strip needs four hooks there: an attach call in the
  constructor that hands Purple a relayout callback, the strip's height added to `getMainSectionTop`, and one
  placement call in each layout branch.

## F8. The recording survives switching chats (default on)

### Android

- **Leaving the chat.** One hook in `MediaController.stopRaiseToEarSensors` (`:2439`): when the dock keeps the
  recording, `stopRecording` is set to false for this call, so a chat pause no longer pauses it. The sensor teardown
  below it is unchanged. The decision (`PurpleRecordingDock.keepsOnChatPause(account)`) separates a chat being
  covered (F8 setting) from the app going to the background (F7 setting); see F7 for how it tells them apart.
- **Back press and taps on the chat while locked.** One early return at the top of `ChatActivity.checkRecordLocked`
  through `purple`: when F8 is on, it docks the recording and returns false, so the back button finishes the fragment
  and an `overlayView` tap passes through, instead of the discard dialog. This covers all three callers.
- **Tapping the message box.** While recording is locked, the record panel covers the field and swallows every
  touch (`recordPanel.setOnTouchListener((v, event) -> true)`, `ChatActivityEnterView.java:9765`). One hook line
  after it lets `PurpleRecordingDock` install a listener that keeps swallowing touches but docks the recording on a
  tap in the timer and "slide to cancel" area.
- **Releasing the chat's record UI.** Docking must reset the enter view to idle without stopping the recorder.
  `updateRecordInterface` is protected (`:8832`) but `recordingAudioVideo` is private (`:730`), so this needs an
  extension point: a 4-line package method in `ChatActivityEnterView`,
  `boolean purpleReleaseRecordUi() { recordingAudioVideo = false; updateRecordInterface(RECORD_STATE_CANCEL, true); return voiceOnce; }`.
  It is the tail of upstream's `cancelRecordingAudioVideo` (`:6199-6211`) without the `stopRecording` call, and it
  returns the view-once flag so the dock keeps it.
- **Other chats ignoring the docked recording.** One guarded early return at the top of
  `ChatActivityEnterView.didReceivedNotification`: while a recording is docked, record notifications reach only the
  enter view of the target chat. Without it, `recordPaused` (no guid) would put the docked recording's paused panel
  into whatever chat is open.
- **Typing while docked.** Text, stickers and media sent from the message box go to the open chat, as always. The
  send button cannot send the recording: the enter view no longer holds it (`audioToSend` is null and
  `recordingAudioVideo` false). If the user opens the target chat while the docked recording is paused, upstream
  restores it from the voice draft into the chat's paused panel; from then on it is in its chat again, not docked,
  and that panel's own send button sends it, as upstream does.

### Desktop

- One early return at the top of `HistoryWidget::preventsClose` and one at the top of `ComposeControls::preventsClose`:
  when F8 is on and the bar is recording voice, adopt and return false, so the section closes without the discard
  box. `ChatWidget::preventsClose` (`history/view/history_view_chat_section.cpp:3830-3831`) goes through
  `ComposeControls`, so topics and replies are covered.
- **Tapping the message box.** One call at the end of `VoiceRecordBar::init` (`:2222`):
  `Purple::DecorateVoiceRecordBar(this)` installs an event filter on the bar. A click in the bar's message area
  (`_messageRect`, `:2157`), or a printable key while the recording is locked, adopts the recording and gives the
  field back. The bar's recording key filter (`:2776-2780`) is removed by `hideFast()`, so Enter in the field then
  sends text, never the recording.
- Check during implementation: that `hideFast()` makes the section show the field again (`ComposeControls` has
  `updateFieldVisibility`, `history_view_compose_controls.h:507`). If the field stays hidden, the adoption hook is
  the place to ask the section to refresh it, since it already runs inside the section; no extra hook is planned.

### Edge cases

- **The target chat is deleted or left while docked.** The dock watches the account's dialog removals and switches
  Send to "Send to another chat" with a notice; Discard and Preview still work.
- **Account switch (Android) or session switch (Desktop).** The dock keeps its own account; the strip shows the
  target's name either way. Send uses the recording's account, never the current one (Work Mode hook rule).
- **Secret chats (Android).** Docking works; Send goes through `MediaController`, which already handles the secret
  chat target. "Send to another chat" is not offered for a secret-chat recording.
- **Scheduled-messages screen.** A recording started there keeps upstream's schedule step: the dock's Send opens
  the same schedule picker (`AlertsCreator.createScheduleDatePickerDialog`, as `ChatActivityEnterView.java:3019`).
- **Raise to speak (Android).** A recording started by raising the phone stays under upstream's proximity logic; it
  is docked only if the user leaves the chat, and then lowering the phone no longer stops it (the sensors are torn
  down with the chat).

## F7. Android voice keeps recording in the background and when locked (default on)

### Design

- **The skip.** The same `stopRaiseToEarSensors` hook as F8. `PurpleRecordingDock` knows whether the app is going to
  the background from `PurplePeekForeground`'s activity callbacks (already registered at
  `messenger/ApplicationLoader.java:348`). If those callbacks turn out to run after `ChatActivity.onPause`, the
  implementer reads `ApplicationLoader.mainInterfacePaused`, which `LaunchActivity.onPause` sets, instead of adding
  a hook.
- **The service.** `messenger/purple/PurpleRecordingService.java`, a foreground service of type `microphone`.
  - It starts when a recording starts, while the app is in the foreground. Android 14 allows a microphone
    foreground service only to start from the foreground, and Android 9 and later give background apps silence
    instead of microphone audio unless such a service runs.
  - It stops when the recording ends (send, discard, crash recovery).
  - It holds a partial wake lock while the recording is not paused, so the screen-off CPU sleep does not starve
    `AudioRecord`.
  - Manifest: one `<service>` line (`android:foregroundServiceType="microphone"`, not exported), next to the
    existing Purple entries.
- **The notification.** A Purple channel "Recording" (low importance, no sound).
  - Title: the target's name, or "Recording" under a preset that hides it. Text: "Voice message" with a chronometer
    while recording (`setUsesChronometer`, base set from the recording's start minus paused time), or
    "Paused · 3:12" while paused.
  - Actions: Pause or Resume, Send, Discard. Discard asks for a second tap: the action turns into "Tap again to
    discard" for 5 s. Send runs the same checks as the strip; when a check needs UI (paid message, slow mode,
    schedule), the action opens the app on the dock instead.
  - Tapping the notification opens the target chat.
- **Round videos** are not covered by F7: going to the background pauses a round (F6), because camera access ends.

### Edge cases

- **Notification permission denied (Android 13+).** The service still runs; Android shows it in the task manager
  only. The dock shows a one-time hint in the strip: "Allow notifications to control recordings from the lock
  screen".
- **The system kills the service or the process.** The recording is lost from memory, and its file is recovered by
  F10 on the next start.
- **Bluetooth headset microphone.** Upstream routes the microphone through SCO (`setBluetoothScoOn(true)` in
  `startRecordingV2`, `MediaController.java:4531`). That continues in the background; disconnecting the headset
  ends SCO and the recorder continues on the phone microphone, as in the foreground today.
- **Battery saver.** Foreground services keep running under battery saver; no special handling.

## F6. Round video picture-in-picture recording (default on)

### The interaction (both clients)

- While a round is recording, tapping the dimmed area around the camera shrinks the camera into a bubble (about
  120dp, Desktop 160px) in the bottom-right corner, above the compose area. The dim goes away and the chat is
  usable: scroll, read, open media, type.
- The bubble shows the live camera, a progress ring with the timer, and on tap-and-hold or hover two small buttons:
  flip camera and pause/resume. Dragging moves it and it snaps to the nearest corner; the corner is remembered.
- Tapping the bubble expands it back to the full recorder.
- The strip shows the round as a docked recording (kind: video) whenever the bubble exists, with the same
  Pause/Resume, Preview, Send and Discard.
- **Background or lock pauses the round** (camera access ends): `PurpleChatHooks.onPause` (an existing hook,
  `ChatActivity.java:30296`) calls the chat's own video pause path (`toggleVideoRecordingPause`,
  `ChatActivity.java:2445-2449`, which calls the camera view's `togglePause`, `InstantCameraView.java:670-702`). The strip shows "Paused" with Resume.

### The spike (Android, before any cross-chat work)

Question: can the round's preview move to an activity-level window, and the round outlive its chat screen, without
restarting the camera or the encoder?

What the code says:

- The camera does not draw into the `TextureView` directly in the legacy recorder. `CameraGLThread` renders the
  camera's own texture into an EGL window surface created on the `TextureView`'s `SurfaceTexture`
  (`InstantCameraView.java:1652`), and the encoder has its own input. So only the preview output depends on the view.
- The chat's view is detached whenever another fragment fully covers it (`ActionBarLayout` calls
  `onRemoveFromParent`, the same path F1's round PiP handover uses). Detaching a `TextureView` calls
  `onSurfaceTextureDestroyed`, and upstream's listener then shuts the camera thread down and closes the camera
  (`InstantCameraView.java:842-859`). In V2, the controller treats it as a fatal error
  (`utils/recorder/round/RoundVideoCameraController.java:430-435`).
- Android supports keeping a `SurfaceTexture` across views: if `onSurfaceTextureDestroyed` returns false, the
  texture is not released, and a new `TextureView` can adopt it with `setSurfaceTexture`. The existing EGL surface
  (legacy) or camera output surface (V2) keeps working because it targets the texture, not the view.
- `PipRoundVideoView` already shows that an application-level window over the activity survives navigation
  (`ui/Components/PipRoundVideoView.java:291-294`).

Spike steps (about 1.5 agent-days, on a device):

1. Add the two candidate hooks: one early return at the top of each `onSurfaceTextureDestroyed`
   (`InstantCameraView.java:842`, `RoundVideoCameraController.java:430`) that returns false while
   `PurpleRoundBubble` has adopted that texture.
2. Create a bubble window like `PipRoundVideoView`'s, with a `TextureView` that calls `setSurfaceTexture` on the
   adopted texture.
3. Open another chat on top and record 30 s. Pass if the preview keeps moving, the file has no gap (frame
   timestamps continuous) and audio continues. Repeat with both recorders and with a camera flip in the bubble.
4. Go back (destroying the original `ChatActivity`) and check the remaining blockers: the `InstantCameraView`'s
   `Delegate` is that `ChatActivity` (send would run on a finished fragment), the upload-while-recording observer is
   removed on detach (`:577-580`; the send then uploads the whole file, which is correct but slower), and V2's
   session callbacks post into the destroyed view.

Outcome rule: if steps 1-3 pass, ship cross-chat bubbles with a Purple `Delegate` wrapper that captures the target
at start (about 3 more agent-days, a later commit group). If step 3 fails, or step 4 shows send cannot be made
independent of the chat, ship the owner-approved fallback below and keep cross-chat for later. Either way the
fallback is built first, because cross-chat builds on it.

### Android fallback design (v1, bubble within the chat)

- **The tap.** One guarded early return at the top of `onTouchEvent` in `InstantCameraView` (`:3793`) and in
  `InstantCameraView2`: `if (org.telegram.ui.Components.PurpleRoundBubble.onTouch(this, ev)) return PurpleRoundBubble.consumed(ev);`
  written as one line. While full-size, a tap outside the camera circle shrinks. While shrunk, touches outside the
  bubble return false, so the `contentView` passes them to the chat below (a `FrameLayout` offers an unconsumed
  `ACTION_DOWN` to the next child down).
- **The shrink.** `PurpleRoundBubble` (in `ui/Components` for package access) animates the camera container's
  scale and translation to the corner and turns off the dim: the chat's dim and blur follow
  `animatorRoundMessageCameraVisibility` (`ChatActivity.java:10802-10803`, reset at `:35917`), which `PurpleChatHooks` sets to false
  while shrunk. No camera or encoder call is made, so recording is not interrupted.
- **Controls on the bubble.** Flip calls the recorder's own switch-camera button handler; pause calls the chat's
  `toggleVideoRecordingPause`.
- **Leaving the chat** with a shrunk round: the round pauses (as on background) and the strip shows it paused with
  Resume, which opens the chat and resumes. This is the "bubble within the chat" limit.

### Desktop

- The recorder's preview is a child widget of the chat's outer container, centred by a `sizeValue` subscription
  (`RoundVideoRecorder::setup`, `ui/controls/round_video_recorder.cpp:1277-1292`). It does not dim the chat;
  the chat around it stays usable already.
- **Shrink-to-float within the chat is possible with one hook**: an early return inside that geometry lambda when
  `Purple::RoundRecorderFloats(raw)` has taken the preview over. Purple installs an event filter on the preview
  (the same call) for drag, snap and expand, and draws flip and pause buttons on hover.
- **Cross-chat on Desktop** needs the `RoundVideoRecorder` to outlive its `VoiceRecordBar`, which owns it through a
  private `unique_ptr`, and the bar cancels on destruction (`:2140-2144`). Within the main section the
  `HistoryWidget` is reused across chats, so a later extension point (handing the recorder to the dock) is small,
  but it is not in v1. Switching chats with a shrunk round shows upstream's discard box, as today.

### Edge cases

- **The 60 s cap** still sends to the target from the bubble (upstream's timer), or pauses into preview with
  F10's "keep" when the user enabled pause-instead-of-send (Desktop `setPauseInsteadSend`, `:2544`).
- **Rotation (Android).** Upstream locks the orientation while recording (`AndroidUtilities.lockOrientation`,
  `InstantCameraView.java:699`); the bubble keeps its corner.
- **Keyboard open (Android).** The bubble moves above the keyboard.
- **Calls.** A call pauses the round like a voice recording (extra 3).

## F9. Seek back and forward (step 5, 10, 15 or 30 s, default 10)

### Android

- **Strip buttons.** Two small buttons, "−10" and "+10" (labels follow the step), left of the speed button in the
  player row. `PurpleRecordingStrip` (the same per-strip helper) adds them and shows them only in the audio player
  style: one hook in `FragmentContextView.updateStyle` hands it the new style. They seek with
  `MediaController.seekToProgress` (the call the session's `onSeekTo` already makes, `MusicPlayerService.java:149-155`),
  clamped to 0 and to one second before the end.
- **Headset rewind and fast-forward.** The media session advertises `ACTION_REWIND | ACTION_FAST_FORWARD` (one hook
  line at each of the two places that build its actions, `:478-499` and `:727-740`), and the session callback gains
  `onRewind` and `onFastForward`, each a one-line body delegating to `PurpleSeek.step(-1 or +1)`. The default
  `onMediaButtonEvent` then maps `KEYCODE_MEDIA_REWIND` and `KEYCODE_MEDIA_FAST_FORWARD` to them.
- The expanded player keeps upstream's long-press rewinding; Purple adds nothing there.

### Desktop

- **Player bar.** Two buttons in the right-hand controls before the volume toggle. Hooks in
  `media/player/media_player_widget.cpp`: one creation call at the end of the constructor, one in
  `updateControlsGeometry` (`:414-437`) that places them and returns the width used, and one in `getTimeRight`
  (`:589-601`) that adds that width, plus one include.
- **Float hover controls.** F1's optional `Purple::DecorateFloat` overlay (designed in `docs/media-design.md`) gets
  the two seek buttons. No new hook.
- **Shortcut commands.** `Command::MediaSeekBackward` and `Command::MediaSeekForward` in the enum (`core/shortcuts.h`,
  +2), their names `media_seek_backward` and `media_seek_forward` in `CommandByName` (`core/shortcuts.cpp:75`, +2,
  which makes them bindable in the custom shortcuts file) and the defaults `alt+shift+left` and `alt+shift+right`
  in `fillDefaults` (`:497`, +2). Upstream binds neither key. Purple handles the requests in its own
  `Shortcuts::Requests()` subscription, so the handler needs no hook.
- **Optional:** two rows in the Shortcuts settings page list (`settings/sections/settings_shortcuts.cpp:128-134`),
  so the commands can be rebound in the UI as well as in the file.
- **System media controls.** One early return at the top of the `commandRequests` handler
  (`media/system_media_controls_manager.cpp:379`): when no video delegate is set, Purple handles `SkipForward` and
  `SkipBackward` for the active audio track by the step, and play/pause while recording (extra 4). Where the
  platform shows skip buttons depends on the platform integration; the handler is harmless where it does not.

### Edge cases

- A seek past the end jumps to one second before it, so the queue does not advance by surprise. A seek before the
  start goes to 0:00.
- Voice and round playback both seek; a round in the float seeks too.
- The step setting changes the button labels live.

## F10. Crash-safe recordings and the Recording bin

### The registry (both clients)

Purple keeps a small manifest, `recordings.json`, in its own directory (Android `files/purple-recordings/`, Desktop
`tdata/purple/recordings/`). One entry per recording:

- an id, the account (Android slot index, Desktop session index), chat, topic and reply ids, kind, start time,
  duration so far, and the view-once flag;
- the media file path, the journal path (rounds), and a state: `recording`, `sending`, `sent`, `unsent`,
  `recovered`, `unrecoverable`.

The entry is written at start and on every state change, with an atomic write (temporary file, then rename).

- **On a clean end**, Send moves the entry to `sending`. Android hands the file to `SendMessagesHelper`, which
  persists and retries its own pending sends across restarts, so `sending` becomes `sent` and the entry is dropped
  once the message is queued. Desktop does not persist pending uploads, so it keeps the entry until the session
  reports the message sent to the server, then deletes the file and the entry.
- **Discard** moves the entry to `unsent` and moves the file into the bin directory (open question 1), or deletes it.
- **On the next start**, every entry still in `recording` or `sending` belongs to a crash. Recovery runs on a
  background thread, moves the result into the bin directory and marks it `recovered` or `unrecoverable`.

Files leave Telegram's cache directories as soon as they are recovered, because "Clear cache" and the storage
auto-cleaner may delete anything in `MEDIA_DIR_*` (Android) or the Desktop cache.

### Voice

- **Android, both recorders.** Bytes already reach the disk (see "Crash safety today"). The dock records the file:
  V2 names it in its listener (`onOutputStarted`), which is private to `MediaController`, so Purple finds it as the
  newest `audio_recording_output_*` or `<time>_*.ogg` file created in `MEDIA_DIR_AUDIO` after the recording's
  start. That is a directory listing at `recordStarted` plus one second, with no hook.
- **Desktop.** One hook in `Private::WriteData` (`media_audio_capture.cpp:259-271`):
  `Purple::MirrorRecordingWrite(l, l->dataPos, buf, buf_size);` before the `memcpy`. Purple keeps a file per
  `Private` object and writes each block at the same offset, so seeks behave like they do in memory and the file
  is byte-identical to the bytes that get sent. Writes are 4 KB blocks from the AVIO buffer, and the Ogg muxer
  closes a page about once a second, so a crash loses at most about two seconds.
- **Recovery** (`purple/purple_ogg_repair.{h,cpp}` in purple-core, pure C++ without Qt so both clients can compile
  it):
  1. walk Ogg pages and check each CRC;
  2. truncate after the last good page;
  3. set the end-of-stream flag on it and recompute its CRC;
  4. count granule positions for the duration.

  The waveform is computed the way each client already does: Android's native waveform call
  (`AudioRecorderNative.nGetWaveform`), Desktop by decoding with FFmpeg and taking a peak per bucket as
  `Media::Capture` does.

### Round video: the journal (exact recovery)

The sent file is unchanged: the recorder still writes upstream's `MP4Builder` layout, and the journal is a sidecar.

- **Format** (`<file>.pjr`, little-endian, append-only):
  - header: the magic `PJR1`, version, the split-mdat flag, the video size and rotation, then per track: kind, MIME
    type, timescale, width and height or sample rate and channels, and the codec configuration bytes (`csd-0`,
    `csd-1`, as `Track`'s constructor reads them, `messenger/video/Track.java:87-120`, `:210-270`). This header is
    the **config header**.
  - batches: a count, then per sample a 24-byte record (track, sync flag, size, file offset, presentation time in
    µs), then a CRC32 of the batch.
- **Writing.** Samples are buffered in memory and appended as one batch every interval (setting `round_journal`,
  default 1 s), on the muxer's own thread, so no locking is needed. At about 77 samples a second (30 video, 47
  audio) that is about 1.8 KB per second, at most about 110 KB for a 60 s round. The journal is fsynced every 5 s
  only on devices where `MP4Builder` syncs too (`setAllowSyncFiles(SharedConfig.deviceIsHigh())`,
  `InstantCameraView.java:3306`), because a process crash keeps written data in the page cache and only power loss
  needs fsync.
- **Hooks.**
  - `Mp4Movie.addTrack` and `Mp4Movie.addSample` (`messenger/video/Mp4Movie.java:67-77`) each get one guarded call
    into `PurpleRoundJournal`, which ignores any movie that was not registered.
  - Registration: one call after `movie.setCacheFile(...)` in `InstantCameraView`'s `VideoRecorder` (`:3301-3302`)
    and one in `RoundVideoMp4Writer.initializeIfReady` (`RoundVideoMp4Writer.java:150-151`). Video conversions,
    the camera screen and story recording use `MP4Builder` too and are never registered.
  - V2 starts a new output file per generation (`RoundVideoSession.createOutputGenerationOnFileThread`, `:1276-1282`).
    Each is registered and journaled; the spike confirms whether a resumed generation contains the retained prefix
    (as voice V2 does, "resume prefix"), in which case recovery uses the newest one, or whether the bin must join
    the parts.
- **Recovery** (`messenger/purple/PurpleRoundRecovery.java`):
  1. read the header and the batches whose CRC matches;
  2. drop samples whose `offset + size` is past the end of the file (not on disk yet);
  3. build a new file through `MP4Builder`'s public API: `createMovie` with the recorded split flag, `addTrack`
     with a `MediaFormat` rebuilt from the header, then `writeSampleData(track, bytes, info, false)` per sample.
     The sample bytes are copied as they are: a video sample on disk is already the 4-byte length plus the NAL data
     that `writeSampleData(..., true)` produced (`MP4Builder.java:162-170`), so writing it with `writeLength`
     false reproduces it exactly;
  4. `finishMovie()`.

  The result has upstream's layout (`ftyp`, `mdat`, `moov` at the end), built by upstream's own code.
- **Staleness.** A journal is stale when its last good batch ends well before the file does (the recorder crashed
  between flushes) or the file was rewritten (V2 trimming). The exact part is still used; repair continues from
  the last journaled sample (below).

### Round video: repair (the fallback)

Repair rebuilds the index by scanning the media data, as untrunc does. It runs when the journal setting is Off, when
the journal is missing, and for the tail after a stale journal's last sample. It needs the config header, which is
written once at the start even when the journal is Off (a few hundred bytes). See open question 2.

What makes it workable, from `MP4Builder`'s actual layout:

- **No chunk grouping on disk.** `writeSampleData` writes one sample per call, in arrival order
  (`MP4Builder.java:107-183`); chunks exist only in the `moov` tables (`createStsc`). So the media data is a plain
  sequence of samples.
- **Split mdat.** With `split` true (always in V2, `RoundVideoMp4Writer.java:153`; in legacy only for secret chats,
  `InstantCameraView.java:3305`) a new 16-byte `mdat` header (size 1, `mdat`, 64-bit size) is written after every
  32 KB (`:147-156`). Between samples, the scanner recognises those 8 signature bytes and skips the header.
- **Video samples delimit themselves**: a 4-byte big-endian length (written by `MP4Builder` as `size - 4`), then
  NAL data.
- **Audio samples do not**: they are raw AAC frames with no ADTS header (`writeLength` false). Their length is only
  known by decoding.

The scanner (`purple/purple_mp4_repair.{h,cpp}` in purple-core, C++17 without Qt):

1. Start after `ftyp`, or at the end of the last journaled sample.
2. At each sample boundary, try in order:
   - an `mdat` header signature, which is skipped;
   - a video sample: length L between 1 byte and the remaining size (and below 1 MB), a first NAL byte with the
     forbidden bit clear and type 1, 5, 6, 7 or 8, and, for type 1 or 5, `first_mb_in_slice` equal to 0. It is
     accepted only if a valid boundary also follows it (one-step lookahead);
   - an audio frame: the client's AAC measure callback decodes from that position with the config header's
     AudioSpecificConfig and returns the bytes consumed for exactly one 1024-sample frame, or failure.
3. Stop at the first position where neither fits, and keep everything before it. A truncated last sample is
   dropped.
4. **Timing.** Audio is exact: frame n starts at n × 1024 / sample rate. Video is anchored to the audio clock: a
   video sample's time is the audio duration written before it, plus a constant encoder offset (taken from the
   journal's exact part when there is one, otherwise 0), then forced to increase by at least 1/60 s. Sync flags
   come from the NAL type (5 is IDR).
5. Hand the sample list to the same rebuild as the journal path.

The AAC measure callback uses FFmpeg's AAC decoder, which both apps already ship. Android's bundled FFmpeg is built
with `--enable-decoder=aac` (`TMessagesProj/jni/prebuild/build_ffmpeg.sh:308`). On Android the scanner and a small
shim (`purple_jni/aac_measure.cpp`) are compiled into upstream's `tmessages` library through `target_sources` in
`purple.cmake`, as the archive bridges already are (`purple_jni/purple.cmake:83`), because that library links
`avcodec` (`TMessagesProj/jni/CMakeLists.txt:64-65`, `:293-307`) and `libpurplecore` does not. Desktop calls it
with its own FFmpeg. The implementer confirms in a host case that the decoder reports bytes consumed per raw frame
on both FFmpeg versions.

**Reliability:**

- **Interleaving:** high. The layout is a flat sample sequence. Misreading audio bytes as a video length is ruled
  out by the lookahead in practice; when the stream cannot be parsed, repair stops instead of guessing.
- **A/V drift with a variable camera frame rate:** this is the weak point, and it is why a nominal-duration repair
  is not good enough. Both recorders keep real timestamps: V2 derives each sample's duration from source time
  deltas (`TrackTimeline.add`, `RoundVideoMp4Writer.java:528-535`) and legacy uses camera timestamps. A camera that
  drops to 15 fps in low light would make a nominal 30 fps repair play video at double speed and end halfway.
  Anchoring to the audio clock bounds the error to the encoders' latency difference, about 30-100 ms. That is
  usable for a recovered message, but not exact.
- **Without a config header** (files from before Purple, or a header write that failed) audio cannot be decoded
  and repair gives up: the file is listed as unrecoverable and kept until auto-clear, so it can still be copied out.
- **Desktop rounds** use FFmpeg's MP4 muxer, whose interleaving is by timestamp rather than arrival, but the
  scanner does not depend on order, only on boundaries.

**How the two combine:**

- Journal on and fresh: exact up to the last batch, repair for the tail (at most one interval), with the encoder
  offset learned from the exact part, so the tail is close to exact too.
- Journal on but stale (gap, CRC failure): exact part plus repair from there.
- Journal Off: config header only, repair from the start.
- No config header: unrecoverable, listed.

**Effort for repair alone:** about 3.5 agent-days. The scanner and the timing take 1.5, the AAC shims on both
clients 1, and a corpus check 1: record real rounds on a device, truncate them at random offsets, and compare repair
against journal recovery of the same files.

### Desktop rounds

Desktop builds rounds from FFmpeg packets (`RoundVideoRecorder::Private`, `av_interleaved_write_frame` at
`round_video_recorder.cpp:644`), which makes an exact recovery copy cheap without touching the sent bytes:

- **Journal setting on**: Purple keeps a second output, a fragmented MP4 recovery copy, with
  `frag_duration` set to the journal interval. The sent bytes stay upstream's in-memory standard MP4.
  - Hooks: one call after `avformat_write_header` in `initEncoding` (`:330`) that copies the codec parameters, and
    one before the write at `:644` that hands Purple a reference to the packet (`av_packet_ref`, because
    `av_interleaved_write_frame` takes ownership).
  - Recovery remuxes the fragments into a standard MP4 with `moov` at the end, the layout upstream sends.
  - A fragmented file is used only as the private recovery copy, never sent. That is why the compatibility
    concern behind rejecting option (b) does not apply.
- **Journal Off**: the same two hooks mirror the standard MP4's bytes (`WriteBytesWrap` writes) and the codec
  parameters, and recovery is the shared repair scanner.

### Rejected options for round video

- **(b) Fragmented MP4 as the sent file.** Rejected (owner): receivers and the server expect upstream's layout for
  rounds, and older clients' round players have not been checked against fragmented files.
- **(c) Segment files** joined at the end. Rejected: it changes the recorder's write path (the upload-while-recording
  offsets would no longer be one file) and needs a remux before send, so the sent file would be built by Purple's
  code rather than upstream's.

### The Recording bin

- **Where.** Android: a "Recording bin" row in Purple settings opens `ui/PurpleRecordingBinActivity.java`. Desktop:
  a "Recording bin" button in the Purple "Media recording" group opens a box (`purple/purple_recording_bin_box.{h,cpp}`).
  The row shows a count when the bin is not empty.
- **The list**, newest first. Each item shows the chat (avatar and name, "Unknown chat" for orphans), kind, duration,
  when it was recorded, and a mark: "Recovered after a crash", "Not sent" or "Partly recovered" (repair stopped
  early).
- **Actions per item:** play (an inline player; rounds play in a small circle); Send to the original chat; Send to
  another chat (the upstream chat picker); Save to Downloads (it reuses `PurpleMediaSave`); Delete. Clear all asks
  for confirmation.
  - Send to the original chat drops the reply if the replied message no longer exists.
  - Sending uses upstream's routes for each kind, so the message looks exactly like a normally recorded one:
    Android `SendMessagesHelper` with a voice document carrying the waveform and duration, as
    `stopRecordingInternal` builds it (`MediaController.java:5408`), and for rounds the `VideoEditedInfo` with
    `roundVideo` that `InstantCameraView.send` builds (`:975-1025`); Desktop `ApiWrap::sendVoiceMessage` with
    `video` set for rounds.
- **Auto-clear.** Items older than `bin_keep` (default 48 h; choices 12 h, 24 h, 48 h, 7 days, never) are deleted at
  start and once an hour while the app runs.
- **One-time orphan scan.** On the first start with F10, Purple scans Telegram's directories once for recordings
  that crashed before Purple existed:
  - `MEDIA_DIR_AUDIO` for `audio_recording_*` files, and `.ogg` files whose last page lacks the end-of-stream flag;
  - `MEDIA_DIR_DOCUMENT` and `files/cache` for `.mp4` files with `ftyp` and `mdat` but no `moov`.

  Each hit becomes an "Unknown chat" bin item. Voice is finalised; rounds are repaired if possible, which is
  unlikely without a config header, and kept raw otherwise. Files referenced by a message in the database are not
  touched, because finished recordings always have `moov` or the end-of-stream flag. Desktop has no orphans to find
  (it kept everything in memory). The scan records that it ran in Purple's device-local preferences.

### Edge cases

- **Disk full.** The mirror and the journal stop writing quietly (a diag line), and the recording itself continues
  as upstream does. The bin shows "Partly recovered" if a crash follows.
- **Secret chats (Android).** Recordings for secret chats are kept in the bin like others but never offered "Send to
  another chat" or "Save to Downloads", and they are deleted when the secret chat is.
- **Several accounts.** The bin lists all accounts' recordings with an account badge; Send uses the recording's own
  account.
- **App update between crash and restart.** The journal and manifest are versioned; an unknown version is listed as
  unrecoverable and kept raw.
- **Work Mode.** An item whose chat the running preset hides is shown as "Hidden chat" without a name, and its
  "Send to the original chat" button opens the chat through the stock route.

## F11. Crash capture into the diagnostic log (Android)

### Design

`messenger/purple/PurpleCrashCapture.java`, started by one new hook line early in `ApplicationLoader.onCreate`
(next to the existing Purple hooks, `:265-267`).

- **Java uncaught exceptions.** It installs a default handler that chains to whatever handler was set before it
  (`BuildVars` installs one only when logs are on, `BuildVars.java:46-58`). The handler builds the scrubbed lines and
  writes them **synchronously** to `files/purple-diagnostics/pending-crash.log` with an fsync, then calls the
  previous handler. The diag log's writer is an asynchronous daemon thread (`PurpleDiagLog.java:44-77`) that dies
  with the process, so it cannot be used here. On the next start the pending file is appended to the diag log and
  deleted.
- **ApplicationExitInfo** (API 30 and later; the app's minimum is 23, `TMessagesProj/build.gradle:138`). On start,
  read `getHistoricalProcessExitReasons(package, 0, 16)` and handle the entries newer than the last one recorded in
  Purple's preferences:
  - every reason gets one line: reason code, status, importance, whether it was the main process, and the PSS and
    RSS at exit in MB (a clamped count);
  - `REASON_CRASH`: matched with the pending Java crash by time, so the two sources make one record;
  - `REASON_CRASH_NATIVE` on API 31 and later: `getTraceInputStream()` returns the tombstone in protobuf form, which
    a small reader without dependencies parses. It keeps the signal and code names, the abort cause class (not the
    message), and up to 12 frames of the crashing thread;
  - `REASON_ANR`: the trace stream is text. It keeps the main thread's state and its top 12 frames;
  - `REASON_LOW_MEMORY`, `REASON_EXCESSIVE_RESOURCE_USAGE` and the others: the line alone.
- **The scrubbed record.** New `PurpleDiagLogStore` factories, so privacy stays a property of the API:
  - `frame(String className, String method, int line)` accepts only Java identifier characters and dots, keeps the
    package only for `org.telegram`, `android`, `androidx`, `java`, `javax`, `kotlin` and `com.google` (anything
    else becomes `other`), drops synthetic lambda suffixes, and logs the line number as a clamped count;
  - `nativeFrame(String library, String function, long relativePc)` keeps the library base name only for the app's
    own libraries (`libtmessages.*`, `libpurplecore`, `libQt6Core`) and otherwise `system`, keeps a demangled
    function name if present, and logs the relative pc in hex for app libraries only (open question 3);
  - `signal(int signo, int code)` maps to names such as `SIGSEGV` and `SEGV_MAPERR`.
  - Exception messages are dropped entirely for crashes (only the class is kept), because a crash message can hold
    anything. Caused-by chains keep each cause's class and its top 3 frames.
  - The new words (reasons, signal names) are added to the vocabulary; class and method names pass through the
    identifier check instead.
- **Layout in the log.** One line `crash start reason=... signal=...`, then one line per frame
  (`crash frame n=3 at=org.telegram.ui.ChatActivity.onPause line=30303`), then `crash end frames=12`. Lines stay under
  512 characters.
- **Export.** Unchanged: the owner exports the diag log from Purple settings after any crash, Purple's or
  upstream's.

### Edge cases

- **A crash inside the handler** is caught and ignored, and the previous handler still runs.
- **A crash loop.** At most 5 crash records per start are written, plus one `crash repeated count=n` line, using the
  existing `PurpleDiagRepeats` limiter.
- **API 26-29** have no exit info: only Java crashes are captured.
- **No identifiers.** Frames name code, not data: no ids, names, numbers or text from the user, which matches the
  diag log rules.

## Extras

### 1. Send to another chat from the dock (default on)

A "Send to…" item in the strip's long-press menu and the bin's actions. The recording stops. Android opens upstream's
chat picker (`DialogsActivity` in select mode), then sends the file with `SendMessagesHelper` as the bin does.
Desktop uses `Window::ShowChooseRecipientBox`, then `ApiWrap::sendVoiceMessage` with a new action. It is not
offered for secret chats or view-once recordings.

### 3. Calls pause the recording and offer Resume (default on)

- **Android.** A Telegram call starting (`NotificationCenter.didStartedCall`), or the phone's audio mode turning to
  `MODE_IN_CALL` or `MODE_IN_COMMUNICATION` (`AudioManager.addOnModeChangedListener`, API 31 and later; earlier,
  upstream's own ringing handler already stops into a draft), pauses the recording with reason "call". After the
  call the strip shows "Paused for a call" with Resume. It never resumes by itself.
- **Desktop.** Purple subscribes to the calls instance's current call and group call; a call starting adopts the
  recording into the dock if needed, then pauses it the same way.

### 4. Headset button while recording (default on; hold-to-start default off)

- **Android.** `PurpleRecordingService` owns a `MediaSessionCompat` that is active only while a recording exists.
  It plays the feedback tone through the session's audio attributes once at start, so Android routes media buttons
  to it (Android 8 and later send buttons to the session that last played audio). It is released when the
  recording ends, so no button is taken otherwise.
  - One press (`onPlay` or `onPause`): pause or resume.
  - Double press (`onSkipToNext`, which `MediaSessionCompat` produces for a double `HEADSETHOOK`): send.
  - Discard is screen-only.
  - Feedback: a short tone (`ToneGenerator`, 80 ms) and a vibration (30 ms for pause or resume, two pulses for send).
  - Hold-to-start (off by default): while a chat is open and the setting is on, the session is active with a silent
    state, and a long press starts a voice recording in that chat. This does take buttons while the chat is open,
    which is why it is off by default.
  - When the microphone is a Bluetooth headset in SCO mode, many headsets send call-control events instead of media
    keys; those are not handled (they belong to calls).
- **Desktop.** One early return at the top of the `Shortcuts::Requests()` handler in `Media::Player::Instance`
  (`media/player/media_player_instance.cpp:1490-1517`, a file F3 already hooks): while a recording exists,
  `MediaPlayPause`, `MediaPlay` and `MediaPause` pause or resume the recording instead of the player. The system
  media controls hook (F9) does the same for platform play/pause.

### 5. Long-recording nudge (default on, 30 min)

At 30 min of recorded time, and every 30 min after that, the dock vibrates (Android) or flashes the strip (Desktop)
and shows "Still recording · 30:00" with Pause and Send. The Android notification updates too. The schedule
(`NextNudgeAt(elapsed, interval)`) lives in purple-core.

### 6. Preview and trim from the dock (default on)

- **Android.** Preview pauses the recording, which makes upstream save it as the target's voice draft, then opens
  the target chat. The chat restores the paused panel with play and trim (`prepareResumedRecording`,
  `MediaController.java:4911`), so preview and trim are upstream's own UI. From there the recording is in its chat
  again.
- **Desktop.** Preview pauses the capture and opens a Purple popup under the strip with play, a waveform and two
  trim handles. It plays the bytes through a local `DocumentData`, as the bar's listen state does with its
  `DummyDocument` (`voice_record_bar.cpp:167`). Trim cuts on Opus packet boundaries with the Ogg finaliser's page
  walker and applies on Send, and Resume continues from the full capture, as upstream's untrimmed resume does.

## Settings

App-wide, in `settings.toml`, next to F3's `[playback]` table. purple-core parses everything; each client ignores
the keys that do not apply to it.

```toml
[playback]
private_chat_queue_p = true   # F3, already on the media branch
seek_step = "10s"             # F9: "5s", "10s", "15s" or "30s"

[recording]
keep_across_chats_p = true    # F8
background_p = true           # F7, Android only
round_float_p = true          # F6
round_journal = "1s"          # F10: "off", "1s", "2s" or "5s"
bin_keep = "48h"              # F10: "12h", "24h", "48h", "7d" or "off" (never clear)
send_elsewhere_p = true       # extra 1
call_pause_p = true           # extra 3
headset_p = true              # extra 4; Desktop: media keys while recording
headset_hold_start_p = false  # extra 4, Android only
long_nudge = "30m"            # extra 5; "off" disables
preview_trim_p = true         # extra 6
```

- **Parsing** follows `ReadPlayback` (`purple/purple_settings.cpp:1563-1585` in purple-core): a `Recording` struct in
  `purple_settings.h`, `ReadBool` with defaults, `ReadSeconds` for durations (`:1587-1600`), a warning for a value
  that is not a table, the scope entry `{ u"recording"_q, SettingScope::AppWide }`, and a "Recording" label in
  `purple_config_diff.cpp`.
- **Choices.** `seek_step` and `round_journal` accept only their listed values; anything else warns and keeps the
  default. `bin_keep` accepts any duration of at least 1 h.
- **"7d".** `ParseDuration` knows `s`, `m` and `h` only (`purple_settings.cpp:2416-2437`). Adding `d` is a
  one-line, additive change that every duration key gains (open question 6). Without it the 7-day choice is written
  as `"168h"`.
- **Android** reads them through one JNI method returning a packed `int[]` (the `voiceQueueKeeps` bridge on the
  `media` branch is the pattern) and writes through the existing TOML splice path.
- **Rows.**
  - Android: in `PurpleSettingsActivity`, a "Media recording" header with a row opening
    `ui/PurpleRecordingSettingsActivity.java` (all keys above, Android ones only), a "Seek step" chooser under the
    existing "Media playback" header, and the "Recording bin" row. Row ids come from a new block
    `0x70750500` to `0x7075050f`; the highest id in use is `0x70750482`, and the `media` branch's
    `ROW_VOICE_QUEUE` is `0x70750027`.
  - Desktop: a "Media recording" group in the Purple section of the Advanced page (`Purple::BuildAdvancedSection`,
    `purple/purple_settings_section.cpp`), with the Desktop keys, the seek step chooser in "Media playback", and the
    "Recording bin" button.
- **Not synced:** the bubble's corner, the orphan scan marker and the last processed exit time are device-local.

## Upstream hooks

Counts are added lines unless stated. "Spike" marks a line that exists only if cross-chat bubbles ship.

### Android (12 files, about +30 −1)

1. `messenger/MediaController.java`, `stopRaiseToEarSensors` (`:2439`). F7 and F8: one guarded line that sets
   `stopRecording` to false, +1. Fully qualified. The file already carries F3's hook.
2. `ui/ChatActivity.java`, `checkRecordLocked` (`:34926`). F8: one early return through `purple`, +1. Hot file.
3. `ui/Components/ChatActivityEnterView.java`, +6:
   - after `recordPanel.setOnTouchListener` (`:9765`), F8: one call, +1;
   - top of `didReceivedNotification`, F8: one guarded early return, +1;
   - extension point `purpleReleaseRecordUi()`, F8: +4. It is the only place that can clear the private
     `recordingAudioVideo`.
4. `ui/Components/FragmentContextView.java`, +4 −1:
   - a field `purple`, the per-instance helper, created with a lambda to `checkPlayer(false)`, +1;
   - the widened hide condition in `checkPlayer`, +1 −1;
   - an early return after the show path makes the view visible, +1;
   - one call in `updateStyle` (F9 seek buttons), +1.

   The widened condition is the one edited line. There is no early-return site that would not change the
   branch, which is the same situation as F1's `updateTextureViewPosition` condition. It is recorded in the README
   section.
5. `ui/Components/InstantCameraView.java`, +3:
   - top of `onTouchEvent` (`:3793`), F6: +1;
   - `VideoRecorder`, after `movie.setCacheFile` (`:3302`), F10 registration: +1;
   - top of the `TextureView` listener's `onSurfaceTextureDestroyed` (`:842`), F6 spike: +1.
6. `ui/Components/InstantCameraView2.java`, top of `onTouchEvent`. F6: +1.
7. `utils/recorder/round/RoundVideoMp4Writer.java`, `initializeIfReady` after `setCacheFile` (`:151`). F10
   registration: +1.
8. `utils/recorder/round/RoundVideoCameraController.java`, `previewListener.onSurfaceTextureDestroyed` (`:430`).
   F6 spike: +1.
9. `messenger/video/Mp4Movie.java`, `addTrack` and `addSample`. F10: one guarded call each, +2.
10. `messenger/MusicPlayerService.java`, +8:
    - `actions |= ...` at the two action builders, F9: +2;
    - `onRewind` and `onFastForward` in the session callback, F9: +6 (annotation, one-line body, brace each).
11. `messenger/ApplicationLoader.java`, `onCreate`. F11: one call, +1.
12. `TMessagesProj/src/main/AndroidManifest.xml`. F7: one `<service>` line, +1.

Without the two spike lines: 10 files are touched in full and two (InstantCameraView, here at +2) partially; the
total is about +28 −1.

`docs/upstream-hooks.baseline` is regenerated with `purple/upstream-hooks.sh baseline` in each commit that adds a
hook. New `file` lines appear for `FragmentContextView.java`, `InstantCameraView.java`, `InstantCameraView2.java`,
`RoundVideoMp4Writer.java`, `RoundVideoCameraController.java`, `Mp4Movie.java` and `MusicPlayerService.java`.
Run `purple/upstream-hooks.sh check` before each commit, with new files marked by `git add -N`.

The README gets a "Media recording" section, next to "Media playback", that names the upstream files and why. It
covers in particular the extension point in `ChatActivityEnterView` and the widened condition in
`FragmentContextView`.

### Desktop (11 files, about +30 −1; optional +2 in a twelfth)

1. `history/history_widget.cpp`, `HistoryWidget::preventsClose` (`:7360`). F8: one early return, +1. The file already
   includes Purple.
2. `history/view/controls/history_view_compose_controls.cpp`, `ComposeControls::preventsClose` (`:6380`). F8: one
   include and one early return, +2.
3. `history/view/controls/history_view_voice_record_bar.cpp`, +3:
   - end of `VoiceRecordBar::init` (`:2222`), F8: one call, +1;
   - top of `VoiceRecordBar::stopRecording` (`:2929`), F10: one call that tells Purple Send or Cancel, +1;
   - one include, +1.
4. `media/audio/media_audio_capture.cpp`, `Instance::Inner::Private::WriteData` (`:259`). F10: one include and one
   call, +2.
5. `ui/controls/round_video_recorder.cpp`, +4:
   - after `avformat_write_header` in `initEncoding` (`:330`), F10: +1;
   - before `av_interleaved_write_frame` (`:644`), F10: +1;
   - the geometry lambda in `RoundVideoRecorder::setup` (`:1285`), F6: one early return, +1;
   - one include, +1.
6. `media/player/media_player_widget.cpp`, F9: one include, one creation call in the constructor, one call each in
   `updateControlsGeometry` and `getTimeRight`, +4.
7. `media/player/media_player_instance.cpp`, the `Shortcuts::Requests()` handler (`:1490`). Extra 4: one early
   return, +1. The file is already hooked by F3.
8. `core/shortcuts.h`, the `Command` enum. F9: +2.
9. `core/shortcuts.cpp`, `CommandByName` and `fillDefaults`. F9: +4.
10. `media/system_media_controls_manager.cpp`, the `commandRequests` handler (`:379`). F9 and extra 4: one include
    and one early return, +2.
11. `mainwidget.cpp`, the strip. One include, the attach call in the constructor, the addend in
    `getMainSectionTop` (+1 −1), and one placement call in each `updateControlsGeometry` branch: +5 −1.
12. Optional: `settings/sections/settings_shortcuts.cpp`, two list rows, +2.

`docs/purple/upstream_hooks.md` gets a section "Media recording (dock, background, PiP, seek, crash safety)" with a
bold entry per file. The feature doc is `docs/purple/media_recording.md`. Measure with
`purple/upstream_report.sh --worktree --near`.

### Places where a pure insertion was impossible

- **Android `ChatActivityEnterView`**: the private `recordingAudioVideo` forces a 4-line extension point. The
  alternative, reflection, would fail silently after an upstream rename.
- **Android `FragmentContextView`**: the visibility machinery is private, with a private delegate. Riding the
  existing show path needs one widened condition. Copying the animation into Purple would be the larger merge risk.
- **Desktop `MainWidget`**: the top bars are stacked by hand, so the strip needs its height in the sum and its
  position in both layout branches.

## Strings

Android `res/values/purple_strings.xml` (Desktop `Telegram/Resources/langs/purple.strings` gets the same texts as
`lng_purple_*` keys):

- `PurpleMediaRecording` "Media recording"
- `PurpleRecordingKeepAcrossChats` "Keep recording when you switch chats"
- `PurpleRecordingBackground` "Keep recording when the phone is locked"
- `PurpleRecordingRoundFloat` "Shrink video messages while recording"
- `PurpleRecordingRoundFloatInfo` "Tap around the camera to keep recording in a small bubble."
- `PurpleRecordingJournal` "Video message crash safety"
- `PurpleRecordingJournalOff` "Off"
- `PurpleRecordingJournalEvery` "Save the index every %1$s"
- `PurpleRecordingJournalInfo` "If the app closes unexpectedly, a video message can be recovered exactly up to the last save. Without it, Purple tries to rebuild it, which can drift slightly."
- `PurpleRecordingBin` "Recording bin"
- `PurpleRecordingBinKeep` "Keep recordings for"
- `PurpleRecordingBinEmpty` "No unsent recordings"
- `PurpleRecordingBinRecovered` "Recovered after a crash"
- `PurpleRecordingBinPartial` "Partly recovered"
- `PurpleRecordingBinNotSent` "Not sent"
- `PurpleRecordingBinUnknownChat` "Unknown chat"
- `PurpleRecordingSendElsewhere` "Send to another chat"
- `PurpleRecordingCallPause` "Pause recording during calls"
- `PurpleRecordingPausedForCall` "Paused for a call"
- `PurpleRecordingHeadset` "Headset button controls recording"
- `PurpleRecordingHeadsetInfo` "Press once to pause or resume, twice to send."
- `PurpleRecordingHeadsetHold` "Hold the headset button to start recording"
- `PurpleRecordingNudge` "Remind me during long recordings"
- `PurpleRecordingStill` "Still recording · %1$s"
- `PurpleRecordingPreviewTrim` "Preview and trim from the recording bar"
- `PurpleRecordingNotificationChannel` "Recording"
- `PurpleRecordingDiscardAgain` "Tap again to discard"
- `PurpleRecordingAllowNotifications` "Allow notifications to control recordings from the lock screen"
- `PurpleSeekStep` "Seek step"
- `PurpleSeekBack` "Back %1$s s" and `PurpleSeekForward` "Forward %1$s s" (accessibility)

## Tests

- **purple-core `tests/test_config.cpp`.** The `[recording]` table and `seek_step`:
  - a missing table gives every default;
  - each boolean on and off;
  - `round_journal` "off", "1s", "2s", "5s", and "3s" or "abc" warn and keep "1s";
  - `seek_step` with the same choice rule;
  - `bin_keep` "12h", "7d" (with the `d` suffix), "off", and below 1 h warns;
  - a table that is not a table warns;
  - the scope is AppWide and the diff label is "Recording".
- **purple-core `tests/test_recording.cpp`, new.** The nudge schedule, bin expiry at the boundary, and seek clamping
  at 0 and at the end.
- **purple-core `tests/test_ogg_repair.cpp`, new.** Synthetic Ogg streams: a clean file is unchanged; a truncated
  page is dropped; a missing EOS is set with the right CRC; a corrupt middle page stops the walk there; a duration
  comes from granule positions.
- **purple-core `tests/test_mp4_repair.cpp`, new.** Synthetic `MP4Builder`-shaped streams from a test writer: fake
  NAL samples with length prefixes, fake audio frames whose lengths a stub measure callback knows, split and unsplit
  `mdat`, truncation at every byte of the last three samples, an audio frame whose first bytes look like a valid
  length (the lookahead must reject it), VFR video (15 and 30 fps) anchored to the audio clock within one audio
  frame, and journal-plus-tail continuation.
- **Android host suite `recording`.** `TMessagesProj/jni/purple_jni/tests/recording/` with a Python driver, and its
  entry in `purple/host_suites.json` with `expects` floors in the same commit (AGENTS.md):
  - `PurpleRecordingDock`'s state machine: dock and undock, guid filtering of record events, keep-on-pause
    decisions for covered versus background, the Work Mode name hiding, and refusal of a second recording;
  - `PurpleRoundJournal`: header and batch encoding, a CRC failure stops at the bad batch, samples past the end of
    the file are dropped, and the interval and Off settings (Off writes the header only);
  - `PurpleRoundRecovery` against the real `MP4Builder`, `Mp4Movie` and `Track` classes from the compile receipt:
    record a synthetic stream through `MP4Builder` with the journal, truncate it, recover it, and compare the rebuilt
    `moov` sample tables with what `finishMovie` produces for the same samples;
  - the bin manifest: atomic writes, crash states becoming recovered, expiry and the orphan scan patterns;
  - headset press classification and feedback choice;
  - `PurpleCrashCapture`: frame scrubbing (non-identifier characters, foreign packages become `other`, lambda
    suffixes dropped, the line limit), tombstone protobuf parsing from a synthetic tombstone, ANR text parsing,
    pending-file merge, and the repeat limit.
- **Android `host-library` cases.** The settings bridge returns the packed values, and the AAC measure shim against
  a short AAC stream encoded at test time.
- **Desktop `purple/tests/recording_cases.cpp`.** Through `purple/test_cases.py`, registered in
  `purple/host_gates.json`: the dock model, the WriteData mirror (seek, rewrite, close), fragmented-copy remux on a
  generated stream, the seek step, and the shortcut command names.
- **Hook pins.** Android: `purple/upstream-hooks.sh check` and the ratchet. Desktop: a case reads the hooked
  upstream files and checks that each named function holds its one `Purple::` call.
- **Device and GUI checks after build** (outside this design's scope):
  - the F6 spike;
  - a locked-phone voice recording of 10 minutes;
  - headset single and double press on a wired and a Bluetooth headset;
  - a forced crash during voice and round recording with the journal at 1 s and Off;
  - an ANR and a native crash appearing in the exported diag log;
  - Desktop adoption on chat switch, and the strip in narrow and wide layouts.

## Atomic commit groups

purple-core, first:

1. "Read the recording table and the seek step", with the `d` duration suffix if question 6 is yes, and their tests.
2. "Finalise an interrupted Ogg recording", with its tests.
3. "Rebuild a round video's index from its media data", the repair scanner with its tests.
4. "Decide recording nudges and bin expiry", with its tests.

Android:

1. Bump purple-core, add the settings bridge and the AAC measure shim (`purple.cmake` `target_sources`), with
   host-library cases.
2. The dock model and the strip: `PurpleRecordingDock`, `PurpleRecordingStrip`, the `FragmentContextView` hooks, and
   the `ChatActivityEnterView` notification guard. The suite and the ratchet.
3. F8: the `checkRecordLocked` and `stopRaiseToEarSensors` hooks, the record panel hook and the extension point.
4. F7: `PurpleRecordingService`, the manifest line, and the notification.
5. Extras 3, 4 and 5: calls, headset and nudge.
6. Extras 1 and 6: send elsewhere and preview through the voice draft.
7. F10 voice: the registry, file discovery, Ogg recovery and the bin screen with auto-clear and the orphan scan.
8. F10 round: the `Mp4Movie` and registration hooks, the journal, recovery, the repair binding, and the journal
   setting row.
9. F6 fallback: the touch hooks in both camera views, `PurpleRoundBubble`, and pause on background.
10. F9: the strip buttons (the `updateStyle` hook) and the media session hooks.
11. F11: `PurpleCrashCapture`, the `ApplicationLoader` hook and the new diag factories.
12. The settings screens and strings, and the README "Media recording" section (it may also land with each group).
13. After the spike, if it passes: the two surface hooks and cross-chat bubbles.

Desktop:

1. Bump purple-core.
2. The dock and the strip: `MainWidget` hooks and the dock model.
3. F8: the adoption hooks (`preventsClose` twice) and the bar decoration.
4. F10 voice: the WriteData mirror, the stop hook, recovery and the bin box.
5. F10 round: the two recorder hooks, the fragmented copy, mirroring when Off, and repair.
6. F6: the in-chat float.
7. F9: the player bar buttons, the float buttons, the shortcuts and system media controls.
8. Extras 1, 3, 4, 5 and 6, including the Desktop preview popup.
9. Settings rows, strings and `docs/purple/media_recording.md`.

## Effort

Agent-days including host tests, excluding build, device and GUI time.

- **purple-core:** settings and rules 1, Ogg finaliser 0.5, MP4 repair scanner 2. Total 3.5.
- **Android:**
  - dock model 2, strip 1.5, F8 hooks 1;
  - F7 service and notification 1.5;
  - extras: headset 1, calls and nudge 0.5, preview and send elsewhere 1;
  - F6: spike 1.5, in-chat bubble 2;
  - F9 1;
  - F10: journal and recovery 2, repair binding 1, bin, registry and orphan scan 2.5;
  - F11 1.5;
  - settings screens 1.

  Total 21, plus about 3 for cross-chat bubbles if the spike passes.
- **Desktop:**
  - dock and adoption 2, strip in `MainWidget` 1.5;
  - F10: voice mirror and recovery 1.5, round copy and repair 1.5, bin box 1.5;
  - F6 in-chat float 1.5;
  - F9 1.5;
  - extras: media keys, calls and nudge 0.5, preview popup 1.5, send elsewhere 0.5;
  - settings 0.5;
  - encryption at rest 1 (if question 4 is yes).

  Total 15.
- **Overall:** about 40 agent-days, plus one device pass on Android and one GUI pass on Desktop.

## Risks

- **`FragmentContextView` is a busy upstream file** with a private state machine. The widened condition rides its
  show path; an upstream rewrite of `checkPlayer` would need the hook re-thought. Mitigation: the hook pin test, and
  the README note that names the dependency.
- **Media button routing on Android** depends on the session having played audio last. The feedback tone handles
  it, but some vendors route differently. Device check on at least two vendors.
- **Repair timing** is approximate by nature (30-100 ms A/V offset). The journal is the default for that reason.
- **The FFmpeg AAC decoder's consumed-bytes behaviour** for raw frames is assumed; a host case confirms it before
  the scanner relies on it.
- **Two recorders of each kind on Android.** Upstream may remove the legacy ones; the hooks in them would then go
  with them, and the V2 hooks remain.
- **Background microphone** policies tighten in new Android versions. The service starts only from the foreground,
  which is what current rules require.
- **Desktop at-rest exposure.** Desktop's cache is encrypted with the local key; plain recording files in `tdata`
  would be a regression without question 4.

## Open questions (6)

1. **Do discarded recordings go to the bin?** A discard is often accidental (a slip off the lock), and the bin could
   undo it. But a user who discards expects deletion.
   - Option A (recommended): yes, as "Discarded" items, cleared by the same `bin_keep` timer, with a setting
     `keep_discarded_p` (default on) so a privacy-minded user can turn it off.
   - Option B: no. Discard deletes at once, and the bin holds only crashes and unsent recordings.
2. **What does the journal setting's Off mean?** The owner's "do both" needs the config header for repair even when
   the journal is Off.
   - Option A (recommended): Off writes no index, but still writes the few-hundred-byte config header once at start,
     so repair works.
   - Option B: Off writes nothing at all. A crash then loses the round as upstream does, and repair can only try
     defaults (usually failing).
3. **F11 native frames: log the relative pc?** Without it, frames in the stripped app libraries carry no function
   names and cannot be symbolised. A relative pc is a code offset, not personal data, but it is a hex number, which
   the diag log otherwise avoids.
   - Option A (recommended): yes, for the app's own libraries only, with the library's build id once per crash.
   - Option B: library and function names only.
4. **Desktop recordings at rest.** Telegram Desktop encrypts its cache with the account's local key. A plain `.ogg`
   or `.mp4` in `tdata/purple/recordings` would sit outside that.
   - Option A (recommended): encrypt mirrors, journals and bin files with AES-CTR keyed from the local key (CTR
     allows writing at offsets). It costs about 1 agent-day and reuses the archive's crypto helpers.
   - Option B: plain files, like Android's media cache.
5. **The dock on Android screens without a context strip** (Settings, profiles, some sheets).
   - Option A (recommended): the notification is enough there, and the strip appears again on the next chat or
     chat list.
   - Option B: also add a small floating pill on those screens through an activity-level window, like the round
     bubble. It is more UI and more code, with no upstream hook.
6. **Add a `d` (days) suffix to purple-core's `ParseDuration`?** It is additive and every duration key gains it.
   - Option A (recommended): yes, so `bin_keep = "7d"` reads naturally.
   - Option B: no, and the 7-day choice is written `"168h"`.
