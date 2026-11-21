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
196 is decided (2026-11-20: nothing is deleted by itself, and Free Up Space, roadmap step S4.4, frees the room by hand), and not built; 211
and 212 are new; and 214 is a fault of the compiler on CI, which waits for the owner's choice of what to do about it.
215 to 217 are the settings' card's, from its review (2026-11-20). 218 to 224 are what the audit of the threads and the
message thread (2026-11-20) and the reviews of its fixes left: the audit's other findings were fixed at once
([changelog](changelog.md)); these were found by reading the code and none was measured, and what the audit only
suspects, or would make faster, is in the [backlog](backlog.md). 230 was found by reading JUCE while
roadmap step S5.2 was reviewed (2026-11-20), and waits to be seen with a plug-in that shows it. 235 was found by reading while
the clips' writers were reviewed (2026-11-21), as 237 was, and 236 is a test that failed once on CI that day.

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
- **230. A controller, the pitch bend or the channel's pressure may move the first parameter of a VST3 that has no
  MIDI mapping** (2026-11-20, found by reading JUCE while roadmap step S5.2 was reviewed; not reproduced, since both
  of the tests' VST3s are built with JUCE, which always has the mapping). Played live or from a clip into such a
  plug-in, the mod wheel, the pedal or the bend would set the parameter whose VST3 ID is 0 (often the plug-in's first)
  to the controller's value, and Phazite's copy of it. Why: JUCE's VST3 host looks each controller up in a table of the
  plug-in's MIDI mapping (`StoredMidiMapping`, `juce_VST3Common.h`), which starts with every entry at parameter ID 0
  and is filled only for a plug-in that has the mapping's interface (`updateMidiMappings()`,
  `juce_VST3PluginFormatImpl.h`); for one without it, "no parameter" is never stored, so every controller is sent on as
  a change of parameter 0. It was so before S5.2, for the controllers and the bend that already reached plug-ins. To
  fix: first see it, with a VST3 built without the interface (not with JUCE) among the plug-ins of roadmap step S3.4;
  then the helper leaves controllers, the bend and the channel's pressure out of what it hands a plug-in that has no
  mapping (the helper can ask the plug-in's own interfaces through `juce::ExtensionsVisitor`), as JUCE workaround, and
  the fault reported to JUCE.

- **218. Slow plug-ins make the whole song late: each has a block's length from its own hand-over.** Two plug-in
  effects in series that each take most of a block, at a small buffer, overrun the device's callback, and everything
  crackles, not those two alone. Why: `SandboxedProcessor::collect()` waits for a block's own length (a millisecond at
  least) from when that plug-in was handed its block, not from when the callback began; plug-ins in series, one handed
  its block late in the callback, and each stretch of a block cut at the loop's end get that time each. One that hangs
  costs one such wait and is skipped after it, so "never the song's" ([realtime-audit.md](realtime-audit.md), finding
  9) holds for a hang, not for slowness. To fix: the callback's own deadline (its start and its length, less a margin
  for the rest of the song) handed down in `devices::ProcessContext`, each plug-in waited for until the earlier of the
  two. It trades a late song for plug-ins that fall silent when what comes before them is slow, as a plug-in that runs
  inside a DAW never does: the owner's to decide.
- **220. States given to several instances in one helper each have a minute from when they were sent, though the helper
  sets them one after another.** A change that gives new states to four instances of a slow sampler at once (an undo,
  a version opened), each taking 20 seconds to set, has the helper ended about 70 seconds in, on its third, and every
  instance of that plug-in's file shows as crashed. Why: a state's request holds the heartbeat's hang off for
  `SandboxedInstance`'s setting limit (60 s) counted from its send (`Helper::Reply::stopHoldingOffHangAfter()`), and
  the helper's message thread takes the requests in turn. To fix: a reply's time starts when the reply before it in
  that helper is answered, or the limit is counted from the helper's last answer.
- **221. A plug-in prepared for live playback while a render plays it at another rate is prepared at the live rate for
  the rest of the render.** An export at 96 kHz of a song that plays at 48 kHz, during which the live plan is made
  again (a plug-in changes its latency as it is switched to rendering): the plug-in plays the rest of the export
  prepared for 48 kHz, at the wrong pitch and speed. Why: `SandboxedInstance::preparedRate` is one value for both, and
  the render's next `finishPreparing()` prepares the channel for whatever it holds. To fix: the rate a render wants
  kept apart from the live one, the live prepare waiting until the render gives the plug-in back. Found by the fix of
  the message thread's wait there (2026-11-20); older than it.
