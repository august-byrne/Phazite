# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread that Tracktion's code causes are in
[realtime-audit.md](realtime-audit.md) instead, and Tracktion's other quirks in [design.md](design.md), section 23.

## 4. Recording from a stop now and then skips the song's first block

- **How it shows:** recording from a stop with no count-in, the first block that the song should play (about 5 ms, at
  256 frames and 48 kHz) now and then isn't played, so what starts in it, such as the metronome's first click, isn't
  heard. What comes in is recorded, and placed, as it should be.
- **Why:** Tracktion's `TransportControl::performRecord()` posts the position that playback starts from, then the play.
  The audio thread applies a posted play at the start of a block (`NodePlaybackContext::nextBlockStarted()`), from where
  the block before began, and a posted position later in the block (`process()`), once the block's own place is set.
  When the position is posted while the audio thread is between those two points of a block, it is applied in that
  block, with the song still stopped, and the play in the next block starts a block late. Tracktion's `performPlay()`
  says as much of setting the position in the wrong block ("actually starting playback 1 block from the start"). The
  `linux-tsan` build showed it in every run of one test (2026-10-29), as its timing lined up there; on Windows, an input
  consumer slowed to take 3 ms a block showed it in 4 runs of 6.
- **How to fix:** in Tracktion, apply a posted start position with the posted play, in the same block (in
  `nextBlockStarted()`, before the play). `external/` is read-only, so report it upstream, or fork. A count-in hides it,
  as the block skipped is then silence before the count-in's first click. The recording tests start between beats until
  then (`tests/AudioRecordingTests.cpp`).

## 5. A device switched off, or on a muted track, plays the rest of its sound when it plays again

- **How it shows:** switch off a device while it sounds (a drum sampler's hit, a reverb's or a delay's tail), or mute
  its track, and switch it on or unmute the track later: the rest of that sound plays then, from where it stopped, as if
  no time had passed. Found while fixing the drum pads' clicks that waited and played late (2026-10-30, 8ecc97c).
- **Why:** Tracktion's `PluginNode::process()` doesn't process a switched-off plug-in (unless it delays its sound and
  plays it bypassed), nor the plug-ins of a muted track or one another track's solo silences
  (`TrackMuteState::shouldTrackContentsBeProcessed()`, unless the track feeds a sidechain). `Plugin::setEnabled()`
  changes the switch alone, and nothing resets the device as it plays again, so its voices and delay lines hold what
  they held, frozen, and go on from there.
- **How to fix:** reset a device (`devices::Processor::reset()` for Phazite's own, `te::Plugin::reset()` for a plug-in)
  at the first block it plays after blocks it didn't: in `DevicePlugin`, the message thread sets an atomic flag as the
  device's switch turns on, or its track is unmuted or no longer silenced by a solo (both the song's, in the model), and
  `applyToBuffer()` resets before it plays when the flag is set. A test switches a device off mid-sound and on again
  after its sound would have ended, and hears silence.
