# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

The file holds two kinds of entries, under the same headings:

- **Entries 19 to 43** are bugs seen in the code while the quality audit's 16 priorities were fixed, each by the session
  that fixed its neighbour, none yet reproduced by a test. Some are notes still to be traced: where an entry gives no
  cause or no fix, that is the first step.
- **Entries 44 to 133** are the open faults of the whole-project quality audit: its bugs, and those of its real-time,
  security and performance findings that are faults rather than wishes. The audit read the code and ran nothing, so each
  entry ends with whether its second reader confirmed it ("confirmed by a second reading", "partly confirmed") or it is
  "not yet checked": check an entry against the code, and reproduce it with a test, before fixing it. Each names the
  audit's area and finding number, by which the audit's own files hold it in full. Timings in them are estimates unless
  they say "measured".
- **Entries from 134 on** were found by the tests of the plan's stage 2, or seen while they were built; each says how.
- **Entries from 160 on** were found while the rest of stage 1 was built, by its tests and by the reviews of its
  changes (2026-11-14).
- **Entries from 171 on** were found while the known bugs were fixed (2026-11-15 on), by the fixes' tests and by the
  reviews of their changes.

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
