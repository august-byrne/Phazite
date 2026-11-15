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

## Versions

- **25. A project opened from a file equal to its last commit may still take an "As opened" Local History snapshot,**
  which suggests the history sees a change that isn't there (seen in a test's reopen; not traced).
- **59. A branch whose last commit can't be read gets a new "Initial commit" on top without a word,** and the older
  commits leave the graph until Verify History and Recover Lost Commits bring them back. Why: the history opens from
  the heads and commits when it read none (`src/model/versions/VersionHistory.cpp`); `readBranchLog()`
  (`src/model/versions/VersionStore.cpp`), which knows the head before, has no caller. Fix: say that the head can't be
  read and offer the newest commit of the branch's log that reads; commit by itself only where the Versions folder has
  no branch. Not yet checked (model-versions 5).
- **61. Clean Up Unused Audio offers audio that Local History and commits that can be recovered still play.** Why:
  `findVersionsAudioPaths()` (`src/model/versions/VersionHistory.cpp`) lists the files of commits on branches and of
  songs left on branches only (`src/model/project/UnusedAudio.cpp`). Fix: count the snapshots' songs and the commits
  that are in the packs and on no branch as using their files. Not yet checked (model-versions 8).
- **62. A damaged piece is never replaced, and Rename Branch can race a commit.** The index keeps the first pack that
  lists a piece and a write skips names it has, so every later commit holding that piece is unreadable too, even where
  another pack has a good copy (`src/model/versions/PieceStore.cpp`). `renameBranch()`
  (`src/model/versions/VersionHistory.cpp`) isn't refused while a commit saves, and both write the branch's record. Fix:
  on a read that fails, try the other packs and forget the bad entry; refuse the rename meanwhile. Not yet checked
  (model-versions 13).
- **172. A commit, a Local History snapshot or another write still queued on the history's thread as the project closes
  is dropped without a word.** `~VersionHistory()` (`src/model/versions/VersionHistory.cpp`) calls
  `thread.removeAll (false)`, which throws away the jobs that haven't started: commit and close at once, and the
  commit may never be written. Fix: let the writes that are queued finish before the history goes (the close waits
  for them as Save As does, `waitForVersionsWritten()`), on a thread of their own if the wait could be long. Seen
  while known bug 60 was fixed; not reproduced by a test.

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

## Devices and playback

- **32. The compressor's Drive jumps as it leaves 0:** at any Drive above 0.01 %, or the moment FET or Tube turn down at
  all, the curve goes from a straight line to tanh at unity gain (2.4 dB down on a full-scale peak). Fix: fade the curve
  in with the drive.
- **33. A compressor made for Duck that automation switches to Compress** doesn't turn its dry copy as its drive turns
  its sound, so a Mix below 100 % combs the top octave there.
- **43. The compressor's driven sound comes 0.137 samples later than the latency it reports** (5.137 against 5: its
  oversampler's filters; the undriven path and the dry copy are 5 exactly). Fix: a fractional delay on the shaper's
  path, as JUCE's oversampler makes up its own with an all-pass.
- **34. The Vinyl's Mix** blends its sound, played through a moving delay at twice the rate, with a dry copy it never
  touches. Not measured; the Saturator's comb (fixed 2026-11-13) came from the same mix.
- **35. Sampler and drum sampler sounds read from float files aren't checked for values that aren't numbers.**
- **36. A jump while playing into a part of a long audio file that isn't read yet is silent until it is read** (it fades
  in then). Play from a stop and the loop's start are read ahead; a jump needs pre-roll.
- **38. The playhead moved while recording:** the new take starts when the engine first sees it, so the tens of
  milliseconds after the jump aren't recorded; and dragging the top bar's place or time field while recording makes a
  take at every step (only the ruler takes its press alone).
- **39. A/B's bounce file isn't read ahead at the loop's start or a new place,** and its own meters aren't guarded
  against values that aren't numbers.
- **70. The synths have one place to fade a stolen voice in, so a second steal within 3 ms cuts the first dead.** A
  chord of several notes played while all 16 voices sound steals as many voices on one sample, and all but one stop at
  once, at whatever level they had. Why: `stealVoice()` copies the stolen voice into the one spare voice, over the last
  (`src/devices/synth/Synth.cpp`, `src/devices/wavetable/WavetableSynth.cpp`, `src/devices/fm/FmSynth.cpp`,
  `src/devices/sampler/Sampler.cpp`). Fix: fade a stolen voice where it is, or keep a few places to fade in. Partly
  confirmed by a second reading (devices-instruments 2).
- **71. The drum sampler steals its oldest voice without a fade, and a pad can't cut its own earlier hits.** The
  longest sound (a crash, a long 808) is cut with a click once 32 voices sound, which fast hats over long sounds reach;
  and an 808 pad's notes always overlap. Why: `startVoice()` (`src/devices/drums/DrumSampler.cpp`) takes the oldest
  voice at once, and the choke leaves out the pad's own voices. Fix: fade a stolen voice over about 5 ms, prefer the
  quietest one that is dying away, and let a pad cut itself. Confirmed by a second reading (devices-instruments 3).
