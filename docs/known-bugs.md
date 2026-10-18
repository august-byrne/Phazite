# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread that Tracktion's code causes are in
[realtime-audit.md](realtime-audit.md) instead, and Tracktion's other quirks in [design.md](design.md), section 23.

## 1. Pitched and tempo-following audio clips play off the beat

*Found 2026-10-17, while measuring Signalsmith Stretch against SoundTouch. The owner: "we definitely don't want the
timing bug", to be fixed at a later date (2026-10-18).*

- **How it shows:** an audio clip that follows the song's tempo (every loop from the browser does) or is pitched in the
  audio clip editor plays early: 13 ms while it plays at its own tempo, about 20 ms when pitched, and 20 to 28 ms when
  stretched, wobbling by about 4 ms from beat to beat. That is enough to flam against a kick on the grid. Clips that
  are neither stretched nor pitched play on time.
- **Why:** Tracktion plays such a clip from a copy that it renders in the background (a proxy: `StretchSegment` in
  `tracktion_AudioClipBase.cpp`), and that render takes no account of the stretcher's delay. SoundTouch's output leads
  its input; with Signalsmith Stretch, which Phazite would otherwise use for its cleaner sound, clips would play 100 to
  134 ms late. When Tracktion stretches as it plays (the browser's preview), it primes the stretcher first
  (`primeStretcher` in `tracktion_WaveNode.cpp`), but that path has offsets of its own.
- **How to fix:** in Tracktion, render each proxy segment from where the stretcher's output lines up with its input, as
  the live path primes it, measuring the delay where the stretcher doesn't report it (SoundTouch reports none). That
  is a report upstream or a fork (design question 4). Then switch clips to Signalsmith Stretch and keep SoundTouch for
  previews ([design.md](design.md), section 23, has the measurements).
- **How to test it:** render a clip of clicks on every beat offline, as export does, followed, stretched and pitched,
  and find where each click's energy lands; a test should hold it within a millisecond of its beat. The measurement of
  2026-10-17 did this in a temporary test, not kept.

## 2. Saving a project puts its plug-ins' settings into the undo history

*Found 2026-10-18, when the tests started using a stand-in plug-in (`tests/TestPlugin.h`) instead of Tracktion's
reverb, which never showed it.*

- **How it shows:** after a plug-in's own settings change (a knob turned in its window, a preset loaded in it), the
  next save adds Tracktion's copy of those settings to the undo history: to the last undo step, or as a step of its own
  if the history was empty. Undoing it takes the settings in the project back to what the plug-in had before, while the
  plug-in keeps playing its new ones, and the project shows an unsaved change; the next save writes them again.
- **Why:** every save, and the autosave, runs Tracktion's `Edit::flushState()`, whose `ExternalPlugin::
  flushPluginStateToValueTree()` writes the plug-in's settings block, program, name, maker, IDs and channel layout
  into its state through the Edit's undo manager. Tracktion's own reverb kept its settings in the state already, so
  its flush wrote nothing new.
- **How to fix:** write those properties before the save, past the undo manager, so that the flush finds them written
  and records nothing: flush into a transaction of its own, take the copy it wrote, take the transaction back out with
  `UndoManager::undoCurrentTransactionOnly()` (which drops it and keeps the redo history), and write the copy's
  properties again without an undo manager, while `Project` ignores the notifications (`isWriting`). Check that the
  undo manager's change message, which makes Tracktion mark the Edit changed, doesn't leave the saved project showing
  an unsaved change. Or, upstream, let `flushPluginStateToValueTree()` write without the undo manager.
- **How to test it:** in `tests/ProjectTests.cpp`, the view settings test saves once to write the stand-in plug-in's
  settings before it clears the undo history; save straight after the plug-in's gain changes instead, and check that
  nothing can be undone.
