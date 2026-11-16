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

## Editing

- **20. Duplicate and Paste make fewer notes than they say** where some would land past a clip that the next clip keeps
  from growing: no note is made where it wouldn't play, but the undo step is still named for all of them ("Duplicate 6
  Notes" for 2). Fix: name the step for what was made, or say in the hint bar that some didn't fit.

## Plug-ins

- **166. What is left of the message thread waiting for plug-ins.** A plug-in added, or brought back by an undo once
  the project's plug-ins have loaded, opens while the message thread waits (`PluginInstances::open()` →
  `host.createInstance`, up to the load limit of 60 s); Reload waits for the helper to start and the plug-in to open
  (`PluginInstances::reload()`, 20 s and more); opening a plug-in's window waits for the helper to make its editor
  (`createSandboxedEditor()` → `openEditor`, up to 10 s); a parameter's text and a typed value wait up to 250 ms each
  (`Parameter::getText()`, `getValueForText()`); and on Linux and macOS closing a plug-in's window still waits up to
  2 s (the window that takes the helper's is Windows' only). Stores, saves, closes, preparing for another rate and
  closing a window on Windows wait for none since 2026-11-15. Fix: the added plug-in opens in the background as a
  project's do (shown as loading, silent meanwhile, its window opening when it is ready); the editor and the texts
  asked without waiting, shown when the answer comes.
- **181. A plug-in's value that is not a number gets in unchecked.** `jlimit` passes NaN on
  (`src/model/plugins/SandboxedInstance.cpp`, about lines 108, 110 and 295; `src/model/devices/ParameterHandle.cpp`,
  about 333; `Parameters.cpp`, about 395); what the helper sends is stored as it comes (about 844); the plug-in's
  text is read with `getFloatValue()`, which can overflow on a long exponent; and a double past a float's range is
  cast in `src/sandbox/core/PluginInfo.cpp` (about 47) and `RemoteInstance.cpp` (about 259). Fix: `std::isfinite`
  where each value comes in, and `readFloat()` (`src/model/base/Json.h`) for the texts. Seen while known bug 148
  was fixed.
- **177. Over a plug-in's window, a shortcut on the digit row doesn't match on layouts where that row types other
  characters** (AZERTY, Czech): the keys sent to plug-in windows (`PluginKeys::getWantedKeys()`) are the keymaps' keys
  as written, not matched the three ways the window's own keys are (`KeyDispatcher::findMatches()`). Today only Space
  and other keys off the digit row run over plug-ins, so nothing shows. Fix: have the helper ask with the
  dispatcher's matching, or send the digit row's forms too. Seen while known bug 112 was fixed.

## Devices and playback

- **85. A loop's wrap and a jump put their work into one block, and the message thread can wait for the disk.** The
  wrap looks through every note before the playhead on every track, starts each resampled clip's resampler from
  nothing, and a jump resets every device, in one block (`src/engine/core/Engine.cpp`,
  `src/engine/audio/AudioRegion.cpp`). And the reading thread holds a file's lock across its read, which the message
  thread takes at every new plan (`src/engine/audio/AudioFile.cpp`). Fix: an index of the notes sounding, resamplers
  started on the reading thread, and reads outside the lock. Not measured. Not yet checked (engine 13).

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

- **173. Aftertouch and channel pressure are neither heard live nor played back.** `engine::toNoteEvent()`
  (`src/engine/nodes/DeviceRunner.cpp`) drops them, `devices::NoteEvent` has no type for them, and `SongPlayer`
  ignores the CONTROL kinds `channelPressure` and `aftertouch` that a MIDI file's import already stores; so a take
  doesn't keep them either. Fix: the event types in the engine, handed to devices and plug-ins, then recorded as the
  other controllers are. Seen while known bug 93 was fixed.

## Saving

