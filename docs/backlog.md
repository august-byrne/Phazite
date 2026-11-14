# Backlog

Everything that is open in Phazite and is not a bug: quality gaps, design changes, missing features, tests, docs and
structure, one row for each item, by area. Each row is ready to become a GitHub issue: it says what is to be done, where
in the repository it starts, how large it is and when the plan takes it up. Bugs are in [known-bugs.md](known-bugs.md),
and nothing is listed in both.

**The columns.**

- **Where** is the file or folder to start reading in, not all that the item touches.
- **Size** is the plan's: S under a day, M one to three days, L about a week, XL several weeks. A size in brackets is
  that of the plan's step the row is a part of. A dash means the plan gives none.
- **Stage** is the plan's stage that takes the item up; a dash means the plan names none yet.
- **Source** names where the item comes from: an area of the whole-project quality audit with the finding's number there
  (`engine 10`), one of the five older audits, which are kept as history in `archive/` (the DSP, recording, format,
  source control and performance audits), or the plan itself. Several sources on a row mean that several readers found
  the same thing.

**The plan's stages**, in order:

| Stage | Name | What it is |
|---|---|---|
| 1 | Safe | The bugs that lose work, and the rest of the bugs |
| 2 | Caught | The test nets, and the docs put in order |
| 3 | Checked | Checks by hand: [hand-checks.md](hand-checks.md) |
| 4 | Decided | Decisions built: the versions cut down, backups kept by age, automation along a control's travel, mute and solo that silence the output only, files from others untrusted |
| 5 | Wide | Wider notes and strips, warped clips heard at once, controllers recorded and edited, MIDI learn. Then a beta, with no promise that its projects open later |
| 6 | Deep | Every built-in device and its editor brought to the best plug-ins' standard |
| 7 | Professional | MIDI out, note effects, multi-out, comping, a cue mix, the Multisampler |
| 8 | Release | A signed installer, the guide, crash reports, the promise of compatibility. Then 1.0 |

## Instruments and effects

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Modulation and expression in the Synth, the FM synth and the Sampler: more than one free-running LFO with one target, tempo sync, aftertouch, MPE | `src/devices/synth/Synth.cpp` | (XL) | 6 | devices-instruments 4 |
| Hard sync and the wavetable's warps band-limited; oversampling for the oscillators that need it | `src/devices/wavetable/WavetableSynth.cpp` | (XL) | 6 | devices-instruments 6 |
| A filter with character: more than the one linear model, self-oscillation, and a Drive that drives instead of lowering the level | `src/devices/dsp/MultimodeFilter.cpp` | (L) | 6 | devices-instruments 7 |
| Pitch, cutoff, table position and levels glide between control steps instead of stepping every 16 samples | `src/devices/synth/Synth.cpp` | — | 6 | devices-instruments 8 |
| Wavetable frames keep more than 4 samples for a cycle of their highest harmonic, as the Synth's tables do | `src/devices/wavetable/Wavetable.cpp` | — | 6 | devices-instruments 9, DSP audit |
| Factory presets made again, so that they use what the engines can do | `resources/presets/` | L | 6 | devices-instruments 5, product-gaps 6 |
| Stock effects with no counterpart: a utility, a gate, a transient shaper, a de-esser, a bitcrusher, tremolo and auto-pan, a convolution reverb, a pitch shifter, a tuner | `src/devices/` | — | — | devices-effects 2, product-gaps 5 |
| Compressor: compressing from the sidechain, look-ahead, automatic make-up gain, stereo link options, a curve set by typed values | `src/devices/dynamics/Compressor.cpp` | L | 6 | devices-effects 4 |
| Reverb: a Room network long enough for its range of decay times | `src/devices/reverb/Reverb.cpp` | L | 6 | devices-effects 5 |
| EQ: mid/side, dynamic bands, a linear-phase mode, more than eight bands | `src/devices/eq/Equalizer.cpp` | L | 6 | devices-effects 7 |
| Delay: a feedback path that can be clean, and more than the basic echo | `src/devices/delay/Delay.cpp` | M | 6 | devices-effects 8 |
| Oversampling by choice, 4x and 8x, for the Saturator and the Filter | `src/devices/saturation/Saturator.cpp` | (L) | 6 | devices-effects 15 |
| Stretch modes: a formant switch, re-pitch, and a mode that slices drums at their hits | `src/devices/stretch/Stretch.cpp` | (L) | 5 | devices-effects 6, recording audit |
| Multiband: its attack, release and curve worked out once a block for each band, not for every sample | `src/devices/dynamics/Multiband.cpp` | — | 6 | devices-effects 13, performance audit |
| FM synth: four voices at once in SIMD registers, or operators skipped once their envelopes have ended; the wavetable synth's unison voices in SIMD | `src/devices/fm/FmSynth.cpp` | — | — | performance audit |
| A sampler's sound made ready (its hits found, its octaves made) in the background, not on the message thread | `src/model/devices/SamplerSound.cpp` | — | — | performance audit |
| MP3 files: a test of where a click lands, then the encoder's delay and padding trimmed by the LAME tag | `src/model/engine/AudioFiles.cpp` | — | — | DSP audit |
| A set of sounds to listen to for each device, and a verdict for each device once it is done | `src/devices/` | S each | 6 | plan |

