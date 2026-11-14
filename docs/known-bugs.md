# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

The quality audit of 2026-11-13 read the whole project and listed about 300 findings; its 16 priorities are fixed, and the
rest wait to be moved here and into GitHub issues. Below are the bugs found while those 16 were fixed, each seen in the
code by the session that fixed its neighbour, none yet reproduced by a test. Some are notes still to be traced: where an
entry gives no cause or no fix, that is the first step.

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

## Saving

- **40. A backup that can't be copied fails without a word** (`Project::backUpProjectFile()` ignores `copyFileTo()`'s
  result). The save itself goes on, rightly; the user should be told that no backup was made.
- **41. The library's facts file and the run marker are still written with JUCE's `replaceWithText()`,** which doesn't
  check its write (W30 in [juce-workarounds.md](juce-workarounds.md)). Neither holds the user's work: a facts file cut
  short is read again from the sounds, and a missing marker only skips the crash check once.
