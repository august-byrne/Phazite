# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread that Tracktion's code causes are in
[realtime-audit.md](realtime-audit.md) instead, and Tracktion's other quirks in [design.md](design.md), section 23.

## 6. Exports, stems, freezes and bounces sound a little otherwise than the song plays

- **How it shows:** since Phazite's own engine plays the song (2026-11-02), what an export writes, its stems, and a
  track frozen or bounced in place are still Tracktion's renders: a track panned off centre is up to 3 dB louder on one
  side than it plays (Tracktion's linear pan law against Phazite's constant-power one), a bent automation line bends half
  as much, a mono clip sent before the fader reaches its return on the left alone, modulators start 0.51 s before the
  song, and a reverb starts warmed by Tracktion's pre-roll; a frozen track plays the file Tracktion rendered.
- **Why:** rendering moves to Phazite's engine in T5 ([tracktion-migration.md](tracktion-migration.md)); T2 moved
  playback and recording.
- **How to fix:** T5: render the song (`renderWithOwnEngine()`), its stems, and a track's devices up to its freeze
  point through Phazite's engine, with loudness normalization and dither of Phazite's own; then the stems' golden test
  compares them with Phazite's render of the song, and the frozen track's (`tests/GoldenMixerTests.cpp`) compares it
  with the track played live to within the fingerprints' tolerance again.