## Playback engine

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Short fades at Stop, at the loop's end and at a jump, where audio is cut now | `src/engine/core/Engine.cpp` | S | 1 | engine 10 |
| Mute and solo silence a strip's output only, its instruments and effects playing on (which also ends known bug 78) | `src/engine/core/Engine.cpp` | M | 4 | plan |
| Wider notes: a MIDI channel, pitch bend, controllers, aftertouch and poly pressure, program change | `src/devices/core/Processor.h` | L | 5 | engine 12, plugin-hosting 7 |
| Stretched, pitched and reversed clips heard at once, stretched as they play, instead of silent until their render is done | `src/model/arrangement/ClipRenders.cpp` | L | 5 | model-arrangement 4, product-gaps 3 |
| Time-signature changes along the song | `src/model/transport/Tempo.cpp` | M | 5 | model-playback 12, product-gaps 14 |
| Wider strips: mono tracks, more than one output pair, a cue mix | `src/engine/nodes/Strip.cpp` | XL | 7 | plan, product-gaps 10 |
| Scale, measured first: a benchmark of 100 tracks, the project benchmark at 128 samples and on a laptop, a scheduler that doesn't spin on what it waits for, strips played in smaller blocks than the device's, read-ahead before a jump | `src/engine/core/Workers.cpp` | XL | 7 | engine 11, performance audit |
| Parameter smoothing that lasts longer than one block, and channel meters that keep less than 64 blocks | `src/engine/nodes/DeviceRunner.cpp` | — | — | engine 14 |
| The click: a level to set and an output to choose; audio files of more than two channels played whole | `src/engine/transport/Click.cpp` | — | — | engine 15 |
| Files at another rate than the engine's converted once into the song's cache, if many such clips at once ever weigh on a song | `src/engine/audio/Resampler.cpp` | — | — | performance audit, DSP audit |
| A key to its sound measured with a wired output (the target is under 15 ms), and the whole start timed, by hand | `docs/hand-checks.md` | — | 3 | performance audit |

