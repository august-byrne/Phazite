# Phazite roadmap

*Status: draft for review, October 2026.* This is the plan for getting from today's smoke-test app to v1 of the [design](design.md).
- **Order:** each step builds on the ones before it and ends with something that can be built, tested and tried.
- **Sizes:** relative effort, S < M < L < XL, for ordering only. They are not dates.

## How each step is done

1. **Plan:** confirm the step's scope. If a decision changes, update [design.md](design.md).
2. **Build:** follow `CLAUDE.md`: architecture rules, real-time rules, formatting, SPDX headers, licences. Move the code the step touches toward the [architecture track](#architecture-track)'s structure.
3. **Test:**
   - unit tests for model logic and DSP tests for devices;
   - the architecture check passes (`sh tools/check-architecture.sh`), and the golden tests pass, or their files change on purpose and the commit says why;
   - a `windows-debug` build with zero warnings, plus `windows-release` for performance work;
   - CI green on Windows and Linux.
4. **Try:** a short manual check that says what to click and listen for.
5. **Review and commit:**
   - read the whole diff (no secrets, no personal data) and pass the git hooks;
   - one commit per step, or per logical part of a step.

## Where we are

Done before this plan (progress since then is under [Status](#status)):
- **Tooling:** CMake presets, the MSVC toolchain wrapper and the Windows setup script.
- **Dependencies:** Tracktion Engine and JUCE as pinned submodules.
- **Smoke-test app:** play/stop, audio settings, out-of-process plug-in scanning, ASIO® on Windows.
- **Checks:** git hooks for commit identity, secrets and formatting; clang-format; CI on Windows and Linux with warnings as errors.

## Decisions and when they're needed

| Open question ([design section 25](design.md#25-open-questions)) | Needed before |
|---|---|
| 1. Algorithmic AI engines in v1 | **Decided:** yes, trained models later |
| 2. First outline styles | **Decided:** pop, rock, electronic, hip-hop/trap |
| 3. UI font and its licence | **Decided:** Inter, bundled, SIL OFL 1.1 |
| 4. Engine fixes: workarounds or a fork | **Decided:** simple workarounds first, a fork if they get awkward; replacing a part of the engine is a separate decision for each part (A5) |
| 5. MP3 export | Step 1.8 |
| 6. Airwindows effects | Step 2.8 |
| 7. Sandboxing before or after v1 | Phase 3 |
| 8. Ableton Live keymap in v1 | Step 3.7 |
| 9. Single-key tools and the computer keyboard | **Decided:** Ableton's way, Shift and a letter for the shortcut while the keyboard plays notes |

## Architecture track

*Added 2026-10-14* ([design section 24](design.md#24-code-structure)). This track moves the code toward the structure in the design without a rewrite. Its phases are called A0 to A5 (phases 0 to 5 of the architecture review), so they aren't confused with the phases below.
- **Small steps:** after each one the app builds, behaves the same and passes its tests, golden tests included, and each is its own commit.
- **Alongside the features:** only A0 and A1 pause feature work. Later phases move each area of the code when a step touches it.

| Phase | Goal | When |
|---|---|---|
| A0. Safety net and limits | Make restructuring safe, and stop new breaks of the rules | Before step 1.6 (done 2026-10-14) |
| A1. Untangle the model | One job per folder, and no UI code in the model | Before step 1.6 (done 2026-10-15) |
| A2. One path for every edit | The UI decides nothing about the project | Alongside steps 1.6 to 1.9 |
| A3. Phazite's types at the UI boundary | The UI knows nothing about Tracktion | One view at a time; the playlist and the mixer before step 2.1 |
| A4. A UI a designer can work on | A designer changes how Phazite looks without editing C++ | When the designer joins; any time after A1 |
| A5. Engine parts replaced one at a time | Replace a part of Tracktion only where Phazite gains something | Only after a dated decision for each part ([design section 23](design.md#23-tracktion-engine-what-it-gives-and-what-we-build)) |

**A0. Safety net and limits (M)**
- **Golden tests**, which bring forward the render comparisons under [Cross-cutting work](#cross-cutting-work):
  - representative projects saved, reopened and saved again, and compared with committed files, leaving out what changes on every save;
  - a project saved by today's version, kept to check that older projects still open as the same song;
  - a short offline render compared with a committed level fingerprint, within a tolerance;
  - the model's commands, their order, and a script running each of them, with the undo step it makes and every command's state, compared with committed files.
- **The architecture check:** `tools/check-architecture.sh` in CI's Checks job, as pre-commit check 7 and in pre-push, counting what breaks the rules against limits that can only go down. A raise needs a reason in its commit message, which CI and pre-push check. It also checks that every quoted include and every source path in `CMakeLists.txt` names a file exactly, as Linux needs. The counts at the start:
  - model includes of `ui/` or `app/`: 0;
  - JUCE window includes in the model: 3 (0 after A1);
  - Tracktion includes in `ui/`: 7, and Tracktion references: about 545;
  - undo steps started in `ui/`: 46;
  - model files on include loops: 10 (0 after A1).
- **Docs:** the structure and decisions in [design.md](design.md#24-code-structure), the rules in `CLAUDE.md`, and this track. CLAUDE.md's advice to link controls to Tracktion parameters becomes a stopgap until A3, and step 2.1 no longer builds on Tracktion's `CurveEditor`.
- *Done when:* CI fails as soon as a count goes over its limit or a limit rises without a reason, and the golden tests pass on Windows and Linux.
- *Status:* done (2026-10-14): the architecture check runs in CI's Checks job (with dash and mawk), as pre-commit check 7 and in pre-push; the golden tests of projects, the 0.1.0 format, a render and the model's commands pass on Windows, and in CI on Linux too since 2026-10-15.

**A1. Untangle the model, without changing behavior (L)**
- **Folders by area:** `model/<area>/` with `base/` and `commands/` at the bottom, `services/`, `ui/<area>/` with `theme/`, `components/` and `input/`, and `app/`, which becomes a library the tests link.
- **Headers:** the model gets Tracktion from one small header instead of through the command registry's, and has no include loops. At the start of A1, Project and Playback, Sections and Outlines, and the commands and Playback included each other; since 2026-10-14 no model file is on an include loop.
- **Project data:**
  - one schema file with every property Phazite saves, a schema version and migrations;
  - the grid's fixed size and a key's scale, saved as Tracktion enum numbers, become Phazite's own values, and older projects are converted when they open;
  - projects from a newer Phazite are handled as [design question 17](design.md#25-open-questions) proposes until it is decided;
  - view state stops counting as an unsaved change.
- **Commands:** the registry splits. The model keeps the commands, running one inside a named undo step, enabled and ticked state, and search; key handling, the bridge to JUCE's menus and keymap storage move to `ui/input/`. One undo convention, `phazite::UndoStep`, and one way to find the open project, `ProjectContext`.
- **Input and view code leaves the model:** the computer keyboard and how the main window's keys reach it, the Smart Tool and its hit zones, the time line's zoom and the grid's spacing in pixels (where grid lines fall is beat arithmetic on the tempo map, and stays in the model), track heights in pixels, the drum rows' display order, drag-and-drop payloads, the texts for decibels and pan, and the playlist's mouse options in the keymap. Tracktion's hook for devices' windows moves to the app.
- **Commands become tables:** the logic in command registration becomes named model functions with tests.
- **Services and the app:** a typed settings service with one list of keys (today 14 keys are read and written in 5 files, in 3 ways); the sound pack, the library and the previewer belong to the app, not to the main window's content.
- *Done when:*
  - the architecture check's counts of JUCE window includes and window classes in the model, Tracktion includes in the model outside its prelude, model undo steps outside the undo code, model files on include loops and files outside an area folder are all 0;
  - every command's logic can be called and tested without the registry;
  - the app behaves as before, except for the two changes decided on 2026-10-14 (view state no longer marks the project as changed; older projects' grid sizes and key scales are converted), question 17's handling of projects from a newer Phazite, and the fixes the golden tests found (Phase 1, after step 1.5); the golden files change only for that conversion and those fixes.
- *Status:* done (2026-10-15), pending a check by hand that the app behaves as before. Each step was a commit of its own that built and passed the tests, golden tests included:
  - folders by area; `app/` is the `phazite_app` library the tests link, and tests reach the window's content without a screen;
  - the model gets Tracktion from `src/model/base/Tracktion.h` only, and no model file is on an include loop;
  - `src/model/base/Schema.h` names everything a project saves, with the format version (1) and the migrations that convert older projects when they open (`src/model/base/Migrations.h`): the grid's fixed size and keys' scales are saved as Phazite's own names, and a project from a newer Phazite isn't opened;
  - the command registry keeps the commands, their undo steps, state and search; keys, keymaps (with the playlist's mouse options) and JUCE's menus are in `src/ui/input/`, with the main window's keys in `WindowKeys`; `phazite::UndoStep` is the one undo convention, and `ProjectContext` the one way to the open project;
  - `src/services/` holds the settings, with one list of keys, and recent projects; the app owns the sound pack, the library and the previewer;
  - the computer keyboard, the Smart Tool, the time line's zoom, the grid's spacing in pixels, track heights in pixels, the drum rows' order, drag-and-drop payloads and the texts for values are UI code, and Tracktion's hook for devices' windows is the app's (`AppUIBehaviour`);
  - the commands are tables of named model functions, each with tests;
  - view state no longer counts as an unsaved change.
- *Done when, checked 2026-10-15:*
  - `sh tools/check-architecture.sh` counts 0 for model-window-includes, model-window-references, model-tracktion-includes, model-undo-steps, model-include-loops and flat-files, and 0 for unresolved-includes and cmake-sources too;
  - every command the model registers calls a named model function that tests call without the registry. The views' own commands mostly open or show something; the few that still edit in the UI, such as Quantize and the piano roll's Apply, move into the model with A2's direct edits;
  - the golden files changed only for three of the fixes the golden tests found and for the conversion to format version 1, besides a new file that lists the commands the app registers.

**A2. One path for every edit (L, alongside steps 1.6 to 1.9)**
- Model functions open their own named undo steps.
- A drag is a gesture session that begins, updates, then commits or rolls back as one step.
- The UI's direct edits (about 20) and the editing rules in UI files (where dropped files go, what Draw places, that sections never overlap, and others) move into model functions with tests.
- The big files split: the playlist into layout, clip tools, drop target, commands and view; the piano roll into session, note layout, note tools and commands; the channel strip into one file per class. The browser gets a model, and the playlist and the piano roll share one set of gesture code.
- *Done when:* the UI starts no undo steps and makes no direct edits, and no source file is much over 1,000 lines.

**A3. Phazite's types at the UI boundary (XL, one view at a time)**
- IDs for tracks, clips and devices, built on Tracktion's `EditItemID`; stable note IDs saved in the project; selections held as IDs.
- A view model per view (playlist, piano roll, mixer, transport), with plain values and grouped change signals.
- Meters and the playhead read through small polled sources, ending each mixer strip's own polling.
- Parameter handles for knobs and faders, replacing `ui::ParameterAttachment`, and a waveform source for clips.
- One view at a time; then `ui/` loses Tracktion's include path, and the model's Tracktion-using headers move into `model/<area>/internal/`.
- Tests build songs with shared helpers.
- *Done when:* `src/ui/` has no Tracktion references and can't include Tracktion, and the model's public headers are free of Tracktion types.

**A4. A UI a designer can work on (L, any time after A1)**
- `resources/design/tokens.json`, in the DTCG format, becomes the one source of the tokens, and a small build step generates `DesignTokens.h`.
- New tokens for transparency, sizes, corner radii and fonts.
- A gallery app shows every control and view in all three themes, with sample data.
- Debug builds get the Melatonin Inspector (MIT).
- User-facing text moves into a strings table, with one helper for key names.
- A one-page guide for the designer.
- *Done when:* any color, size or font can be changed in `tokens.json` and seen in the gallery without editing C++.

**A5. Engine parts replaced one at a time (only after a dated decision)**
- Step 1.6's devices start it: engine-neutral DSP in `src/devices/`, each in a thin Tracktion plug-in.
- For a bigger part: an interface in front of it, the replacement behind it, a switch-over, then the old code deleted.
- Tracktion's Edit stays the project store until something replaces it; the schema and its migrations make a converter manageable.
- *Done when,* for each part: the golden tests pass on the new code, and the old code is gone.

## Phase 0: Foundations

**0.1 Code structure and tests (S)**
- Split `src/` into:
  - `app/`: startup and the main window;
  - `model/`: Phazite's project model on top of Tracktion (commands, sections, chords, outlines);
  - `ui/`: components and look-and-feel.
- A static library target, `phazite_core`, holds everything testable without a window. The app links it.
- A test runner using doctest (MIT, bundled with Tracktion Engine), run by CTest locally and in CI. On Linux it runs headless.
- *Done when:* CI runs the tests on Windows and Linux.
- *Status:* done for `app/`, `model/` and `ui/`. Since 2026-10-14 the [architecture track](#architecture-track) organizes them by area, adds `services/`, and makes `app/` a library the tests link. Built-in DSP goes in `devices/` from step 1.6; the chord engine and the sound generator (steps 2.5, 2.6 and 3.1) follow the same rules: no Tracktion or window code in their logic.

**0.2 Engine hooks (S)**
- A `PropertyStorage` subclass: a neutral user name (no personal data), the app version and the settings folder.
- `EngineBehaviour`: the recording folder, raised device limits, plug-in scanning settings.
- `UIBehaviour`: the focused project, plug-in windows, progress for long tasks.
- Projects load on the message thread, not on `EditLoader`'s background thread.
- *Done when:*
  - a test saves a project and finds no operating-system user name in it;
  - freeze and render run without assertions.

**0.3 Real-time audit (M, decision gate)** *Can run alongside 0.4–0.7.*
- **The test:** a headless test loads a project with sends, automation, modulators and a plug-in, and plays it. CI runs it under the `linux-asan` and `linux-tsan` presets.
- **Optional:** Clang's RealtimeSanitizer (Clang 20 or newer), which reports allocations and locks inside functions marked as non-blocking.
- **The output:** a list of engine violations on Phazite's code paths, each with an action. Per decision 4: a simple workaround where one exists, otherwise a fork. Every problem is reported upstream.
- *Done when:* the list exists and each violation has its action.

**0.4 Projects, saving and autosave (M)**
- The project folder layout ([design section 18](design.md#18-projects-saving-and-export)).
- Save and load, with Phazite's own data inside the project file.
- Autosave, crash recovery, backups and recent projects.
- *Done when:*
  - after the app is killed mid-session, restarting it offers the autosaved state;
  - tests cover save/load round trips.
- *Status:* done (`src/model/project/Project.h` with tests, and the app's `src/app/ProjectSession.h`): new, open, open recent and save; an autosave every 2 minutes; the restore question at startup. Still to try by hand: end Phazite in Task Manager mid-session, then start it again. Since 2026-10-14 a project counts as changed only when it differs from how it was opened or last saved.

**0.5 Commands, keymaps and undo (M)**
- A command registry with names, descriptions, search synonyms and categories.
- One named undo step per command.
- Keymap files (Phazite default and FL Studio), and key rebinding.
- The command palette (Ctrl+K), and macros made of chained commands.
- *Done when:* every action in the app so far is a command, can be found in the palette, and shows by name in the undo history.
- *Status:* done: the registry, keymaps (Phazite and FL Studio, in `resources/keymaps/`), named undo, macros and the palette's search (`src/model/commands/CommandRegistry.h`, with tests); the palette (Ctrl+K), menus and shortcuts run the app's commands, including Quit, Open Recent and Save As, and **Options > Keyboard Shortcuts** changes keys (kept per keymap). Undo and Redo show the step they apply to by name ("Undo Change Volume") in the Edit menu and the palette; an undo history panel can come later.

**0.6 UI shell and design system (L)**
- **The main window:** docking panels that resize, collapse and detach to another monitor. Layouts: Beat, Mix and Full.
- **Design language** ([design.md](design.md#design-language)):
  - bundle the Inter font as separate files with its licence, use tabular digits for numbers, and fall back to the system font;
  - design tokens for color (dark, light and high-contrast themes), type sizes, spacing and control heights;
  - pick an icon set under the MIT or ISC licence;
  - HiDPI scaling.
- **Base controls:** knob, slider, button, meter and menu, with keyboard focus and accessibility handlers.
- **Help:** the hint bar and hover help.
- *Done when:*
  - the empty shell runs with all its panels, and layouts switch;
  - text renders in Inter on Windows and Linux, with tabular digits in readouts;
  - Windows Narrator reads the base controls;
  - CPU is near zero when idle.
- *Status:* the shell is in (`src/ui/`): the window with its menus, panels that resize and collapse, the Beat, Mix and Full layouts, the design tokens with three themes (`src/ui/theme/DesignTokens.h`, contrast checked by tests), Inter with tabular digits, the hint bar, hover help and the command palette. The base controls are in too (`src/ui/components/Controls.h`): knob, fader and level meter, buttons with a visible keyboard focus, and links to Tracktion parameters with named undo steps. They show in the top bar (master level and volume) and in the inspector, which shows the synth track's channel (volume, pan, mute, solo, level). The interface size goes from 75 % to 200 % (View menu, Ctrl+= and Ctrl+-). The browser, inspector and editor can each move to a window of their own, on a second monitor if there is one. The icons are Lucide's (`resources/icons/lucide/`, ISC). Still to try by hand: Windows Narrator reading the controls, and idle CPU.

**0.7 Audio, MIDI and the computer keyboard (S)**
- A settings page: audio device (including ASIO), buffer size and MIDI inputs.
- Computer-keyboard MIDI input.
- *Done when:* a MIDI keyboard and the computer keyboard both play the starter synth.
- *Status:* done: **Options > Settings** with Audio (device type including ASIO, device, sample rate and buffer size), MIDI (each MIDI input with an on/off switch, and the computer keyboard) and Shortcuts tabs; the computer keyboard (`src/ui/input/ComputerKeyboard.h`) and live MIDI routing (`src/model/engine/LiveInput.h`), with tests. Still to try by hand: a MIDI keyboard and the computer keyboard playing the starter synth.

## Phase 1: Make a beat (milestone 1)

**1.1 Playlist (XL)**
- **Timeline:** a ruler, smooth zoom and scroll, an adaptive grid and snap.
- **Tracks:**
  - create, delete, reorder, rename, color and resize;
  - folders that collapse and become buses;
  - pinning and focus mode.
- **Clips:**
  - audio clips with waveforms, and MIDI clips with note previews;
  - the Smart Tool, single-key tools and selection;
  - split, duplicate, alias and make unique, delete and ripple delete, nudge, fades and crossfades.
- **Placing content:** drag and drop from the browser, with automatic naming and coloring.
- *Done when:* a beat can be laid out from one-shots and MIDI clips with just the keyboard and the Smart Tool.
- *Status:* done, in four parts, pending a check by hand of the "done when" above.
  - **Part 1, done:** the timeline (`src/ui/timeline/TimelineView.h`: positions in beats and zoom; `src/model/arrangement/Grid.h`: a grid that adapts to the zoom or has a fixed size, with snap and triplets) and the tracks (`src/model/arrangement/Tracks.h`: add, delete, duplicate, move, select, mute, solo, colors from the color-blind-safe palette, heights). The playlist (`src/ui/playlist/Playlist.h`) has the ruler, the playhead, scrolling, zoom and track headers that rename, recolor, resize and move tracks; live MIDI and the inspector follow the selected track.
  - **Part 2, done:** clips (`src/ui/playlist/ClipComponent.h`): audio clips with waveforms and fades, MIDI clips with their notes, and aliases marked with a `*`. The Smart Tool (`src/ui/timeline/SmartTool.h`, `src/model/arrangement/Clips.h`): trim at the edges, fades at the top corners, a time range from the upper half or empty space, and moving, copying (Ctrl) or aliasing (Ctrl+Shift) from the lower half, across tracks, snapped unless Alt is held, each drag one named undo step. Delete, duplicate and select all work on clips (`src/model/arrangement/Selection.h`), and double-clicking a track with an instrument adds a MIDI clip.
  - **Part 3, done:** the single-key tools (Draw, Paint, Slice, Slip, Mute, Delete, Select and Zoom, on FL Studio's letters, with toolbar buttons), and editing commands (`src/model/arrangement/Selection.h`): split, duplicate as alias, make unique, mute, nudge, quantize clip starts, delete and close the gap, insert and remove time, crossfades from the Smart Tool and fade shapes. A time range is a selection of its own (`phazite::RangeSelection`) that Delete, Duplicate, Split and the time commands act on.
  - **Part 4, done:** folders that group tracks (grouped, ungrouped, collapsed; plain until they hold an effect, then buses), pinned tracks in a band that doesn't scroll, focus mode, and audio and MIDI files dropped from Explorer (`src/model/arrangement/Import.h`): copied into the project's `Audio` folder, on new tracks named after them and colored by their names.

**1.2 Sections and song outlines (M)**
- The sections strip. Move, duplicate (also as aliases), delete and resize a section across all tracks.
- Outline data files for pop, rock, electronic and hip-hop/trap.
- Apply an outline when creating a project, or later.
- The energy curve and the section tips.
- *Done when:*
  - applying the electronic outline creates its seven sections at the project tempo;
  - moving the Drop moves everything in it (unit-tested).
- *Status:* done, pending a check by hand. Sections are Tracktion's arranger clips with a role, a tip and an energy level (`src/model/arrangement/Sections.h`). Moving, copying (also as aliases), deleting and resizing a section edit every track (`src/model/arrangement/Arrangement.h`): clips are split at its edges, automation goes along, and Insert and Remove Time stretch sections instead of splitting them. The outlines are data files (`resources/outlines/*.outline.json`, read by `src/model/arrangement/Outlines.h`), offered by **File > New Project**, **Add > Song Outline** and the time bar's right-click menu. The sections show in the playlist's time bar (`src/ui/playlist/SectionsStrip.h`), over the bar numbers with the energy curve faintly behind, and as ghosts behind the tracks; the hint bar shows each section's tip. Both "done when" checks are unit tests (`tests/SectionTests.cpp`). Before this step, the playlist's time line, grid and tools moved into `src/ui/timeline/TimelineEditor.h`, for the piano roll to share.

**1.3 Piano roll (XL)**
- **Editing:**
  - FL Studio's mouse model, the tools, grid and snap;
  - multi-clip editing and note audition.
- **Lanes and visual aids:**
  - the velocity lane;
  - ghost notes, scale shading and chord stamps.
- **Drum mode.**
- **Transforms with preview:** quantize, legato, chop and transpose. More come in phase 2.
- *Done when:* a bass line and chords are quick to write, and drum mode works with the drum sampler.
- *Status:* done, pending a check by hand. The piano roll (`src/ui/pianoroll/PianoRoll.h`) shares the playlist's time line, grid and tools (`src/ui/timeline/TimelineEditor.h`) and shows the notes of the MIDI clips selected in the playlist, several at once, each in its clip's color; double-clicking a clip opens it. FL Studio's mouse model: a click adds a note with the last length and velocity (a drag that follows moves it), right-click deletes, Shift+right-click opens the menu, the edges resize, Ctrl+drag selects in a rectangle and Shift+drag copies. Paint, Slice, Mute, Delete, Select and Zoom work on notes, and notes play on their track's instrument as they are placed. The velocity lane, ghost notes from other tracks (double-right-click one to edit its clip), shading outside the song's key with optional snapping, chord stamps, and drum mode (a sampler's pads, or the General MIDI drums, in an order the track keeps, with rows that can be hidden). Quantize (strength, swing, ends), legato, chop and transpose (in semitones or steps of the key) show their result before it is applied. The note edits are commands (`src/model/notes/Notes.h`, with tests in `tests/NoteTests.cpp`); with notes selected, the note keys come before the playlist's (`Command::keyPriority`). Drum mode with the drum sampler waits for step 1.6, which brings the sampler; until then it works with any instrument.

**1.4 Mixer (L)**
- Channel strips: fader, pan, mute, solo, meters, and device slots with key controls.
- Sends and returns (pre/post), folder buses, and a master with a loudness meter.
- *Done when:* a mix with a reverb return and a bus works and matches the playlist's tracks.
- *Status:* done, pending a check by hand. The mixer (`src/ui/mixer/MixerView.h`) has a strip for every track, in the playlist's order and colors (`src/ui/mixer/ChannelStrip.h`): the track's name and output, its devices (an on/off switch and two key controls each; click to open its window, right-click to turn it off, move or remove it), its sends with a level and a Pre/Post switch, pan, mute, solo, fader and meter. A folder becomes a bus with its first device and turns plain again without one; return tracks take an aux bus (`src/model/mixer/Mixer.h`, with tests in `tests/MixerTests.cpp`). The master's strip has its devices, fader and meter, and the loudness meter: a plug-in at the end of the master's devices that wraps Tracktion's real-time-safe EBU R128 meter (`src/model/engine/LoudnessMeterPlugin.h`). Devices open in windows of their own (`src/ui/devices/PluginWindow.h`), with a knob for each parameter of a built-in device. Until step 1.6 the device list offers Tracktion's reverb and 4OSC synth, besides scanned plug-ins ([real-time audit](realtime-audit.md), finding 16). A guarded real-time audit scenario plays the "done when" mix: a bus, a send to a reverb return and the loudness meter.

**First feedback round on steps 1.1 to 1.4 (2026-10-13)**, after the owner tried them ([design decision log](design.md#26-decision-log)):
- **Fixes:** dismissing the track color menu crashed the app (two other menus had the same mistake); a held key repeated its command, so holding a tool's key ended a drag after a copy or two and holding Space started and stopped playing; every mouse-wheel step was handled twice in the playlist and the piano roll.
- **Playlist:** track colors run through their rows; dragging past an edge scrolls; "+ Track" moved over the track headers, with a row after the last track that adds one; the sections became a band in the time bar and ghosts behind the tracks; four more outlines.
- **Piano roll:** bar 1 is the clip's start; a selected track shows its clip nearest the playhead, and on a track without clips the first note starts one; a key is a track's, off until chosen; no Drums button.
- **Mixer:** strips keep their parts apart at any height, and the mixer scrolls when it is shorter than they need.
- **Keys and menus:** tool keys spring back after a hold; the digits 1 to 9 pick tools and M turns the computer keyboard on and off; the menu bar is File, Edit, Add, View, Options and Help.
- **Tests that drive the UI:** the components are in a library of their own (`phazite_ui`), and `tests/UiTestHelpers.h` puts them in a window that isn't on any screen, so tests send them mouse and key events through JUCE as the app does (`tests/PlaylistTests.cpp`, `tests/PianoRollTests.cpp`, `tests/MixerViewTests.cpp`).

**Second feedback round (2026-10-13):**
- **The loop:** playback loops the whole song and grows with it, as FL Studio's song mode does (`src/model/transport/Playback.h`); a new project looped a fixed 4 bars, so clips placed later never played. Loop Selection and Loop Section loop a part on purpose, and Loop the Song goes back. A Loop button in the top bar (L), and the loop drawn in the rulers.
- **Track headers:** a handle moves the track, so clicks elsewhere on the header don't; an arrow collapses a track to a thin row (and a folder's tracks out of sight), as do U and dragging the bottom edge past the smallest height; stronger track colors, with names in black or white, whichever reads better; dragging along the headers selects the tracks it passes, and Alt+click on an arrow collapses every track.
- **FL Studio's keymap:** a click on empty space places the last clip used, as FL's Draw tool does; a time range selected with Ctrl+drag loops while it is selected; Shift+right-click opens the menu.
- **Mixer, redesigned after research into the mixers people praise** ([design](design.md#8-the-mixer)): a strip reads top to bottom as the sound goes; the instrument has a slot of its own above the effects, with menus that list only instruments or only effects (`src/ui/devices/DeviceMenus.h`); a track whose notes have no instrument asks for one in the mixer, the inspector and the piano roll. "+ Send" makes a return with an effect in one step; returns are solo-safe and sit next to the master; folders have a Bus switch; selecting a channel outlines what it feeds and what feeds it.
- **Mute and solo:** one control for the playlist and the mixer (`ui::MuteSoloButtons`); a solo never changes the mute switches, right-click M solos a track alone, and a track silenced by solos shows its M half lit.
- **The loop, refined after research:** Ctrl+L loops the selection, or the song with none; playback that doesn't loop stops at the song's end.

**1.5 Browser (L)**
- Folders, the plug-in list, search, tags and favorites.
- Preview, with loops in time and key.
- Drag-and-drop rules and the plug-in picker (F8).
- *Done when:* finding and placing a kick takes seconds, and loops preview in time.
- *Status:* done, pending a check by hand. The browser (`src/ui/browser/Browser.h`) is a tree of places, as FL Studio's: Favorites, Recent, Sounds by Type, Phazite Sounds, Instruments, Effects, the user's folders and **Add Folder...**; search finds sounds and devices by their names and tags as you type (`src/services/Library.h`, which indexes the sounds on a background thread). Clicking a sound or moving to it with the arrow keys previews it (`src/model/engine/Preview.h`): loops in time with the project, stretched by SoundTouch, and on its bars while it plays. Dragging puts sounds and devices onto the playlist (a track, or below the tracks for a new one) and devices onto mixer channels; Enter puts the selected item on the selected track at the play start. The plug-in picker (F8, `src/ui/browser/PluginPicker.h`) adds a device by name. Tests: `tests/LibraryTests.cpp` and `tests/BrowserTests.cpp` (finding and placing a kick, and the loop previewing in time, are both there). Phazite Sounds: 85 CC0 sounds for a new user to start with (9.3 MB): drums, 808s, loops, effects and foley from CC0 libraries and Phazite's own synthesis. They are an optional download from their own repository, [phazite-sounds](https://github.com/august-byrne/phazite-sounds), whose CI checks that each is listed as CC0 with its source; `tools/soundkit/` renders and zips them. Phazite offers them once, in a welcome window (`src/ui/shell/Welcome.h`), and in the browser until they are installed; `src/services/SoundPack.h` downloads the pinned release, checks its SHA-256 and unpacks it into `Documents/Phazite/Packs` (tests: `tests/SoundPackTests.cpp`, `tests/WelcomeTests.cpp`). Still to come in later steps: drops onto device slots and drum pads (step 1.6), loops in key once songs have a key, previews through the track's devices, tempo detection for loops whose names don't give one, and collections.
- **Fixes from the third round of feedback (2026-10-13):** Space stops back at the play start (the last click in a ruler, or the song's start in a project just opened), and Shift+Space (Ctrl+Space in FL Studio's keymap) pauses; play, stop and the computer keyboard work while a plug-in window has the focus (`src/ui/input/PluginKeys.h`); each effect has a Mix knob; the inspector's channel strip fills its height.

**Architecture phases A0 and A1 (2026-10-14 to 2026-10-15, done)**, before step 1.6: golden tests, the architecture check, and untangling the model, with no new features ([architecture track](#architecture-track)).
- **Fixes the golden tests found (2026-10-14),** where they showed wrong behavior; the golden files change only where a fix says so:
  - Go to End goes to the end of the song, the end of the bar its last clip ends in, not to the end of its sections.
  - A note edit makes its clip longer only into the empty space after it, never over the next clip on its track.
  - Opening a project with MIDI clips no longer counts as an unsaved change: Phazite compares the project with how it was opened or last saved, since Tracktion sends notifications that change nothing.
  - A new project's starting content (the starter song, an outline's sections) is part of its first save: no `*` right after it is made, and Undo doesn't remove it.
  - Duplicate (Ctrl+D, Ctrl+B in FL Studio's keymap) works on a selected time range: what lies in it goes again right after it, and the range moves to the copy (Undo moves it back).

**1.6 First built-in devices (XL)**
- The first devices:
  - drum sampler: pads, choke groups, tuning and envelope;
  - subtractive synth;
  - parametric EQ with an analyzer;
  - compressor with sidechain;
  - reverb;
  - limiter.
- Each is real-time safe and has an editor and DSP tests.
- Each device's DSP goes in `src/devices/` without Tracktion types, in a thin Tracktion plug-in, so that it could run on another engine later ([architecture track](#architecture-track), A5). Devices render the same twice in a row offline (seeded randomness), so the golden render tests can cover them.
- These devices replace Tracktion's 4OSC in the starter project.
- *Done when:* a beat can be made and mixed with built-ins only, and every device passes the real-time checks from step 0.3.
- *Status:* in progress (2026-10-15). In: the devices' layer (`src/devices/`, a fifth layer in the architecture check, with DSP building blocks and their tests), the thin Tracktion plug-in that runs any of them (`src/model/devices/DevicePlugin.h`, with each effect's latency-aligned mix), handles through which editors reach devices without Tracktion types, and the EQ with its own editor and analyzer, offered in the device menus (projects that hold one are format version 2). Next: the compressor, limiter, reverb, synth and drum sampler, then the starter project.

**1.7 Transport, MIDI recording and Capture (M)**
- Play, stop and loop; metronome and count-in.
- Editing tempo, key and time signature.
- MIDI recording (overdub or replace), and Capture.
- *Done when:* playing an idea and then pressing Capture turns it into a clip.

**1.8 Export (S)**
- Render to WAV and FLAC with loudness normalization.
- *Done when:* an exported file plays correctly and meets the chosen loudness target.

**1.9 First launch and starter project (M)**
- The first-run flow: "Coming from…?" and audio setup.
- The starter project and the hint-bar guidance.
- The new-project dialog with outlines.
- *Done when:* a newcomer gets sound in under a minute without help.

**Milestone 1 check:** a usability test with one newcomer and one FL Studio user.
- **Task:** each makes an 8-bar beat from an outline, with drums, bass and chords, mixes it with built-ins and exports it.
- **Outcome:** their hesitations and errors decide the fixes, and settle the keymap and the details of the Smart Tool.

## Phase 2: Finish a track (milestone 2)

**2.1 Automation lanes (L)**
- The lane editor, drawn by Phazite over the model's automation data: it reads points as plain values and changes them through model functions and commands, one named undo step per gesture. Not Tracktion's `CurveEditor`, a component that edits Tracktion's objects directly, in its own style. It comes after architecture phase A3 for the playlist and the mixer.
- Right-click → Automate, the write modes and shapes, keyboard editing, and the last-touched hotkey.
- *Done when:* any built-in or plug-in parameter can be automated in two clicks, and the four write modes behave as described.

**2.2 Automation clips (M)**
- The automation clip type, "Create automation clip", and aliased automation clips.
- *Done when:* an automation clip made from a plug-in knob drives it, and its aliases follow edits.

**2.3 Modulators and macros (L)**
- Add a modulator and assign it by dragging. Rings on controls show the modulation.
- Editors for the six modulator types, and macros.
- *Done when:* an LFO dragged onto a filter cutoff moves it, with the amount shown on the knob.

**2.4 Containers and presets (L)**
- Group devices into containers with macros.
- Device, track and container presets; drag a track into the browser to save it.
- *Done when:* a container saved as a preset loads into another project with its macros working.

**2.5 Chord lane and progression outlines (L)**
- Phazite's chord format and the chord lane.
- The progression library (about 40): audition and apply, and follow key changes.
- Chords written to the engine, and chord highlighting in the piano roll.
- Generated chord, bass and arpeggio parts.
- *Done when:*
  - picking a progression for a section plays it in the project key;
  - a bass part generated from it follows the chord lane.

**2.6 Sound generator v1 (L)**
- **The drum synth device:** kick, 808, snare, clap, hats, tom, rim and percussion.
- **The generator panel:**
  - a grid of sounds, with audition in context;
  - keep, favorite and lock;
  - more like this, and history.
- **Project fit and recipes:** sounds fit the project's key, tempo and loudness, and each keeps its recipe.
- *Done when:* generating, auditioning in the loop and keeping a kick tuned to the song's key works end to end, and a kept sound reopens with its recipe.

**2.7 Mixer depth (M)**
- The sidechain source picker, the routing view, and a per-track CPU meter.
- *Done when:* a kick-to-bass sidechain is set up from the picker and shows as a cable in the routing view.

**2.8 More devices (XL)**
- Multiband dynamics and a maximizer in the style of Maximus and OTT.
- A tempo-synced delay, a saturator, a filter and a utility.
- A better synth.
- *Done when:* each device passes its DSP and real-time tests and holds up in A/B listening against reference tools.

**2.9 Freeze, bounce in place and stems (M)**
- *Done when:* a heavy plug-in track can be frozen and unfrozen, and exporting stems produces one file per track or bus.

**2.10 Plug-in windows and plug-in presets (M)**
- *Done when:* plug-in windows remember their positions and plug-in states save as presets.

**Milestone 2 check:** produce a complete track, from first idea to master, with automation and modulation, using Phazite and your own plug-ins.

## Phase 3: v1 polish (milestone 3)

**3.1 Chord suggestions (L)**
- The theory engine, which scores candidate chords and explains its choices.
- Next chord, fill a section, replace, and fit my melody.
- The safe ↔ adventurous slider.
- Local learning from accepted suggestions.
- Optionally, pop and rock calibration from the public-domain McGill Billboard data.
- *Done when:*
  - suggestions respond to the project's key, melody and section;
  - each suggestion explains itself;
  - accepting one is a single undo step.

**3.2 Sound generator, part 2 (M)**
- Breeding favorites.
- Kit-aware generation, which complements the drums already used.
- Automatic tags.
- *Done when:* breeding two favorite kicks produces new kicks with traits of both.

**3.3 Stability (M)**
- A scan timeout.
- A plug-in that crashed while loading is disabled on the next start.
- Polished crash recovery.
- *Done when:* a deliberately hanging or crashing test plug-in can't stall the scan or lose the project.

**3.4 Audio recording (L)**
- Record-arm, monitoring, punch-in and basic takes.
- *Done when:* recording a vocal or guitar over a loop, with count-in and punch-in, works with an ASIO interface.

**3.5 Performance pass (L)**
- Profile against the targets in [design section 20](design.md#20-performance): idle CPU, large projects, startup time.
- *Done when:* the targets are met, or the gaps are written down with a plan.

**3.6 Accessibility pass (M)**
- Keyboard navigation everywhere, screen-reader names, contrast and scaling.
- *Done when:* core tasks can be done with Narrator and the keyboard alone, and the themes pass WCAG 2.2 contrast.

**3.7 Keymaps and help (M)**
- The FL Studio keymap completed, and the Ableton Live keymap if decided.
- Hover-help texts and short in-app lessons.
- *Done when:* every command has help text, and the keymap presets cover all the core actions.

**3.8 Usability rounds (S each)**
- Repeat the milestone test with new people and fix what they trip over.
- *Done when:* new users finish the milestone task without help.

## Phase 4: v1 release

**4.1 Installer (M)**
- A Windows installer and uninstaller, with file associations.
- An optional, opt-in update check.

**4.2 Licences and notices (S)**
- Third-party notices in the app and installer, LGPL compliance for SoundTouch, and Inter's OFL text and copyright notice.
- An About box and trademark notices, including the ASIO logo question.

**4.3 Code signing (S, decision)**
- A certificate, or an open-source signing program, so Windows doesn't warn users.

**4.4 Documentation (M)**
- The basics of a user guide, a README update and a contributor guide.

**4.5 Release process (S)**
- Versioning, a changelog and release builds from CI.
- A Linux build as a beta.

## After v1

Roughly in order of value:

1. Plug-in sandboxing, on by default (XL).
2. CLAP hosting (L).
3. The clip launcher, as a jam panel that records into the playlist (L).
4. The AI assistant, acting through commands (L).
5. Audio take lanes and comping (L).
6. DAWproject import and export (M).
7. Neural sound generation with our own model, trained on public-domain audio (XL).
8. Audio-to-MIDI, automatic sample tags and "find similar" (L).
9. macOS, with Audio Units (L).
10. MPE controllers, Linux screen readers (AT-SPI), translations and a scripting API (M–L each).

## Cross-cutting work

- **Testing:**
  - unit tests for model operations;
  - DSP tests: frequency responses, gain curves, null tests;
  - golden tests (architecture phase A0): saved projects compared with committed files, and short offline renders with committed fingerprints, within a tolerance;
  - usability tests at each milestone.
- **Architecture:** keep to the rules in `CLAUDE.md`, which `tools/check-architecture.sh` checks; each step moves the area it touches toward the [architecture track](#architecture-track)'s structure.
- **Performance:** benchmarks for audio CPU and UI frame time, tracked from phase 1 onward.
- **Documentation:** keep [design.md](design.md) and this roadmap current, and update the status table after each step.
- **Licences:** check every new dependency, model or dataset against the list in `CLAUDE.md` before using it.

## Risks

| Risk | Mitigation |
|---|---|
| The UI is by far the largest piece of work | Build vertical slices, run usability tests at each milestone, and use engine features instead of reinventing them |
| Engine code breaks the real-time rules | Measured in step 0.3; workarounds or a fork are decided then |
| Built-in effects fall short of FL Studio's | Listening and A/B tests against references, plus DSP tests |
| Sandboxing and CLAP are complex | Scheduled after v1; v1 gets the stability basics |
| Scope creep | v1 scope is fixed in [design section 1](design.md#1-vision-and-scope); new ideas go under "After v1" |
| Restructuring changes behavior by accident | Golden tests of saved projects, renders and commands (A0); every step builds and passes the tests, one commit each |
| The architecture work holds up features | Only A0 and A1 pause features; A2 to A4 move each area when a feature touches it; A5 only after a dated decision |
| The layers erode over time | `tools/check-architecture.sh` in CI and before every commit, with limits that can only go down |

## Status

| Phase | Status |
|---|---|
| 0. Foundations | Done, pending checks by hand (0.4: crash recovery; 0.6: Narrator and idle CPU; 0.7: MIDI and computer keyboard) and Linux CI ([real-time audit](realtime-audit.md)) |
| 1. Make a beat | In progress: steps 1.1 to 1.5 (the playlist, sections and song outlines, the piano roll, the mixer, the browser) done, with three rounds of fixes from trying them (2026-10-13), pending checks by hand; step 1.6 in progress (the EQ in) |
| 2. Finish a track | Not started |
| 3. v1 polish | Not started |
| 4. v1 release | Not started |
| Architecture track | A0 (2026-10-14) and A1 (2026-10-15) done; A2 in progress alongside steps 1.6 to 1.9: drags have a gesture session in the model, and knobs and faders change parameters through it (the UI's undo steps 46 to 44) |

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
