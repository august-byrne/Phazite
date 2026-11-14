# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

The file holds two kinds of entries, under the same headings:

- **Entries 19 to 43** are bugs seen in the code while the quality audit's 16 priorities were fixed, each by the session
  that fixed its neighbour, none yet reproduced by a test. Some are notes still to be traced: where an entry gives no
  cause or no fix, that is the first step.
- **Entries from 44 on** are the open faults of the whole-project quality audit: its bugs, and those of its real-time,
  security and performance findings that are faults rather than wishes. The audit read the code and ran nothing, so each
  entry ends with whether its second reader confirmed it ("confirmed by a second reading", "partly confirmed") or it is
  "not yet checked": check an entry against the code, and reproduce it with a test, before fixing it. Each names the
  audit's area and finding number, by which the audit's own files hold it in full. Timings in them are estimates unless
  they say "measured".

What is open and is not a bug (quality gaps, design changes, missing features, tests and structure) is in
[backlog.md](backlog.md).

## Editing

- **19. Automation and tempo points snap to the grid's lines as they are dragged.** A point off the grid jumps onto it at
  the first move, also a move straight up or down, as notes and clips did before 2026-11-13. Why: the drag snaps the
  point's place, not its move (`src/ui/playlist/PlaylistAutomation.cpp`, `src/ui/playlist/TempoLane.cpp`). Fix: snap the
  move with `TimelineEditor::snapMove()`, as the piano roll and the playlist do.
- **20. Duplicate and Paste make fewer notes than they say** where some would land past a clip that the next clip keeps
  from growing: no note is made where it wouldn't play, but the undo step is still named for all of them ("Duplicate 6
  Notes" for 2). Fix: name the step for what was made, or say in the hint bar that some didn't fit.
- **21. Copies of a copying drag that came back to its start lie on their originals until the mouse goes up,** and a tool
  change in the middle of the drag keeps them (`cancelGesture`). Fix: take the copies away as the drag returns.
- **44. A note dragged past its clip's end and back leaves the clip longer for good.** A clip of bars 1 to 4 whose note
  is dragged to beat 9 and back to beat 3 ends at beat 12 after the release, so the song's end and the loop move; drags
  that paint or copy notes do the same. Why: each step of the drag sets the notes, and `growToHold()`
  (`src/model/notes/Notes.cpp`) only lengthens (`src/ui/pianoroll/PianoRollNoteTools.cpp`). Fix: keep each shown clip's
  length from the press and restore it before each step, or grow once at the release. Confirmed by a second reading
  (ui-timeline-editors 1).
- **45. Undo, or any command, runs in the middle of a drag.** Ctrl+Z while a clip is dragged undoes the drag's step and
  starts one without a name, which the rest of the drag goes into; redo's history is gone. Before the drag's first
  change it undoes the step before. Why: `undo()` (`src/model/commands/Undo.cpp`) doesn't ask `Gesture::isInProgress()`,
  and the key dispatcher runs commands while a mouse button is down (`src/ui/input/KeyDispatcher.cpp`). Fix: switch
  Undo and Redo off while a gesture is in progress, and run no command that changes the song then. Not yet checked
  (tests-ui 3).
- **46. A start typed or dragged in the inspector for several notes can put them all on their clip's start.** In a clip
  that starts at beat 32, notes at 33 and 34 moved back 3 beats both land on 32. Why: `changeNotes()` and
  `setNotesTo()` (`src/model/notes/NotesModel.cpp`) limit the move by the song's start, not the clip's, and each note is
  then kept in its clip on its own. Fix: limit the shared amount by each note's clip, as `nudgeNotes()` does. Not yet
  checked (model-arrangement 10).
- **47. Slipping a reversed audio clip shrinks it, and can leave it with no length.** Forwards, the part can run past
  the file's end into silence. Why: `slipClip()` (`src/model/arrangement/Clips.cpp`) only keeps the offset above 0, and
  `setAudioPart()` (`src/model/arrangement/AudioClips.cpp`) then cuts the part at the file's start. Fix: keep the
  offset within the file's length less the part's, both ways, and take the length from the drag's start. Not yet checked
  (model-arrangement 6).
- **48. Paste can put clips on a return track, where they neither show nor play** yet count towards the song's end:
  clips copied from two tracks, pasted with the last track selected. Why: `getTracksForClips()`
  (`src/model/arrangement/Clipboard.cpp`) leaves out folders only. Fix: leave out returns too, and add tracks, or say
  so, when a paste runs out of them. Not yet checked (model-arrangement 7).
- **49. Remove Time and deleting a section can leave two key changes on one beat,** and which holds depends on the
  order they were made in. Why: every key change in the range is moved to its start and none is removed
  (`src/model/chords/Chords.cpp`). Fix: keep the last one and remove the others, as `removeTempoMapTime()` does for the
  tempo. Not yet checked (model-arrangement 8).