## Plug-in hosting

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Plug-ins told that a render is offline | `src/host/core/Instance.cpp` | (M) | 1 | plugin-hosting 6 |
| A late, busy or crashed effect faded to silence, in time with the song, instead of its input passed through dry | `src/model/plugins/SandboxedProcessor.cpp` | (M) | 1 | plugin-hosting 13 |
| No helpers or shared memory left behind after a crash of Phazite's; the helper's platform code for macOS and Linux | `src/host/core/Platform.cpp` | S | 1 | plugin-hosting 15 |
| About twenty VST3s tried by hand | `docs/hand-checks.md` | M | 3 | product-gaps 7 |
| Channels, pitch bend, controllers and pressure to plug-ins, SysEx, and MIDI out of them | `src/model/plugins/SandboxedProcessor.cpp` | L | 5 | plugin-hosting 7, product-gaps 2 |
| Multi-out plug-ins, and plug-ins that aren't stereo | `src/host/core/Instance.cpp` | L | 7 | plugin-hosting 7, product-gaps 4 |
| A merge of two edits inside one plug-in, offered and never done by itself | `src/model/versions/SongMerge.cpp` | — | — | format audit |
| The knob values of a plug-in that wasn't playing as a version was saved | `src/model/plugins/PluginInstances.cpp` | — | — | format audit |
| The files a plug-in loads by itself kept with the project. Not planned: nothing outside the plug-in knows them | `src/model/plugins/PluginInstances.cpp` | — | — | format audit |

## Song model

### Saving, undo and files

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| An undo depth to set (about 30 steps in practice now) | `src/model/base/Song.cpp` | (S) | 1 | model-project-services 10 |
| Backups kept by age, not by a count of ten that Auto Save turns over in under two hours | `src/model/project/Project.cpp` | S | 4 | model-project-services 6 |
| Files from others untrusted: a song's values checked against their ranges as it opens, sound paths kept inside the project, readers fuzzed | `src/model/project/Project.cpp` | L | 4 | format audit, plan |
| A project and its versions keep no full path that can hold the user's name: an audio file whose copy into the project failed, a plug-in's file in a per-user folder | `src/model/base/internal/Schema.h` | — | — | format audit |
| The formats named and versioned when compatibility starts: a file that names the store's format, its version and its hash, room for a pack's entries to grow, a format name and version on the project file | `src/model/versions/PieceStore.cpp` | (L) | 8 | format audit, product-gaps 12 |

### Recording

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Takes that survive a crash or a power cut: the file's header kept up to date, and the take found again | `src/model/transport/Take.cpp` | M | 5 | model-playback 8, product-gaps 10 |
| Arming a track never stops and reopens the audio device | `src/model/transport/AudioRecorder.cpp` | M | 5 | model-playback 4 |
| An input meter on the track while it is armed, a clip mark after 0 dBFS, and pre-roll | `src/model/transport/RecordingModel.h` | (M) | 5 | recording audit, product-gaps 10 |
| Several MIDI tracks armed at once | `src/model/transport/RecordingModel.h` | — | — | recording audit |
| A setting for what monitoring's latency does to where a recording is placed | `src/model/transport/Take.cpp` | — | — | recording audit |
| Take lanes and comping: a lane for each take under its track, a range of a lane played in the main lane with short crossfades, Next and Previous Take | `src/model/transport/RecordingModel.h` | L | 7 | recording audit, product-gaps 10 |
| Warp commands (Warp From Here, Set 1.1.1 Here, Warp as a loop of so many bars), which need tempo detection, and a sensitivity for the hits found | `src/model/arrangement/AudioClipModel.h` | — | — | recording audit |
| More bouncing: Paste Bounced Audio, and a selection of clips bounced to a new track | `src/model/mixer/FreezeModel.h` | — | — | recording audit |

### Arrangement

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| MIDI import of every track of a file, and MIDI export | `src/model/arrangement/Import.cpp` | M | 5 | model-arrangement 2, model-arrangement 11, product-gaps 11 |
| Notes and clips found by an index, and linked clips that share their notes instead of each keeping a copy | `src/model/notes/Notes.cpp` | L | 5 | model-arrangement 5 |
| Duplicate Track makes clips of its own, not clips linked to the original's (a decision to take) | `src/model/arrangement/Tracks.cpp` | — | — | model-arrangement 9 |
| Warp markers written into and read from DAWproject files | `src/model/project/DawProject.cpp` | — | — | recording audit |