- **160. A new project whose first save fails leaves its folder behind, and the same name is refused when tried again.**
  `Project::create()` (`src/model/project/Project.cpp`) makes the project's folder and its subfolders, then saves; if
  that save fails (a full disk, a folder another program holds) it returns nothing with the error, but the folder stays,
  and `makeProjectFolder()` refuses a name whose folder exists. Found by reading (2026-11-14, tests-model 8); no test
  reaches it, as nothing a test can arrange makes that save fail once the folder could be made. Fix: delete the folder
  when the first save fails, as `saveCopyAs()` and `finishMoveTo()` do; test it with a write that fails (a seam in
  `writeWholeFile()`, or `tests/HeldFile.h` on a file the save writes, if one can be held before it is made).

- **169. A first save, seldom, says it couldn't copy a file of the project's Versions folder.** CI met it twice on
  loaded machines (2026-11-14): linux-debug in `tests/ProjectViewTests.cpp` ("couldn't copy
  Versions/branches/.../....xml into the project's new folder") and windows-debug in `tests/AppTests.cpp` (a first save
  with leftover copies beside it, whose message the test didn't keep). The first save waits for the history's thread
  before it copies (`waitForVersionsWritten()`, `src/app/ProjectSession.cpp`), and nothing else is known to write there
  meanwhile, so what gets in its way isn't known. Since then the message says why a file couldn't be copied
  (`copyFile()`, `src/model/project/Project.cpp`: not read, not written, or changed while it was copied), the removal
  of leftover copies doesn't look into the copy being made, and the AppTests case prints the save's errors. Fix: read
  the reason the next time it happens.

## Mixer and automation

- **174. In Write, a control held while Play is pressed:** the punch-in sets the value to the lane's within the held
  control's drag step, so Escape puts the value set by hand back to where it was before the drag while the pass goes
  on writing the lane's value (`src/model/playback/AutomationWriter.cpp`). Minor; seen while known bug 163 was fixed.
- **175. A take after a jump that lands exactly where the take before ended lets go of a held pedal for one tick at
  the seam.** The take before is placed before the next exists, so it writes its pedal-up at its end; between the
  parts of one take (a loop's wrap) nothing is let go of. Rare. Fix: place the takes of one recording together.
  Left while known bug 93 was fixed.
- **176. A take that ends while a control is held (Stop pressed, or a write that fails, in the middle of a drag)
  vanishes from the playlist until the mouse goes up,** when its clip is placed: the clip shown while recording needs
  the engine's recording, which the take lets go of as it ends (`getInProgress()`,
  `src/model/playback/PlaybackSession.cpp`). Cosmetic. Left while known bug 163 was fixed.

- **164. A project that already holds a device on a drum sampler's track listening to one of its own pads' tracks**
  isn't put right as it opens: the model refuses the choice now (`isFedBy()`, `src/model/mixer/Sidechain.cpp`), but
  `makeConsistent()` doesn't clear one a file brings. Fix: clear a sidechain that the model would refuse, with the
  case in `tests/InconsistentProjectTests.cpp`. Seen while known bug 101 was fixed.

## Files from others, and privacy

- **106. A DAWproject written with 4 GB of audio or more is damaged,** and the export says it worked: JUCE's zip
  writer keeps sizes in 32 bits (`src/model/project/DawProject.cpp`). Reading one is capped since 2026-11-14:
  `project.xml` and `metadata.xml` at 256 MB each, and what it unpacks at what an archive of its size can hold
  (`getUnpackLimit()`, `src/model/project/internal/DawProject.h`). Fix: refuse the export past 4 GB, or warn, until
  JUCE writes ZIP64. Not yet checked (model-project-services 12).
- **148. Casts of a number that is not finite to `int` remain in the model outside the file readers.** A value that is
  infinite or not a number reaching such a cast (from a plug-in's parameter, or a calculation that divides by nothing)
  is undefined behaviour in C++. The readers keep what a file says finite (`makeConsistent()`, `setTempo()`, the
  settings and the helper's messages), but nothing checks each cast's other sources. Seen while the readers were
  fuzzed. Fix:
  find the casts (`(int)` and `static_cast<int>` of a float or a double in `src/model/`) and make each safe, or keep its
  input finite where it comes from; UBSan in `linux-asan` reports one that runs.
- **170. A number in a project file with an exponent of ten digits or more overflows an `int` as it is read**
  ("1e12345678901" as a clip's length, say): undefined behaviour in C++, in practice a wrong exponent, after which the
  loader puts the value within its range. JUCE reads an exponent's digits into an `int` without a check
  (`CharacterFunctions::readDoubleValue()`, which `String::getDoubleValue()` and so every number read from a song's
  XML uses). JSON is guarded (`parseJson()`, JUCE workaround W38); the fuzzer hasn't made such an attribute yet. Fix:
  as the song loads, before any number is read from it, take a property whose text has an exponent that long for one
  out of its range (`makeConsistent()`, `src/model/project/Project.cpp`), the same for the versions' pieces and a
  DAWproject's attributes; and have the fuzzer write such exponents (`makeExtremeNumber()`, `tests/Fuzzer.cpp`).
- **179. A project another tool wrote with booleans as "true" and "false", or an ID as "12.0", opens as changed.**
  Phazite writes "1" and "0" and whole IDs; both forms play alike, but the versions compare the text, so such a file
  equal to its last commit shows changes and takes an "As opened" snapshot (numbers with a range are put into
  Phazite's form since known bug 25's fix; `keepInRange()`, `src/model/project/Project.cpp`). Fix: as the song
  loads, write each boolean and ID property as Phazite writes it, from one table of them. Seen while known bug 25
  was fixed.

## Slow on the message thread

Unless an entry says it was measured, each is the audit's reading of the code, and needs a benchmark before and after
its fix.

- **122. Notes and clips are found by looking through the song at every call, so editing many notes is quadratic.**
  Each move of the mouse in a drag of notes looks through every track's children several times for each note
  (`src/model/notes/Notes.cpp`, `src/model/arrangement/Clips.cpp`, `src/model/base/Selection.cpp`); and the piano
  roll's hover and marquee read every note, and the other tracks' notes, at each mouse event
  (`src/ui/pianoroll/PianoRollNotes.cpp`). The benchmarks' clips hold 16 notes. Fix: find the clip once a call and keep
  its notes by ID; test against what the paint already keeps; benchmark a clip of 5,000 notes. Partly confirmed by a
  second reading (model-arrangement 3, ui-timeline-editors 9).
- **123. Whole audio files are read and copied on the message thread.** Importing compares a file of the same name
  byte for byte and copies it inside the undo step (`src/model/arrangement/Import.cpp`); Normalize reads the clip's
  whole part (`src/model/arrangement/AudioClips.cpp`); Quantize Audio loads up to ten minutes of audio and finds its
  hits at once (`src/model/arrangement/Transients.cpp`). A long recording stops the window for seconds. Fix: background
  jobs with their progress, the undo step made as they finish. Not yet checked (model-arrangement 12).
- **124. Every new plan reads all the automation again, asks the disk about each audio clip's file twice and makes the
  tempo map several times for each clip,** and changes of view state make a new plan too (moving the playhead no longer
  does, since 2026-11-14) (`src/model/playback/SongPlayer.cpp`,
  `src/model/arrangement/AudioClips.cpp`, `src/model/playback/PlaybackSession.cpp`). The benchmark's song has no audio
  clips. Fix: one tempo map for each compile, what is known of a file kept until it changes, no plan for view state;
  benchmark a song of 500 audio clips. Not yet checked (model-playback 10).
- **125. An export of stems compiles a whole player and plan for every stem before it starts,** on the message thread,
  and holds them all at once (`src/model/playback/OfflineRender.cpp`, `src/model/project/Export.cpp`). Fix: compile
  each stem as its turn comes. Not yet checked (model-playback 11).
- **126. Switching branches and starting a merge read whole songs on the message thread.** On the 32-track benchmark
  song a branch switch keeps the window for 1.6 s in a Debug build, about three quarters of it reading two songs from
  the store (the rest: splitting the song left on the branch, applying the new one and compiling it); the start of
  a merge for 1.4 s, 86 % of it reads (measured 2026-11-15; `Benchmark: what a branch switch and a merge spend on
  the message thread, by part`, `tests/Benchmarks.cpp`). The badge, opening a commit, a waiting merge's reads and
  the Project view's look for unused audio no longer run there. Why: both commands apply the song in the call that
  reads it (`src/model/versions/VersionsModel.cpp`), and 65 test call sites expect the result in that call. Fix: the
  switch and the merge read on the history's thread and finish when the read is in, the view showing "Switching..."
  meanwhile; the commands' callers then wait for that.
- **128. The browser's search looks through the whole index on the message thread at each key,** stops at 50,000
  files and 200 results without saying that there are more, and doesn't search folders' names
  (`src/services/Library.cpp`, `src/services/BrowserModel.cpp`). Fix: search keys made ahead, a search that runs in
  the background and can be cancelled, and a word where results are cut. Not yet checked (ui-shell-browser 6).
- **145. A plan's compile costs about 0.4 ms a track, on every edit.** With 100 busy tracks every edit, and every step
  of a drag, takes about 40 ms of the message thread, measured on 2026-11-14 ([performance.md](performance.md),
  "a song of 100 tracks"), so a drag stutters in a large song. Why: every edit compiles the whole song again
  (`src/model/playback/SongPlayer.cpp`; see also 124). Fix: compile again only what changed (roadmap step S5.7), with
  the 100-track benchmark before and after.

## Tests

- **182. The test program can crash as it exits, after every case has passed.** Seen once (2026-11-15,
  `SandboxedPluginTests`, "SEGFAULT" after "Status: SUCCESS!"): the main thread was in `ExitProcess`, past every static
  destructor, when a Windows thread-pool worker started (the graphics stack's, under JUCE's Direct2D windows) and ran
  the program's thread-local initialiser, doctest's `g_oss`, which allocated after the C runtime's lock was gone. No
  code of Phazite's was on its stack, and it didn't happen again in five runs. Fix, if it comes back: end the test
  program without tearing the runtime down (`_exit` after the last check), which gives up the check of JUCE's leak
  detector after `main` (`tests/TestMain.cpp`), or make that check before leaving `main`.

- **180. Other tests compare against the wall clock with a slack, and can fail on a loaded machine.** Found by the
  survey made as known bug 178 was fixed (2026-11-15); none is a fault of Phazite's. A beat compared within 0.001
  while both sides add the time since the last block (`tests/EnginePlaybackTests.cpp`, about line 606); a block's
  length against the longest gap, and a worker that must take a task within about 64 ms (`tests/EngineTests.cpp`,
  about lines 1146 and 2992); levels of plug-ins played with `Timing::realTime`, where a late block plays silent, and
  the stand-ins' hang limits of one and two seconds (`tests/SandboxedPluginTests.cpp`, `tests/SandboxedPlugins.h`;
  `tests/SandboxTests.cpp` missed a 7,000 ms limit by 3 ms once); timeouts of 250 ms to 4 s read from the clock (the
  Listen key in `CompressorTests`, `MultibandTests` and `ExciterTests`; the audition window in the drum sampler's
  tests; the peak hold in `ControlsTests`; the typing pause in `ProjectViewTests`; the panel hide in `TopBarTests`;
  Auto Save's tick in `ProjectTests`; the CPU meter's bound in `MixerModelTests`); `dispatchPendingMessages()` failing
  after 5 s (`tests/UiTestHelpers.cpp`); and `settle (300)` used as "the engine has the song" (`PlaybackTests`,
  `MixerModelTests`, `StatusBarTests`, `DrumSamplerEditorTests`). Fix: a clock the test drives for the timeouts, a
  condition for each wait, and blocks played for what is measured against playback, as `tests/README.md` asks.