- **50. Keys are always named with sharps,** "A# major" beside chords spelled "Eb", in the piano roll's key button, the
  inspector and the chord lane's menus. Why: `describeKey()` (`src/model/notes/Notes.cpp`) takes JUCE's note names,
  while the chords take `getRootLetter()` (`src/model/chords/ChordValues.cpp`). Fix: one function spells both. Not yet
  checked (model-arrangement 13).
- **51. The Spread voicing adds a perfect fifth over the bass whatever the chord,** so a diminished or augmented chord
  gets a note it doesn't hold, and an inverted one a fifth over its third. Why: `voiceSpread()`
  (`src/model/chords/ChordValues.cpp`) starts every chord with its bass and seven semitones above. Fix: take the fifth
  from the chord's own notes. Not yet checked (model-arrangement 14).
- **52. Small ones in the notes and clips.** Chop and Arpeggiate stop after 256 pieces and delete the rest of a long
  note; the last clip of a group of linked clips still shows as linked after the others are deleted; a file is analysed
  for its hits twice when it is asked for again meanwhile; a length typed stops at a sixteenth of a beat and one
  dragged at a sixty-fourth; Quantize picks the nearest line before swing, then swings it
  (`src/model/notes/Notes.cpp`, `src/model/arrangement/Clips.cpp`, `src/model/arrangement/Transients.cpp`). Not yet
  checked (model-arrangement 15).
- **53. Selected automation points are kept by their indices, which go stale.** With points 3 and 4 selected, undoing
  an earlier point's addition leaves the selection on what were points 4 and 5, and Delete or a drag acts on those.
  Why: `Selection` keeps `int`s and only checks them against the curve's count (`src/model/base/Selection.cpp`). Fix:
  clear or remap the selection when a curve's points change by undo, redo or automation writing, or give points IDs.
  Not yet checked (model-project-services 5).
- **54. Shift or Ctrl and a click on a distant track scrolls the playlist back to the first selected track,** hiding
  what was clicked. Why: every change of the selection ends in `scrollToShow()` of the first selected track's row
  (`src/ui/playlist/Playlist.cpp`). Fix: scroll for selections made by keys and commands only, to the track that
  changed. Not yet checked (ui-timeline-editors 5).
- **55. The wheel is read by its direction, not by how far it turned.** A two-finger scroll over the velocity lane
  runs velocities to 127 or 1, an undo step for each event, also for every selected note when none is under the
  pointer; slow trackpad scrolls are lost to rounding; there is no pinch zoom. Why: `VelocityLane::mouseWheelMove()`
  (`src/ui/pianoroll/PianoRollLanes.cpp`) and the row heights (`src/ui/pianoroll/PianoRoll.cpp`) take the sign alone.
  Fix: add up parts of a notch, one gesture for a burst, a note under the pointer for velocity. Not yet checked
  (ui-timeline-editors 7).
- **56. Small ones in the lanes' gestures.** A double-click on an empty part of an automation lane adds a point, then
  deletes it: two undo steps for nothing. Draw places the clip selected first, where the code's comment says last.
  Shift pressed in the middle of a point's drag makes its value jump (`src/ui/playlist/PlaylistAutomation.cpp`,
  `src/ui/playlist/Playlist.cpp`). Not yet checked (ui-timeline-editors 12).
- **57. In the inspector, typing the lowest of several items' values does nothing.** With notes of velocities 64 and
  100 selected ("64 to 100"), typing 64 leaves them as they are; the same for clips' lengths and gains. Why: the
  field's value is the lowest item's, and JUCE's slider skips a typed value equal to its own
  (`src/ui/mixer/Inspector.cpp`). Fix: apply what was typed whether or not it equals the shown value. Not yet checked
  (ui-core-mixer-app 5).

## Versions

- **22. A track restored from a commit with an unreadable piece inside it** puts the stand-in element, with its `missing`
  property, into the song (`src/model/versions/ApplySong.cpp`, the "gone since" branch). Fix: refuse the restore, and say
  which piece is missing.
- **23. A merge skips the cleanup every other apply runs** (`removeBrokenReferences()`, `forgetSidechainLoops()`;
  `finishMerge()` in `src/model/versions/VersionsModel.cpp`), so it can leave modulation and sidechains that point at
  nothing.
- **24. A clip moved on the other branch and deleted on this one comes back from a merge without a note in the window.**
- **25. A project opened from a file equal to its last commit may still take an "As opened" Local History snapshot,**
  which suggests the history sees a change that isn't there (seen in a test's reopen; not traced).
- **26. `getTracksInFolder()` doesn't count an empty folder inside the folder,** though its comment reads as if it did.
- **58. A second merge of a branch that went on working after a merge gave its items new IDs can give two items one
  ID.** The first merge renumbers their clip 10 to 12; their branch then makes a clip 12 of its own; the second merge
  turns 10 into 12 beside it. It needs two computers working on one history. Why: `translateIDs()`
  (`src/model/versions/SongMerge.cpp`) translates every ID, not only those the parting song holds. Fix: translate
  those alone, and renumber any other ID of theirs that equals a translation's target. Confirmed by a second reading
  (model-versions 4).