### Mixer, automation and presets

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Automation kept, drawn and nudged along the control's travel, not in hertz and milliseconds | `src/model/automation/Automation.cpp` | L | 4 | model-mixer 1 |
| Devices moved, copied and duplicated between tracks, and deleted, duplicated and dragged in the mixer and the Devices view | `src/model/mixer/MixerModel.h` | M | 5 | model-mixer 2, ui-core-mixer-app 4 |
| The strip: input trim, polarity, mono, width, a choice of pan law, a track delay | `src/model/mixer/MixerModel.h` | M | 5 | plan, ui-core-mixer-app 1 |
| Presets that carry their own samples, and say which of their parts are missing | `src/model/presets/Presets.cpp` | M | 5 | model-mixer 13 |
| Buses send to returns, and returns go elsewhere than the master | `src/model/mixer/MixerModel.cpp` | (L) | 7 | model-mixer 9 |
| A device keeps its sidechain when it goes into a container | `src/model/mixer/Containers.cpp` | (L) | 7 | model-mixer 10 |
| Macros automated; macros and modulators modulated | `src/model/modulation/Modulation.cpp` | (L) | 7 | model-mixer 12 |

### Versions

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| A design note of what the versions are cut down to, for approval | `docs/design/versions.md` | S | 4 | plan |
| The cut itself: staging, a commit checked out on its own and ticked merge lines removed, the per-computer records parked; plain words where git's words stand | `src/model/versions/VersionsModel.cpp` | L | 4 | ui-shell-browser 10, model-versions 12, source control audit |
| The store swept of what nothing names any more | `src/model/versions/PieceStore.cpp` | (M) | 4 | model-versions 7 |
| Verify History checks the audio, the snapshots and the songs left on branches too | `src/model/versions/VersionHistory.cpp` | (M) | 4 | model-versions 10 |
| The open song tied to the commit it grew from | `src/model/versions/VersionHistory.cpp` | (M) | 4 | model-versions 9 |
| One undo for every source control operation: a log of operations (a branch switch, a deletion) to take back | `src/model/versions/VersionHistory.cpp` | — | — | source control audit |
| The undo history kept across a branch switch | `src/model/versions/VersionsModel.cpp` | — | — | source control audit |
| One old commit reverted while the later ones are kept | `src/model/versions/SongMerge.cpp` | — | — | source control audit |
| Undo Last Commit on a merge's commit | `src/model/versions/VersionHistory.cpp` | — | — | source control audit |
| Backup of the history to another folder, then the cloud | `src/model/versions/VersionStore.cpp` | — | — | source control audit |
| Only the named commits shown, once histories grow long | `src/ui/versions/VersionsView.cpp` | — | — | source control audit |
| A commit's changes marked on the song in the playlist | `src/ui/versions/VersionsView.cpp` | — | — | source control audit |
| Exports tied to the commits they were made from | `src/model/project/Export.h` | — | — | source control audit |

## UI and UX

