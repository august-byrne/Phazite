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

- **42. A render ends a plug-in's helper that keeps one block for 30 seconds,** though the plug-in may only be slow (six
  times the live hang limit: `renderHangFactor` in `src/model/plugins/internal/SandboxedProcessor.h`). Before 2026-11-13
  a render waited without end, which hung Phazite on a real hang. Not seen with a real plug-in. Fix, if one needs it: a
  setting, or a question to the user once the limit is reached.
- **165. Closing a plug-in's window keeps the message thread waiting about 2 seconds,** so closing a project with
  several plug-in windows open waits that long for each in turn. Why: `SandboxedEditor`'s destructor
  (`src/app/SandboxedEditor.cpp`) asks the helper to close its editor and waits for the answer (`closeMilliseconds`),
  while the helper's window, a child of Phazite's, can't be destroyed until Phazite's thread takes the messages Windows
  sends it then, which it doesn't while it waits: the wait always runs out (measured 2.0 to 2.8 s with the tests'
  stand-in, 2026-11-14, `tests/SandboxedPluginTests.cpp`). Fix: take the helper's window out of Phazite's (or give it
  `WS_EX_NOPARENTNOTIFY`) and close it without waiting, then check that a hung helper can't hang Phazite's window as it
  goes.
- **166. What is left of the message thread waiting for plug-ins** after S1.4 (opening, saving, Auto Save and closing a
  project wait for none): a store that isn't a save (Local History's snapshot, a commit, copying a track, a preset
  saved) still asks the plug-ins whose states may have changed and waits for them together, up to 10 seconds; a
  plug-in prepared for another sample rate or a longer block waits for its helper (`SandboxedInstance::prepareNow()`,
  up to 10 seconds, behind whatever else the helper does, such as opening another plug-in); and a plug-in added, or
  brought back by an undo once the project's plug-ins have loaded, opens while the message thread waits, as before
  (`src/model/plugins/PluginInstances.cpp`). Fix: the same asks without waiting (`SandboxedInstance::askState()`,
  `PluginInstances::whenStatesIn()`), and prepare and open in the background, the engine skipping the plug-in
  meanwhile.
- **167. A plug-in with latency that joins a playing song moves the other tracks' delay compensation in one step.** As
  a project's plug-ins load in the background, each comes in with a new plan and fades in over 5 ms, but the delays
  that keep the other tracks in time with it change at once (`src/model/playback/SongPlayer.cpp`,
  `src/engine/core/Engine.cpp`): a click or a skip on tracks that play meanwhile. Fix: fade the mix out and in across
  a plan whose compensation changed, as a jump does. Left by S1.4; not measured.
- **168. Windows shutting down while a close waits for a plug-in's settings never shows the save question.** The
  close gathers the plug-ins' states first, with a window after a second whose Cancel goes on without them
  (`ProjectSession::withPluginStates()`, `src/app/ProjectSession.cpp`); a session that ends during that wait ends
  Phazite before the question. The autosave holds the changes. Fix: answer the session's end by going on without the
  states at once. Left by S1.4.
- **177. Over a plug-in's window, a shortcut on the digit row doesn't match on layouts where that row types other
  characters** (AZERTY, Czech): the keys sent to plug-in windows (`PluginKeys::getWantedKeys()`) are the keymaps' keys
  as written, not matched the three ways the window's own keys are (`KeyDispatcher::findMatches()`). Today only Space
  and other keys off the digit row run over plug-ins, so nothing shows. Fix: have the helper ask with the
  dispatcher's matching, or send the digit row's forms too. Seen while known bug 112 was fixed.

## Devices and playback

- **36. A jump while playing into a part of a long audio file that isn't read yet is silent until it is read** (it fades
  in then). Play from a stop and the loop's start are read ahead; a jump needs pre-roll.
- **39. A/B's bounce file isn't read ahead at the loop's start or a new place,** and its own meters aren't guarded
  against values that aren't numbers.
- **78. A track unmuted, or no longer silenced by a solo, in the middle of a note stays silent until its next note.**
  Why: a silenced strip throws its notes away but still counts them as sounding, so nothing starts them again
  (`src/engine/core/Engine.cpp`). Fix: start held notes again as the strip is heard again; or, as planned, let mute and
  solo silence a strip's output only, its instrument playing on. Not yet checked (engine 6).
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
- **171. A device asleep on silence shows no modulation.** With nothing playing into it (an EQ on a silent track), a
  knob's ring dot and a graph's handles stay at the value set by hand while an LFO runs, and move again only when
  sound comes in. The sound is right: it is silent. Why: `DeviceRunner::begin()` (`src/engine/nodes/DeviceRunner.cpp`)
  sets the values played to those set by hand before it returns for a sleeping device, and modulation is applied only
  in the stretches a device processes, so `ParameterHandle::getValue()` equals `getBaseValue()`. Fix: on the sleeping
  path, apply the modulators' values to the values kept as played, without processing the device. Found by the test
  of drags under modulation (`tests/GraphDragTests.cpp`), which plays a synth into the EQ for that reason.
