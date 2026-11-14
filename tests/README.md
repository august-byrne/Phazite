# tests

Phazite's tests, in the `phazite_tests` target, which links `phazite_app` and `phazite_ui` and is written with doctest
(MIT, `external/doctest`, compiled into the tests only, as a system include with its warnings off). Each `*Tests.cpp` file holds the tests of one area; `ctest --preset <preset>` runs the
executable once for each test file. The rules for writing tests are in `CLAUDE.md`, "Tests".

## Helpers

| File | What it gives a test |
|---|---|
| `TestMain.cpp` | The test runner: doctest inside a JUCE environment. |
| `TestHelpers.h` | The shared includes and helpers; every test file includes it. |
| `TestSong.h` | A project in a temporary folder, open in a project context of its own, with the app's commands, for building a song through the model as the UI does. |
| `TestClips.h` | MIDI clips, audio clips and notes made through the model, by the song and IDs, for a song made without a project (`phazite::createSong()`). |
| `SongIDs.h` | A song's IDs checked: every item's once in the song, a note's once in its clip, and what names an item naming one the song has. |
| `UiTestHelpers.h` | For tests of components: a `TestWindow` that isn't on any screen (it works headless, as on Linux CI), through which a `Mouse` and `pressKey()` reach the components the way JUCE delivers real events, mouse and key listeners included, and which says what was asked to repaint (`takeRepaintedArea()`); `dispatchPendingMessages()` delivers every message posted before the call and what those post in turn; `dispatchMessagesUntil()` waits for the effect of a message another thread posts. |
| `AppScreen.h` | The main window's content built as the app builds it, in a `TestWindow`, for tests and benchmarks that need the whole window. |
| `EnginePlayback.h` | Phazite's engine playing the open project, driven a block at a time, the same every time; `Timing::asRender` for comparing what plug-ins in helpers play, whose blocks otherwise keep a deadline in real time. |
| `RenderHarness.h` | A song rendered offline through Phazite's engine, always the same way; its fingerprint (the RMS and peak level of each 100 ms block on each side), compared with a golden one or another render's. |
| `GoldenFiles.h` | Golden files: what Phazite makes, compared with the files committed in `golden/`, each difference listed. |
| `TestDevices.h` | Devices of the tests' own that Phazite's engine plays as its own: a gain, an instrument that plays its notes' velocity, a delay with latency, an effect that plays its sidechain. |
| `TestPlugin.h` | The stand-in for a plug-in (a VST3): the effect of `host/TestPlugins.h`, in versions as an update makes them, which loads in the tests' helper as every plug-in does. |
| `SandboxedPlugins.h` | While one lives, the test engine knows the tests' stand-ins as if a scan had found them, each loading into the tests' helper. |
| `TestAudioDevice.h` | An audio device of a type of its own on JUCE's device manager, its blocks played when a test asks. |
| `HostedDevice.h` | The tests' audio device opened as the app opens one, for tests and benchmarks that process blocks themselves on a thread of their own. |
| `RealTimePlayback.h` | Playing in real time through the tests' audio device, for tests that need an audio device with inputs. |
| `AudioThreadMonitor.h` | Counts allocations and frees on audio threads, for the real-time audit; it measures only in Windows Debug builds. |
| `MotionTestHelpers.h` | What the motion effects' tests share: an effect played through its runner, and a tone read in what comes out. |
| `SoundPackTestHelpers.h` | A small Phazite Sounds pack and a release that pins it. |

## The nets

What a change must keep true, and where each net is:

