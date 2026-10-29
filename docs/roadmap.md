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
| 5. MP3 export | **Decided:** none; WAV and FLAC |
| 6. Airwindows effects | **Decided:** plug-ins that aren't Phazite's own are never built in; optional downloads (step 2.10) |
| 7. Sandboxing before or after v1 | **Decided:** before v1, on by default, unless it costs a lot of performance (step 3.3) |
| 8. Ableton Live keymap in v1 | **Decided:** yes (step 3.7) |
| 9. Single-key tools and the computer keyboard | **Decided:** Ableton's way, Shift and a letter for the shortcut while the keyboard plays notes |
| 10. The computer keyboard at startup | **Decided:** off until turned on |
| 11 to 19 (the mixer, plug-in windows, newer projects, loops, duplicated ranges) | **Decided** by Claude, for the owner to review (2026-10-16): folders are buses (done), a strip's Out menu and a send knob for every return (step 2.9), empty new tracks and no reverb return (as now), keys to plug-ins per plug-in (step 2.12, done), newer projects not opened and duplicates replacing (as now), the loop's and metronome's switches not unsaved changes (done) |

## Architecture track

*Added 2026-10-14* ([design section 24](design.md#24-code-structure)). This track moves the code toward the structure in the design without a rewrite. Its phases are called A0 to A5 (phases 0 to 5 of the architecture review), so they aren't confused with the phases below.
- **Small steps:** after each one the app builds, behaves the same and passes its tests, golden tests included, and each is its own commit.
- **Alongside the features:** only A0 and A1 pause feature work. Later phases move each area of the code when a step touches it.

| Phase | Goal | When |
|---|---|---|
| A0. Safety net and limits | Make restructuring safe, and stop new breaks of the rules | Before step 1.6 (done 2026-10-14) |
| A1. Untangle the model | One job per folder, and no UI code in the model | Before step 1.6 (done 2026-10-15) |
| A2. One path for every edit | The UI decides nothing about the project | Alongside steps 1.6 to 1.9 |
| A3. Phazite's types at the UI boundary | The UI knows nothing about Tracktion | One view at a time; the playlist and the mixer before step 2.1 (done 2026-10-16) |
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
- *Status:* done (2026-10-14): the architecture check runs in CI's Checks job (with dash and mawk), as pre-commit check 7 and in pre-push; the golden tests of projects, the 0.1.0 format, a render and the model's commands pass on Windows, and in CI on Linux too since 2026-10-15. The 0.1.0 project went on 2026-10-18, when the owner decided that pre-alpha keeps no backward compatibility (design.md, section 18).

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
  - the model gets Tracktion from `src/model/base/internal/Tracktion.h` only, and no model file is on an include loop;
  - `src/model/base/internal/Schema.h` names everything a project saves, with the format version (1) and the migrations that convert older projects when they open (`src/model/base/Migrations.h`): the grid's fixed size and keys' scales are saved as Phazite's own names, and a project from a newer Phazite isn't opened;
  - the command registry keeps the commands, their undo steps, state and search; keys, keymaps (with the playlist's mouse options) and JUCE's menus are in `src/ui/input/`, with the main window's keys in `WindowKeys`; `phazite::UndoStep` is the one undo convention, and `ProjectContext` the one way to the open project;
  - `src/services/` holds the settings, with one list of keys, and recent projects; the app owns the sound pack, the library and the previewer;
  - the computer keyboard, the Smart Tool, the time line's zoom, the grid's spacing in pixels, track heights in pixels, the drum rows' order, drag-and-drop payloads and the texts for values are UI code, and Tracktion's hook for devices' windows is the app's (`AppUIBehaviour`);
  - the commands are tables of named model functions, each with tests;
  - view state no longer counts as an unsaved change.
- *Done when, checked 2026-10-15:*
  - `sh tools/check-architecture.sh` counts 0 for model-window-includes, model-window-references, model-tracktion-includes, model-undo-steps, model-include-loops and flat-files, and 0 for unresolved-includes and cmake-sources too;
  - every command the model registers calls a named model function that tests call without the registry. The views' own commands mostly open or show something; the few that still edit in the UI, such as Quantize and the piano roll's Apply, move into the model with A2's direct edits;
  - the golden files changed only for three of the fixes the golden tests found and for the conversion to format version 1, besides a new file that lists the commands the app registers.
- *Since 2026-10-18:* the format versions and migrations are gone, and question 17 waits: pre-alpha keeps no backward compatibility until the owner says so (design.md, section 18).

**A2. One path for every edit (L, alongside steps 1.6 to 1.9)**
- Model functions open their own named undo steps.
- A drag is a gesture session that begins, updates, then commits or rolls back as one step.
- The UI's direct edits (about 20) and the editing rules in UI files (where dropped files go, what Draw places, that sections never overlap, and others) move into model functions with tests.
- The big files split: the playlist into layout, clip tools, drop target, commands and view; the piano roll into session, note layout, note tools and commands; the channel strip into one file per class. The browser gets a model, and the playlist and the piano roll share one set of gesture code.
- *Done when:* the UI starts no undo steps and makes no direct edits, and no source file is much over 1,000 lines.
- *Status:* done (2026-10-15), alongside step 1.6, pending a check by hand that editing works as before. Each step was a commit of its own that built and passed the tests, golden tests included:
  - model functions open their own named undo steps (`phazite::UndoStep`), and a drag holds a `phazite::Gesture` (`src/model/commands/internal/Undo.h`) from its first change until the mouse goes up; knobs, faders and the mix knob change parameters through one. A gesture can also roll its changes back, which no tool offers yet (Escape during a drag could);
  - the sections strip, the track header, the device menus, the mixer strip, the piano roll (the key menu, velocities, transforms and the note tools) and the playlist (dropped files, double-clicks, adding, coloring and moving tracks, and the clip tools) edit through model functions, and the rules they held moved with them: where dropped files go (`importFiles()`), what Draw and Paint place (`placeClip()`) and what Slip does (`slipClip()`); the mute and solo buttons use the track switches the commands use;
  - the playlist is split into a file per part (`PlaylistParts.h`: the view, the rows, the tools, drops, and the track headers with the commands), the piano roll likewise (the view, the rows, the commands, the note grid and its tools), and the mixer strip into one file per class;
  - the browser has a model (`src/services/BrowserModel.*`): its places, what each holds, search results and its rows' texts, without window code;
  - the playlist's and the piano roll's tools share one gesture (`src/ui/timeline/TimelineGesture.*`).
- *Done when, checked 2026-10-15:*
  - `sh tools/check-architecture.sh` counts 0 UI undo steps (46 when A2 began), and the UI's Tracktion references went from 538 to 518;
  - a search of `src/ui/` finds no call that changes the project except through a model function (or `ui::ParameterAttachment`, until A3);
  - no source file is over 1,000 lines: the largest, `src/ui/timeline/TimelineEditor.cpp`, has 977.

**A3. Phazite's types at the UI boundary (XL, one view at a time)**
- IDs for tracks, clips and devices, built on Tracktion's `EditItemID`; stable note IDs saved in the project; selections held as IDs.
- A view model per view (playlist, piano roll, mixer, transport), with plain values and grouped change signals.
- Meters and the playhead read through small polled sources, ending each mixer strip's own polling.
- Parameter handles for knobs and faders, replacing `ui::ParameterAttachment`, and a waveform source for clips.
- One view at a time; then `ui/` loses Tracktion's include path, and the model's Tracktion-using headers move into `model/<area>/internal/`.
- Tests build songs with shared helpers.
- *Done when:* `src/ui/` has no Tracktion references and can't include Tracktion, and the model's public headers are free of Tracktion types.
- *Status:* done (2026-10-16), a commit per view: Phazite's own beats and IDs (`src/model/base/Beats.h`, `IDs.h`); views that follow the project context, with the selection by IDs (`SongSelection`) and what changed in the song in groups (`SongChanges`, `src/model/project/ProjectContext.h`); and the mixer: a model of it by IDs (`src/model/mixer/MixerModel.h`, with tests), through which the strips, the inspector, the device menus and the top bar's master controls work without Tracktion's types, the meters reading polled level sources and the strips refreshing when the song changes instead of polling it; parameter handles made in the model only (`internal/` headers), and the devices' windows moved into the app (`src/app/PluginWindow.h`); the transport and the tempo as the top bar shows them (`src/model/transport/TransportModel.h`, a tempo drag one undo step through a handle), and the engine as the UI reaches it (`src/model/engine/EngineContext.h`: the audio devices, the CPU and the MIDI inputs) for the top bar and the settings; and the time line that the playlist and the piano roll share, its ruler, grid, zoom, playhead and loop in Phazite's beats (`src/model/arrangement/TimelineModel.h`), its tools' drags each one undo step through `SongGesture`, while the two editors turn their clips' and notes' beats into Tracktion's at its edges until their own steps; and the playlist by IDs (`src/model/arrangement/PlaylistModel.h`, with tests): its rows, the clips and what it draws of them, its tools' edits and what is dropped on it, with the time range selected across tracks by beats and IDs (`SongRange.h`) and the audio clips' waveforms drawn by the app (`src/app/ClipWaveforms.h`), the playlist refreshing its rows and clips when the song changes; its sections strip by IDs, with a test that drives it with the mouse; and notes' IDs, saved in the project: each note has one that no other note of its clip has, which it keeps through edits, undo and redo, and a copy of a clip keeps them; and notes as plain values, in Phazite's beats with its own scales and chords (`src/model/notes/NoteValues.h`), the transforms keeping the IDs of the notes they replace; and the piano roll by note IDs (`src/model/notes/NotesModel.h`, with tests): the clips it shows and their notes, ghost notes, its tools' edits (a drag's notes set in one call per mouse move), keys, drum rows and auditioning, with the notes selected by their references in the song's selection, and a test that drives its tools with the mouse; and the browser: its previewer and library made without Tracktion in their headers (the previewer made by the app, `src/model/engine/internal/Preview.h`; the library through the engine context); and last, the headers the UI includes made free of Tracktion (the project context, the command registry, song outlines, export and new projects, their Tracktion sides in `internal/` headers, and the track colours that menus show), the model's 30 other Tracktion-using headers moved into their areas' `internal/` folders (the prelude is `src/model/base/internal/Tracktion.h`), and `phazite_ui` compiled without Tracktion's include path, the architecture check counting any `internal/` header the UI includes. Tracktion references in `src/ui/`: 518 when A3 began, none at its end.

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
- The first bigger part: time-stretching (step 2.18, the owner's go-ahead of 2026-10-28 to depart from Tracktion for subsystems where the best solution comes down to that).
- Tracktion's Edit stays the project store until something replaces it; the one schema file makes a converter manageable.
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
- *Status:* done for `app/`, `model/` and `ui/`. Since 2026-10-14 the [architecture track](#architecture-track) organizes them by area, adds `services/`, and makes `app/` a library the tests link. Built-in DSP goes in `devices/` from step 1.6; the chord engine and the sound generator (steps 2.7, 3.1 and 3.2) follow the same rules: no Tracktion or window code in their logic.

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
- *Status:* done (`src/model/project/internal/Project.h` with tests, and the app's `src/app/ProjectSession.h`): new, open, open recent and save; an autosave every 2 minutes; the restore question at startup. Still to try by hand: end Phazite in Task Manager mid-session, then start it again. Since 2026-10-14 a project counts as changed only when it differs from how it was opened or last saved.

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
- *Status:* done: **Options > Settings** with Audio (device type including ASIO, device, sample rate and buffer size), MIDI (each MIDI input with an on/off switch, and the computer keyboard) and Shortcuts tabs; the computer keyboard (`src/ui/input/ComputerKeyboard.h`) and live MIDI routing (`src/model/engine/internal/LiveInput.h`), with tests. Still to try by hand: a MIDI keyboard and the computer keyboard playing the starter synth.

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
  - **Part 1, done:** the timeline (`src/ui/timeline/TimelineView.h`: positions in beats and zoom; `src/model/arrangement/internal/Grid.h`: a grid that adapts to the zoom or has a fixed size, with snap and triplets) and the tracks (`src/model/arrangement/internal/Tracks.h`: add, delete, duplicate, move, select, mute, solo, colors from the color-blind-safe palette, heights). The playlist (`src/ui/playlist/Playlist.h`) has the ruler, the playhead, scrolling, zoom and track headers that rename, recolor, resize and move tracks; live MIDI and the inspector follow the selected track.
  - **Part 2, done:** clips (`src/ui/playlist/ClipComponent.h`): audio clips with waveforms and fades, MIDI clips with their notes, and aliases marked with a `*`. The Smart Tool (`src/ui/timeline/SmartTool.h`, `src/model/arrangement/internal/Clips.h`): trim at the edges, fades at the top corners, a time range from the upper half or empty space, and moving, copying (Ctrl) or aliasing (Ctrl+Shift) from the lower half, across tracks, snapped unless Alt is held, each drag one named undo step. Delete, duplicate and select all work on clips (`src/model/arrangement/internal/Selection.h`), and double-clicking a track with an instrument adds a MIDI clip.
  - **Part 3, done:** the single-key tools (Draw, Paint, Slice, Slip, Mute, Delete, Select and Zoom, on FL Studio's letters, with toolbar buttons), and editing commands (`src/model/arrangement/internal/Selection.h`): split, duplicate as alias, make unique, mute, nudge, quantize clip starts, delete and close the gap, insert and remove time, crossfades from the Smart Tool and fade shapes. A time range is a selection of its own (`phazite::RangeSelection`) that Delete, Duplicate, Split and the time commands act on.
  - **Part 4, done:** folders that group tracks (grouped, ungrouped, collapsed; plain until they hold an effect, then buses), pinned tracks in a band that doesn't scroll, focus mode, and audio and MIDI files dropped from Explorer (`src/model/arrangement/internal/Import.h`): copied into the project's `Audio` folder, on new tracks named after them and colored by their names.

**1.2 Sections and song outlines (M)**
- The sections strip. Move, duplicate (also as aliases), delete and resize a section across all tracks.
- Outline data files for pop, rock, electronic and hip-hop/trap.
- Apply an outline when creating a project, or later.
- The energy curve and the section tips.
- *Done when:*
  - applying the electronic outline creates its seven sections at the project tempo;
  - moving the Drop moves everything in it (unit-tested).
- *Status:* done, pending a check by hand. Sections are Tracktion's arranger clips with a role, a tip and an energy level (`src/model/arrangement/internal/Sections.h`). Moving, copying (also as aliases), deleting and resizing a section edit every track (`src/model/arrangement/internal/Arrangement.h`): clips are split at its edges, automation goes along, and Insert and Remove Time stretch sections instead of splitting them. The outlines are data files (`resources/outlines/*.outline.json`, read by `src/model/arrangement/Outlines.h`), offered by **File > New Project**, **Add > Song Outline** and the time bar's right-click menu. The sections show in the playlist's time bar (`src/ui/playlist/SectionsStrip.h`), over the bar numbers with the energy curve faintly behind, and as ghosts behind the tracks; the hint bar shows each section's tip. Both "done when" checks are unit tests (`tests/SectionTests.cpp`). Before this step, the playlist's time line, grid and tools moved into `src/ui/timeline/TimelineEditor.h`, for the piano roll to share.

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
- *Status:* done, pending a check by hand. The piano roll (`src/ui/pianoroll/PianoRoll.h`) shares the playlist's time line, grid and tools (`src/ui/timeline/TimelineEditor.h`) and shows the notes of the MIDI clips selected in the playlist, several at once, each in its clip's color; double-clicking a clip opens it. FL Studio's mouse model: a click adds a note with the last length and velocity (a drag that follows moves it), right-click deletes, Shift+right-click opens the menu, the edges resize, Ctrl+drag selects in a rectangle and Shift+drag copies. Paint, Slice, Mute, Delete, Select and Zoom work on notes, and notes play on their track's instrument as they are placed. The velocity lane, ghost notes from other tracks (double-right-click one to edit its clip), shading outside the song's key with optional snapping, chord stamps, and drum mode (a sampler's pads, or the General MIDI drums, in an order the track keeps, with rows that can be hidden). Quantize (strength, swing, ends), legato, chop and transpose (in semitones or steps of the key) show their result before it is applied. The note edits are commands (`src/model/notes/internal/Notes.h`, with tests in `tests/NoteTests.cpp`); with notes selected, the note keys come before the playlist's (`Command::keyPriority`). Drum mode with the drum sampler waits for step 1.6, which brings the sampler; until then it works with any instrument.

**1.4 Mixer (L)**
- Channel strips: fader, pan, mute, solo, meters, and device slots with key controls.
- Sends and returns (pre/post), folder buses, and a master with a loudness meter.
- *Done when:* a mix with a reverb return and a bus works and matches the playlist's tracks.
- *Status:* done, pending a check by hand. The mixer (`src/ui/mixer/MixerView.h`) has a strip for every track, in the playlist's order and colors (`src/ui/mixer/ChannelStrip.h`): the track's name and output, its devices (an on/off switch and two key controls each; click to open its window, right-click to turn it off, move or remove it), its sends with a level and a Pre/Post switch, pan, mute, solo, fader and meter. A folder becomes a bus with its first device and turns plain again without one; return tracks take an aux bus (`src/model/mixer/internal/Mixer.h`, with tests in `tests/MixerTests.cpp`). The master's strip has its devices, fader and meter, and the loudness meter: a plug-in at the end of the master's devices that wraps Tracktion's real-time-safe EBU R128 meter (`src/model/engine/internal/LoudnessMeterPlugin.h`). Devices open in windows of their own (`src/app/PluginWindow.h`), with a knob for each parameter of a built-in device. Until step 1.6 the device list offers Tracktion's reverb and 4OSC synth, besides scanned plug-ins ([real-time audit](realtime-audit.md), finding 16). A guarded real-time audit scenario plays the "done when" mix: a bus, a send to a reverb return and the loudness meter.

**First feedback round on steps 1.1 to 1.4 (2026-10-13)**, after the owner tried them ([design decision log](design.md#26-decision-log)):
- **Fixes:** dismissing the track color menu crashed the app (two other menus had the same mistake); a held key repeated its command, so holding a tool's key ended a drag after a copy or two and holding Space started and stopped playing; every mouse-wheel step was handled twice in the playlist and the piano roll.
- **Playlist:** track colors run through their rows; dragging past an edge scrolls; "+ Track" moved over the track headers, with a row after the last track that adds one; the sections became a band in the time bar and ghosts behind the tracks; four more outlines.
- **Piano roll:** bar 1 is the clip's start; a selected track shows its clip nearest the playhead, and on a track without clips the first note starts one; a key is a track's, off until chosen; no Drums button.
- **Mixer:** strips keep their parts apart at any height, and the mixer scrolls when it is shorter than they need.
- **Keys and menus:** tool keys spring back after a hold; the digits 1 to 9 pick tools and M turns the computer keyboard on and off; the menu bar is File, Edit, Add, View, Options and Help.
- **Tests that drive the UI:** the components are in a library of their own (`phazite_ui`), and `tests/UiTestHelpers.h` puts them in a window that isn't on any screen, so tests send them mouse and key events through JUCE as the app does (`tests/PlaylistTests.cpp`, `tests/PianoRollTests.cpp`, `tests/MixerViewTests.cpp`).

**Second feedback round (2026-10-13):**
- **The loop:** playback loops the whole song and grows with it, as FL Studio's song mode does (`src/model/transport/internal/Playback.h`); a new project looped a fixed 4 bars, so clips placed later never played. Loop Selection and Loop Section loop a part on purpose, and Loop the Song goes back. A Loop button in the top bar (L), and the loop drawn in the rulers.
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
- *Status:* done, pending a check by hand (2026-10-15). In: the devices' layer (`src/devices/`, a fifth layer in the architecture check, with DSP building blocks and their tests), the thin Tracktion plug-in that runs any of them (`src/model/devices/internal/DevicePlugin.h`, with each effect's latency-aligned mix), handles through which editors reach devices without Tracktion types, the EQ with its own editor and analyzer, and the compressor with its editor and a sidechain that listens to another track (`src/model/mixer/internal/Sidechain.h`), the true-peak limiter with its meters, the reverb with a graph of its fades, the synth with a panel of its controls, and the drum sampler with its pads, a synthesized default kit and sound files dropped on pads (which the piano roll's drum rows name), all offered in the device menus. They replace Tracktion's 4OSC and reverb in the device list, and the starter project plays on them only: a beat on the drum sampler and the arpeggio on the synth, with a golden render of it (`tests/golden/renders/starter.json`); MIDI files preview on the synth too.

**1.7 Transport, MIDI recording and Capture (M)**
- Play, stop and loop; metronome and count-in.
- Editing tempo, key and time signature.
- MIDI recording (overdub or replace), and Capture.
- *Done when:* playing an idea and then pressing Capture turns it into a clip.
- *Status:* done (2026-10-15), pending a check by hand. In: the metronome (Tracktion's click, louder on bars' first beats) and a one-bar count-in; the tempo and time signature in the top bar, the tempo a field to drag, scroll or type (a key is a track's, set in the piano roll since step 1.3); MIDI recording onto the track live MIDI plays, with overdub or replace and input quantize, each take one undo step, the song's own loop set aside while it records; and Capture (Ctrl+Shift+C), which sets the tempo from the notes in a song without clips. Tests play notes in real time through Tracktion's hosted audio device and check the takes and captured clips, and the real-time audit guards the metronome and a recording.

**1.8 Export (S)**
- Render to WAV and FLAC with loudness normalization.
- *Done when:* an exported file plays correctly and meets the chosen loudness target.
- *Status:* done (2026-10-15), pending a check by hand. In: File > Export Song (Ctrl+Shift+R; Ctrl+R in FL Studio's keymap) and its window (`src/ui/shell/ExportPanel.h`), which keeps its choices: WAV or FLAC, 44.1 to 96 kHz, 16-bit dithered, 24-bit or 32-bit float, and Normalize to a loudness under a true-peak ceiling. The model (`src/model/project/Export.h`) renders the song to its end with its tails on Tracktion's renderer, in the background with progress and Cancel, into a file that replaces the chosen one only once it is complete, then measures it again for the report, which says when the ceiling kept it under its target. Tests export WAV and FLAC files and check that they play the song, meet -14 and -23 LUFS within 0.07 LU, keep under the ceiling, and leave an older file alone when cancelled. No MP3 ([question 5](design.md#25-open-questions), decided 2026-10-16).

**1.9 First launch and starter project (M)**
- The first-run flow: "Coming from…?" and audio setup.
- The starter project and the hint-bar guidance.
- The new-project dialog with outlines.
- *Done when:* a newcomer gets sound in under a minute without help.
- *Status:* done (2026-10-15), pending the check by hand that its "done when" needs: a newcomer trying it. In: the welcome window's three pages, opened at every first launch ("Coming from another music app?", which picks FL Studio's keymap or Phazite's; the audio output with a test sound; and Phazite Sounds); the starter project with its Synth track selected, so the computer keyboard plays at once; the guide in the hint bar (`src/ui/shell/Guide.h`: play, play notes, add a sound, record, export, each gone once done, and Help > Getting Started Tips); and the new-project window (`src/ui/shell/NewProjectPanel.h`, with `src/model/project/NewProject.h`): name, tempo, time signature, key, song outline, and a Drums and a Synth track to start with.

**Milestone 1 check:** a usability test with one newcomer and one FL Studio user.
- **Task:** each makes an 8-bar beat from an outline, with drums, bass and chords, mixes it with built-ins and exports it.
- **Outcome:** their hesitations and errors decide the fixes, and settle the keymap and the details of the Smart Tool.

## Phase 2: Finish a track (milestone 2)

**2.1 Automation lanes (L)**
- The lane editor, drawn by Phazite over the model's automation data: it reads points as plain values and changes them through model functions and commands, one named undo step per gesture. Not Tracktion's `CurveEditor`, a component that edits Tracktion's objects directly, in its own style. It comes after architecture phase A3 for the playlist and the mixer.
- Right-click → Automate, the write modes and shapes, keyboard editing, and the last-touched hotkey.
- *Done when:* any built-in or plug-in parameter can be automated in two clicks, and the four write modes behave as described.
- *Status:* done (2026-10-16), checked by hand by the owner (2026-10-29): automation in the model, by IDs and plain values (`src/model/automation/AutomationModel.h`, with tests): the parameters each track's devices offer, the lanes a track shows (view state, saved in the project; since 2026-10-17 one for every automated parameter, collapsing rather than hiding), points in beats and their edits as named undo steps, shapes, each track's write mode (Tracktion's touch, latch and write, which record a pass as one undo step), the selected points with Delete, Duplicate and the arrow keys, Automate Last Touched (Ctrl+Shift+A) and Show Automation (A); automation keeps its beats when the tempo changes, and the values automation plays aren't unsaved changes. In the playlist (`src/ui/playlist/PlaylistAutomation.cpp`, with tests that drive it with the mouse): lanes under their tracks, the master's under a Master row, each with a header (a chevron that collapses it, the parameter, the track's write mode) and its curve, which the tools edit (add, move, bend, draw steps or a free line, paint shapes, select, delete); Automate on every knob's and fader's right-click and on a device slot's menu (design.md, section 10).

**2.2 Automation that goes with the music (M)** (automation clips until 2026-10-17)
- Lanes are the one way to automate (the owner's decision, design.md section 10): automation moves and copies with the clips above it and with time ranges selected across tracks, as in Ableton Live, Bitwig and Studio One. Automation clips, built on 2026-10-16, are gone.
- *Done when:* moving, copying or duplicating clips or a selected time range does the same to the automation of their tracks' parameters under them, in the same undo step, and a project saved with automation clips plays the same with lanes.
- *Status:* done (2026-10-17), checked by hand by the owner (2026-10-29): automation clips removed; a project saved with them opened with each clip's curve as its parameter's own automation until 2026-10-18, when that conversion went with the others (design.md, section 18). Automation goes with its clips (`moveAutomation()` and `deleteAutomation()` in `src/model/automation/internal/Automation.h`, tests in `tests/AutomationFollowsClipsTests.cpp` and `tests/PlaylistTests.cpp`): clips dragged, copied with Ctrl, nudged or duplicated along their tracks, clips deleted with Close Gap, and time ranges deleted or duplicated on tracks take their tracks' automation along, in the same undo step; clips dragged to another track leave it. Steady playback of dense lane automation allocates nothing on the audio thread (a guarded real-time audit scenario).

**2.3 Modulators and macros (L)**
- Add a modulator and assign it by dragging. Rings on controls show the modulation.
- Editors for the six modulator types, and macros.
- *Done when:* an LFO dragged onto a filter cutoff moves it, with the amount shown on the knob.
- *Status:* done (2026-10-16), checked by hand by the owner (2026-10-29): modulation in the model (`src/model/modulation/ModulationModel.h`, with tests): each track's modulators of the six kinds and its macros (Tracktion's modifiers and macro parameters), added and removed as named undo steps; what they modulate, by an amount, set, changed and taken away as named steps; a drag of the amount as one step; the range a modulated parameter moves over, for the ring; modulators' parameters with clear names, and a step sequencer's steps. A test plays an LFO on the synth's cutoff in real time and sees it move; another reopens a saved project with its modulators; a guarded real-time audit scenario plays every kind. In the window: the ring around a modulated knob, dragged to change the amount; Modulate on every knob's right-click; a Modulators section in each channel strip, whose grips drag onto knobs and whose names open the modulators' editors (output, knobs, steps), with tests that drive them with the mouse (design.md, section 10).

**2.4 Containers and presets (L)**
- Group devices into containers with macros.
- Device, track and container presets; drag a track into the browser to save it.
- *Done when:* a container saved as a preset loads into another project with its macros working.
- *Status:* done (2026-10-16), checked by hand by the owner (2026-10-29): containers in the model (`src/model/mixer/ContainerModel.h`, with tests in `tests/ContainerTests.cpp`): effects of a channel grouped into a container in the first one's place and ungrouped again, each one named undo step, as Tracktion racks of their own wired one after the other; their eight macros, which modulate their devices, and their devices, which the mixer's functions, automation and the track's modulators reach as any device; a duplicated track's containers play racks of their own; devices are selected on their own in the song's selection, which Group into Container (Ctrl+G) and Ungroup Container (Ctrl+Shift+G) act on. Presets (`src/model/presets/PresetModel.h`, with tests in `tests/PresetTests.cpp`, the "done when" among them): a device's, a container's or a track's in a file of its own, without what ties it to its project, loaded into any project with new IDs as one undo step, a drum sampler's sounds copied in. In the window: Ctrl-click selects devices in the mixer, their menus group, ungroup and save presets; a container's slot shows its mix and macros, and its window (`src/ui/mixer/ContainerEditor.h`) its macros, to turn, rename and drag onto its devices' knobs, and its devices in a row; the browser has a Presets place, saves the selected device's or track's preset (Save Device Preset..., Save Track Preset...) and a track dragged into it by its handle, and presets go into the song as devices do, by a drop or Enter (tests in `tests/ContainerUiTests.cpp`). A guarded real-time audit scenario plays a container with a macro and an LFO at work (design.md, section 9).

**2.5 The window: the whole song in the centre, the selection below (L)** (the owner's decision, 2026-10-17, [design section 4](design.md#4-the-window))
- The centre shows the playlist or the mixer, chosen by tabs at its top (F5 and F9 too); the editor dock shows the piano roll and a new Devices view (F6); the Audio placeholder tab goes.
- The Devices view: the selected track's modulators and macros, then its instrument and effects, in a row and in line. Phazite's own devices show their editors and containers their macros and devices, and their windows go; plug-ins and Tracktion's built-in devices show their key knobs and a button for their windows. A click on a device's name in the mixer shows it there.
- The inspector shows the properties of what is selected (a track, clips, a section, or the song), not a copy of a mixer channel.
- *Status:* done (2026-10-17), checked by hand by the owner (2026-10-29). The centre's Playlist and Mixer tabs (`src/ui/shell/Panels.h`; F5 and F9). The Devices view (`src/ui/mixer/DevicesView.h`, F6; tests in `tests/DevicesViewTests.cpp`): the track's modulators and macros, moved there from the mixer's strips, then panels with Phazite's devices' editors (the EQ's graph shrinks to fit), a container's macros with its devices inside it, and a plug-in's key knobs with a button for its window; the model gives a device's channel and handle by ID (`MixerModel.h`), and Phazite's own devices and containers have no windows of their own any more. The inspector as the properties of the selection (`src/ui/mixer/Inspector.h`; tests in `tests/InspectorTests.cpp`): a track's name, color, volume, pan, mute and solo, output, instrument, devices, automation mode, lanes and sends; clips' kind, place, length, mute, alias and fade shapes (fades' lengths are dragged on the clip); a section's name, color, place, length, energy and tip; and the song's tempo, time signature, length, tracks and sections.
- *Done when:* someone who hasn't used Phazite finds the mixer and the playlist from the tabs without a key, edits an EQ in the Devices view under the mixer, drags an LFO from its first column onto a cutoff, and changes a clip's fade shape and a track's send in the inspector.

**2.6 Audio clip editor (L)**
- The editor dock's editor for audio clips, in the piano roll's tab, which names what it shows: the selected audio clip's waveform, with its gain, fades, pitch, reverse and stretching to the song's tempo. Double-clicking an audio clip opens it, as a MIDI clip's double-click opens the piano roll.
- *Done when:* a vocal clip can be trimmed, faded, turned up, pitched and stretched to the tempo in the editor, each change one undo step.
- *Status:* done (2026-10-17), checked by hand by the owner (2026-10-29) ([design section 7](design.md#the-audio-clip-editor)). An audio clip by ID in the model (`src/model/arrangement/AudioClipModel.h`, with tests in `tests/AudioClipModelTests.cpp`): its file, where all of it plays in the song, and how the clip plays it; its gain, pitch, reverse, following the song's tempo and the tempo its audio was played at, each change one named undo step, a change of speed keeping the clip's start and the audio in it; what the selection asks the editor dock's first tab to show; and Reverse and Follow Tempo as commands for the selected clips, on the playlist's clip menu. An audio clip's end now stops where its audio does when its start was trimmed. The editor (`src/ui/audioclip/AudioClipEditor.h`, with tests in `tests/AudioClipEditorTests.cpp` that drive it with the mouse, the "done when" among them): the song's bars, all of the file with what the clip's edges trim off shaded, edges and fade handles to drag (a double-click takes a fade away), a drag on the audio moving it inside the clip, and Gain, Pitch, Fine, Reverse, Follow tempo and Tempo down its left; it takes the dock's first tab, named "Audio clip", while an audio clip or a track that holds audio is selected, and double-clicking an audio clip opens it. A real-time audit scenario plays a clip turned up, pitched and reversed from its rendered copy.

**2.7 Chord lane and progression outlines (L)**
- Phazite's chord format and the chord lane.
- The progression library (about 40): audition and apply, and follow key changes.
- Chord highlighting in the piano roll. (The chords are no longer written to the engine: design section 14 says why.)
- Generated chord, bass and arpeggio parts.
- *Done when:*
  - picking a progression for a section plays it in the project key;
  - a bass part generated from it follows the chord lane.
- *Status:* done (2026-10-18), checked by hand by the owner (2026-10-29) ([design section 14](design.md#14-chords)). Chords as plain values (`src/model/chords/ChordValues.h`, tests in `tests/ChordValueTests.cpp`): Roman numerals with accidentals, kinds and inversions, read, written and named in any key, the keys progressions play in, and the notes of twelve part styles, voiced to move little. Forty progressions in `resources/progressions/progressions.json` (`Progressions.h`), by mood and genre. The chord lane in the model (`ChordModel.h`, tests in `tests/ChordModelTests.cpp`): chords and key changes in the project, progressions laid over a range, chords added, changed, resized and deleted, each one named undo step; chords that go with the song's time; parts on new tracks with Phazite's synth that follow the chords until their notes are edited. In the window (tests in `tests/ChordLaneTests.cpp`): the lane under the playlist's bar numbers with its menus and a dragged right edge, its header with the song's key, the Chord Progressions window (play and put on the lane), Add menu items and commands for the parts, the inspector's Follows the chords, and the piano roll's tint of chord notes. The chords aren't written into Tracktion's chord track, which nothing would read (design section 14 says why).

**2.8 Sound generator v1** — moved to phase 3, into [step 3.2](#phase-3-v1-polish-milestone-3) (the owner's decision, 2026-10-18). The steps after it keep their numbers.

**2.9 Mixer depth (M)**
- The sidechain source picker, the routing view, and a per-track CPU meter.
- A strip's Out line as a menu: Master, the folder buses (choosing one moves the track into that folder) and a new bus from the selected tracks ([question 12](design.md#25-open-questions)).
- A send knob for every return on every strip, off until first touched ([question 13](design.md#25-open-questions)).
- *Done when:* a kick-to-bass sidechain is set up from the picker and shows as a cable in the routing view, and a track is sent to a reverb return with one drag.
- *Status:* done (2026-10-18), checked by hand by the owner (2026-10-29) ([design section 8](design.md#8-the-mixer)). In the model (`src/model/mixer/MixerModel.h`, tests in `tests/RoutingTests.cpp`): the returns each track can send to, with a send-level handle that makes the send at its first change, in that change's undo step (`ParameterHandle` can now hold a device the first change makes); where each channel's sound goes, the master or a bus, set as one step that moves the track into or out of the bus's folder; any device's sidechain by its ID, plug-ins included; each channel's CPU load from Tracktion's measurement of each device; and the routing's wires (outputs, sends, sidechains). In the window: a send knob for every return on every strip and in the inspector, "+ Send" for new returns, the Out button's menu, a CPU bar along each strip, a Sidechain submenu in every device's menu, and the routing view (`src/ui/mixer/RoutingView.h`) behind the mixer's Routing button, whose cables are dragged from Out jacks; tests drive a strip's knob with the mouse and drag cables in the routing view, the "done when" among them.

**2.10 More devices (XL)**
- Multiband dynamics and a maximizer in the style of Maximus and OTT.
- A tempo-synced delay, a saturator, a filter and a utility.
- A better synth.
- Phazite's own devices only: plug-ins that aren't, such as Airwindows' effects and Surge XT, may be offered as optional downloads next to Phazite Sounds, never built in ([question 6](design.md#25-open-questions)).
- *Done when:* each device passes its DSP and real-time tests and holds up in A/B listening against reference tools.
- *Status:* done (2026-10-18), A/B listened to and checked by hand by the owner (2026-10-29) ([design section 9](design.md#9-devices-containers-and-presets)). Six new devices in `src/devices/` (filter, multiband dynamics in OTT's style, a maximizer in Maximus's, saturator, tempo-synced delay and utility, with Linkwitz-Riley crossovers in `dsp/Crossover.h`) and unison for the synth, each registered as Phazite's own and offered in the device menus, with a panel of its controls (`DevicePanel` now shows switches for what is on or off). DSP tests in `tests/EffectDeviceTests.cpp` (the delay's timing and ping-pong, the saturator's curves and level, the filter's response, the utility's controls, the multiband's flat split and both-way squeeze, the maximizer's ceiling, the synth's unison); two guarded real-time audit scenarios (the six in a row with the filter swept, and the synth with seven unison voices).

**2.11 Freeze, bounce in place and stems (M)**
- *Done when:* a heavy plug-in track can be frozen and unfrozen, and exporting stems produces one file per track or bus.
- *Status:* done (2026-10-18), checked by hand by the owner (2026-10-29) ([design section 18](design.md#18-projects-saving-and-export)). **Freeze Track** renders a track up to its fader (or a sidechained device, or a send before the fader) in the background with a progress window, plays the file from the project's new `Cache` folder, shows a snowflake, frosted clips and greyed devices, opens frozen next time, and unfreezes when what it froze changes (`src/model/mixer/FreezeModel.h`, `internal/Freeze.h`); not an undo step. **Bounce in Place** renders a track before its fader onto a new audio track under it and mutes and collapses the original, as one undo step. **Stems** are a choice in the Export window: a file for each channel that reaches the master, lined up and not normalized, into "*project* Stems" (`src/model/project/Export.h`). Tests in `tests/FreezeTests.cpp` and `tests/ExportTests.cpp`.

**2.12 Plug-in windows and plug-in presets (M)**
- A "Send All Keys to the Plug-in" switch in each plug-in window's menu, off at first and remembered for that plug-in ([question 16](design.md#25-open-questions)).
- *Done when:* plug-in windows remember their positions, plug-in states save as presets, and a name can be typed into a plug-in that draws its own text fields.
- *Status:* done (2026-10-18), checked by hand by the owner (2026-10-29) ([design section 9](design.md#9-devices-containers-and-presets)). A plug-in's window opens where it was last, and its place is saved with the project as view state, neither an undo step nor an unsaved change (`rememberWindowPosition()` in `src/model/mixer/internal/Mixer.h`); one left on a screen that is gone comes back on the nearest. Its menu bar's **Plug-in** menu (`src/app/PluginWindow.h`) has **Save Preset...**, **Load Preset** (the presets saved for it, loaded into it with their mix as one undo step: `loadPresetInto()` in `src/model/presets/PresetModel.h`) and **Send All Keys to the Plug-in**, kept for the plug-in in the settings (`phazitePluginsGettingAllKeys`), which gives every key typed into its window to the plug-in (`src/ui/input/PluginKeys.h`). Tests in `tests/PluginWindowTests.cpp`, `tests/PluginKeysTests.cpp`, `tests/PresetTests.cpp` and `tests/ProjectTests.cpp`.

**2.13 Wavetable synth (L)** (the owner's request, 2026-10-18)
- A wavetable synth in the style of Serum, after research into the wavetable synths people use ([design section 9](design.md#9-devices-containers-and-presets)).
- *Done when:* a Serum-style patch can be built from its own tables and from a Serum wavetable file, an envelope sweeps each note's table, it doesn't alias, and it passes the real-time audit.
- *Status:* done (2026-10-18), checked by hand and A/B listened to by the owner (2026-10-29). **Wavetable** (`src/devices/wavetable/`): two wavetable oscillators with warps (sync, bend, pulse width, mirror, FM and ring modulation from the other), a sub and noise, unison up to eight voices across the stereo field and the table, a filter, three envelopes, two LFOs and eight modulation slots for each note; frames band-limited every half octave; 13 factory tables made from formulas; the user's tables from sounds, Serum's 'clm ' chunk read, copied into the project (`src/model/devices/internal/Wavetables.h`); an editor that draws each table with the frame it plays (`src/ui/devices/WavetableEditor.h`). Tests in `tests/WavetableTests.cpp`, `tests/WavetableModelTests.cpp`, `tests/WavetableEditorTests.cpp`, a real-time audit scenario and a golden render.

**2.14 Comb filters (S)** (the owner's request, 2026-10-18)
- A comb filter wherever a filter type is chosen, as the synths and filters people use have one ([design section 9](design.md#9-devices-containers-and-presets)); and an audit of the other filter types they share ([design question 20](design.md#25-open-questions)).
- *Done when:* the synth, the wavetable synth and the Filter device offer a comb whose peaks fall on the cutoff's multiples, which sweeps without clicks, stays stable at full resonance and passes the real-time audit.
- *Status:* done (2026-10-18), checked by hand and A/B listened to by the owner (2026-10-29). **Comb +** and **Comb -** after the other types of all three (`src/devices/dsp/CombFilter.h`), with the cutoff as their pitch and the resonance as their ring; the Filter device reports its ring as its tail. Building it showed that the Filter device's Type took the place of its plug-in's own type when changed, so a project lost the Filter when it next opened: its value is kept apart now. Tests in `tests/DspTests.cpp`, `tests/EffectDeviceTests.cpp`, `tests/SynthTests.cpp`, `tests/WavetableTests.cpp` and `tests/DevicePluginTests.cpp`, and a real-time audit scenario.

**2.15 More filter types (M)** (the owner's choice from the audit of step 2.14, 2026-10-18)
- Every filter type in one shared piece of code, and the types the audit recommended in all three filters ([design section 9](design.md#9-devices-containers-and-presets), [question 20](design.md#25-open-questions)).
- *Done when:* the synth has a notch and a 24 dB slope; the synth, the wavetable synth and the Filter device offer a vowel the cutoff moves through, a phaser and a sample and hold; projects from before sound as they did; and they pass the real-time audit.
- *Status:* done (2026-10-18), checked by hand and A/B listened to by the owner (2026-10-29). `src/devices/dsp/MultimodeFilter.h` holds every type, with each device's own resonance curve; the synth's **Notch** and **Filter Slope** (12 or 24 dB), and **Vowel** (a tenor's formants from Csound's table, the cutoff as the second formant, from U to I), **Phaser** (six all-pass stages, three notches, feedback) and **Sample & Hold** (twice the cutoff's rate, resonance ringing at the cutoff) after the other types of all three. Tests in `tests/DspTests.cpp`, `tests/EffectDeviceTests.cpp`, `tests/SynthTests.cpp` and `tests/WavetableTests.cpp`, and two real-time audit scenarios. Since 2026-10-18 all three offer the types as one list (`dsp::filterModeNames`), each with a Slope.

**2.16 Performance (M)** (the owner's request, 2026-10-21: "a lag between when I press a note on the keyboard, and when I hear the sound", and a thorough audit of Phazite's performance)
- The audit and its plan are in [performance-audit.md](performance-audit.md): measuring (A), correctness first (B), latency you can see and choose (C), idle and audio CPU (D), the UI thread (E) and robustness (F), with the owner's three decisions ([design section 20](design.md#20-performance)).
- *Done when:* each part's "Done when" in the audit holds, and its tables are measured again.
- *Status:* done, checked by hand by the owner (2026-10-29). A and B done (2026-10-21): the benchmark suite (`--test-suite=benchmark`, `tests/Benchmarks.cpp`) and the `windows-profile` preset; untitled recordings that survive a crash, key taps that always play, look-ahead devices that drop their latency when off, the wavetable synth's modulation and the glides on time, and voice stealing that fades. Tests in `tests/AppTests.cpp`, `tests/ProjectTests.cpp`, `tests/LiveInputTests.cpp`, `tests/SynthTests.cpp` and `tests/WavetableTests.cpp`. C done (2026-10-21), checked by hand by the owner (2026-10-29): the Audio settings show every device setting and, under them, how long a note takes to sound, from the buffer, the output's latency and the song's delay compensation (`getDelayCompensation()` in `src/model/mixer/MixerModel.h`), and warn of a Bluetooth output, as the welcome window does (`src/model/engine/AudioDevices.h`); a first run picks Windows' Low Latency Mode at 128 to 256 samples where offered; and a **Low Latency** switch in the top bar (`transport.lowLatency`) bypasses Phazite's look-ahead devices, turns delay compensation off and shrinks the buffer, leaving renders alone. Tests in `tests/AudioDevicesTests.cpp`, `tests/AudioSettingsTests.cpp`, `tests/LiveInputTests.cpp`, `tests/MixerModelTests.cpp` and `tests/WelcomeTests.cpp`. It found known bug 3, live notes lost after the UI thread was busy (first taken for two limiters; fixed 2026-10-28). D, E and F done (2026-10-21), measured in [performance-audit.md](performance-audit.md), section 7: devices sleep on silence (a stopped 20-track song from 17 % of a core to 2 %), the synths' worst cases 29 % and 42 % cheaper, parameters glide; a knob's change from 5.5 ms of view updates to 0.02 ms, the unsaved-changes check from 12.7 to 0.8 ms, autosave on a background thread (5 ms on the UI thread); after a crash Phazite asks how to open the last project, with its plug-ins off if wanted; "No audio output" in the hint bar; "Opening..." as it starts; a time limit for each plug-in scanned; one Phazite at a time; a log file. Tests in `tests/DevicePluginTests.cpp`, `tests/MixerTests.cpp`, `tests/ProjectContextTests.cpp`, `tests/ProjectTests.cpp`, `tests/AppTests.cpp` and `tests/AppShellTests.cpp`. An outside review, checked and acted on 2026-10-27 ([performance-audit.md](performance-audit.md), section 8): the playlist makes components only for the clips in view (a scroll step in a 2,400-clip song from 43 ms to 0.2 ms of the UI thread, a clip's move from 385 ms to 1.5 ms); meters and device pictures repaint only when what they draw moves, from one timer; components that fill themselves are opaque; the playhead moves with the display's frames; waveforms and the piano roll's notes are kept between paints; and the standard mouse pointers stay made. Link-time optimisation and AVX2, measured, don't speed the devices up, and stay off. Tests in `tests/PlaylistTests.cpp`, `tests/ControlsTests.cpp` and `tests/DeviceEditorTests.cpp`, and a benchmark of scrolling and zooming a big song.

**2.17 Devices and the window (L)** (the owner's requests, 2026-10-21, after the audit: a picture with its own character for every built-in device, live where it shows what a knob does; the synths' controls stacked, with pages where they don't fit; panels that hide or fill the window, a title bar with the menus, Undo, Redo and the command search, as VS Code has, and a status bar; sidechains that work and show what they do; a Stereo Expander in place of the Utility, and a Phaser, a Flanger and a Vocoder; the instrument apart from the effects, a device row that doesn't run off sideways, and built-in devices in windows of their own; modulators that switch off; no sound after a pause)
- Parts, each committed on its own: modulators on and off; the stop and pause fix; a picture for every device, with a shared frame for their editors; the synths' layouts; Instrument and Effects tabs, folded device cards and device windows; the four new devices; sidechains; the title bar, the panels and the status bar.
- *Done when:* every built-in device shows what it does as it plays, in a layout that fits a 1280 x 800 window's dock without scrolling sideways; a maximized window shows four effects folded in the Effects tab, and a synth in the Instrument tab without scrolling sideways; an EQ pops out as "EQ - Bass" and comes back folded; "kick ducks bass" takes two clicks and shows its ducking; each panel hides and fills the window from the title bar; an LFO switched off leaves the cutoff alone, still assigned, as one undo step.
- *Status:* done (2026-10-29), checked by hand by the owner. Done 2026-10-21: the stop and pause fix (notes that only just began are cut, as playback stops); modulators on and off (`setModulatorOn()` in `src/model/modulation/ModulationModel.h`; tests in `tests/ModulationModelTests.cpp`, `tests/ModulationUiTests.cpp`, `tests/DevicesViewTests.cpp` and `tests/PresetTests.cpp`); a picture for every device on a shared frame, every effect's mix in its header, and the synths in columns that fold into tabs (about 780 and 950 pixels wide at the default dock's height, from over 3,000 and 6,000; [design section 9](design.md#9-devices-containers-and-presets); tests in `tests/DynamicsEditorTests.cpp`, `tests/EffectEditorTests.cpp`, `tests/SynthEditorTests.cpp` and each editor's tests); the Stereo Expander in place of the Utility (`src/devices/stereo/StereoExpander.h`, `tests/StereoExpanderTests.cpp`); the Phaser and the Flanger, with LFOs in time with the song (`src/devices/motion/`, `tests/MotionEffectTests.cpp`, their editors' tests in `tests/EffectEditorTests.cpp`); the Vocoder, on the voice's track with a buzz, noise, the voice or a track through the sidechain as its carrier (`src/devices/vocoder/`, `tests/VocoderTests.cpp`); and sidechains, chosen in one place, each device's header, with Duck Under in the track's menu, the compressor set up to duck, its key's high cut and Listen (`src/ui/devices/SidechainMenus.h`; tests in `tests/SidechainTests.cpp`, `tests/RoutingTests.cpp`, `tests/DevicesViewTests.cpp`, `tests/CompressorTests.cpp` and `tests/CompressorEditorTests.cpp`). Also done (2026-10-21): the instrument apart from the effects, the editor dock's Instrument and Effects tabs, and effects as cards folded but one (`src/ui/mixer/DevicesView.h`, `src/ui/shell/Panels.h`; tests in `tests/DevicesViewTests.cpp` and `tests/AppShellTests.cpp`). Also: built-in devices in windows of their own (`src/app/PluginWindow.h`, `src/ui/devices/DeviceWindowContent.h`); panels that hide, fill the window one at a time and keep their sizes (`src/ui/shell/PanelLayout.h`; tests in `tests/PanelLayoutTests.cpp` and `tests/AppShellTests.cpp`); and the window's own title bar on Windows, with the menus, Undo and Redo beside the command search, the panels' switches and the window's buttons, and the status bar, with the save state, the hints, the audio output and the CPU (`src/ui/shell/TitleBar.h`, `StatusBar.h`; tests in `tests/TitleBarTests.cpp` and `tests/StatusBarTests.cpp`). And on 2026-10-29: the compressor's history draws the track its sidechain listens to in that track's colour, named (`src/ui/devices/LevelHistoryView.h`); the mixer slot of each of Phazite's devices that turns the sound down (the compressor, the limiter, the maximizer and the multiband) shows how far along its right edge, from a reading any number of displays share (`src/devices/core/FallingValue.h`); a folded effect card shows its editor's picture alone, live, over its key knobs, and a click on it opens the card (`ShowsPictureOnly` in `src/ui/devices/DeviceEditors.h`); and the status bar shows what is selected, where MIDI goes and the delay compensation (`src/ui/shell/StatusBar.h`). Tests in `tests/CompressorEditorTests.cpp`, `tests/DevicesViewTests.cpp` and `tests/StatusBarTests.cpp`.

**2.18 Phazite's own time-stretching (L)** (the owner's report, 2026-10-28: "sometimes when sounds are playing, they will stutter ... it's obvious on transients like snare"; and the go-ahead to depart from Tracktion Engine for subsystems such as time-stretching where the best solution comes down to that)
- *Why:* Tracktion stretches clips with SoundTouch, which plays 12 to 50% of drum hits twice when it slows a loop down and plays stretched clips early (known bug 1). It offers Rubber Band only with its R2 engine, which is clean on drums but muddier than SoundTouch on a held chord. Rubber Band's R3 engine did well on both ([design section 23](design.md#23-tracktion-engine-what-it-gives-and-what-we-build)).
- The first bigger engine part replaced ([architecture phase A5](#architecture-track)): an interface, the replacement behind it, the switch-over, then the old code deleted.
  - Rubber Band Library 4.0 (GPL-2.0-or-later) as a pinned submodule, built into `phazite_core` from its single-file build, with its licence among the notices.
  - An engine-neutral stretcher in `src/devices/`: R3, offline, over a clip's whole audio, its beats pinned to the song's with key frames, and with the clip's pitch.
  - The switch-over: a clip that follows the tempo, or is pitched or reversed, plays a file that Phazite renders into the project's `Cache` folder, and Tracktion's clip becomes a plain clip of that file. Phazite's own state on the clip (`Schema.h`) keeps the original file, its tempo, pitch and reverse. A tempo change, or a change to the clip, lays the clip out again from its beats in the same undo step (Tracktion keeps a plain clip's length in seconds) and renders it again in the background. The clip is silent until its render is ready, as now; long clips render in pieces, so that they start sooner.
  - The browser's previews: measure R3 rendering ahead against a stretcher that runs as they play, and use the better one.
  - Then the old code goes: Phazite's playback uses none of Tracktion's auto-tempo, proxies or SoundTouch.
- *Done when:* a drum loop stretched from 120 BPM to anywhere from 90 to 140 plays every hit once, each within 1 ms of its beat, in the app and in export; a held chord stretched keeps -15 dB or less of its energy between its notes (SoundTouch: -6 to -10 dB); a 4-bar loop's render is ready within a second; known bug 1 is gone; the golden tests pass, their stretched clips regenerated with the reason; and nothing of Tracktion's stretching is used.
- Also (the owner's request, 2026-10-28): what a tempo change stretches shows under the tempo field as it changes, with switches for samples and patterns ([design section 4](design.md#4-the-window)).
- *Status:* done (2026-10-28), checked by hand and by listening by the owner (2026-10-29). Rubber Band 4.0 is a pinned submodule (`external/rubberband`); `src/devices/stretch/Stretch.h` renders a file with R3, offline, its pitch and backwards too; a clip that follows the tempo, is pitched or reversed keeps what it is made from in Phazite's properties (`Schema.h`) and plays a render in the Cache folder as a plain clip (`model/arrangement/internal/AudioClips.h`), which the project's `ClipRenders` makes in the background (`internal/ClipRenders.h`), and exports and freezes make first; the tempo command lays following clips out again for the new tempo in its undo step; Tracktion's own auto-tempo and auto-pitch for clips of loops are undone as each clip is made. Measured: no hit played twice; hits within 2.5 ms of their beats, half within 1 ms (the test allows that, as R3 moves an attack by up to about 2 ms, against SoundTouch's 8 to 28 ms early); a held chord -16 to -29 dB between its notes; R3 renders stereo about 9 times faster than it plays (`windows-profile`, the benchmark "rendering audio clips stretched and pitched": a 4-bar loop at 120 BPM in 0.9 s, a minute in 6.7 s). Tempo changes show what they stretch: the commands Samples Follow Tempo (every sample's Follow tempo at once) and Patterns Follow Tempo (the song's: off, a tempo change keeps its patterns' notes at their times), and a panel under the tempo field while it changes (`src/ui/shell/TempoStretchPanel.h`). Tests in `tests/StretchTests.cpp`, `tests/TempoTests.cpp`, `tests/TopBarTests.cpp`, `tests/AudioClipModelTests.cpp`, `tests/AudioClipEditorTests.cpp` and `tests/BrowserTests.cpp`. The browser's previews too (the same day): a previewed loop is rendered by R3 into memory as it starts, at the output's rate, and plays as soon as a quarter of a second is ready, the render keeping ahead: about 55 ms after it is chosen, all of it in a second, for four bars at 120 BPM previewed at 100 (`windows-profile`, the benchmark "a loop previewed in time, until it can start"; `src/model/engine/internal/PreviewLoop.h`; a test in `tests/TempoTests.cpp` finds each hit once and on its beat, twice round the loop, and the real-time audit's preview scenario is now guarded); R3 offline needs no study pass, which only counts frames, so renders read their file once; and no stretcher of Tracktion's is compiled in. On 2026-10-29, as the owner asked, a tempo change shows that clips are being stretched: "Stretching 2 clips" with a bar of how far that has come in the status bar, and each clip waiting for its render faint, with its own bar along its bottom edge (`getClipRenderProgress()` and `getClipsBeingRendered()` in `src/model/arrangement/AudioClipModel.h`; tests in `tests/TempoTests.cpp`, `tests/StatusBarTests.cpp` and `tests/PlaylistTests.cpp`). Long clips still render whole rather than in pieces (the owner: "keeps it simple, and the users shouldn't be changing tempo constantly"). The 1 ms target, measured again on 2026-10-29 from 80 to 160 BPM (the owner: fine for now, with a note): R3 puts the hits 0 to 1.5 ms from their beats, by an amount that depends on the tempo, give or take up to 1 ms more, and none of its settings does better: its short window put them 2 to 5 ms late and left -5 to -9 dB between a held chord's notes, and key frames moved nothing. Rubber Band's R2 engine, with the study pass it needs, puts every hit within 0.5 ms of one shift that depends only on the stretch (about 19 ms × (ratio − 1), which could be taken off), but leaves -5 to -8 dB between a chord's notes (R3: -16 to -32), so it would suit drum loops only: a choice for later, as Ableton's Beats mode is ([design section 7](design.md#the-audio-clip-editor)).

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

**3.2 Sound generator (XL)** (its first part was step 2.8 until the owner moved it here, 2026-10-18)
- **The drum synth device:** kick, 808, snare, clap, hats, tom, rim and percussion.
- **The generator panel:**
  - a grid of sounds, with audition in context;
  - keep, favorite and lock;
  - more like this, and history.
- **Project fit and recipes:** sounds fit the project's key, tempo and loudness, and each keeps its recipe.
- **Then:** breeding favorites, kit-aware generation (which complements the drums already used), and automatic tags.
- *Done when:*
  - generating, auditioning in the loop and keeping a kick tuned to the song's key works end to end, and a kept sound reopens with its recipe;
  - breeding two favorite kicks produces new kicks with traits of both.

**3.3 Stability (XL)**
- A scan timeout.
- A plug-in that crashed while loading is disabled on the next start.
- Polished crash recovery.
- **Plug-in sandboxing, on by default** ([question 7](design.md#25-open-questions), moved before v1 on 2026-10-16): plug-ins run in helper processes, and one that crashes leaves its slot saying so, with Reload, while Phazite plays on. First measure what it costs (CPU, latency per block, memory) against hosting in Phazite's own process; if the cost is large, it is a choice per plug-in instead, and the owner decides.
  - *How* (an outside review, 2026-10-27, [performance-audit.md](performance-audit.md), section 8): each sandboxed plug-in is a round trip to its helper at every block, so to hold 128 samples: audio in shared memory, one wake-up each way per block, the helpers' audio threads registered with Windows' "Pro Audio" scheduling (MMCSS), and Tracktion's worker threads waiting on several helpers at once rather than one after another. Measure the round trip before choosing one process per plug-in or one per group.
- *Done when:* a deliberately hanging or crashing test plug-in can't stall the scan, crash Phazite or lose the project.

**3.4 Audio recording (L)**
- Record-arm, monitoring, punch-in and basic takes.
- *Done when:* recording a vocal or guitar over a loop, with count-in and punch-in, works with an ASIO interface.
- *Status:* done (2026-10-29), pending the check by hand with an ASIO interface. An arm button beside each audio track's M and S, in its header and its mixer strip, whose right-click chooses the input ("In 1", "In 1+2"), when the input is heard (while armed, always or never) and Arm Only This Track; the command Arm Track; Record records the armed tracks' inputs with what is played on MIDI, in one undo step; Punch In (Record's menu) keeps only what is played in the looped range, cross-fading 10 ms at its edges; each time round a looped range is a take of one clip, "Take 2/3", chosen from its menu or with Previous Take and Next Take; the audio settings' Recording correction ([design section 17](design.md#17-recording-and-capture)). Phazite records itself, with no lock or allocation on the audio thread ([real-time audit](realtime-audit.md), findings 1 and 13; its new guarded scenario records a tone onto a track that hears it): `src/model/transport/RecordingModel.h`, `internal/AudioRecorder.h`, `src/ui/components/ArmButton.h`. Tests in `tests/AudioRecordingTests.cpp`, through Tracktion's hosted audio device with its output brought back into its input, as a cable would: the metronome's clicks land one block (the cable's delay) after their beats to the sample, and on them with the correction; takes round a loop; punching in over a clip; and in `tests/PlaylistTests.cpp`, the arm button.

**3.5 Performance pass (L)**
- Profile against the targets in [design section 20](design.md#20-performance): idle CPU, large projects, startup time.
- *Done when:* the targets are met, or the gaps are written down with a plan.

**3.6 Accessibility pass (M)**
- Keyboard navigation everywhere, screen-reader names, contrast and scaling.
- *Done when:* core tasks can be done with Narrator and the keyboard alone, and the themes pass WCAG 2.2 contrast.

**3.7 Keymaps and help (M)**
- The FL Studio keymap completed, and an Ableton Live keymap ([question 8](design.md#25-open-questions), decided 2026-10-16).
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
- Third-party notices in the app and installer (Rubber Band's GPL), and Inter's OFL text and copyright notice.
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

1. CLAP hosting (L).
2. The clip launcher, as a jam panel that records into the playlist (L).
3. The AI assistant, acting through commands (L).
4. Audio take lanes and comping (L).
5. DAWproject import and export (M).
6. Neural sound generation with our own model, trained on public-domain audio (XL).
7. Audio-to-MIDI, automatic sample tags and "find similar" (L).
8. macOS, with Audio Units (L).
9. MPE controllers, Linux screen readers (AT-SPI), translations and a scripting API (M–L each).

(Plug-in sandboxing moved before v1, into step 3.3, on 2026-10-16.)

## Cross-cutting work

- **Testing:**
  - unit tests for model operations;
  - DSP tests: frequency responses, gain curves, null tests;
  - golden tests (architecture phase A0): saved projects compared with committed files, and short offline renders with committed fingerprints, within a tolerance;
  - usability tests at each milestone.
- **Architecture:** keep to the rules in `CLAUDE.md`, which `tools/check-architecture.sh` checks; each step moves the area it touches toward the [architecture track](#architecture-track)'s structure.
- **Performance:** benchmarks for audio CPU and UI frame time, tracked from phase 1 onward.
- **Documentation:** keep [design.md](design.md) and this roadmap current, and update the status table after each step.
- **Known bugs:** [known-bugs.md](known-bugs.md) lists the bugs found and not fixed yet; each is fixed before v1 unless the owner says otherwise.
- **Licences:** check every new dependency, model or dataset against the list in `CLAUDE.md` before using it.

## Risks

| Risk | Mitigation |
|---|---|
| The UI is by far the largest piece of work | Build vertical slices, run usability tests at each milestone, and use engine features instead of reinventing them |
| Engine code breaks the real-time rules | Measured in step 0.3; workarounds or a fork are decided then |
| Built-in effects fall short of FL Studio's | Listening and A/B tests against references, plus DSP tests |
| Sandboxing and CLAP are complex | Sandboxing is measured first (step 3.3) and becomes a choice per plug-in if it costs too much; CLAP waits until after v1 |
| Scope creep | v1 scope is fixed in [design section 1](design.md#1-vision-and-scope); new ideas go under "After v1" |
| Restructuring changes behavior by accident | Golden tests of saved projects, renders and commands (A0); every step builds and passes the tests, one commit each |
| The architecture work holds up features | Only A0 and A1 pause features; A2 to A4 move each area when a feature touches it; A5 only after a dated decision |
| The layers erode over time | `tools/check-architecture.sh` in CI and before every commit, with limits that can only go down |

## Status

| Phase | Status |
|---|---|
| 0. Foundations | Done, pending checks by hand (0.4: crash recovery; 0.6: Narrator and idle CPU; 0.7: MIDI and computer keyboard) and Linux CI ([real-time audit](realtime-audit.md)) |
| 1. Make a beat | Steps done, pending checks by hand and the milestone 1 check (a usability test with a newcomer and an FL Studio user): steps 1.1 to 1.5 (the playlist, sections and song outlines, the piano roll, the mixer, the browser), with three rounds of fixes from trying them (2026-10-13); step 1.6 (all six devices, and the starter project on them), step 1.7 (metronome, tempo, recording and Capture), step 1.8 (export to WAV and FLAC, normalized to a loudness) and step 1.9 (first launch, the guide and the new-project window), 2026-10-15 |
| 2. Finish a track | Done (2026-10-29), every step checked by hand and the new devices and stretching A/B listened to by the owner, who found them good (2026-10-29): 2.1 (automation lanes), 2.3 (modulators and macros) and 2.4 (containers and presets), 2026-10-16; 2.2 (lanes that go with the music), 2.5 (the window: the song in the centre, the Devices view, the properties inspector) and 2.6 (the audio clip editor), 2026-10-17; 2.7 (the chord lane and progressions), 2.9 (mixer depth), 2.10 (six new devices and the synth's unison), 2.11 (freeze, bounce in place and stems), 2.12 (plug-in windows and presets), 2.13 (the wavetable synth), 2.14 (comb filters) and 2.15 (more filter types), 2026-10-18; 2.16 (performance), 2026-10-21; 2.17 (devices and the window), 2026-10-29; 2.18 (Phazite's own time-stretching), 2026-10-28, with its progress shown on 2026-10-29; 2.8, the sound generator, moved to phase 3 (2026-10-18) |
| 3. v1 polish | Under way: 3.4 (audio recording), 2026-10-29, pending a check by hand with an ASIO interface |
| 4. v1 release | Not started |
| Architecture track | A0 (2026-10-14), A1 and A2 (2026-10-15) and A3 (2026-10-16) done, A2 pending a check by hand: the UI begins no undo steps (46 when A2 began) and makes no direct edits, the playlist, piano roll and mixer strip are split into a file per part, the browser has a model, and the two editors' tools share one gesture. A3: IDs and beats, views that follow the project, the mixer by IDs, the transport and the engine for the top bar and the settings, the shared time line in Phazite's beats, the playlist by IDs, note IDs saved in the project (format version 3), the piano roll by note IDs and the browser by IDs and plain values; the UI names and includes no Tracktion and compiles without it, and the model's headers that use Tracktion are in `internal/` folders. On 2026-10-18 the format versions, the migrations and the support for Tracktion's devices in old projects went: pre-alpha keeps no backward compatibility (design.md, section 18) |

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