- **162. A Stop that comes within the 3 ms in which a jump fades out makes one small step in the sound.** The stop's
  fade begins at the old place, as it should, and the jump is then taken at once, without the rest of its own fade
  (`src/engine/core/Engine.cpp`, `stopTransport()`). Fix: let the jump's fade run out before the stop is applied.
  Left by the fix of the fades (S1.2); not measured.

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
- **127. The mixer makes a strip for every channel and brings all of them up to date at any change of a device,**
  those scrolled out of view too, eight times a second while a fader is dragged (`src/ui/mixer/MixerView.cpp`,
  `src/ui/mixer/ChannelStrip.cpp`). No benchmark has a mixer. Fix: only the strips whose channel changed and that are
  in view; a benchmark of 200 tracks. Not yet checked (ui-core-mixer-app 10).
- **128. The browser's search looks through the whole index on the message thread at each key,** stops at 50,000
  files and 200 results without saying that there are more, and doesn't search folders' names
  (`src/services/Library.cpp`, `src/services/BrowserModel.cpp`). Fix: search keys made ahead, a search that runs in
  the background and can be cancelled, and a word where results are cut. Not yet checked (ui-shell-browser 6).
- **144. A note added repaints the playlist's whole lane area and makes every clip component again,** and the playlist
  makes seven components for every track's header, in view or not. Why: `Playlist::songChanged()`
  (`src/ui/playlist/Playlist.cpp`) treats any change in the clips' group of changes, a note's included, as a change of
  every clip; the headers are made for the whole song. The performance gates (`tests/PerformanceGateTests.cpp`) hold
  both: the repaint as a known exception, which fails once a note repaints its clip alone, so that the exception goes
  with the fix; and the headers as at most seven components a track, to be lowered with a fix. Fix: lay out and repaint
  the clips that changed, and make headers only for the tracks in view, as the clips are.
- **145. A plan's compile costs about 0.4 ms a track, on every edit.** With 100 busy tracks every edit, and every step
  of a drag, takes about 40 ms of the message thread, measured on 2026-11-14 ([performance.md](performance.md),
  "a song of 100 tracks"), so a drag stutters in a large song. Why: every edit compiles the whole song again
  (`src/model/playback/SongPlayer.cpp`; see also 124). Fix: compile again only what changed (roadmap step S5.7), with
  the 100-track benchmark before and after.

## Tests

- **178. `tests/LiveInputTests.cpp` fails a timing check on a loaded machine** ("Every live note sounds, however long
  the message thread was busy", `isNear (samples, first, slack)` at line 362): it failed four times in runs beside
  other builds on 2026-11-15 and passed each time alone. A test waits for a condition, never for a time: its slack
  is a time. Fix: measure the notes' places against the blocks played, not against the clock.

- **153. In a Debug build an assertion that fires on the audio thread writes to the log** (`JUCE_LOG_ASSERTIONS`, which
  the tests' assertion net needs): a string made and a file written there, which the real-time audit then counts. No
  assertion fires on the audio thread in the audit's scenarios today; one that did would show as an allocation as well
  as a failed test.

- **149. JUCE asserts on a date that is no date in a damaged zip** (`juce_core/time/juce_Time.cpp`, in Debug builds) as
  `juce::ZipFile` reads an entry. Phazite never uses an entry's date, so a damaged DAWproject only raises the assertion;
  the DAWproject fuzz case declares it (`ExpectedAssertion` in `tests/FuzzTests.cpp`). Whether Phazite reads the zip's
  entries without JUCE's date, or the assertion stays declared, is open question Q7 in
  [juce-workarounds.md](juce-workarounds.md#open-questions).
- **150. Full screen is switched in no test, and the main window's tests run on Windows only.**
  `tests/MainWindowTests.cpp` switches the theme, the interface size and the keymap, but not Full Screen
  (`view.fullScreen`): Windows' kiosk mode needs a real window, and JUCE asserts for one on no screen. And the file is
  compiled for Windows alone, as the window reads its theme and the screens from the system, which Linux CI can't
  without a display server. Fix: a full-screen case in a real window, as the devices' windows in that file are; on
  Linux, a virtual display (Xvfb) in CI, or the window's theme and screens taken through `MainWindow::Surroundings`.
