# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

The entries keep the numbers they were given, so that the changelog, the tests and the commits that name them stay
right: 19 to 43 were seen while the quality audit's priorities were fixed, 44 to 133 came from the audit itself, 134
to 170 from the tests of stage 2 and the rest of stage 1, those from 171 to 190 were found while the known bugs were
fixed (2026-11-15 and 16), and those from 191 on since. Each fix has a line in the [changelog](changelog.md), under
"The known bugs". A round on 2026-11-19 fixed what could be fixed without the owner (186, 198, 199, 200, 204 to 210)
and entered what it and its reviews found (211 to 214). What stays, each entry saying why: 136 is the written reason of a
test's exception; 137 and 197 wait for the owner's answer; 185, 191 and 213 can only be built and tried on Linux;
196 turns on the deleting of what the versions hold, which needs a decision and a review of its own first; 211
and 212 are new; and 214 is a fault of the compiler on CI, which waits for the owner's choice of what to do about it.

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

- **212. A plug-in whose load ended without it, for another reason than not being installed, shows as missing**
  (2026-11-19, found while 191 was looked into). Where the plug-in host didn't start, the load took too long, or the
  host stopped as it started, the device's status is `missing` (`finishOpening()`, `src/model/plugins/PluginInstances.cpp`):
  the project's notes list it among the plug-ins not installed on this computer, though it is; its slot looks as
  any device's and is silent; Reload refuses it; and nothing asks for the load again until the project opens again
  (the retry after a scan takes only plug-ins that weren't known). The reason is kept now
  (`PluginInstances::getError()`), and nothing shows it. To fix: a status of its own, shown in the slot with its
  reason, which a click reloads as it does a crashed plug-in, and which `getPluginsMissing()` leaves out.

## Versions

- **196. The versions' store lets go of nothing yet: a project's `Versions` folder only grows.** An undone version, a
  deleted branch's versions and the song left on it, the snapshots Local History no longer lists, and their bounces and
  edited messages keep their room for ever. Recover Lost Versions stops offering them after 30 days, and the room the
  Project view shows never falls but by Delete Bounce. The weekly sweep runs (it walks what is named, says when it can't
  be sure, and joins this computer's packs by size, so their count stays small), but its letting go is off in the
  product (`sweepLetsGo` in `src/model/versions/internal/StoreSweep.h`; the tests turn it on with `SweepLetsGo`). Why:
  three reviews of S4.4 each found a way for the letting go to lose work that two computers sharing a folder need, and
  the copying that should stop it isn't sound yet:
  - a song recovered from another computer's log, named only in this computer's records, was never copied into this
    computer's packs, so the other computer's sweep could take it (the copy now takes the songs the branches name and
    the merges whole songs hold, but nothing proves the list of what a computer names is complete);
  - two computers: before either may delete a piece of its own, every version and song the other's history names must
    be in the other's own packs, and that must hold for every command that starts naming one, on both computers,
    whatever has arrived through the sync;
  - one computer id on two machines (a settings folder carried by a roaming profile or a restored backup) makes the two
    one writer, so one's sweep deletes what the other needs; binding the id to the machine gave every existing install
    a new id at its first run (and again after a CPU swap on Windows, or a BIOS update on Linux), which hides the songs
    left on branches and Local History filed under the old one;
  - the first copy is the size of a whole history: it must never make a save or Save As wait behind it on the message
    thread;
  - a clock once set ahead keeps this computer's stamps ahead until real time passes them (the stamps' floor is never
    lowered, so that a clock set back can't make new work look old): new versions show as saved "now" for as long, and
    Local History isn't thinned meanwhile; the letting go, which goes by the stamps, must decide how far it trusts a
    stamp ahead of the clock (a version's shown time is its stamp);
  - what a project's file, its autosave and its backups name as the version the song grew from
    (`findVersionsNamedByProject()`) is not yet among what the sweep's walk takes as named.

  To fix, the plan: let go only in a history that one computer alone has ever written to (every pack's writer, every
  branch record, log and `local/` folder is this computer's), which takes the two-computer cases out by construction
  and covers most projects; a history two computers share is not swept until collaboration is designed. That still
  needs the list of what a computer's records name taken from one place that the walk uses, with a test for each
  command that starts naming a version or a song; the rule for stamps ahead of the clock; what the project's file
  names among the roots; and its own review. Found by the reviews of S4.3 and S4.4 (2026-11-17).
- **197. A history written by a build before S4.4 is not read.** The packs' layout changed (an entry holds when its piece
  was written, a pack who wrote it, and a pack's name takes its writer in): a project whose `Versions` folder an earlier
  build wrote lists its branches, but their versions and its Local History can't be read, and Clean Up Unused Audio
  offers nothing while such a pack is in the folder. Nothing of the old history is deleted or written over, and the
  song opens and saves versions as ever. Phazite keeps no backward compatibility yet, so this is as decided; it is
  entered so that it is not a surprise. To fix, if the owner wants the old histories: a reader of the old layout that
  writes each old pack again in the new one, once, as a history opens.
- **211. A merge can make two items one where the other line merged a later version of a line than the one this line
  grew from** (found by the reviews of known bug 199's fix, 2026-11-19; older than it). idea-2 keeps P (Pad at n), then
  P' (Rhodes added at n+1). C, counting from the same place, made Keys at n and merges P': its merge gives Pad a new
  ID (recorded, n to n+2). D was made from P, adds Bass at n+1 and merges C; the parting is P. C's record is never
  read, so C's Keys at n is taken for the parting's Pad (Keys and Pad become one item, with clashes where D edited
  Pad) and C's Pad comes in again at n+2. Both songs hold each ID once, so the check that refuses a merge which would
  put two items under one ID (199's fix) doesn't see it. Likewise where merges crossed and two versions could be the
  parting: the newest is chosen, and the records of merges that only the other one follows go unread. Why:
  `findRenumbering()` (`src/model/versions/VersionHistory.cpp`) keeps a merge's records only if the other head
  follows the merged-in version; a merge of a version after the parting on a third line is dropped, though its
  `from` IDs are the parting's wherever the item was in the parting. To fix: for the other line's records only
  (`theirMerges`, `theirMergesAtParting`), also keep a merge whose merged-in version follows the parting; a `from`
  the parting doesn't hold (an item added after it) crosses to `from`, and the merge's own renumbering of what it
  adds deals with a collision. With tests: the P and P' history through the model, the same with a note in a clip P'
  added, and a criss-cross with two candidate partings. It changes which records every merge reads, so it needs its
  own review. Also then: the merge's check that each ID is held once (`holdsEachIDOnce()` in `VersionHistory.cpp`)
  and the loader's (`SongPieces.cpp`) made one function, which looks at a channel's fader and VCA IDs and skips
  elements of kinds it doesn't know, as the loader's does.
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

- **191. `SandboxedPluginTests`: under AddressSanitizer on CI, a plug-in's load now and then ends without the
  plug-in** (2026-11-17, twice on `main`'s runs; 2026-11-19 on `b3d34c37`, in "A save asks slow plug-ins for their
  states at once...": after 20.5 s, "no parameter 60"). The plug-in is no longer loading and has no parameters, so
  the next thing the test asks of it fails. Not the first lead (a load that meets a helper on its way out): there is
  one helper for each plug-in file, and the helpers crashed or ended by the cases before were other files'; that a
  load could go to a helper that had crashed a moment before was true, and is fixed, but none of the three failures
  was that. What fits: the 20.5 s is the helper's limit to start (`startMilliseconds`, `src/model/plugins/PluginHost.cpp`,
  kept by `Helper.cpp`'s watching thread) and no other limit. The likely cause, not proved (it needs Linux and the
  sanitizer): JUCE's `ChildProcess` allocates between `fork()` and `exec` (`juce_SharedCode_posix.h`, the `argv`
  array), and under GCC 13's AddressSanitizer a child forked while another thread held the allocator's lock waits
  on it for ever, never becomes the helper, and is ended at the limit. Since the fix round a failed test says the
  plug-in's status and why its load ended (`PluginInstances::getError()`, `TestSong::describePlugin()`): "Phazite's
  plug-in host didn't start." after about 20 s confirms the start. To tell a child that never became the helper
  from a helper slow to start: at a start that timed out on Linux, read `/proc/<child>/exe` (still the program
  that started it: it never did `exec`). To fix then: start the helper on Linux with `posix_spawn`, which allocates
  nothing in the child, as a JUCE workaround.
- **213. Linux: a plug-in asked for just after its helper's process was ended can still be kept off as one that
  crashed as it loaded** (2026-11-19, CI's Clang job on `e5a255c3`, the first run of the test of known bug 191's
  fix there: `SandboxedPluginTests`, "A plug-in asked for just after its helper crashed opens in a fresh helper...",
  round 8 of 24, after eight rounds that passed). The test ends a helper with SIGKILL, waits until `/proc` shows it a
  zombie or gone, and asks for the plug-in again: the answer was "It crashed as it loaded, and stays off until it is
  reloaded." On Windows the same test passes every round, and it isn't run on Linux until this is found. Either
  `Helper::hasProcessEnded()` said the first helper still ran (JUCE's `ChildProcess::isRunning()` on Linux is
  `waitpid()` with `WNOHANG`, which the watching thread calls too: one of the two reaps the child, and the other
  gets no child), or the fresh helper's load failed as the first helper was taken down beside it (its pipe closed,
  its `Running` deleted on the endings' thread, a signal sent to a process number: `Helper::kill()`). To find it:
  run the test on Linux with the plug-in's status and error said at each step (`TestSong::describePlugin()` shows
  how), and which of the two helpers the failed load was sent to (`getHelper()`'s second try in
  `src/model/plugins/PluginHost.cpp`).

## The build

- **214. The compiler of CI's Windows jobs (MSVC 19.52) gets some sums of beats wrong in optimized builds**
  (2026-11-19, found through known bug 204; MSVC 19.51 on the owner's PC, GCC and Clang get them right). Seen, each in
  a CI run: `BeatRange (std::max (a.getStart(), b.getStart()), std::min (a.getEnd(), b.getEnd()))` ending at its
  start (windows-release and windows-profile: Consolidate over part of a clip lost its notes, 204); a test of such
  sums crashing (windows-release); and, in a probe of single sums (windows-profile), `std::max (std::max (a, b),
  std::min (c, d))` of four named `BeatPosition`s giving 0, and a `std::min` of two kept in a local giving 0: no
  temporary in either, so it isn't the code's fault. The same sums with a plain class of one double, with doubles
  and with strings came out right, and so did every one of them in the next run, after more probes were added to
  the same file: which sum goes wrong depends on what is compiled around it. So the installer CI builds
  (windows-release) may hold such a sum anywhere `BeatPosition`s or `BeatDuration`s are compared through `std::min`
  and `std::max`, and the tests show only those a test happens to reach. What was done: the six places that made a
  range of `std::min` and `std::max` of getters ask the range for its union or intersection, and
  `tests/CompilerProbeTests.cpp` runs the sums one at a time, guarded, and says what it finds as a warning (ctest
  shows a test's output only when it fails: to read it, make its last line a `CHECK` on a branch of its own). To
  settle it, one of: CI's Windows jobs pinned to the MSVC the owner's PC has, if the runner's image holds it
  (`cmake/toolchains/windows-msvc.cmake` takes the newest); the fault reported to Microsoft with a case cut from the
  probe; or `BeatPosition` and `BeatDuration` given `min` and `max` of their own that compare the doubles, and
  every `std::min` and `std::max` of them in `src/` moved to those, with a count in `tools/check-architecture.sh`.
  Until then a release for others should be built with a compiler the tests passed on without this entry's cases.
