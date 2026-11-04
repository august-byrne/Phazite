# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

## 7. A test of plug-in instruments in helpers can fail while other tests run beside it

- **How it shows:** `SandboxedPluginTests`, "Plug-in instruments in helpers, handed their blocks as the engine's block
  starts, play their notes and automation", failed once in a full `ctest` run on 2026-11-03 (four test files at once):
  the first synth's level over its first second was below its tone's. Alone it passed three times in a row, and the
  next full run passed.
- **Why (likely):** the test plays block by block (`EnginePlayback.h`), but each synth plays in a helper process, whose
  block has a deadline (`SandboxedProcessor.h`): on a loaded machine a block that misses it is silent, which lowers that
  second's level.
- **How to fix:** let the test's engine wait for a helper's block without a deadline, as a render's block does
  (`PlaybackSession::lendToRender()`), so that the test is the same every time; or check that no block missed its
  deadline before comparing levels.