- **59. A branch whose last commit can't be read gets a new "Initial commit" on top without a word,** and the older
  commits leave the graph until Verify History and Recover Lost Commits bring them back. Why: the history opens from
  the heads and commits when it read none (`src/model/versions/VersionHistory.cpp`); `readBranchLog()`
  (`src/model/versions/VersionStore.cpp`), which knows the head before, has no caller. Fix: say that the head can't be
  read and offer the newest commit of the branch's log that reads; commit by itself only where the Versions folder has
  no branch. Not yet checked (model-versions 5).
- **60. Undo after a merge leaves the merge waiting, and a waiting merge is forgotten as the project closes.** After
  the undo the next commit still gets both parents; after a close and reopen, neither the merge nor the IDs it gave
  anew are recorded. Why: `finishMerge()` (`src/model/versions/VersionsModel.cpp`) makes an undo step that nothing ties
  to the merge the history holds, which lives in memory only. Fix: drop the waiting merge with its undo and bring it
  back with its redo, and keep it in the computer's record. Not yet checked (model-versions 6).
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
- **63. "Create new branch" that fails renames the branch the song is on.** Why: the picker runs the command, which
  says only that it ran, then renames the current branch to the name typed (`src/ui/versions/BranchPicker.cpp`). It
  needs a write that fails. Fix: one model function that makes a named branch and says whether it did. Not yet checked
  (ui-shell-browser 13).

## Plug-ins

- **27. A plug-in that crashed during an export at another sample rate is reloaded at the export's rate.** Why:
  `SandboxedInstance::prepareToPlay()` returns early for a crashed instance without keeping the rate asked for.
- **28. A plug-in that had crashed before a render started renders silent, or dry, without a word.** Only a crash or a
  hang during the render fails it. Fix: fail the render at its start, naming the plug-in.
- **29. A render's first block is skipped if the plug-in's channel is still busy with a late block of live playback.**
- **30. Cancelling a render can take up to 10 seconds** when a plug-in that hangs must also be prepared for another
  sample rate: the helper can't prepare it while it keeps the block, and Phazite waits the prepare's time limit out.
- **31. `SandboxedInstance::playBlock()` (JUCE's `processBlock` path) notices a helper that has gone only at the hang
  limit,** not at once as the engine's path does.

- **42. A render ends a plug-in's helper that keeps one block for 30 seconds,** though the plug-in may only be slow (six
  times the live hang limit: `renderHangFactor` in `src/model/plugins/internal/SandboxedProcessor.h`). Before 2026-11-13
  a render waited without end, which hung Phazite on a real hang. Not seen with a real plug-in. Fix, if one needs it: a
  setting, or a question to the user once the limit is reached.
- **64. A block's deadline has no floor.** The engine cuts a block at a loop's end and after a count-in, so a part can
  be 1 to 5 samples long, and a plug-in gets 21 to 104 microseconds for it at 48 kHz, less than a round trip to its
  helper takes: the effect passes its input dry, an instrument is silent, and the next part may find the channel busy.
  Why: `SandboxedInstance::getDeadlineNanoseconds()` (`src/model/plugins/SandboxedInstance.cpp`) is the part's length.
  Fix: take the deadline from the device's block, with a floor near a millisecond. Not measured. Partly confirmed by a
  second reading (plugin-hosting 5).
- **65. A mono plug-in: an effect leaves the right channel dry, and an instrument plays on the left only.** Why: for a
  plug-in that refuses stereo the helper falls back to the plug-in's own layout and plays channel 0 alone, and Phazite
  copies both channels back (`chooseLayout()` and `playPiece()` in `src/host/core/Instance.cpp`). Fix: in the helper,
  add the input up to mono and copy a mono output to both channels, by the plug-in's own channel counts. Not yet
  checked (plugin-hosting 8).
- **66. Phazite trusts counts that a helper can write.** After a block it copies as many channels as the shared
  memory's header says, read again from memory the helper writes (`src/model/plugins/SandboxedProcessor.cpp`), and
  `readParameters()` and `readPluginInfo()` (`src/sandbox/core/PluginInfo.cpp`) size their lists from a count in the
  message: a helper gone wrong can make Phazite read and write out of bounds, or fail an allocation on the pipe's
  thread. The helper keeps what it reads within limits; Phazite doesn't. Fix: use the count Phazite handed over, cap a
  message's counts by the bytes it has left, and take a malformed message for a lost helper. Not yet checked
  (plugin-hosting 9).
- **67. As a project opens, and as a plug-in reloads, the plug-in's program is set after its state,** the order the
  code's own comment warns against: a plug-in whose state doesn't bring its program back gets the factory program over
  the saved settings. Why: `setCurrentProgram()` after the instance is made with its state
  (`src/model/plugins/PluginInstances.cpp`). Which plug-ins it harms isn't known. Fix: never set a program after a
  state. Not yet checked (plugin-hosting 11).
