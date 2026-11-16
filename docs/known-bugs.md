# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

The entries keep the numbers they were given, so that the changelog, the tests and the commits that name them stay
right: 19 to 43 were seen while the quality audit's priorities were fixed, 44 to 133 came from the audit itself, 134
to 170 from the tests of stage 2 and the rest of stage 1, and those from 171 on were found while the known bugs were
fixed (2026-11-15 and 16). That round fixed every entry but the three below, each of which says why it stays: one is
the written reason of a test's exception, one waits for the owner's answer, and one can only be built and tried on
Linux. Each fix has a line in the [changelog](changelog.md), under "The known bugs".

What is open and is not a bug (quality gaps, design changes, missing features, tests and structure) is in
[backlog.md](backlog.md).

## Plug-ins

- **185. Linux: a plug-in's window shows blank after JUCE destroys or remakes the native window it is in.** The
  editor's area stays empty until the window is closed and opened again. Why: X destroys every child of a destroyed
  window, and the editor hears of the change (`componentPeerChanged()`) only after JUCE has destroyed its old window,
  so the socket and the helper's window in it are gone before Phazite can take them out (JUCE workaround W52). This
  happens when a peer is remade (`setAlwaysOnTop()` on Linux, `setUsingNativeTitleBar()`, a change of style flags) or
  the window is deleted with the editor still in it; no plug-in window of Phazite's does any of these while an editor
  is in it today. Fix: Phazite notices that the helper's window has gone and asks the helper for its editor again in
  the new socket (as a reload remakes editors), with a Linux CI test that remakes the peer under an open editor; or
  JUCE tells a component before its peer is destroyed. Found by the review of known bug 165's Linux side
  (2026-11-16); never seen, and the Linux code is not yet compiled by CI.

## Devices and playback

- **136. The FM leaves a little under its held notes where a modulator sits a few hertz from a whole multiple of its
  carrier** (Fine or Detune a little off): frequency modulation then puts a sideband a few hertz above 0 Hz (0.083 of
  the notes' level at one drawn setting), which no blocker takes away without the lowest notes too. At its defaults
  the offset is gone (0.08 before, 0.0085 with its 12 dB blocker at 5 Hz). It is what FM synthesis does, heard as
  nothing; the conformance suite keeps it as an exception with an allowance of 0.10 and this reason. Not a fault to
  fix: kept here so that the exception has its entry.
- **137. A Free LFO, step sequencer or breakpoint envelope runs on while the song is stopped and isn't started again by
  Play,** so a song with one never plays live as it exports, where it starts afresh. The golden songs with one say so
  (`runsFree` in `tests/GoldenModulationTests.cpp`) and aren't compared live against their render. Whether Play starts
  Free modulators again is the owner's to decide ([open-questions.md](open-questions.md)).
