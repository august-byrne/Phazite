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

## Slow on the message thread

Unless an entry says it was measured, each is the audit's reading of the code, and needs a benchmark before and after
its fix.

- **183. Loading a preset that holds sounds copies them into the project on the message thread.** A drum kit or a
  sampler preset whose sounds are outside the project has each copied, and compared in full with a file of the same
  name, in the call that loads it (`copyDeviceFilesInto()`, `src/model/devices/DeviceFiles.cpp`, from
  `src/model/presets/Presets.cpp`), where a sound put on a pad, an import and Swap's Keep have gone to the
  background since known bug 123 was fixed (2026-11-16). A kit of long samples from a slow disk stops the window
  while they copy. Fix: bring them in as those do (`bringFilesIntoProject()`,
  `src/model/arrangement/internal/Import.h`), the preset applied once the copies are made.

- **184. Opening a project whose file can't be read reads its branch's last commit on the message thread.** When the
  project file can't be read, Phazite offers the current branch's last commit instead, and `readLastCommitSong()`
  (`src/model/versions/VersionHistory.cpp`) opens the store and joins that commit's whole song, with the loader's
  checks, on the message thread before the offer shows: about 80 to 190 ms on the 32-track benchmark song in
  `windows-profile`, more with a long history's packs to open. Rare: it needs a damaged project file. Why: opening
  asks, then opens, all in one call. Fix: read it with `ProjectContext::holdForWork()` and show the offer once it is
  in. Left as the rest of known bug 126 was fixed (2026-11-16).

## Tests

- **180. The plug-in tests still compare against the wall clock.** The rest of the tests no longer do (2026-11-16);
  what is left is in `tests/SandboxedPluginTests.cpp` and `tests/SandboxedPlugins.h`: the stand-ins' hang limits of one
  and two seconds for every test; bounds on "doesn't wait" (`timeKept (...) < 500` and the like: closing a plug-in's
  window failed at 856 ms and 2.8 s under load, a crashed plug-in's state, a save, `processBlock`, a render's
  cancel); waits for a time that stand for a condition (`dispatchMessagesUntil (false, N)`); and the spacing of a
  large state's answers measured on the real clock. Fix: the short limits only in the tests of hangs, with the
  stand-in stalling until released; each bound against a long limit for the wait it guards, or a structural check;
  each time wait made its condition; the spacing judged on the clock a test holds (`devices::Clock`), as
  `tests/README.md` says ("Real time without the wall clock").