- **68. A plug-in's editor has the wrong size whenever Interface Size isn't 100 %:** cropped below it, with black
  margins above it, and a resizable editor is made larger at each opening. Why: `SandboxedEditor`
  (`src/app/SandboxedEditor.cpp`) takes the plug-in's size in pixels for its own size in the scaled window, and the
  helper sizes its window by the area that covers (`src/host/core/Editor.cpp`). Traced in the code, not run. Fix: size
  it by the plug-in's size divided by the scale, place it again when the scale changes, and test editors at 75 and
  150 %. Confirmed by a second reading (ui-core-mixer-app 0).
- **69. Opening, closing and saving a project keep the message thread waiting for each plug-in in turn.** Each
  plug-in opens with a request that may take a minute, with nothing shown and no way to cancel, though each has a
  process of its own; closing costs up to 100 ms an instance; a save asks every plug-in for its state, one after
  another (`src/model/plugins/PluginInstances.cpp`, `src/model/plugins/PluginHost.cpp`). Fix: open instances in the
  background, several helpers at once, with a notice of the progress; wake the helper's thread as an instance stops; ask
  only the plug-ins that reported a change for their state. Not yet checked (plugin-hosting 10).

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
- **37. A held instrument note that is silent for longer than the instrument's tail lets the instrument sleep.** Fix: ask
  the device whether it has voices sounding.
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
- **76. The metronome and the count-in aren't compensated for the song's latency.** With a device of 4096 samples'
  latency on the master the click comes 85 ms before the music at 48 kHz; notes played to the click land early by as
  much, and the count-in's bar and the song's first beat part the same way. Why: `playClicks()` and `playCountIn()`
  (`src/engine/core/Engine.cpp`) add the clicks straight to the outputs, while the song leaves the master its latency
  later. Fix: play the clicks through a delay of the plan's output latency. Confirmed by a second reading (engine 3,
  model-playback 3).
- **77. An audio clip moved, slipped or rendered again while the playhead is in it plays on from its old place** until
  the next stop, jump or time round the loop. Why: a region's state is kept by its clip's ID from plan to plan and
  works its place in the file out again only after a jump (`src/engine/audio/AudioRegion.cpp`). Fix: keep the clip's
  start, its start in the file and the file in the state, and take a change of them for a jump. Not yet checked
  (engine 5).
- **78. A track unmuted, or no longer silenced by a solo, in the middle of a note stays silent until its next note.**
  Why: a silenced strip throws its notes away but still counts them as sounding, so nothing starts them again
  (`src/engine/core/Engine.cpp`). Fix: start held notes again as the strip is heard again; or, as planned, let mute and
  solo silence a strip's output only, its instrument playing on. Not yet checked (engine 6).
- **79. A transport queue that fills freezes the shown playhead for good, and can lose a Stop.** It takes 64 commands
  within one block: a fast drag of the playhead at 4096 samples, or an output that stalls. Why: a command is dropped
  once 64 wait, after the count of jumps asked for has gone up, which the count of jumps taken then never reaches
  (`src/engine/core/Engine.cpp`). Fix: carry the last jump and the play state in atomics, so that nothing can
  overflow. Not yet checked (engine 7).
- **80. During an export, a freeze or a bounce the engine takes neither live notes nor transport commands.** MIDI
  played meanwhile sounds as one burst when the song's plan comes back, note-offs past 1,024 events are lost, and past
  63 commands the playhead freezes as in 79. Why: the engine takes them only while it has a plan, and a render borrows
  it (`src/engine/core/Engine.cpp`, `src/model/playback/PlaybackSession.cpp`). Fix: take and drop live events, and
  keep the last state asked for, in every block without a plan. Not yet checked (tests-audio 4).
- **81. The engine's worker threads don't flush denormals to zero.** What the engine itself plays on them (a
  container's crossover filters, the modulators' followers) slows down as its input dies away, and can differ by the
  thread that played the strip. Why: `juce::ScopedNoDenormals` is set in `Engine::process()` alone, not in the workers
  (`src/engine/core/Workers.cpp`). Fix: set it as a worker starts. Not yet checked (engine 8).
- **82. With one output channel open, only the left side of the mix plays,** and what is panned right is gone. Why:
  the master's channels go to the outputs one to one (`src/engine/core/Engine.cpp`). Fix: with one output, play half
  the sum of both sides. Not yet checked (engine 9).
- **83. The plans an engine played are freed half a second after a session lets go of them, though a block may still
  play one:** a block held longer (a debugger, a suspended machine, a driver that stalls) goes on with freed memory.
  Why: `letGoOfPlans()` (`src/model/playback/PlaybackHost.cpp`) waits for two blocks or 500 ms, then frees. Fix: after
  the wait, leave the plans to the handover's own count. Not yet checked (tests-audio 12, model-playback 14).
