# Phazite roadmap

*Status: draft for review, October 2026.* This is the plan for getting from today's smoke-test app to v1 of the [design](design.md).
- **Order:** each step builds on the ones before it and ends with something that can be built, tested and tried.
- **Sizes:** relative effort, S < M < L < XL, for ordering only. They are not dates.

## How each step is done

1. **Plan:** confirm the step's scope. If a decision changes, update [design.md](design.md).
2. **Build:** follow `CLAUDE.md`: real-time rules, formatting, SPDX headers, licences.
3. **Test:**
   - unit tests for model logic and DSP tests for devices;
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

| Open question ([design section 24](design.md#24-open-questions)) | Needed before |
|---|---|
| 1. Algorithmic AI engines in v1 | **Decided:** yes, trained models later |
| 2. First outline styles | **Decided:** pop, rock, electronic, hip-hop/trap |
| 3. UI font and its licence | **Decided:** Inter, bundled, SIL OFL 1.1 |
| 4. Engine fixes: workarounds or a fork | **Decided:** simple workarounds first, a fork if they get awkward |
| 5. MP3 export | Step 1.8 |
| 6. Airwindows effects | Step 2.8 |
| 7. Sandboxing before or after v1 | Phase 3 |
| 8. Ableton Live keymap in v1 | Step 3.7 |
| 9. Single-key tools and the computer keyboard | Step 1.1 |

## Phase 0: Foundations

**0.1 Code structure and tests (S)**
- Split `src/` into:
  - `app/`: startup and the main window;
  - `model/`: Phazite's project model on top of Tracktion (commands, sections, chords, outlines);
  - `ui/`: components and look-and-feel;
  - `devices/`: built-in DSP;
  - `gen/`: the sound generator and chord engine.
- A static library target, `phazite_core`, holds everything testable without a window. The app links it.
- A test runner using doctest (MIT, bundled with Tracktion Engine), run by CTest locally and in CI. On Linux it runs headless.
- *Done when:* CI runs the tests on Windows and Linux.

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
- *Status:* done (`src/model/Project.h` with tests, and the app's `src/app/ProjectSession.h`): new, open, open recent and save; an autosave every 2 minutes; the restore question at startup. Still to try by hand: end Phazite in Task Manager mid-session, then start it again.

**0.5 Commands, keymaps and undo (M)**
- A command registry with names, descriptions, search synonyms and categories.
- One named undo step per command.
- Keymap files (Phazite default and FL Studio), and key rebinding.
- The command palette (Ctrl+K), and macros made of chained commands.
- *Done when:* every action in the app so far is a command, can be found in the palette, and shows by name in the undo history.
- *Status:* done: the registry, keymaps (Phazite and FL Studio, in `resources/keymaps/`), named undo, macros and the palette's search (`src/model/Commands.h`, with tests); the palette (Ctrl+K), menus and shortcuts run the app's commands, and **Options > Keyboard Shortcuts** changes keys (kept per keymap). There is no undo history panel yet, so step names only show inside Tracktion's undo manager.

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
- *Status:* the shell is in (`src/ui/`): the window with its menus, panels that resize and collapse, the Beat, Mix and Full layouts, the design tokens with three themes (`src/ui/DesignTokens.h`, contrast checked by tests), Inter with tabular digits, the hint bar, hover help and the command palette. The base controls are in too (`src/ui/Controls.h`): knob, fader and level meter, buttons with a visible keyboard focus, and links to Tracktion parameters with named undo steps. They show in the top bar (master level and volume) and in the inspector, which shows the synth track's channel (volume, pan, mute, solo, level). The interface size goes from 75 % to 200 % (View menu, Ctrl+= and Ctrl+-). Still to do: detaching panels to another monitor and the icon set. Still to try by hand: Windows Narrator reading the controls, and idle CPU.

**0.7 Audio, MIDI and the computer keyboard (S)**
- A settings page: audio device (including ASIO), buffer size and MIDI inputs.
- Computer-keyboard MIDI input.
- *Done when:* a MIDI keyboard and the computer keyboard both play the starter synth.
- *Status:* done: **Options > Settings** with Audio (device type including ASIO, device, sample rate and buffer size), MIDI (each MIDI input with an on/off switch, and the computer keyboard) and Shortcuts tabs; the computer keyboard (`src/model/ComputerKeyboard.h`) and live MIDI routing (`src/model/LiveInput.h`), with tests. Still to try by hand: a MIDI keyboard and the computer keyboard playing the starter synth.

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

**1.2 Sections and song outlines (M)**
- The sections strip. Move, duplicate (also as aliases), delete and resize a section across all tracks.
- Outline data files for pop, rock, electronic and hip-hop/trap.
- Apply an outline when creating a project, or later.
- The energy curve and the section tips.
- *Done when:*
  - applying the electronic outline creates its seven sections at the project tempo;
  - moving the Drop moves everything in it (unit-tested).

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

**1.4 Mixer (L)**
- Channel strips: fader, pan, mute, solo, meters, and device slots with key controls.
- Sends and returns (pre/post), folder buses, and a master with a loudness meter.
- *Done when:* a mix with a reverb return and a bus works and matches the playlist's tracks.

**1.5 Browser (L)**
- Folders, the plug-in list, search, tags and favorites.
- Preview, with loops in time and key.
- Drag-and-drop rules and the plug-in picker (F8).
- *Done when:* finding and placing a kick takes seconds, and loops preview in time.

**1.6 First built-in devices (XL)**
- The first devices:
  - drum sampler: pads, choke groups, tuning and envelope;
  - subtractive synth;
  - parametric EQ with an analyzer;
  - compressor with sidechain;
  - reverb;
  - limiter.
- Each is real-time safe and has an editor and DSP tests.
- These devices replace Tracktion's 4OSC in the starter project.
- *Done when:* a beat can be made and mixed with built-ins only, and every device passes the real-time checks from step 0.3.

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
- The lane editor, based on Tracktion's `CurveEditor`.
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
  - headless engine tests in CI that compare renders with reference files, within a tolerance;
  - usability tests at each milestone.
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

## Status

| Phase | Status |
|---|---|
| 0. Foundations | In progress: 0.1 to 0.5 and 0.7 done ([real-time audit](realtime-audit.md)); 0.6's shell, base controls and interface size done, its icons and detachable panels to do |
| 1. Make a beat | Not started |
| 2. Finish a track | Not started |
| 3. v1 polish | Not started |
| 4. v1 release | Not started |

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
