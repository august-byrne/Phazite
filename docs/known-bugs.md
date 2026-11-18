# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

The entries keep the numbers they were given, so that the changelog, the tests and the commits that name them stay
right: 19 to 43 were seen while the quality audit's priorities were fixed, 44 to 133 came from the audit itself, 134
to 170 from the tests of stage 2 and the rest of stage 1, and those from 171 on were found while the known bugs were
fixed (2026-11-15 and 16). That round fixed every entry but four, each of which says why it stays: one is
the written reason of a test's exception, one waits for the owner's answer, one can only be built and tried on
Linux, and one needs a file made by another program to show, and a saved value to fix. Each fix has a line in the
[changelog](changelog.md), under "The known bugs". 191, a test file that fails in some runs on CI's Linux builds, came the day after (2026-11-17), with 190, which is fixed.

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

## Versions

- **186. After a song's IDs were given anew as it opened, Restore and Merge against its earlier commits do wrong
  instead of refusing.** Only a file from another program can lead here: one whose largest ID or next ID lies within one
  session's count below 3 * 2^60 (`schema::renumberMark`) opens as it is, what the session makes gets IDs past the
  mark, and the next opening gives every ID from 2^61 up anew (the accepted edge written at `renumberMark` in
  `src/model/base/internal/Schema.h`; `tests/InconsistentProjectTests.cpp` pins it). The song itself keeps all it
  holds. But the commits and Local History snapshots made before the crossing hold the IDs as they were, below the
  mark, so the versions take them for commits of this song: a track restored from one comes back beside the track it
  became, Restore of a track the song has takes it out as one added since, and a Merge whose parting commit is one of
  them matches nothing and brings its tracks in a second time. Either also takes on that commit's count of IDs, just
  below the mark, so a later session can cross again. (Commits made after the crossing are refused with their reason,
  as they should be.) Why: nothing records that a song's IDs were given anew, so a commit from before can't be told
  from one of the song as it is. Fix: the loader writes into the song that it gave IDs anew and from which ID up (one
  saved value, with its entries in `Schema.h`, the loader's table and `tests/SongCheck.cpp`); a commit carries it
  with its song; Restore and Merge refuse a commit without it when the song has it, and a branch switch refuses such a
  commit or carries the old-to-new map through `joinSong()` so that the plug-ins' parameter values, kept by device
  ID, follow. About a day, with tests. Found by the reviews of the loader's IDs (2026-11-16); never seen, and no file
  Phazite wrote from a project of its own can reach it.

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

## Tests

- **195. A performance gate fails after other test files in one process** (seen by four agents, 2026-11-17): "Performance
  gate: a stopped song's devices work no block once their sound has ended" fails (the Drum Sampler and the FM synth
  each worked 188 blocks as the song opened) when `phazite_tests` runs it after other test files in one process (after
  `AppTests`, and in a run of the playback and mixer files), and passes alone and with its own file, so `ctest`, which
  starts each file by itself, passes. Something an earlier test leaves behind makes a stopped song's devices work as the
  song opens: a counter the gate reads that is the program's rather than the song's, or state an earlier test file
  leaves (the audio device, a setting). To fix: find the earlier case by bisecting the run's test files, then make the
  gate count its own song's blocks or the earlier test put back what it changed.
- **191. `SandboxedPluginTests`, "A plug-in opened from its project keeps the settings the project holds when it
  crashes", failed once under AddressSanitizer** (2026-11-17, `main`'s own run of `7df779c8`; the same commit's run on
  its branch passed): the stand-in effect, added and waited for until no plug-in was loading, had no Gain parameter
  (`tests/TestSong.cpp`, "no parameter 1"). So its load ended without it: given up at a time limit that a helper
  started under a sanitizer on a busy runner missed, or met by the helper that the test case before it had just
  crashed on purpose. To fix: find which limit (the helper's start, or the
  plug-in's load in it) from the plug-in's status at that point, which the test should say when it fails; then either
  the tests' helper is given longer under the sanitizers, as the tests' waits are, or the load is asked for again.
  Seen again the same day, under AddressSanitizer again (`main`'s own run of `7514292b`, whose branch run passed), in
  "A project with slow plug-ins opens at once": of its two slow plug-ins, one never reached its gate in two minutes
  and was no longer loading, so its load had ended without it. Both times the test case just before had crashed or
  ended a helper on purpose, which points at the second cause: a load that meets a helper on its way out.
