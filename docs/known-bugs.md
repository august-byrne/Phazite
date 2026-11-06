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

## 8. Switching the theme while Phazite runs leaves some controls in the old theme's colours

- **How it shows:** after Settings > Theme (or the activity bar's Theme menu) switches between the dark and the light theme,
  the text already in a field keeps the old theme's colour (the inspector's Name field: light grey on white after
  switching to the light theme), and a button that needs attention (Choose instrument) keeps its old warning mix until
  what it shows changes. Found on 2026-11-05, in headless screenshots of both themes taken after a switch. Starting
  Phazite in a theme draws everything in it.
- **Why:** `MainWindow::setTheme()` sends every window a look-and-feel change, which repaints them, but a
  `juce::TextEditor` keeps the colour each piece of its text was typed in, and components that set a colour from the
  palette once (`setColour (...)` when their state changes) set it again only then.
- **How to fix:** in each component that sets colours from the palette, set them again in `lookAndFeelChanged()` (as
  `TopBar` does); and in fields that hold text, call `applyColourToAllText (findColour (juce::TextEditor::textColourId))`
  there. A test switches the theme and checks a field's text and the instrument button's colour.

## 9. A knob set back to its default still counts as a change in source control

- **How it shows:** turn a knob of one of Phazite's own devices (the compressor's Input Gain) and double-click it back to
  its default: the Versions view still lists the device as changed since the last version ("Keys: Compressor"), though
  it plays as it did. Found on 2026-11-06 with a test, while fixing the same for a track's mute and solo and a clip's
  mute, which the owner had found; a track's volume and pan set back are no change.
- **Why:** a device's and a modulator's values set by hand are kept in their VALUES element, a property for each value
  set and none for one never set (`schema::VALUES`; `setValueSetByHand()` in `src/model/devices/Parameters.cpp`). Set
  back to its default, the value stays, as a property, and the song's comparisons (`findChangedProperties()` in
  `src/model/versions/SongDiff.cpp`, which the merge and the comparison with the saved copy share in kind) count a
  property one side has and the other hasn't as a change, as they did the switches kept as "0".
- **How to fix:** keep no value at its default in the song: every place that writes VALUES (a value set by hand, a
  device's mix, its sidechain's values, a preset loaded into a device) removes a value at its default, and a VALUES
  element left without values, so that a song set back by hand holds what it held before, as the switches now do. Preset
  files can keep their defaults. There is no backward compatibility to keep; regenerate the goldens and say why. A test
  in `tests/VersionsTests.cpp` turns a knob and sets it back, as the one for the switches does.