- **222. A meter that a plug-in tells as a parameter from its own window's timer still counts as a value set by
  hand.** Phazite drops a change of a parameter that can't be automated when the plug-in tells it on its audio thread
  (JUCE workaround W58), which is where VST3's output parameters come from; one pushed from the plug-in's editor (its
  `performEdit` on the helper's message thread) can't be told from a knob turned there, and marks the song changed
  each time. To fix: needs the parameter's read-only flag, which JUCE's VST3 host doesn't hand on; or a parameter that
  changes many times a second with no gesture begun taken for a meter.

## Versions

- **196. Nothing frees the room of the versions' store yet: a project's `Versions` folder only grows.** Decided on
  2026-11-20 (the owner): the store deletes nothing by itself, ever; the room is freed by hand, by a command, Free Up
  Space (roadmap step S4.4), which lists what it would remove and works only in a history one computer has written.
  What follows is the state of the code, and why the sweep's own letting go stays off. An undone version, a
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

  To fix, the plan (Free Up Space, by hand): let go only in a history that one computer alone has ever written to (every pack's writer, every
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
- **237. A merge's choice for where an audio clip's part of its file starts can move where it ends, and the end's own
  clash still shows the choice made before.** Both lines moved the start and the end of one audio clip's part of its
  file, differently; the start chosen first, past the end the song holds, takes the end along (a clip's part never
  ends before it starts), and the clash on the end still shows its old choice, which chosen again is then refused.
  Choosing the end first, or the other value for the end after, works. Why: a choice on a clip's property sets the
  clip's whole value (`chooseClash()`, `src/model/versions/VersionsModel.cpp`), and a value keeps the part in order.
  Found by reading (2026-11-21); needs both clashes on one clip. To fix: a choice that would move another property
  of the clip that has a clash of its own sets both clashes' choices to what the song then holds (or to none), with a
  test in `tests/VersionsMergeWaitingTests.cpp`.

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

- **223. A clip's render is absent for a moment as a newer copy takes its place, and a plan made then plays the clip
  silent.** An export (which now makes the renders it needs on its own thread) and the background threads may make the
  same render at once; each writes a part file and moves it over the render (`renderClipAudio()`,
  `src/model/arrangement/ClipRenders.cpp`), and JUCE's `moveFileTo()` deletes the target before it moves. A compile in
  that instant finds no file. To fix: replace the file in one step (`writeWholeFile()`'s way, or a move that replaces),
  and an export that leaves to the background threads the renders they are already making.

## Notes and clips

- **235. A DAWproject import can put MIDI clips and audio clips on one track.** Audio and notes don't share a track
  in Phazite (a model function that puts a clip on a track asks `canTrackHold()`), and the import's reader
  (`src/model/project/DawProject.cpp`, where it adds a track's clips) puts whatever the file's track holds on one
  track without asking. Such a track then plays both, and the edits that ask `canTrackHold()` refuse on it what the
  import did. Found by reading, while the clips' writers were reviewed (2026-11-21); not seen with a real file. To
  fix: the import gives a track's audio clips a track of their own beside it (as the file's track is named, with
  " (audio)"), with a test in `tests/DawProjectTests.cpp` on a file whose track holds both; then the clips' own
  `addClip()` can ask `canTrackHold()` itself.

## The window

- **219. The browser opens a sound's file three or four times on the message thread at each step through its list.**
  At each arrow key the preview checks the file and opens it, a loop that plays in time opens it again and makes its
  whole stretched buffer, and the details open it for its levels and once more for what they say of it
  (`Previewer::play()` in `src/model/engine/Preview.cpp`, `SoundView::show()` in `src/ui/browser/SoundView.cpp`).
  Milliseconds on a local drive; in a OneDrive folder whose files aren't on the disk, or on a share, each step waits
  for the file. To fix: the length and the format from what the library's listing knows; the readers made on a job, as
  the clips' waveforms' are; the preview and the details started once the selection has rested a moment.
- **224. A waveform zoomed in far is drawn in steps, from its levels of 256 samples.** Where a pixel is fewer than 256
  samples (a short hit fitted in the audio clip editor, a playlist zoomed in far), a waveform shows as steps a level
  wide once its levels are read, where it showed the samples themselves. Why: a waveform lets its file go once its
  levels are read (JUCE workaround W59: it kept every clip's file open otherwise), and JUCE's thumbnail, with no reader,
  draws from the levels alone; before, it read the file's samples for what it drew, on the thread that paints
  (`CachedWindow::refillCache()`), which waited for a slow drive inside the paint. To fix: the fine levels of the
  stretch in view read on a job and kept while it shows. How fine a waveform must look close in is the owner's to say.
- **215. While the settings show, the system's own Close of the window does nothing** (the taskbar's Close and the
  window's menu on Windows, the window manager's close button on Linux), a click on its taskbar button or Win+Down
  doesn't minimize it, and its sides and bottom don't resize it: each only brings the window forward; Alt+F4 quits,
  and macOS's close button does. JUCE ignores a close request for a window that holds a modal component (`WM_CLOSE` and `WM_DELETE_WINDOW` call nothing; `SC_CLOSE` calls `inputAttemptWhenModal()`, which
  a click on another window calls too), and its resize border takes no input under a modal component. To fix: take
  the close request in the window's own message handling while a card shows (a JUCE workaround), or show the card
  without JUCE's modal state, with its own gate for the mouse and the screen readers.
- **216. While the settings show, a key a plug-in's own window passes back still plays its note or starts and stops
  the song** (`PluginKeys::keyPressed()`, `src/app/`): the plug-in's window is the helper's, outside the card's
  gate. The dialog the settings were before had the same gap. To fix: pass those keys through
  `KeyDispatcher::keyPressedOverModal()` while a card shows.
- **217. During a save that waits for plug-ins' states with the settings open, Space starts the song behind the
  card**: the hold of the window's input becomes the top modal component for the wait and lets its own keys through
  (play and stop among them). To fix: the hold's key gate asks whether a card shows, and passes only what the card's
  list passes.

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
- **236. `LockFreeTests`, the transport's race: on CI's Windows release build it once found no block held.** The case
  that races the window's reads of the playhead against the audio thread's blocks (`tests/LockFreeTests.cpp`, "the
  race held places": `CHECK (heldBlocks > 0)`) failed with 0 on 2026-11-21 (the run of 1ef4d615), and passed on
  the runs before and after and on every local run. The check says that the race really met the state it is for;
  on a runner where the two threads never overlap in it, it is 0 and nothing was raced. To fix: the race runs
  until it has held a block or a count of rounds has passed (as its asks were spread over the blocks in e53425de),
  never a fixed number of reads that a slow start can use up.

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