- **84. Held live notes stick when the live track changes or a MIDI input is unplugged.** Hold a key, click another
  instrument's track, let go: the note-off goes to the new track. Why: `PlaybackSession::setLiveTrack()`
  (`src/model/playback/PlaybackSession.cpp`) only notes the new track, and an input that goes ends no notes
  (`src/model/engine/AudioIO.cpp`). Fix: end every note, and the pedal, on the track that was live. Not yet checked
  (model-playback 9).
- **85. A loop's wrap and a jump put their work into one block, and the message thread can wait for the disk.** The
  wrap looks through every note before the playhead on every track, starts each resampled clip's resampler from
  nothing, and a jump resets every device, in one block (`src/engine/core/Engine.cpp`,
  `src/engine/audio/AudioRegion.cpp`). And the reading thread holds a file's lock across its read, which the message
  thread takes at every new plan (`src/engine/audio/AudioFile.cpp`). Fix: an index of the notes sounding, resamplers
  started on the reading thread, and reads outside the lock. Not measured. Not yet checked (engine 13).
- **86. A clip's render that fails starts again for ever.** A reversed, pitched or tempo-following clip whose file is
  missing, or a Cache folder that can't be written: the render fails, is taken off the list and queued again, the
  message thread and a job at full speed for as long as the project is open. Why: nothing remembers a render that
  failed (`src/model/arrangement/ClipRenders.cpp`). Fix: remember it by its file and its source's date, try again only
  when the clip, the source or the cache changes, and show the failure on the clip. Confirmed by a second reading
  (model-arrangement 1).

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
- **138. Bounce in Place collapses the original track outside its undo step** (view state), so Undo leaves it collapsed.

## Saving

- **40. A backup that can't be copied fails without a word** (`Project::backUpProjectFile()` ignores `copyFileTo()`'s
  result). The save itself goes on, rightly; the user should be told that no backup was made.
- **41. The library's facts file and the run marker are still written with JUCE's `replaceWithText()`,** which doesn't
  check its write (W30 in [juce-workarounds.md](juce-workarounds.md)). Neither holds the user's work: a facts file cut
  short is read again from the sounds, and a missing marker only skips the crash check once.
- **87. An autosave that can't be read keeps the project's own file from opening.** With Restore chosen, only the
  autosave is read; if it doesn't parse, Phazite offers the newest backup or the last commit in its place, though the
  project's file is whole and newer than both. Why: `Project::open()` (`src/model/project/Project.cpp`) and what
  `ProjectSession` offers after it (`src/app/ProjectSession.cpp`). Fix: fall back to the project's file, and say that
  the autosave couldn't be restored. Not yet checked (model-project-services 9).
- **88. With Auto Save on, a save that fails keeps the user from quitting, New and Open.** Where the project's folder
  is gone or can't be written (a drive unplugged, a network share), each of them shows the same error again; the only
  way out is to turn Auto Save off, which nothing says. Why: `closeThen()` (`src/app/ProjectSession.cpp`) gives up when
  the save fails. Fix: go on to the question that offers Save As, Don't Save and Cancel. Not yet checked
  (ui-core-mixer-app 3).
- **89. Auto Save asks every plug-in for its whole state on the message thread, a second after each change, and saves
  an old state where a plug-in doesn't answer.** Each request waits up to 10 seconds, for plug-ins that changed and
  those that didn't; one that times out gives the state before, and the save says it worked. Why:
  `saveInBackground()` stores the devices' states first, on the message thread (`src/model/project/Project.cpp`,
  `src/model/plugins/PluginInstances.cpp`). Fix: ask only the plug-ins that reported a change, without waiting on the
  message thread, and count a time-out as a save that failed. Confirmed by a second reading (model-project-services 3).
- **90. A new project's first save copies its Audio, Cache and Versions folders on the message thread.** With
  gigabytes of recordings the window stands still for the copy, which needs the space twice and can't be cancelled.
  Why: `moveTo()` copies each folder before it returns (`src/model/project/Project.cpp`). Fix: copy as a background
  job with its progress, or move the files where both folders are on one drive. Not yet checked
  (model-project-services 8).
- **91. Opening a DAWproject writes a copy of an audio file for every clip that plays it:** a loop in 32 clips becomes
  32 files. Why: `addAudioClip()` unpacks the file for each clip into a name that isn't taken
  (`src/model/project/DawProject.cpp`). Fix: unpack each file of the archive once, and use a file of the Audio folder
  that is the same. Not yet checked (tests-model 11, model-project-services 4).

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

- **96. Time edits and clip moves leave behind the automation of plug-ins that aren't loaded.** With plug-ins off
  after a crash, or missing on this computer, Insert Time, Remove Time and automation that follows its clips skip those
  curves, which are out of step with the song when the plug-in loads again. Why: they go through
  `getParametersWithCurves()` (`src/model/automation/Automation.cpp`), which keeps the curves whose parameter is found,
  and a plug-in's is found through its instance. Fix: for time edits, every curve whose device is in the song. Not yet
  checked (model-mixer 4).