| Net | Where | What a change does |
| --- | --- | --- |
| Undo and Redo are exact | `UndoCheck.h`: compares the song's text before a change, after it, after Undo and after Redo, and walks the history back and forth | Run a change through `check.command()`, `check.check()` or `check.checkSeveral()`; the header says what the comparison leaves out |
| Every command is run | `CommandGoldenTests.cpp`: the script runs every model command where it changes something, each through an `UndoCheck` | Run a new command in the script, or list it in `getCommandsNotRun()` with why |
| Every saved name is in the golden project | `GoldenProjectTests.cpp`: the representative project holds every element, property and value `Schema.h` declares | Add a new saved name to `addRepresentativeContent()`, or to `getNamesNotInTheProject()` with why |
| Playback at every condition | `PlaybackConditions.h`: five pairs of rate, block size and tempo (`atEveryCondition()`), with the engine's own invariants in `PlaybackConditionTests.cpp` | Wrap a test of anything playback promises everywhere in `atEveryCondition()` |
| Live playback as the render | `RenderHarness.h`: `checkGoldenRender()` renders a golden song, renders a copy and plays it live, and all three must match; what only live playback plays is a golden through `checkGoldenAudio()` (`GoldenLiveTests.cpp`) | A song that can't match live passes a `liveDiffers` reason |
| Fingerprints | `RenderHarness.h`: for each 100 ms, each side's RMS and peak, ten band levels, the RMS frequency and where the power lies | They don't see phase, a steady sound a few samples late, or a pitch change under a tenth of a semitone |
| Every device keeps the rules | `DeviceConformanceTests.cpp` runs every device of `getOwnDevices()` at 44.1, 48 and 96 kHz: numbers, nothing, tail, latency, mix, blocks, automated blocks, starting over, offset | A new device is picked up by itself; one that can't keep a rule gets a line in the `exceptions` table with its reason, and the test fails once the exception isn't needed |
| A JUCE assertion fails its test | `TestMain.cpp`: in Debug builds an assertion that fires during a test case fails it, with its file, line and stack; one outside a case, or as the program ends (leaked objects), fails the run | Fix what raises it, or list it in `allowedAssertions` with JUCE's file and line and why (empty today); a test that makes the fault on purpose declares it with `ExpectedAssertion` (`TestHelpers.h`) |
| Nothing allocates on the audio thread, edited or not | `RealtimeAuditTests.cpp`: every block counts unless a scenario names warm-up blocks with a reason; a scenario's `Edit`s run on the message thread at set blocks while the song plays | Add an `Edit` to a scenario for an edit that could reach the audio thread |
| Real time without the wall clock | `RealTimePlayback.h`: `waitForBlocks()` and `blocksFor()` wait for audio played | A real-time test never waits for time; `VersionsHearingTests`, which measures call lengths, runs alone in ctest |
| A real VST3 | `vst3/TestVst3s.h`: a mono effect (gain, invert, 100 samples of latency, programs) and a stereo instrument with an input bus, built by `juce_add_plugin` for the tests only; `Vst3PluginTests.cpp` scans and loads them through the tests' helper as a user's plug-in | Their paths are `PHAZITE_TEST_VST3_EFFECT` and `PHAZITE_TEST_VST3_SYNTH` |
| The editors' tools and drags | `EditorScreens.h`: `PlaylistScreen` and `PianoRollScreen`, an editor in a `TestWindow` with the app's commands and keys; `EditorToolTests.cpp` drives every tool with the mouse, `EditorDragTests.cpp` changes the song between a press and its release | Name items through the model by IDs, and call `deliverChanges()` after each gesture |
| The shipped programs start | ctest's `PhaziteStarts` runs `Phazite --smoke-test` (its window shows with a new project, paints and quits: 0, or 1, or 77 where there is no display, which ctest skips); `ShippedProgramsTests.cpp` starts `PhaziteHost` and sees it answer | `PhaziteStarts` needs the whole preset built, not `phazite_tests` alone |
| Measuring a device | `DspProbe.h`: signals, measurements and `probe::Device`, the device on its runner as the engine plays it | New device tests use it instead of their own `play()` and DFT helpers; `PHAZITE_DEVICE_REPORT=1` prints what each rule measured |

## host/

The tests' helper program, `phazite_test_host`, which every plug-in in the tests loads in (its path is
`PHAZITE_TEST_HOST`): it hosts VST3s and the stand-ins of `host/TestPlugins.h`, a format of their own, "Test"
(`host/TestFormats.cpp`). `host/SandboxTestHelpers.h` opens the stand-ins in a helper and plays blocks through them;
`host/ThreadCpu.h` gives a thread's CPU time for the benchmarks.

## Suites that ctest skips

Run these from the test executable with the arguments shown.

| Suite | What it does |
|---|---|
| `--test-suite=benchmark --no-skip` | The benchmarks (`Benchmarks.cpp`): the devices, live notes, a project stopped and playing, saving and the UI's work. The sandbox's benchmarks (`SandboxBenchmarks.cpp`) play the stand-ins, and Surge XT where it is installed, in a helper and in the tests' process. Run from `windows-profile`. |
| `--test-suite=preset-report --no-skip` | The preset report (`PresetReport.cpp`), from `windows-profile`, with `PHAZITE_PRESET_REPORT` naming its folder: measures every instrument preset and writes WAV files of them, as [docs/presets.md](../docs/presets.md) says. |
| `--test-suite=song-export --no-skip` | The song export (`ExampleSongTests.cpp`), with `PHAZITE_SONG_EXPORT` naming its folder, `PHAZITE_SONG` one song by its id ("exodus") and `PHAZITE_SONG_SOLOS` "all" or tracks' names between commas, each also soloed in the mix: writes each example song's mix and stems as WAV files, to listen to and to measure against its original. |
| `--test-suite=readme-picture --no-skip` | Paints `docs/images/phazite.png`, the README's picture of the window (`ReadmePicture.cpp`). |

The real-time audit, `--test-suite=realtime-audit`, counts allocations on the audio thread and shows where they come
from ([docs/realtime-audit.md](../docs/realtime-audit.md)); run it from the `windows-debug` build.

## Golden tests

The golden tests (`--test-suite=golden`: `*Golden*.cpp` and the catalogue case in `AppShellTests.cpp`) compare with
files committed in `golden/`:

| Folder | What is compared |
|---|---|
| `golden/projects/` | What Phazite saves. |
| `golden/renders/` | What it plays: level fingerprints of offline renders, never audio, of songs made with `TestSong.h` that show everything that plays, each rendered through Phazite's engine, of the open project and of a copy of its song alone, and checked before it is compared, with `RenderHarness.h`. |
| `golden/commands/` | The model's commands: ids, texts, switches and order; a script that runs each of them, with the saved result, the undo step each makes, and every command's enabled, ticked and name state along the way; and every command the app registers, in order, with its keys (`app-catalogue.txt`). |
| `golden/format/` | The formats' test vectors: the song's rules, every device's parameters, and a pack with what it holds ([docs/project-format.md](../docs/project-format.md), [docs/versions-format.md](../docs/versions-format.md)). |
| `resources/presets/` | The presets that come with Phazite, each as Save Preset writes it (the golden case in `FactoryPresetTests.cpp`). |

`ScrollingFrameTests.cpp` checks what the playlist and the piano roll show from frame to frame while they follow the
playhead, by painting frames as the window does, by each renderer Phazite's windows paint with (Direct2D, and the
software renderer that Linux's use): nothing beside the time line changes, everything on it moves as far as the song
played, and what stays in view stays put.