- **72. A wavetable or a sampler's sound that was swapped out is freed by a count of `process()` calls,** which the
  runner makes for stretches of 1 to 32 samples, while the wavetable's readers keep the old table until their next
  control step (`src/devices/wavetable/WavetableSynth.cpp`, `src/devices/sampler/Sampler.cpp`). It is safe as the code
  stands only because the freeing happens in a later call that sets a table or a sound. A sampler's note started in the
  call that swaps its sound is reset as that call ends. Fix: free by a count the audio thread moves on at a control
  step, or hand over through `src/engine/core/Handover.h`. Not yet checked (devices-instruments 11).
- **73. Small things depend on the sample rate and on how blocks are split.** An envelope follows a sustain level that
  moves at a fixed step a sample, so in half the time at 96 kHz (`src/devices/dsp/Envelopes.h`); the synth's drift
  moves one step for each stretch whatever its length, so faster where notes split blocks (`Synth::moveDrift()`,
  `src/devices/synth/Synth.cpp`); white noise is 3 dB lower in the band that is heard at 96 kHz; the drive fades over
  32 samples at any rate. Fix: work each out from seconds and the rate as the device is prepared. Not yet checked
  (devices-instruments 12).
- **74. Turning a Multiband band's saturation on or off skips or repeats a few samples.** Why: the bands change in
  one sample from a plain delay to oversamplers that start silent, and back (`src/devices/dynamics/Multiband.cpp`); the
  Filter fades the same change over 64 samples. Fix: play both ways through a fade of 64 samples. Not yet checked
  (devices-effects 10).
- **75. The delay's echo and the reverb's Room lines are one sample longer than set:** 2 % long at 1 ms, which
  detunes a comb made with the delay. Why: they read their delay lines before they push the new sample, and don't take
  the one sample off as the chorus does (`src/devices/delay/Delay.cpp`, `src/devices/reverb/Reverb.cpp`). Fix: read one
  sample less. Not yet checked (devices-effects 12).
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

- **134. The Synth, the Phaser, the Flanger and the Chorus count their control steps from where each block starts, and
  the EQ glides its bands over each block,** so what they play depends a little on how the same samples are cut into
  blocks: with still values up to 39, 73, 35 and 31 dB under their sound; with automation in blocks of 441 samples the
  Synth 35, the EQ 37, the Flanger 21 and the Chorus 8 to 12 dB under it. Found by the device conformance suite
  (`tests/DeviceConformanceTests.cpp`), which lists each as an exception with its allowance. Fix: count the steps from
  the song's sample position, as the Wavetable, the FM, the Sampler and the Drum Sampler do, which are exact.
- **135. The Synth with Osc 2 Sync on, and the Wavetable with its warps (Sync, Bend, Pulse Width), put a constant offset
  under held notes,** up to 0.88 of the notes' level. Found by the conformance suite, whose offset rule leaves those
  three parameters at their defaults. Fix: a DC blocker in each voice, as the FM has.
- **136. The FM leaves an offset that follows its envelope,** about 0.08 of the notes' level at its defaults: its 5 Hz
  DC blocker takes out only what is steady. An exception in the conformance suite, with an allowance of 0.15.
- **137. A Free LFO, step sequencer or breakpoint envelope runs on while the song is stopped and isn't started again by
  Play,** so a song with one never plays live as it exports, where it starts afresh. The golden songs with one say so
  (`runsFree` in `tests/GoldenModulationTests.cpp`) and aren't compared live against their render. Whether Play starts
  Free modulators again is the owner's to decide ([open-questions.md](open-questions.md)).

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
- **163. A control's drag taken back by Escape, in Write, while the loop wrapped during the drag,** takes back the
  points the writer wrote at the wrap too, which were in the drag's step; and a control grabbed before playback
  started has no record of the pass as it was, so its Escape uses an earlier grab's record if one is left
  (`AutomationWriter::rememberGrab()`, `src/model/playback/AutomationWriter.cpp`). Fix: keep the wrap's points in a
  step of their own, and clear the grabs as playback starts. Seen while Escape was built; neither has a test.

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

## Recording

- **92. A note played just before a take's first sample is thrown away,** so a downbeat played a few milliseconds
  early after a count-in is lost, as is any key in the first samples that the output's latency covers. Why: the take
  keeps a note-on only at or after its start (`src/model/transport/Take.cpp`). Fix: keep note-ons from a short time
  before the start, moved to it. Confirmed by a second reading (model-playback 1).
- **93. MIDI recording and Capture keep notes only.** The sustain pedal, pitch bend, the mod wheel and aftertouch are
  heard live and gone from what was recorded. Why: a take keeps note-ons, note-offs and all-notes-off of what was
  played (`src/model/transport/Take.cpp`, `src/model/transport/Capture.cpp`); only a MIDI file's import makes the
  song's controller elements. Fix: write them from the take's events, with a test that records a held pedal.
  Confirmed by a second reading (product-gaps 0, model-playback 2).