- **97. Switching a folder between a bus and a plain folder loses its volume and pan automation and modulation**
  (Undo brings them back; the next opening deletes them). Why: `makePlainFolder()` and `makeBus()`
  (`src/model/mixer/Buses.cpp`) give the folder's fader a new ID, and curves and modulations are kept by the old one.
  Fix: one ID for both, or move the curves and modulations in the same undo step. Not yet checked (model-mixer 5).
- **98. Moving clips with their automation changes the shape of bent automation outside what moved;** Paste and the
  time edits do the same. Why: `addEdge()` (`src/model/automation/Automation.cpp`) splits a segment with a straight
  point, so the first part keeps its bend over a shorter span and the rest is a straight line. Fix: give both parts
  bends that keep the shape. Not yet checked (model-mixer 6).
- **99. A preset loaded while a track writes automation in Latch or Write writes a lane for every parameter of the
  device;** a Swap that is put back leaves the preset it tried as automation. Why: loading sets each value by hand, and
  `AutomationWriter::parameterChanged()` (`src/model/playback/AutomationWriter.cpp`) punches in for each. Fix: let
  what sets many values at once (a preset, a swap, a sampler's sound) say so, and the writer leave those alone. Not yet
  checked (model-mixer 7).
- **100. Deleting a return leaves the sends to it in the song:** each is still offered for automation and modulation,
  does nothing, and is written into a DAWproject as a send to a channel the file doesn't have. Why: `deleteTracks()`
  (`src/model/arrangement/Tracks.cpp`) forgets sidechains, not sends. Fix: remove them in the same undo step. Not yet
  checked (model-mixer 8).
- **101. A device on a drum sampler's track can be given one of its own pads' tracks as its sidechain,** which the
  engine can't play: the pad isn't separated any more, its track is silent, and the device never ducks. Why:
  `isFedBy()` (`src/model/mixer/Sidechain.cpp`) doesn't know that a pad's track takes its sound from its sampler's
  track. Fix: add that rule. Not yet checked (model-mixer 11).
- **102. Pads' tracks are left behind when their drum sampler is removed or replaced:** they play nothing, and Bring
  Pad Back can't be reached. Why: `removeDevice()` and `setInstrument()` (`src/model/mixer/Mixer.cpp`) and
  `deleteTracks()` leave the tracks' pad properties. Fix: clear them, or delete the empty tracks, in the same undo
  step. Not yet checked (model-mixer 14).
- **103. Automation is written at the engine's playhead, not where it is heard,** so what is written lands late by
  the output's latency and the song's delay compensation; and a jump back while writing with the loop on is taken for
  the loop's wrap, which writes the held value through to the loop's end. Why: `AutomationWriter`
  (`src/model/playback/AutomationWriter.cpp`) stamps points with the session's playhead. Fix: stamp them with the heard
  playhead, and end the pass at a jump. Not yet checked (model-playback 13).
- **104. Small ones in the mixer's model.** Every new ID sets a property on the song's root, after which the freeze
  keeper compares every frozen track in full (`src/model/mixer/Freeze.cpp`), which a large MIDI import with frozen
  tracks may feel (not measured); Write mode's punch-in as playback starts sets a value without an undo step of its
  own; "dry level" and "wet level" are names no device has (`src/model/automation/Automation.cpp`); the cache of
  wavetables read
  never lets one go (`src/model/devices/Wavetables.cpp`). Not yet checked (model-mixer 15).

## Files from others, and privacy

- **105. A preset's file is trusted.** A track preset's volume has no upper limit and plays as written, hundreds of
  decibels if the file says so; and a pad's, a wavetable's or a sample's path may be any file of the computer, which
  is copied into the project's Audio folder, or a network path, which makes Windows connect to it. Why:
  `setChannelVolume()` (`src/model/mixer/Mixer.cpp`) keeps no upper limit, and the presets' paths go to
  `copyIntoProject()` unchecked (`src/model/presets/Presets.cpp`, `src/model/devices/DeviceFiles.cpp`). Fix: keep
  channels' and chains' volumes within the fader's range wherever they are written and read; take a preset's paths for
  audio files only, never network paths, and say which were refused. Confirmed by a second reading (model-mixer 0).
- **106. A DAWproject archive is unpacked without limits, and one written past 4 GB is damaged.** A small hostile
  archive can use up the memory or the disk: its `project.xml` is read whole and its files are copied out whatever
  their size (`src/model/project/DawProject.cpp`). Names are made safe, so no file lands outside the project. Written,
  an archive with 4 GB of audio or more is damaged and the export says it worked: JUCE's zip writer keeps sizes in 32
  bits. Fix: cap `project.xml` and each file by its stated size and the space left; refuse or warn past 4 GB. Not yet
  checked (model-project-services 12).
- **107. A saved preset holds the full paths of its sound files, the user's name in them,** and its sounds don't go
  with it. Why: saving a preset turns each path in the project into the file's full path
  (`src/model/presets/Presets.cpp`, `src/model/devices/DeviceFiles.cpp`). The privacy page promises that for project
  files only. Fix: copy a preset's samples beside it and keep their paths from the preset; until then, say it on the
  privacy page. Not yet checked (docs 8).

## Window, shell and keys

- **108. The first lesson's second step hides the piano roll it asks for.** The step says to press F7; the lower panel
  already shows the piano roll, and showing a view that shows hides its panel, so the step counts as done and the next
  asks for clicks in a piano roll that is gone (`src/ui/shell/Lessons.cpp`, `src/ui/shell/AppShell.cpp`). Fix: count
  the step as done while the clip editor shows; run the lesson through the whole window in a test. Confirmed by a
  second reading (ui-shell-browser 1).
- **109. Hiding the left panel leaves a looping preview playing and a swap open,** with nothing in view to stop it.
  Why: the browser stops them in `visibilityChanged()` (`src/ui/browser/Browser.cpp`), which JUCE calls for its own
  switch, not when the panel around it is hidden. Fix: tell the browser when it stops showing. Not yet checked
  (ui-shell-browser 3).
- **110. The shortcuts editor takes away keys that commands share on purpose.** Giving a command a key that another
  has removes it from every other command; Phazite's keymap shares ten keys by design (Esc, Up and Down among them),
  and a shared key can't be set up again. There is no search either. Why: the editor is JUCE's
  `KeyMappingEditorComponent` (`src/ui/shell/SettingsPanel.h`). Fix: a list of Phazite's own, searched by name and
  key, that shows every command on a key and allows the keys that priorities tell apart. Not yet checked
  (ui-shell-browser 4).
- **111. In a panel's own window the note keys run shortcuts instead of playing notes.** With the lower panel in a
  window of its own and the computer keyboard on, A S D F run single-key shortcuts there, Shift and a letter does
  nothing, and nothing lets held keys go as the window loses the focus. Why: the panel's window has the key dispatcher
  alone as its key listener (`src/ui/shell/AppShell.cpp`), not what the main window has (`src/ui/input/WindowKeys.cpp`).
  Fix: give it the same. Not yet checked (ui-shell-browser 5).
- **112. On keyboards other than the US layout the musical keyboard is scrambled and some shortcuts can't be
  reached.** On AZERTY the piano's shape breaks; on QWERTZ Y and Z swap and the top note needs Shift; "=" needs Shift
  on German and French layouts, so Zoom In and its like never match. Why: the note keys are characters, mapped through
  the layout in use (`src/ui/input/ComputerKeyboard.cpp`, `resources/keymaps/phazite.json`). Fix: map the musical
  keyboard by the keys' places; bind zooming to "+" too. Not yet checked (ui-core-mixer-app 8).
- **113. A project double-clicked while any dialog is open is dropped without a word.** Why:
  `anotherInstanceStarted()` (`src/app/Main.cpp`) brings the dialog forward and returns; nothing keeps the file to open
  it afterwards, as its comment says. Fix: keep it and open it once the dialog has gone, or say why it didn't open.
  Not yet checked (ui-core-mixer-app 11).
- **114. The library's indexer follows folder loops, and looking for favourites that moved reads files on the message
  thread.** A link that loops in a sound folder is followed to the path's limit and can fill the index of 50,000 files
  with repeats; and each listing of Favorites hashes up to 64 files of the same size for every member that is missing,
  again each time (`findMovedMembers()`, `src/services/Library.cpp`). Fix: don't follow loops; look for moved members
  on the indexer's thread, once for each index. Not yet checked (model-project-services 11).

## Device editors

- **115. The Stereo Expander's scope is mirrored:** a sound on the left alone is drawn along the line marked R. Why:
  `toDot()` (`src/ui/devices/StereoExpanderEditor.cpp`) takes left less right for the dot's place across. Fix: right
  less left, and a test that a left-only block's dots lie left of the centre. Confirmed by a second reading
  (ui-device-editors 0).
- **116. Several graphs' handles jump to the pointer as they are taken,** so a click, or a move of one pixel, changes a
  value: an EQ's dot taken at its edge moves by up to 12 pixels, a Multiband's crossover by up to half as much again,
  and a click near one of the reverb's cut lines moves it. Why: these drags set the value from where the pointer is,
  though a handle is taken from up to 12 pixels away (`src/ui/devices/EqEditor.cpp`,
  `src/ui/devices/MultibandEditor.cpp`, `src/ui/devices/ReverbEditor.cpp`, `src/ui/devices/DelayEditor.cpp`). Fix: keep
  where the handle was taken and drag from there, as the dynamics curve does; apply nothing on a press. Not yet
  checked (ui-device-editors 2).
- **117. A drag in a graph starts from the modulated value and writes it as the value set by hand.** With an LFO on a
  wavetable's Position, a press on the table at the LFO's peak writes the peak. Why: these drags read
  `ParameterHandle::getValue()`, the value as it plays, where knobs read `getBaseValue()`
  (`src/ui/devices/WavetableEditor.cpp`, `src/ui/devices/SynthParts.cpp`, `src/ui/devices/SaturatorEditor.cpp`,
  `src/ui/devices/MultibandEditor.cpp`, `src/ui/devices/LevelHistoryView.cpp`). Traced in the code, not run. Fix: one
  shared way for graphs to start a drag, from the value set by hand. Not yet checked (ui-device-editors 3).
- **118. In the EQ each move of the wheel over Q is an undo step of its own, and the wheel turns the selected band's Q
  anywhere on the graph.** Why: `mouseWheelMove()` (`src/ui/devices/EqEditor.cpp`) sets the value outside a gesture;
  the other graphs turn through `graph::WheelGesture`. Fix: the same here, and only over a dot. Not yet checked
  (ui-device-editors 4).
- **119. The compressor's history draws the detector's level as what came in,** after the input gain and the
  sidechain's low cut, and for some characters an average: on drums the output can draw several decibels above the
  "input" while the device turns down (`src/ui/devices/CompressorEditor.cpp`, `src/devices/dynamics/Compressor.cpp`).
  Fix: draw the input's peak, as Duck does. Not yet checked (ui-device-editors 6).
- **120. The analyzers transform the two channels' average, and the stereo tap is too small at high sample rates.**
  Sound that is out of phase between the channels shows nothing, and sound panned hard reads 6 dB low
  (`src/ui/devices/DeviceGraph.cpp`); the tap holds 8,192 pairs, 43 ms at 192 kHz against a tick of 33 ms, so a tick
  that comes a little late loses samples, which shows as a flash (`src/devices/core/SampleTap.h`). Fix: analyze each
  channel and draw the louder; size the taps for a quarter of a second at the device's rate. Not yet checked
  (ui-device-editors 8).
- **121. The random LFO is drawn as four steps a cycle, where it holds one value a cycle** (`getDrawnLfoValue()` in
  `src/ui/devices/SynthParts.cpp`, against `src/devices/dsp/Oscillators.h`), in the synth's, the FM synth's, the
  sampler's and the wavetable synth's editors. Fix: draw one level across the cycle, or the value the device holds. Not
  yet checked (ui-device-editors 11).

## Slow on the message thread

None of these is measured: each is the audit's reading of the code, and each needs a benchmark before and after its fix.

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
  tempo map several times for each clip,** and changes of view state make a new plan too, so a drag of the playhead
  compiles at every turn of the message loop (`src/model/playback/SongPlayer.cpp`,
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
- **130. Editors work their curves out from nothing at every repaint.** The EQ makes every band's coefficients again
  for each column of its curve, 30 times a second while its analyzer moves (`src/ui/devices/EqEditor.cpp`); the
  reverb's and the sampler's pictures do alike (`src/ui/devices/ReverbEditor.cpp`, `src/ui/devices/SamplerEditor.cpp`).
  Fix: keep the curve until a value changes. Not yet checked (ui-device-editors 13).

## Tests

- **131. Some checks that fail would end the test program instead of reporting.** A `REQUIRE` that throws while a
  thread can still be joined, or inside a thread, ends the process (`tests/SandboxTests.cpp`); and pointers are used
  after a `CHECK`, or unchecked (`tests/DevicesViewTests.cpp`), so a real regression there hides the file's other
  cases behind a crash. Fix: a guard that joins, `CHECK` inside threads, `REQUIRE` before a pointer is used. Not yet
  checked (tests-infrastructure 15).

- **139. A real-time check fails under a loaded full run.** "The song plays in real time while an old version renders in
  the background, no block held up" (`tests/VersionsHearingTests.cpp`) measures playback against the wall clock and,
  with three other test files running beside it, sometimes sees no block played in its window (`longestBlock > 0`
  fails). It passes alone. The full run is heavier since the golden songs are also played live. Fix: wait for the
  first block before measuring, or run the wall-clock files one at a time in ctest (`RUN_SERIAL`).

## Build and packaging

- **132. The folder beside the app goes stale, and the installer packs it.** Presets deleted from `resources/presets`
  stay next to the app, and an older `PhaziteHost` can: the copies are steps after the app's link, which run only when
  the app links again, and copying a folder never deletes (`CMakeLists.txt`, `packaging/windows/Phazite.iss`). The
  tests read `resources/presets`, so nothing catches it. Fix: make the folder anew from a target with real
  dependencies, and build releases from clean trees only. Partly confirmed by a second reading (build-ci-tools 0).
- **133. One working tree holds both kinds of line endings.** `* text=auto` in `.gitattributes` names no ending, so a
  file has the ending of whatever wrote it last, and those bytes are compiled in (the JSON) or shipped (the presets).
  Fix: `* text=auto eol=lf`, with `*.cmd` and `*.bat` kept as they are, and the files normalized once. Not yet checked
  (build-ci-tools 8).
