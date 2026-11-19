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

- **212. A plug-in whose load ended without it, for another reason than not being installed, shows as missing**
  (2026-11-19, found while 191 was looked into). Where the plug-in host didn't start, the load took too long, or the
  host stopped as it started, the device's status is `missing` (`finishOpening()`, `src/model/plugins/PluginInstances.cpp`):
  the project's notes list it among the plug-ins not installed on this computer, though it is; its slot looks as
  any device's and is silent; Reload refuses it; and nothing asks for the load again until the project opens again
  (the retry after a scan takes only plug-ins that weren't known). The reason is kept now
  (`PluginInstances::getError()`), and nothing shows it. To fix: a status of its own, shown in the slot with its
  reason, which a click reloads as it does a crashed plug-in, and which `getPluginsMissing()` leaves out.

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