### Timeline, playlist, piano roll and audio clip editor

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Escape abandons any drag, in every editor and lane (it does in the piano roll alone) | `src/ui/playlist/PlaylistTools.cpp` | (M) | 1 | ui-timeline-editors 6, tests-ui 10 |
| Automation lanes' drags scroll the view at its edges, and points are selected across lanes | `src/ui/playlist/PlaylistAutomation.cpp` | — | — | ui-timeline-editors 11 |
| A clip dragged or drawn onto a track of the other kind is refused, as the design's rule says that audio and notes don't share a track | `src/ui/playlist/PlaylistTools.cpp` | — | — | ui-timeline-editors 8 |
| The audio clip editor zooms and scrolls | `src/ui/audioclip/AudioClipEditor.cpp` | L | 5 | ui-timeline-editors 4 |
| Controller, pitch-bend and pressure lanes in the piano roll, for what clips hold and play unseen, with note chance and grooves after them | `src/ui/pianoroll/PianoRollLanes.cpp` | L | 5 | ui-timeline-editors 3, product-gaps 9, model-arrangement 11 |
| Snapping to clips' edges and markers as well as the grid; trim and slice on several items at once | `src/ui/timeline/TimelineEditor.cpp` | M | 5 | ui-timeline-editors 10 |
| Why a clip moved a beat in the big song costs 8.1 ms of the message thread where it cost 3.2 (the overview's marks, probably), and why Save grew from 103 to 145 ms | `src/ui/playlist/Playlist.cpp` | — | — | performance audit |
| A waveform's picture kept while a long clip crosses the view's edge, and clips not repainted for a knob turned while playing | `src/app/ClipWaveforms.cpp` | — | — | performance audit |
| The displays ticked at the display's frames, with the views' own timers folded into that tick and the playback session's stopped while nothing plays (about 1 % of a core at rest) | `src/ui/components/DisplayTicker.h` | — | — | performance audit |
| Frames that JUCE shows a frame late reported to JUCE with the measurements, if they bother anyone | `src/ui/timeline/FrameLog.cpp` | — | — | performance audit |

### Device editors

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The compressor's and the multiband's curves set precisely: typed values, a readout of threshold and ratio, the keyboard, an axis with room below -18 dB | `src/ui/devices/DynamicsCurve.cpp` | M | 6 | ui-device-editors 1 |
| One grammar for every graph: what a double-click, a right-click, a press and the fine-drag key do | `src/ui/devices/DeviceGraph.h` | L | 6 | ui-device-editors 9 |
| Drum pads sound on the press, not the release, and at more than one fixed velocity | `src/ui/devices/DrumSamplerEditor.cpp` | S | 6 | ui-device-editors 5 |
| Automate and Modulate on switches, choices and graphs' handles | `src/ui/devices/ParameterControls.cpp` | M | 6 | ui-device-editors 10 |
| Histories and analyzers that take more than one reading a tick, and show nothing stale as an editor comes back | `src/ui/devices/LevelHistoryView.cpp` | — | 6 | ui-device-editors 7 |
| The wavetable's picture shows Warp, and controls show what the synth's own matrix does to them | `src/ui/devices/WavetableEditor.cpp` | — | 6 | ui-device-editors 12 |
| The pages' bar overflows into a menu, and a part keeps its switch under tabs | `src/ui/devices/DeviceLayout.cpp` | — | 6 | ui-device-editors 14 |
| The EQ's editor on the shared frame; no private timers, pixel sizes or alphas in single editors | `src/ui/devices/EqEditor.h` | — | 6 | ui-device-editors 15 |

### Mixer and controls

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The wheel over a knob or a fader: one undo step for a burst, and no change by a wheel that only scrolls past | `src/ui/components/Controls.cpp` | (S) | 5 | ui-core-mixer-app 2 |
| Typed values take a decimal comma, and a bare number's unit shows | `src/ui/components/ValueText.cpp` | (S) | 5 | ui-core-mixer-app 9 |
| Several strips at once: linked faders, strips selected together and reordered; strip widths, a taller fader, RMS meters and a clip light that holds | `src/ui/mixer/ChannelStrip.cpp` | L | 5 | ui-core-mixer-app 1 |
| Right-click menus, and the rows' actions in the browser and Source Control, reached from the keyboard | `src/ui/components/Controls.cpp` | M | 5 | ui-core-mixer-app 7, ui-shell-browser 12 |
| Fine drag switched by its key during a drag, and knob drags that go on past the screen's edge | `src/ui/components/Controls.cpp` | — | — | ui-core-mixer-app 12 |
| Small ones: a theme colour that goes stale, a menu action that holds its component by a bare pointer, the system's settings for less motion and transparency followed | `src/ui/mixer/DeviceSlots.cpp` | — | — | ui-core-mixer-app 13 |

### Shell

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The main window remembers its size, place, monitor and whether it was maximised | `src/app/MainWindow.cpp` | S | 1 | ui-shell-browser 0 |
| Esc or the window's close button asks before it cancels a running export | `src/ui/shell/ExportPanel.cpp` | (M) | 1 | ui-shell-browser 8 |
| The Account button, which does nothing, taken out of the activity bar until it does something | `src/ui/shell/ActivityBar.cpp` | — | — | ui-shell-browser 14 |
| Secondary windows that don't block the main one: Settings, the plug-in scan, Chord Progressions and the rest | `src/app/MainWindow.cpp` | M | 5 | ui-shell-browser 2, ui-core-mixer-app 6 |
| One Settings window that holds what is spread over three tabs and four menus | `src/ui/shell/SettingsPanel.cpp` | L | 5 | ui-shell-browser 9 |
| The text the user reads in a strings table, with one term and one spelling for each thing; with it the design tokens in a file a designer edits, and a gallery app (architecture phase A4) | `src/ui/theme/DesignTokens.h` | L | 8 | ui-shell-browser 11, docs 5 |
| A session with Narrator, and what it finds fixed | `docs/hand-checks.md` | M | 3 | plan |
| Clips, notes, automation points and graphs' handles in a form a screen reader finds | `src/ui/playlist/ClipComponent.cpp` | L | 7 | tests-ui 5 |

## App and services

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Clean Up Unused Audio counts what the backups, the autosave and the file as saved play | `src/model/project/UnusedAudio.cpp` | S | 1 | model-project-services 13 |
| DAWproject import: a report of what a file held that Phazite left out, and files written by other DAWs in the tests | `src/model/project/DawProject.cpp` | M | 1 | model-project-services 4, tests-model 12 |
| Updates and the installer signed: the update file's signature checked with a key built into Phazite, the installer's with Windows, and the file held from its check to its start | `src/services/Updates.cpp` | M | 8 | model-project-services 7, tests-model 14, product-gaps 15 |
| The rescan of the MIDI inputs every four seconds taken off the message thread; what the autosave costs on the message thread measured apart | `src/model/engine/AudioIO.cpp` | — | — | performance audit |

## Tests

"In progress" rows are being built in stage 2 now.

| Item | Where | Size | Stage | Status | Source |
|---|---|---|---|---|---|
| UBSan fails a test, and the helper's sanitizer reports are kept | `CMakePresets.json` | S | 2 | In progress | tests-infrastructure 0, tests-infrastructure 1, build-ci-tools 1 |
| A JUCE assertion fails its test | `tests/TestMain.cpp` | S | 2 | In progress | tests-infrastructure 5 |
| Engine and device tests at other conditions: 44.1 kHz, blocks of 441 samples, 140 BPM, with the cases that hid the audit's bugs | `tests/EnginePlayback.h` | M | 2 | In progress | tests-audio 6, tests-audio 7, devices-instruments 10, devices-effects 9 |
| A golden for live playback, which the renders don't cover | `tests/GoldenRenderTests.cpp` | M | 2 | In progress | tests-audio 10 |
| Undo and Redo exact after every command | `tests/UndoTests.cpp` | M | 2 | In progress | tests-model 4, tests-ui 12 |
| The allocation guard on a song being edited, from the first block, on Linux too, and on more than one thread | `tests/RealtimeAuditTests.cpp` | M | 2 | In progress | tests-infrastructure 3, tests-infrastructure 4 |
| The command script runs every command, and the golden project holds every kind of data | `tests/CommandGoldenTests.cpp` | M | 2 | In progress | tests-model 6, tests-model 7 |
| All nine timeline tools driven by the mouse; the song changed between a press and its release; items off the grid and parts of a wheel notch | `tests/PlaylistTests.cpp` | M | 2 | In progress | tests-ui 0, tests-ui 4, ui-timeline-editors 14 |
| The shipped programs started in CI; MainWindow and the devices' windows made in a test; the device editors painted | `tests/AppScreen.h` | M | 2 | In progress | tests-infrastructure 6, tests-infrastructure 7, tests-ui 1, tests-ui 2 |
| The readers fuzzed: project files, presets, DAWproject, the versions' packs, the sandbox's messages and shared memory | `tests/SandboxTests.cpp` | L | 2 | In progress | tests-infrastructure 8, tests-model 9, tests-audio 11 |
| Fingerprints of the golden renders that see pitch, spectrum and small timing errors | `tests/RenderHarness.h` | M | 2 | In progress | tests-infrastructure 11 |
| A gate on speed: a device that gets much slower fails a check | `tests/Benchmarks.cpp` | M | 2 | In progress | tests-infrastructure 9 |
| Tests in the light theme and at other window sizes and scales; the design-token rule counted by the architecture check | `tools/check-architecture.sh` | M | 2 | In progress | tests-ui 7, tests-ui 8 |
| A long run every night under ASan and TSan, with songs of several configurations | `.github/workflows/ci.yml` | M | 2 | In progress | tests-infrastructure 14 |
| A device conformance suite on a shared library of DSP probes: every device through the same checks (values that aren't numbers, blocks of no length, mono, extremes, reported latency against measured delay) | `tests/TestDevices.h` | M | 2 | In progress | tests-audio 8, tests-audio 3, tests-audio 9 |
| A real VST3 built in the repository and loaded by the tests | `tests/host/TestPlugins.cpp` | M | 2 | In progress | tests-infrastructure 2, plugin-hosting 14 |
| Versions tests with folders and buses, clips moved between tracks, restarts, and random edits; random tests of the song's text, the pieces and apply-song | `tests/VersionsMergeTests.cpp` | M | 2 | Open | tests-model 3, tests-model 10, model-versions 14 |
| The failure paths of saving and recovery: a save that fails, a first save that fails and leaves its folder, undo across view-state changes | `tests/ProjectTests.cpp` | — | 2 | Open | model-project-services 14, tests-model 8 |
| The lock-free parts raced: the event queue (which has no test), the handover, the playhead's stamps | `src/engine/core/EventQueue.h` | — | 2 | Open | tests-audio 5 |
| Cut, Copy and Paste tested beyond their five cases | `tests/ClipboardTests.cpp` | — | 2 | Open | tests-model 13 |
| Tests that wait on the wall clock made to wait for a condition, and kept apart from the files that load every core | `tests/RealTimePlayback.h` | — | 2 | Open | tests-infrastructure 12, tests-audio 14, tests-model 15 |
| The drum machines' voices pinned by more than a peak level and two levels over time | `tests/DrumSynthesisTests.cpp` | — | 2 | Open | tests-audio 13 |
| The screen-reader walk and the "every control explains itself" walk run over a song with tracks, clips and devices, not one track or an empty project | `tests/AppShellTests.cpp` | — | 2 | Open | tests-ui 5, tests-ui 6 |
| Graph-drag tests that pin the axes; the keymaps' Shift rule asserted on the keymap files; smoke tests renamed or made to assert; the hard-coded device list replaced | `tests/EffectEditorTests.cpp` | — | 2 | Open | tests-ui 11, tests-ui 13, tests-ui 14 |
| The components with no test or a token one, most important first, by the count of test cases against lines for each folder | `tests/MixerViewTests.cpp` | — | 2 | Open | tests-ui 9, tests-ui 15 |
| Golden renders regenerated one at a time, each with its reason, not all at once inside large commits | `tests/GoldenFiles.cpp` | — | 2 | Open | tests-infrastructure 10 |

## Docs

All of these are stage 2, and in progress now, except the guide's pages that wait for a later stage.

| Item | Where | Size | Stage | Status | Source |
|---|---|---|---|---|---|
| The checks by hand in one list, so that "done" says what a person has checked | `docs/hand-checks.md` | S | 2 | In progress | docs 2, product-gaps 7 |
| The audits' open findings in known-bugs.md and this backlog, the audits kept as history | `docs/backlog.md` | M | 2 | In progress | docs 6, docs 14, devices-effects 14 |
| CLAUDE.md's layout as a table, with the detail in a README for each folder, and one layer map | `CLAUDE.md` | M | 2 | In progress | docs 0, docs 12, docs 13 |
| The architecture on a page of its own, where three documents each hold a layer map | `CONTRIBUTING.md` | S | 2 | In progress | docs 12 |
| design.md as present-tense pages, a table of decisions and an archive | `docs/design/decisions.md` | L | 2 | In progress | docs 1, product-gaps 13, ui-shell-browser 15, ui-timeline-editors 13 |
| The open questions, and the choices made for review, in one list | `docs/open-questions.md` | M | 2 | In progress | docs 3 |
| roadmap.md cut down to its status, the open steps and the risks, with the scope it really has | `docs/roadmap.md` | M | 2 | In progress | docs 4, docs 15, product-gaps 8 |
| The versions as built, on one page in one vocabulary | `docs/design/versions.md` | S | 2 | In progress | docs 9, model-versions 15 |
| The current performance numbers on one page | `docs/performance.md` | S | 2 | In progress | docs 11 |
| The guide: a page for each view and each device (the devices' pages wait for stage 6, the versions' page for stage 4) | `docs/guide/` | L | 2 | Open | product-gaps 15 |

## Product gaps

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The checks by hand: recording through an ASIO® interface, a MIDI keyboard, a DAWproject opened in another DAW | `docs/hand-checks.md` | — | 3 | product-gaps 7, docs 2 |
| MIDI learn | `src/model/engine/AudioIO.cpp` | M | 5 | product-gaps 1 |
| Export of a range or the loop, as MP3, and templates to start a project from | `src/model/project/Export.h` | M | 5 | product-gaps 11 |
| Note effects that play as notes come in (an arpeggiator, chords, a scale), and MIDI from plug-ins | `src/devices/core/DeviceInfo.h` | L | 7 | product-gaps 2 |
| MIDI output, clock out, and a MIDI input for each track | `src/model/engine/AudioIO.cpp` | L | 7 | product-gaps 1 |
| Any track or bus as a track's output | `src/model/mixer/MixerModel.h` | (M) | 7 | product-gaps 14 |
| The Multisampler as a plain SFZ player with one piano; with it, sampled acoustic instruments | `src/devices/sampler/Sampler.cpp` | XL | 7 | product-gaps 6 |
| Release work: crash reports with symbols, Linux tried, compatibility of projects from 1.0 on | `packaging/windows/Phazite.iss` | L | 8 | product-gaps 15, product-gaps 12 |
| Out of 1.0: video, notation, a clip launcher, scripting, CLAP, ARA, macOS, translations, Ableton Link, control surfaces | `docs/roadmap.md` | — | — | plan, product-gaps 4, product-gaps 14 |

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

## Structure and build

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| CI builds `windows-profile`, the preset the README names; its jobs have time limits; the version comes from the tag | `.github/workflows/ci.yml` | S | 1 | build-ci-tools 10, docs 7 |
| Precompiled headers for JUCE's module headers in Phazite's own files (a full build is about 5,300 compile-seconds, most of it those headers read again) | `cmake/PhaziteHelpers.cmake` | M | 2 | build-ci-tools 4 |
| A Clang build with static analysis, and ASan on Windows; after them macOS and a measure of coverage | `CMakePresets.json` | M | 2 | build-ci-tools 3, tests-infrastructure 13 |
| Each layer a library of its own, so that the build proves the layers and the architecture check doesn't only count them | `CMakeLists.txt` | L | 5 | build-ci-tools 7 |
| The size of files counted by the architecture check, as phase A2's limit of 1,000 lines isn't | `tools/check-architecture.sh` | S | 5 | docs 10 |
| The solo rule in one place, where the track list and the song player each have it; with it the previews' sync and the song player's size | `src/model/playback/SongPlayer.cpp` | M | 5 | model-playback 15 |
| Release builds with symbols kept, and a dump left by a user's crash | `CMakeLists.txt` | M | 8 | build-ci-tools 2 |
| Linux for a beta: PipeWire or JACK beside ALSA, HTTPS, install rules | `CMakeLists.txt` | L | 8 | build-ci-tools 5 |
| The installer's silent update tested, and an uninstaller that minds a helper still running | `tools/test-installer.ps1` | S | 8 | build-ci-tools 6 |
| The pins made whole: clang-format by its hash, `format.sh` warning of another version, action and submodule pins refreshed by a bot, a SECURITY.md | `tools/format.sh` | — | — | build-ci-tools 9 |
