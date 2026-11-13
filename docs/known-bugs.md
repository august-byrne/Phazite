# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

## 18. A bus that is a sidechain's key goes silent while another track is soloed

- **How it shows:** a sub on its own track ducks under the Drums folder, mixed as a bus (its compressor's Sidechain set to
  Drums). Soloing the sub stops the ducking: the compressor hears nothing. With the kick's own track as the key, the soloed
  sub ducks as before. The owner saw it on 2026-11-13: the sub's EQ showed other frequencies under each kick in the whole
  song (the ducking's, which are real), and none soloed.
- **Why:** a track soloed out plays on only while a sidechain listens to it itself (`Plan::Strip::feedsSidechain`, set
  where `SongPlayer` plans a device's sidechain; `Engine::isSilent()`), and the key is its sound before its mute. A bus
  that is a key plays on, but the tracks in it are soloed out (`SongPlayer::updateAudibility()`), so they play nothing into
  it, and its sound before its mute is silence.
- **How to fix:** in `updateAudibility()`, keep a track that is only soloed out (not muted) playing while a folder it is in
  is a sidechain's key: its sound goes into that bus alone, whose own solo then silences it after the key is taken. Its
  sends to returns, which a solo never silences, would be heard meanwhile: leave them out while it plays only for a key,
  or ask the owner. The test that shows it: the bass soloed, its sidechain listening to a bus with a steady tone in it,
  hears nothing; with the tone's own track as the key, it hears it (`tests/SidechainTests.cpp`).
