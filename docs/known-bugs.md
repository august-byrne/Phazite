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

- **209. A plug-in named as one of Phazite's devices shares that device's presets** (2026-11-19, found as the browser
  put presets under their devices): a presets folder is named by the device's name (`getPresetFolderName()`), so a
  plug-in called "Reverb", "EQ", "Delay" or "Filter" opens, in the browser and in Swap, to Phazite's presets, which it
  can't load, and Phazite's device lists the plug-in's. To fix: plug-ins' presets in folders of their own, such as
  `Plug-ins/<name>`, in `getPresetFolderName()` and `BrowserModel::getShelf()`, Other Presets looking inside
  `Plug-ins/` for plug-ins not scanned.

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
- **199. A merge can make two different items one where both lines hold another item under an ID a merge gave away.**
  Two lines count their IDs on from where they parted, so each makes items with the same IDs; a merge gives what it
  brings in a new ID where this line holds another item under it, and records that (RENUMBERED). A later merge of the
  same line again is right. These are not:
  - **A merge back** (one computer is enough): main merges idea-2, whose Pad (ID c) becomes a because main's Lead holds
    c. idea-2 goes on, edits Pad, and merges main: main's records say the item at a is idea-2's c, but idea-2 holds no
    record for it, and main's Lead at c meets idea-2's Pad at c: Pad and Lead become one item (Lead's name, devices and
    clips on it, or clashes where idea-2 edited them), and Pad comes in again at a.
  - **Two computers that each merged the same version**, then one merges the other's line: right since S4.5 where both
    lines' own items collide the same way; wrong where only the other line's merge moved an item (a device on the merged
    track that only one computer's own track also has; a track one computer's merge kept under its ID): the other line's
    own item keeps the old ID and takes the merged item's identity, so that what named the one (sends, sidechains,
    modulations, automation) names the other, edits land on the wrong item, and where this line edited the item its
    untouched clips can go as "taken away" by the other side. Cancel Merge, Undo and the versions bring it back.
  - **A note in a clip that was itself given a new ID:** a note's translation is looked up under the other line's clip,
    so it never applies, and the other line's own note takes the merged note's place while that comes in again.
  Why: `crossRenumbering()` (`src/model/versions/VersionHistory.cpp`) gives the other line's own item a new ID only where
  this line has a record for that ID, and refuses a translation onto an ID the other song holds; `translateIDs()`
  (`SongMerge.cpp`) keys notes by the clip before the items' translation. To fix, one rule: for each of the other line's
  latest records "from becomes to" where its song holds `to` and the parting holds `from`: if its song also holds
  `from` (its own item), that item gets a new ID first, whether or not this line has a record for `from`; then `to` is
  translated to this line's ID for `from`, or to `from` itself, which is free then; notes keyed by the clip after the
  items' translation. With tests for the merge back, the device on one side only, the track kept under its ID, and
  the note in a renumbered clip, each checked with `checkSong` and for every item being there once with its own
  content. Found by the three reviews of S4.5 (2026-11-18); the merge back is older than stage 4.

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
- **198. A sound file replaced on disk at the same path plays as first read** (found in S4.5's review, 2026-11-17; older than
  it): a plan keeps a track's audio while its clips don't change, without asking the disk again, so a file replaced at
  the same path (a DAWproject import unpacking over files the song plays; another program writing a file in `Audio/`
  again) plays as it was first read until that track's clips change, and the plan's check of its audio asserts in a
  debug build when a plan is compiled in between (`SongPlayer.cpp`, `checkAudio`). To fix: whatever replaces a played
  file tells the player (a change marked for the tracks that play it), or kept files are looked at once a plan and the
  track's audio made again when one changed.

## Tests

- **205. `VersionsTests`, the plug-in update's case, failed once in CI's linux-clang job** (2026-11-18, the run of
  `4ff18a92`): after the project opened again, its plug-ins loaded and their settings stored as a save stores them,
  the changes didn't name the update ("updated 1.0 → 2.0", its last check). `39633f06` made that case wait for the
  plug-in after the project opens again, for a failure of the same check in a release build; this is that failure
  again, on another compiler, so the wait isn't the whole of it. To fix: find what `describeChanges()` reads of the
  plug-in's stored settings there and whether `saveWithPluginStates()` can return before the reopened plug-in's
  answer is in the song (the test should print the stored version when the check fails).
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