- **94. A full disk or a write that fails during a take is neither noticed nor told.** The file is short, the clip
  made for it covers frames it doesn't have, and the user hears silence there later. Why: the take's writer ignores
  what its writes return and counts the frames as written (`src/model/transport/Take.cpp`). Fix: note a write that
  fails, stop counting, tell the user, and warn before recording where little space is left. Not yet checked
  (model-playback 7).
- **95. An audio device that restarts at another sample rate during a take** (an interface unplugged, another program
  changing the rate) leaves the take writing into a file opened at the old rate, so the rest plays at the wrong speed
  (`src/model/playback/PlaybackHost.cpp`, `src/model/transport/Take.cpp`). Fix: end the take as the device starts at
  another rate. Not yet checked (model-playback 14).

## Mixer and automation

- **103. Automation is written at the engine's playhead, not where it is heard,** so what is written lands late by
  the output's latency and the song's delay compensation; and a jump back while writing with the loop on is taken for
  the loop's wrap, which writes the held value through to the loop's end. Why: `AutomationWriter`
  (`src/model/playback/AutomationWriter.cpp`) stamps points with the session's playhead. Fix: stamp them with the heard
  playhead, and end the pass at a jump. Not yet checked (model-playback 13).
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

## Window, shell and keys

- **110. The shortcuts editor takes away keys that commands share on purpose.** Giving a command a key that another
  has removes it from every other command; Phazite's keymap shares ten keys by design (Esc, Up and Down among them),
  and a shared key can't be set up again. There is no search either. Why: the editor is JUCE's
  `KeyMappingEditorComponent` (`src/ui/shell/SettingsPanel.h`). Fix: a list of Phazite's own, searched by name and
  key, that shows every command on a key and allows the keys that priorities tell apart. Checked in the code
  (2026-11-14, stage 1): JUCE's `ChangeKeyButton` asks to reassign a key another command has, then calls
  `removeKeyPress (key)`, which takes it from every command, and the editor has no hook to change that (only
  `shouldCommandBeIncluded()`, `isCommandReadOnly()` and `getDescriptionForKeyPress()` are virtual), so the fix is the
  editor of Phazite's own the entry describes, larger than a small bug's fix (ui-shell-browser 4).
- **112. On keyboards other than the US layout the musical keyboard is scrambled and some shortcuts can't be
  reached.** On AZERTY the piano's shape breaks; on QWERTZ Y and Z swap and the top note needs Shift; "=" needs Shift
  on German and French layouts, so Zoom In and its like never match. Why: the note keys are characters, mapped through
  the layout in use (`src/ui/input/ComputerKeyboard.cpp`, `resources/keymaps/phazite.json`). Fix: map the musical
  keyboard by the keys' places; bind zooming to "+" too. Not yet checked (ui-core-mixer-app 8).
- **146. Thirteen controls have no tooltip,** so the hint bar says nothing over them: the Project view's Notes field,
  the header of Source Control's Graph, seven of the EQ editor's band controls (Band On, Band Shape, Slope, Frequency,
  Gain, Q, Output), and the Low Cut and High Cut knobs of the Vinyl and of the Delay (`src/ui/project/ProjectView.cpp`,
  `src/ui/versions/VersionsView.cpp`, `src/ui/devices/EqEditor.cpp`, `src/ui/devices/VinylEditor.cpp`,
  `src/ui/devices/DelayEditor.cpp`). The walk of every control (`tests/AppShellTests.cpp`) lists them in `knownSilent`
  and fails once one has a tooltip, so take each out of the list with its fix. Their wording waits for an answer
  ([open-questions.md](open-questions.md), row 56).

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
- **126. The versions' differences and the store's reads run on the message thread.** The activity bar's badge asks
  for the changes four times a second, which builds maps of every note of each changed track
  (`src/model/versions/VersionHistory.cpp`, `src/ui/shell/ActivityBar.cpp`); opening a commit, starting a merge and
  switching branches unpack and check whole songs before they return. Fix: on the history's thread, comparing pieces by
  their names from the top down. Not yet checked (model-versions 11).
- **127. The mixer makes a strip for every channel and brings all of them up to date at any change of a device,**
  those scrolled out of view too, eight times a second while a fader is dragged (`src/ui/mixer/MixerView.cpp`,
  `src/ui/mixer/ChannelStrip.cpp`). No benchmark has a mixer. Fix: only the strips whose channel changed and that are
  in view; a benchmark of 200 tracks. Not yet checked (ui-core-mixer-app 10).
- **128. The browser's search looks through the whole index on the message thread at each key,** stops at 50,000
  files and 200 results without saying that there are more, and doesn't search folders' names
  (`src/services/Library.cpp`, `src/services/BrowserModel.cpp`). Fix: search keys made ahead, a search that runs in
  the background and can be cancelled, and a word where results are cut. Not yet checked (ui-shell-browser 6).
- **129. While the Project view shows, every branch's song is read again on the message thread every five seconds and
  after each edit,** to find the audio nothing plays (`src/ui/project/ProjectView.cpp`,
  `src/model/project/UnusedAudio.cpp`). Fix: find it as a background job, and keep each branch's files until the
  versions change. Not yet checked (ui-shell-browser 7).
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
