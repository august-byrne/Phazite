# Migrating off Tracktion Engine

*Status: written 2026-10-30 at the owner's request; T0 done and T1 under way, on the owner's go-ahead of 2026-10-30.* It takes stock of what Phazite still uses Tracktion Engine for, lays out the phases that would move each part to code of Phazite's own, and lists the Tracktion-isms that go with them. Sizes are relative effort, S < M < L < XL, as in the [roadmap](roadmap.md); they are not dates.

**How this sits with the earlier decision.** Design decision 1 (2026-10-14, [design section 23](design.md#23-tracktion-engine-what-it-gives-and-what-we-build)) kept Tracktion and allowed replacing a part only after a dated decision for that part; the stretching of audio clips (2026-10-28) and the five parts decided on 2026-10-30 ([roadmap](roadmap.md#architecture-track) A5) were the first. This plan lays out the whole path (architecture phase A6), so that each part's decision is made with the rest in view. Each phase still starts on the owner's go-ahead, dated in the [decision log](design.md#26-decision-log).

**Why.** Each part Phazite has taken over so far (stretching, presets, the selection, sections, modulation) removed workarounds and opened something Tracktion couldn't do. What remains costs Phazite in three ways:
- **Workarounds.** Phazite carries one for each Tracktion behaviour that doesn't fit it: 70 are listed [below](#tracktion-isms-to-drop), from undo that doesn't restore what plays to time kept in seconds.
- **The real-time rules.** Most of the [real-time audit](realtime-audit.md)'s 25 findings are in Tracktion's code, which Phazite can only report upstream or avoid; 5 races and 1 leak in it are suppressed in the sanitizer builds (`tools/tsan-suppressions.txt`, `tools/lsan-suppressions.txt`).
- **What it holds back.** Live notes timed by an approximate clock (audit findings 22 and 23); modulation applied once a block to everything but Phazite's own devices; and a playback graph that Phazite can extend only with plug-ins of Tracktion's kind, so that sandboxed plug-ins, and CLAP after v1, would have to be wedged in as Tracktion plug-ins.

## What Tracktion does for Phazite today

Snapshot of 2026-10-30 (Phazite at `a613ff7`, Tracktion 3.5.0 at `bb38617`). Only the model (`src/model/`) and a little of the app (`src/app/`) name Tracktion; the UI, the services and the devices' DSP name none, and the UI compiles without Tracktion's headers.

| Part | What Tracktion does | Where in Phazite | How deep | Phase |
|---|---|---|---|---|
| The project | Tracktion's Edit, a `juce::ValueTree`, is the project, and the `.phazite` file is its XML (which Phazite writes itself). Phazite's own state is a PHAZITE element in it, and properties on Tracktion's TRACK, PLUGIN, clip and NOTE elements. Unsaved changes lean on Tracktion's changed flag | `model/project/`, `model/base/internal/Schema.h` | Deep | T1 |
| IDs | Tracks', clips', devices' and modulators' IDs are Tracktion's item IDs; sections, macros and containers' racks take numbers from the Edit's counter | `model/base/internal/Tracktion.h` | Deep | T1 |
| Undo | The Edit's undo manager, whose automatic unnamed steps Phazite holds off with inhibitors | `model/commands/` | Deep | T1 |
| Tempo, time signature, key | One tempo and one time signature at the song's start; the song's key in Tracktion's pitch sequence; every conversion between beats and seconds | `model/transport/Tempo.cpp`, and everywhere | Deep | T1 |
| Tracks | The track list, folders, mute and solo | `model/arrangement/Tracks.cpp` | Deep | T1 |
| Clips and notes | MIDI and audio clips placed in seconds, notes in Tracktion's MIDI lists, aliases as link groups, fades as Tracktion's curve types, splitting, paste; Tracktion plays Phazite's renders of stretched clips as plain clips | `model/arrangement/`, `model/notes/` | Deep | T1, T2 |
| Automation | Curves with points in seconds, played and recorded in read, touch, latch and write modes; values written into the project as they play | `model/automation/` | Deep | T1, T2 |
| Modulation | Modifiers (six kinds), macros and what they modulate, moved once a block; Phazite moves its own devices' parameters within the block | `model/modulation/`, `model/devices/internal/DeviceModulation.h` | Deep | T1, T2 |
| The mixer | The channel strip as Tracktion plug-ins: volume and pan, meters, sends and returns, a VCA on folders; buses as submix folders; sidechain routing; a plug-in's mix as Tracktion's dry and wet levels | `model/mixer/` | Deep | T1, T2 |
| Containers | Racks: a rack type played by a rack instance | `model/mixer/Containers.cpp` | Deep | T1, T2 |
| Phazite's devices | A Tracktion plug-in around each device's DSP, which gets its parameters, notes, time and tempo, sidechain and latency compensation from Tracktion | `model/devices/DevicePlugin.cpp` | Deep | T2 |
| Plug-ins | Hosting (`ExternalPlugin`), their windows (through the UI behaviour), the scan's child process (driven by Phazite's scanner, with its time limit) | `model/mixer/`, `model/devices/`, `model/engine/PluginScanner.cpp`, `src/app/` | Deep | T3 |
| Playback | The graph and its threads, delay compensation, the transport and its loop, the metronome and the count-in | `model/transport/`, `model/engine/` | Deep | T2 |
| Live input | MIDI inputs (the computer keyboard plays into a virtual one), monitoring, guide notes | `model/engine/LiveInput.cpp` | Deep | T4 |
| Recording | MIDI recording, and Capture through Tracktion's retrospective buffer; for audio, the inputs, their routing, monitoring and timing (Phazite writes the files) | `model/transport/` | Deep | T4 |
| Audio devices | `te::DeviceManager` around JUCE's device manager | `model/engine/EngineContext.cpp` | Deep | T4 |
| Rendering | Export and stems (`EditRenderer`, `RenderQueue`, loudness normalization, dither), freeze and bounce (`FreezePointPlugin`, render jobs) | `model/project/Export.cpp`, `model/mixer/Freeze.cpp` | Deep | T5 |
| Previews | An Edit of its own for each sound the browser plays, its transport and loop | `model/engine/Preview.cpp` | Medium | T5 |
| Engine setup and settings | The engine, its behaviour and its UI callbacks; Tracktion's `Settings.xml` holds Phazite's own settings too, the plug-in list, the audio setup and the MIDI input's options | `model/engine/EngineSetup.*`, `src/app/AppServices.cpp` | Deep | T3, T4, T6 |
| Small helpers | The grid's snap levels (`TimecodeSnapType`), scales (`te::Scale`), fade shapes, the loop information in audio files, the audio formats Tracktion reads | `model/arrangement/Grid.cpp`, `model/notes/Notes.cpp`, `model/arrangement/Import.cpp` | Light | T1 |

**Already Phazite's own:** the UI, the commands and keymaps, and the model's functions by IDs, beats and plain values; the devices' DSP; stretched, pitched and reversed clips (Rubber Band R3); waveforms; presets; the selection; sections; the chord lane; note IDs; the time edits (insert, remove, move and copy time); the clipboard; the project folder and autosave; the list of settings; the files of audio recordings; and the modulation of its own devices within a block.

**Not used:** Tracktion's selection manager, `EditLoader`, clipboard and clip launcher; its instruments and effects (but for a tone generator in the audit's tests); its time-stretchers; the content of its chord, arranger and marker tracks; its waveform thumbnails; and every option it leaves off by default (ARA, ReWire, video, REX, control surfaces, Ableton Link, FFmpeg, LAME). `EditFileOperations` is used in two tests only.

**How much names Tracktion** (`te::`, counted 2026-10-30): arrangement 769, mixer 641, transport 279, devices 181, engine 179, automation 165, project 157, notes 149, modulation 108, presets 86, chords 74, base 65, the app 47, commands 43. In the tests about 1,180, in 94 of 134 files: 69 files build a Tracktion Edit themselves (169 times), where 21 make a project through the model. The project goldens are Tracktion's Edit XML, and the render goldens are made by Tracktion's renderer.

**What comes in through Tracktion besides itself:** JUCE (Tracktion's submodule, `external/tracktion_engine/modules/juce`, with `juce_dsp`, `juce_osc` and `juce_gui_extra` pulled in by Tracktion), doctest (bundled with Tracktion), and choc's FIFO (used by `AudioRecorder`).

## How to migrate

**The seam is already there.** Since architecture phase A3 the UI reaches the project only through the model's functions, by Phazite's own IDs, beats and plain values. The migration happens behind that seam: the UI, the commands and the keymaps don't change, and neither do the tests that drive the model through its public functions.

**The order: first own the project, then own the sound.** Phazite's project becomes the truth while Tracktion still plays it, through a mirror that Phazite builds and keeps in step; then Phazite's own engine replaces Tracktion's behind the mirror, one capability at a time, both engines running side by side until their renders agree; then Tracktion goes. The other way round (an engine first, playing Tracktion's project) would build the new engine on the very conventions this plan exists to drop: time in seconds, Tracktion's IDs, its undo.

**Rules for every phase:**
- Each phase is its own decision, dated in the design's decision log, and starts on the owner's go-ahead. Each step in it ends with something that builds, passes every test and can be tried, as the roadmap's steps do, in its own commit.
- The golden tests are the safety net. The render fingerprints and the command scripts must not change unless a step means them to; the project goldens change format once, on purpose, when Phazite's own file replaces Tracktion's. Pre-alpha keeps no backward compatibility (2026-10-18), so that change needs no migration code.
- Phazite's engine code follows the [real-time rules](../CLAUDE.md) from its first line, and each part of it gets a guarded scenario in the real-time audit before it plays by default.
- Engine code is engine-neutral, as `src/devices/` is: JUCE's `juce_core`, `juce_audio_basics` and `juce_dsp`, nothing of Tracktion or of the model, so that it is tested on its own.
- Performance is measured before and after each engine step (`tests/Benchmarks.cpp`, [performance-audit.md](performance-audit.md)).
- Features keep being built between steps, on the model's functions, which don't change.

## The phases

| Phase | What Phazite takes over | Size | Needs |
|---|---|---|---|
| T0 | Safety nets: golden renders of everything that plays, a harness for two engines, tests that build projects through the model | M | — |
| T1 | The project: Phazite's own document, file and undo; Tracktion's Edit a mirror of it | XL | T0 |
| T2 | The sound: Phazite's own playback engine, side by side with Tracktion's until they agree | XL | T1 |
| T3 | Plug-ins: hosting of Phazite's own, sandboxed (A5 part 5, step 3.3) | XL | T0; T2 to play through it |
| T4 | Inputs and recording: audio devices, MIDI inputs, live playing, recording and Capture | L | T2 |
| T5 | Rendering: export and stems, freeze and bounce, previews | M | T2, T3 |
| T6 | Tracktion removed: the mirror, the workarounds and the submodule go | M | all |

T3 can start beside T1: hosting doesn't depend on who owns the project, and its first step is a measurement.

### T0. Safety nets (M)

Nothing in T1 to T5 can be checked by ear alone, so first the tests that will tell two engines, or two project stores, apart.
- **Golden renders of everything that plays,** each a level fingerprint as `tests/golden/renders/` keeps them, never audio. Today there are three (a mix, the starter project, the wavetable synth). Added: a MIDI clip into each instrument; audio clips plain, looped, faded, stretched, pitched and reversed; aliases; automation in each mode, with its curve shapes; each modulator and a macro; sends, returns, a bus and a sidechain; a device that delays its sound on one of two parallel paths (latency compensation); a tempo change and a time signature change mid-song; the metronome and the count-in; a frozen track; an export's stems.
- **A harness for two engines:** a test renders the same project through either engine and compares the fingerprints, so that T2's engine is checked against Tracktion's feature by feature. Until T2 it runs Tracktion's on both sides.
- **Tests that build projects through the model:** a test song made with the model's functions (tracks, clips, notes, devices, modulation), so that tests stop building Tracktion's objects themselves. New tests use it from T0; the 69 files that build Tracktion Edits move to it as T1 reaches their areas.
- **A count that only goes down:** the architecture check counts `te::` in the model by area, so that its budget file shows the migration's progress and no new code adds to it.
- *Done when:* every feature in the list has a golden render; the harness runs; the test song covers what the tests build; the budgets count `te::` per area.

**Done (2026-10-30).** What was built, and where it differs from the list above:
- **49 golden renders** (`tests/golden/renders/`), each built through the model and checked before it is compared, so that a broken render never becomes a golden file (`tests/GoldenRenderTests.cpp`, `GoldenModulationTests.cpp`, `GoldenMixerTests.cpp`, `GoldenMetronomeTests.cpp`): the three there were, which still match; a MIDI clip into each instrument; audio clips plain with their gain, faded with each of the four fade shapes, stretched to follow the tempo, pitched and reversed; an alias; volume automation through bent lines, a hold and a jump, pan automation, and a device's parameter automated; each modulator but the random source, a track's macro and a container's; a sidechain, a send before the fader, mute and solo, sends and returns and a bus (in the mix); delay compensation across two paths; a song at 90 BPM in 3/4; the metronome and the count-in; a frozen track; an export's stems; and each of Phazite's 13 effects. Most are renders of a change, whose check compares the song before and after it ("the modulator moves the sound").
- **Not in the list after all:** a tempo change or a time signature change mid-song (a song has one of each, from its start, until the tempo lane); audio clips that loop (Phazite never loops a clip's audio: a loop follows the tempo, which the stretched render shows); automation's write modes (they only act while a control moves as the song plays live, which `tests/AutomationModelTests.cpp` tests); the random source, which Tracktion doesn't seed, so that it moves differently each time (a test checks that it moves).
- **The metronome and the count-in** sound only in real time, so they play through Tracktion's hosted device, kept from their first click on (`tests/RealTimePlayback.h` keeps what it plays).
- **The harness** (`tests/RenderHarness.h`) renders each song through a reference engine, Tracktion's, which the golden files hold, and a candidate: until T2, Tracktion's playing an Edit made afresh from a copy of the project in a context of its own. So it already checks what T1 asks of the document, that the project's state alone makes what it plays.
- **The test song** (`tests/TestSong.h`): a project made through the model's functions, by IDs, beats and plain values, with the app's commands; every new golden render uses it.
- **The counts:** `tools/check-architecture.sh` counts `te::` in each model area (`tracktion-arrangement`... `tracktion-transport`), in `src/app`, `src/services` and `tests/`, as budgets that only go down.
- **Found on the way** (in the [list below](#tracktion-isms-to-drop)): Tracktion's modulators keep where they were from one render to the next (an LFO's phase, a note follower's last note), so the same Edit rendered twice can differ; a new song's master fader starts at -3 dB, which stems leave out.

### T1. The project of Phazite's own (XL)

Phazite's project becomes the truth, and Tracktion's Edit becomes a mirror that a bridge builds from it and keeps in step, one way: Tracktion never writes into the project.
- **What holds it:** a `juce::ValueTree` in Phazite's own schema (`Schema.h`), with `juce::UndoManager` for undo, saved as XML. That is what the sections and the chord lane already use, it gives notifications, undo and saving for free, the golden tooling already compares XML, and JUCE stays after Tracktion. Plain C++ structures would need all three written again, for nothing the model needs: its functions already hand out plain values.
- **What it holds, in this order, each a step of its own with its golden files regenerated and read:**
  1. the tempo map and time signatures, in beats, and the song's key: everything else is placed on them;
  2. tracks, folders, buses, returns and the master, with their routing, sends, mute and solo, and their view state;
  3. clips and notes, in beats: MIDI clips with their notes and IDs, audio clips with their files, gain, pitch, reverse, tempo and fades (as Phazite's own fade shapes), and aliases as clips that play one shared list of notes;
  4. automation lanes, in beats;
  5. devices: Phazite's own by their type and parameter values; plug-ins by their identity, their parameters' own IDs and their state; containers with their devices and macros;
  6. modulators and macros, with Phazite's own defaults, and what they modulate, by amount;
  7. the transport's state that the project keeps (the loop and the play start, in beats; the metronome and count-in settings as Phazite's own values).
- **IDs of Phazite's own:** each kind of item numbered by the project, as notes already are.
- **The project file:** Phazite's own document, saved as it is held, with nothing of Tracktion's in it (no global tracks, clip slots, scenes, groove or video elements, no Tracktion enum numbers).
- **Undo:** the document's own, so that what plays always follows it.
- **The bridge (the mirror):** builds a `te::Edit` from the document and turns each change into the change Tracktion needs; Tracktion's changed flag, its notifications and the values it writes as it plays no longer reach the project. Recording, which Tracktion does for MIDI, hands what it recorded to the model, which writes it into the document.
- *Done when:* the mirror's Edit can be thrown away and built again from the document at any moment with no difference in what plays or what is saved; no model function reads Tracktion's state as the truth; the project file holds nothing of Tracktion's; every golden render and command script is unchanged.

**How T1 moves the song, part by part (decided 2026-10-30, by Claude):** each step leaves Phazite whole, so while some parts are in the document and others still in Tracktion's state, both are kept in one tree and one undo history:
- The document is Phazite's state in the Edit (`getPhaziteState()`), where the sections and the chord lane already are; one undo manager, the Edit's, holds both. So a change that touches both (a track deleted with the clips that are still Tracktion's) undoes as one, as it does today.
- The mirror is a set of reconcile functions, one for each part (`src/model/mirror/`): each writes into Tracktion's state what differs from the document, with the Edit's undo manager, and changes nothing when nothing differs. The model's functions call one after they change the document. An undo or a redo puts back both trees together, so the mirror has nothing to do then, but for what Tracktion keeps outside its undo history (tracks' mute, solo and solo isolation), which it sets again after each (`reconcileTrackSwitches()`).
- IDs stay the Edit's, and the file stays the Edit's XML with the document in it, until the last steps: then the document leaves the Edit with an undo manager of its own and IDs of its own, the mirror listens to it and builds the Edit from it (an undo then changes only the document, and the mirror follows), and the file is the document alone.
- The model's functions keep taking the Edit while the document is in it; the budgets of the areas whose functions gain one go up with a reason, and come down when the document leaves.

**T1 steps done:**
1. *The tempo map* (2026-10-30): the song's tempos and time signatures, each from a beat on, are Phazite's (`schema::TEMPOMAP`, `model/transport/internal/Tempo.h`), which every project has; Tracktion's tempo sequence mirrors them (`mirror::reconcileTempoMap()`); the time edits move the document's tempo changes; the bars of the time line, the chord lane and new clips are counted from it. The song's key isn't Tracktion's either: Phazite keeps a key for each track and the chord lane's keys, so Tracktion's pitch sequence, which only a test read, is left alone.
2. *The tracks* (2026-10-30): the song's track list is Phazite's (`schema::TRACKS`, `model/arrangement/internal/Tracks.h`): each track, folder and return in its place, a folder's tracks inside it, with its name and color, mute and solo, its key, its audio input, how it shows (height, collapsed, pinned, drum rows) and its automation lanes; the master's view has an element of its own (`MASTER`). Tracktion's tracks mirror them (`mirror::reconcileTracks()`), made, moved and deleted to match, with the mute, solo and solo isolation (a return's) they play with, and their names and colors too, so that what still reads them from Tracktion reads the song's. Tracktion's tracks still hold what is Tracktion's (clips, devices, a folder's bus devices). Every model function that adds, moves, renames or deletes a track, or reads what a track is, goes through the list; a new song's Edit comes from `createSongEdit()` (`model/project/internal/Project.h`), and tests make theirs with it rather than with Tracktion's `createSingleTrackEdit()`, and change tracks through the model. Two things not moved: sends, which are devices in Tracktion's channel strip, go with the devices (step 5), and so does a folder's being a bus (the devices it mixes through), whose switch, the song's, is in the list already. Tracktion keeps mute, solo and solo isolation outside its undo history, so a step that mutes a track (Bounce in Place mutes the track it rendered) leaves them behind on undo and redo: `phazite::undo()` and `redo()` set them from the song afterwards (`mirror::reconcileTrackSwitches()`), which begins no undo step.
3a. *MIDI clips and their notes* (2026-10-30): the song's MIDI clips are Phazite's (`schema::CLIP` on their tracks' elements, `model/arrangement/internal/Clips.h`), each where it is in beats (its start, its length and how far into its notes it starts), with its name, mute, alias group and chord part, its notes (`schema::NOTE`, each with its ID, from the Edit's IDs so that none is used twice) and the controller events a recording took in (`schema::CONTROL`, in MIDI's own terms). Tracktion's MIDI clips mirror them (`mirror::reconcileClips()`, `reconcileClip()`), in no link group of Tracktion's: the clips of an alias group hold the same notes, which every note edit changes in each (`model/notes/internal/Notes.h`), so that Tracktion's copying between linked clips never runs, and the bulk note edits take each note once a group. The tempo map's reconcile places the clips again. What Tracktion makes itself becomes the song's at once: a recording's clips and notes as it stops, inside its "Record" step (`adoptRecordedMidi()`), and a MIDI file's clip as it is imported (`adoptMidiClip()`); the NoteIDKeeper and `assignNoteIDs()` are gone. Phazite never looped a clip: the mirror turns a loop off. Audio clips are still Tracktion's (step 3b), and the clip functions work on Tracktion's for them.
3b. *Audio clips* (2026-10-30): the song's audio clips are Phazite's too (an audio `schema::CLIP`, `model/arrangement/internal/AudioClips.h`): its file, the part of the file it plays, in the file's own seconds whichever way it plays, where it starts, in beats, how it plays the part (following the song's tempo from its own, pitched, backwards: what was `phaziteSource`, `phaziteTempo`, `phazitePitch` and `phaziteReversed` on Tracktion's clip), its gain, its fades and their shapes (Phazite's names), its takes, its mute and alias group. How long it lasts isn't kept: it follows from the part and the tempo, so a tempo change stretches the clips that follow it again without anything to keep (`FollowingClipsOnBeats` is gone). The mirror lays Tracktion's clip out as `playFilePart()` did, playing the file or Phazite's render of it, and gives it the song's fades fitted to it as Tracktion's own getters would (Tracktion scales them outside the undo history when a clip gets shorter). Every clip edit now works on the song's clips alone: copies, moves, trims, splits (an audio clip loses its fades where it is cut), slips, crossfades, fade shapes, takes (`playTake()`), recording (`AudioRecorder::makeClips()`, a punch clearing the range through the model), import and the bounce.
4. *Automation* (2026-10-30): the song's automation is Phazite's (`schema::AUTOMATION`, `model/automation/internal/Automation.h`): a curve for each automated parameter, by its device's ID and its own (a plug-in's follows its update's new IDs, as its lanes do), with its points in beats, each with its value and its curve. Tracktion's curves mirror them in seconds (`mirror::reconcileCurve()`, `reconcileAutomation()`), which the tempo map's reconcile places again, so that `AutomationOnBeats` is gone. Every edit goes through the song's curve: points added, moved, bent, deleted, drawn and duplicated, automation that goes with clips, cut, copy and paste, and the time edits (room made, a range taken out, moved and copied, as Tracktion's `insertSpace()` and `removeRegionAndCloseGap()` did); Tracktion reckons a curve's values from its mirror until Phazite plays the song itself (T2). A device removed leaves its curves in the song for an undo to bring back, and a project forgets them as it opens; a copied track's devices get their originals' curves (`copyLanesAndCurves()`). Each track's write mode is the song's too (`schema::trackAutomationMode`; the master's on `MASTER`). What Tracktion writes itself becomes the song's: what it records, as recording stops, in its "Record Automation" step (until then, a curve it records is its own, which the mirror leaves alone); the one point of a curve, which it moves to its parameter's value as a control turns; and the latch it puts a track that wrote into as a pass ends (`AutomationWatcher`).

### T2. The sound of Phazite's own (XL)

A playback engine of Phazite's own, in a new engine-neutral layer, `src/engine/`, beside `src/devices/` and below the model, which feeds it from the document.
- **A plan compiled on the message thread and swapped in whole:** the model turns the document into an immutable plan of what plays (nodes and their connections, clips, automation, modulation), and hands it to the audio thread as `DeviceModulation` is handed over: an atomic pointer, the old plan freed on the message thread once the audio thread has let go of it. Editing never locks the audio thread.
- **What it plays,** each part checked against Tracktion's renders before it plays by default:
  1. the transport in beats: play, stop, loop and locate, sample-accurate at the loop's end, through the tempo map, so that a loop and the play start keep their beats when the tempo changes (known bug 5);
  2. MIDI clips into Phazite's instruments, with aliases and loops, and the piano roll's guide notes;
  3. audio clips, streamed from disk by a reading thread ahead of the audio thread, Phazite's renders of stretched, pitched and reversed clips (`ClipRenders`), and fades;
  4. Phazite's devices directly, with no Tracktion plug-in around them, with automation and modulation in every stretch, and for each note once the instruments take it (design section 10): the modulators become engine-neutral DSP (`src/devices/modulation/`), all six kinds;
  5. the mixer: volume and pan, mute and solo, meters, sends and returns, buses with outputs of their own, sidechains, the master and its loudness meter;
  6. delay compensation across the whole graph, from each device's reported latency;
  7. processing on several cores: the graph's independent parts on a pool of real-time threads (Windows' "Pro Audio" scheduling, macOS's audio workgroups);
  8. the metronome and the count-in.
- **Side by side:** a switch picks the engine; the harness of T0 runs every golden render through both.
- *Done when:* every golden render matches Tracktion's within the fingerprints' tolerance; the real-time audit's guarded scenarios allocate nothing on Phazite's engine; the benchmarks are as fast or faster ([performance-audit.md](performance-audit.md)); and the app plays through Phazite's engine by default.

### T3. Plug-ins hosted by Phazite, sandboxed (XL; A5 part 5, step 3.3)

- **First the measurement step 3.3 asks for:** a helper process, audio in shared memory, one wake-up each way, the helper's audio thread on "Pro Audio" scheduling, at 128 samples. Then one helper per plug-in or per group, and whether sandboxing is on by default, decided by the owner from the numbers (decision of 2026-10-16).
- **Hosting:** JUCE's `AudioPluginFormatManager` (VST3; AU on macOS later), in the helpers: each plug-in's parameters by its own IDs (never by index), its state (taken when Phazite saves or copies, never through undo), its latency, sidechain and MIDI, and its editor's window, owned by Phazite.
- **Scanning of Phazite's own,** in a separate process with a time limit for each plug-in; the list of plug-ins kept in Phazite's settings rather than Tracktion's.
- **Into the engine:** a plug-in is a node of T2's graph that sends its audio to its helper and waits for it, with a deadline; a helper that crashes leaves its slot saying so, with Reload. Until T2, the node is a thin Tracktion plug-in, as A5 planned.
- *Done when:* step 3.3's check passes (a deliberately hanging or crashing test plug-in can't stall the scan, crash Phazite or lose the project), and presets, automation, modulation, windows and sidechains work for plug-ins through Phazite's host.

### T4. Inputs and recording (L)

- **Audio devices:** `juce::AudioDeviceManager` directly, as `model/engine/AudioDevices.h` already describes them, with the first run's low-latency setup and ASIO on Windows; the audio setup kept in Phazite's settings.
- **MIDI inputs:** JUCE's MIDI inputs with hot-plugging, and the computer keyboard as an input of its own; live notes timestamped by the engine as they come, so that none is dropped after a busy UI thread (audit findings 22 and 23); the MIDI input's options (overdub, quantize) as Phazite's own settings.
- **Recording:** Phazite already writes audio recordings itself (`AudioRecorder`); it takes its inputs, monitoring and timing from Phazite's engine. MIDI recording, and Capture with a buffer of its own of what came in, write into the document.
- *Done when:* the recording tests pass without Tracktion, known bug 4 can't happen, and recording works with an ASIO interface.

### T5. Rendering (M)

- Export of the song and its stems with loudness normalization and dither; freezing and bouncing a track in place, the frozen files in the project's Cache folder; the browser's previews: all through Phazite's engine, offline, faster than real time and the same every time.
- *Done when:* the export, freeze and preview tests pass on Phazite's engine, and a frozen track's file matches what the track plays.

### T6. Tracktion removed (M)

- The bridge, the prelude (`model/base/internal/Tracktion.h`), the `internal/` headers that only exist for Tracktion, every workaround in the [list below](#tracktion-isms-to-drop), and Tracktion's `Settings.xml` go.
- **What comes in through Tracktion and stays:** JUCE becomes a submodule of Phazite's own, at the commit Tracktion pinned (its modules listed in `CMakeLists.txt` as Phazite uses them); doctest (MIT) is vendored; choc's FIFO in `AudioRecorder` gives way to `juce::AbstractFifo`.
- The architecture rules and their check lose their Tracktion rules and gain one: no Tracktion anywhere; the sanitizer suppressions for Tracktion's races and leak go with it.
- *Done when:* the repository builds and passes every test with `external/tracktion_engine` removed.

## Where it can stop

Each phase leaves Phazite whole, so the migration can pause after any of them. Two stops are worth naming:
- **After T1,** Phazite owns everything it saves and every undo step, and Tracktion only plays it: the data-model Tracktion-isms below are gone, and the engine could stay for good.
- **After T3,** plug-ins run in Phazite's own sandboxed host whatever engine plays them: what users would notice first (a crashing plug-in no longer takes the song with it) doesn't wait for T2.

## What Phazite doesn't need from Tracktion

Phazite builds only what it uses. Not needed: the clip launcher (after v1 at the earliest, design decision 2), Ableton Link, ARA, control surfaces, MIDI learn, DAWproject export, video, Tracktion's instruments and effects (unused since 2026-10-18: they break the real-time rules), its time-stretchers (unused since 2026-10-28) and its MIDI generators (the chord lane makes its own parts).

## The tests on the way

- **What stays:** tests that go through the model's public functions (by IDs, beats and plain values), and the UI tests, which reach the project only that way. They are the migration's check that nothing a user does has changed.
- **What moves:** tests that build Tracktion's objects themselves move to T0's test song area by area, in the T1 step for that area; the engine's tests move in T2 to T5. The test helpers built on Tracktion (`TestHelpers.h`'s engine, `TestPlugin.h`, `HostedDevice.h`, `RealTimePlayback.h`, the project normalisation in `GoldenFiles.cpp`) get Phazite's counterparts in the phase that takes over what they wrap.
- **The goldens:** the render fingerprints and the command scripts are the constant reference from T0 to T6; the project goldens change format once, in T1, and that diff is read in full.
- **The real-time audit** keeps its scenarios and runs them on whichever engine plays; T2 can't make Phazite's engine the default while any guarded scenario allocates on it.

## Risks, and what holds them

| Risk | What holds it |
|---|---|
| The size: Tracktion is about 244,000 lines (design section 23); Phazite's own code is about 87,000 lines, and its tests 38,000 (2026-10-30) | Only what Phazite uses is rebuilt (above); each phase is its own decision, with stops after T1 and T3 |
| Delay compensation and processing on several cores are where engines go wrong | Golden renders with latent devices on parallel paths (T0); ThreadSanitizer in CI (`linux-tsan`); the real-time audit |
| Plug-ins' quirks: their state, their threads, their windows | The test plug-in (`tests/TestPlugin.h`) and real ones (Surge XT, `tools/setup-windows.cmd -WithTestPlugin`); sandboxing contains the crashes |
| MIDI timing | Notes timestamped by the engine, tested to the sample, as the recording tests test the metronome's clicks |
| A feature lost in the switch | Both engines side by side until every golden render agrees; the switch made feature by feature |
| Feature work stalling | Small steps that leave Phazite whole; features keep being built on the model's functions, which don't change |

## Tracktion-isms to drop

Conventions and workarounds that exist only because Tracktion works the way it does. Each goes in the phase that takes over what it works around; until then, each stays, since every one fixes something real. Found in the code on 2026-10-30, most of them for the first time in one place: [design section 23](design.md#23-tracktion-engine-what-it-gives-and-what-we-build) and the [real-time audit](realtime-audit.md) list some of them.

**The project, its IDs and its file (T1)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Phazite's IDs are Tracktion's item IDs (tracks, clips, devices, modulators); sections, macros and containers' racks take numbers from the Edit's counter under Tracktion's name for an ID, "so that Tracktion counts it too"; a macro's ID is its parameter ID read as an item ID | `model/base/internal/Tracktion.h`, `schema::sectionID`, `Containers.cpp`, `Modulation.cpp` | each kind of item numbered by the project, as notes are |
| The playlist stacks overlapping clips assuming Tracktion's IDs grow in the order items are made | `PlaylistModel.h` | an order of Phazite's own |
| Phazite's data on Tracktion's elements: a PHAZITE child of the Edit, properties on TRACK (gone: T1, step 2, puts them in the song's track list), PLUGIN and NOTE elements (clips and their notes are the song's since step 3: a NOTE of Tracktion's carries only the ID of the song's note it plays, `phaziteNoteID`); device parameter values stored as PLUGIN properties named by parameter ID, so that no ID may be one of the properties Tracktion keeps on every plug-in | `Schema.h` | a document whose every name is Phazite's |
| The selection still names Tracktion's items (`EditItemID`s, `te::Selectable*`, found with `findTrackForID` and its kin), and `te::SafeSelectable` serves as the model's weak pointer | `model/base/Selection.cpp`, throughout | IDs only |
| Global tracks in every Edit (tempo, markers, chords, the arranger), and the master as a track | the song's track list is Phazite's (T1, step 2), which hasn't them; the mirror puts its tracks after the last of them (`TrackMirror.cpp`) | tracks are what the user makes, and the master |
| A new Edit's first track is made as Tracktion makes the Edit; a track added once it is made leaves Tracktion a change to mark later, as if the song had changed | a new song's Edit keeps Tracktion's first track, recorded as the song's (`recordFirstTrack()`, `createSongEdit()`) | the Edit made from the song |
| The project file is Tracktion's Edit XML: clip slots, scenes, track comps, video, groove and quantisation elements, Tracktion's attributes (`elastiqueMode`, `proxyAllowed`, `resamplingQuality`, `remapOnTempoChange`), fade shapes as Tracktion's enum numbers, MIDI-learn and control-surface settings | Phazite writes the file itself, and its own `appVersion`, `modifiedBy` and `projectID` | Phazite's own file |
| Tracktion's save renames a track's output by the number of the track it goes to | Phazite never routes a track into another (`DeviceStates.cpp`) | routing by ID |
| The user's full name written into every project by the engine's settings storage | `phazite::PropertyStorage` gives none (`createEngine()`) | no such storage |
| A project's ID replaced as it loads, though the Edit names its temporary folder after it | `Project` reads the saved ID first | the project's identity is Phazite's |
| Projects loaded on a background thread | `phazite::loadEdit()` on the message thread; the saved state fixed before the Edit is made (frozen tracks, unused racks, values set by hand) | Phazite reads its own file |
| Notifications that change nothing (each MIDI clip's mute after a tempo update, aliases copying their notes again, a value written back as a number where it was text) and Tracktion's changed flag | `Project` keeps a copy of the project as opened or saved, compares values both ways (`isSameValue()`), and counts a change only when it differs; `lastSignificantChange` for changes inside plug-ins | a document that tells only of real changes |
| State written into the project while it plays: the playhead, input devices, automated and modulated values | `Project` and the song watcher ignore them (`isAutomatedValue()`) | what plays isn't the project |
| A track's clips put in order asynchronously after one is added, as an undo step that marks the Edit changed | `Project::create()` saves what a new project starts with and opens that save | clips ordered as they're added |
| Audio files held open for a moment after use | released before a project's files are deleted (`UnsavedProjects.cpp`) | files Phazite opens and closes itself |

**Time and the tempo (T1, T2)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Clips and automation placed in seconds, and the clips moved when the tempo changes | *Mostly gone (T1, steps 3 and 4):* the song's clips and automation are in beats, and the mirror places Tracktion's again after a tempo change; `PatternsKeepTheirSpeed` scales a pattern's notes to keep their times when the song keeps patterns at their speed (`Tempo.cpp`) | everything placed in beats; seconds worked out only as it plays |
| The loop and the play start kept in seconds, so that a tempo change moves them off their beats | none yet: known bug 5 | the transport in beats (T2) |
| One tempo and one time signature at the song's start; one key for the song, in the pitch sequence | *Gone (T1, step 1):* the tempo map is Phazite's, which Tracktion's tempo sequence mirrors; keys are Phazite's (each track's, the chord lane's), and the pitch sequence is left alone | a tempo map and keys of Phazite's own |
| The grid's levels are Tracktion's snap types (3 to 18; a beat is 9, a bar 10), and snapping goes through seconds | `GridSettings.h`, `Grid.cpp` | a grid in beats |
| `ClipTrack::insertSpace()` misses clips when its list is unsorted, and moving a clip doesn't sort it | Phazite's time edits move every clip themselves (`Arrangement.cpp`) | the document's own order |
| A clip's maximum length ignores a trimmed start, and its offset can't go below 0 | `Clips.cpp` | clips of Phazite's own |
| `TrackInsertPoint (track, false)` meaning the start of the list for the first track | `getInsertPointAfter()` | an index in the document |

**Undo and values (T1)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Undo puts back a parameter's stored value but not the value that plays; playing it back writes it through the undo manager, which drops the redo history | `phazite::undo()` and `redo()` update every parameter still in the Edit from its stored value (`Undo.cpp`), which misses the master track's modulators (`getAllAutomatableParams()` leaves them out), and take back at once what that writes, which gives back the redo history | a parameter's value is the document's, which the engine reads |
| A parameter's value stored only once it first changes, and undoing that first change removes it | controls store the value first, without an undo step (`ParameterHandle.cpp`, `MixHandle.cpp`), as preset loading does (`Presets.cpp`) | every value in the document from the start |
| An unnamed undo step started about 350 ms after a change, unless a mouse button is down | `Gesture` and `RecordingStep` hold an inhibitor (`internal/Undo.h`) | undo steps begin only where Phazite begins them |
| Devices' states (a plug-in's settings, its window's place, values set by hand) written into the project through the undo manager as Tracktion saves or copies | `storeDeviceState()` and `storeDeviceStates()` (`DeviceStates.h`) before Tracktion's own copies; `restoreValuesSetByHand()` for Phazite's devices, which Tracktion restores only for plug-ins | state taken into the document when Phazite saves or copies, outside undo by design |
| A plug-in's settings kept as a block it reads only when it loads | a custom undoable action calls `restorePluginStateFromValueTree()` (`Presets.cpp`) | the host hands a plug-in its state |
| Tracktion's fader, VCA and send store their values under other names than their parameters ("volume", "pan", "vca", `auxSendSliderPos`) | `Automation.cpp` maps them | one name for each value |
| Mute and solo kept outside undo | Phazite keeps that choice on purpose (CLAUDE.md); a step that mutes on purpose (Bounce in Place) mutes the song's track in its step, and after an undo or a redo the mirror sets Tracktion's switches from the song's (`mirror::reconcileTrackSwitches()`, T1 step 2) | the same choice, Phazite's own |

**Notes, clips and aliases (T1, T2)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Aliases (link groups) mirrored to each other without undo, and a linked audio clip's sound only partly mirrored | aliases are the song's (T1, step 3): the model keeps a MIDI alias group's notes and an audio one's sound the same, and Tracktion's clips are in no link group | an alias plays its original's notes |
| Note IDs rely on Tracktion copying an alias's notes in the same order | gone: notes are the song's, with their IDs (T1, step 3a) | notes of Phazite's own |
| Tracktion's MIDI recording and its MIDI file import write clips and notes into its Edit themselves | the song takes them in as the recording stops (`adoptRecordedMidi()`) and as the file is imported (`adoptMidiClip()`) | recording and import of Phazite's own (T4) |
| A MIDI clip's controller values kept 7 bits up, with Tracktion's own numbers for the pitch wheel, aftertouch and the rest | the song keeps them in MIDI's terms, which `Clips.cpp` converts (`toTracktionControl()`, `makeControlState()`) | events in the document's terms, which the engine plays |
| Loops given an automatic tempo and pitch when added | `EngineBehaviour::newClipCreated()` undoes both | nothing to undo |
| An audio clip's fades scaled down outside the undo history when the clip gets too short for them | the song keeps them as they were set, and the mirror gives Tracktion them fitted to the clip, as its getters would (`ClipMirror.cpp`) | fades the engine fits as it plays |
| A clip whose render isn't ready plays silence | exports and freezes render pending clips first (`ClipRenders.h`) | the engine waits for renders it knows of |

**The mixer and containers (T1, T2)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| The channel strip made of plug-ins (volume and pan, meter, sends, returns, a VCA on plain folders), and a plug-in's mix as dry and wet levels | `Mixer.cpp`, `Buses.cpp`, `LevelSource.cpp` | the strip is the engine's |
| A folder's submix taking the output of its first child, and a folder turned into a submix with a message when a plug-in goes on it | `makeBus()` adds the bus's volume and meter itself | a bus is a track with an output of its own |
| An instrument on a track silences its audio clips | a new track gets the instrument its clips need (`Mixer.cpp`) | instruments and audio clips play side by side |
| Returns must be solo-isolated by hand | the mirror solo-isolates a track that the song's list says is a return (`TrackMirror.cpp`) | solo of Phazite's own |
| Racks: a rack type must stay for the session, as undo brings back other objects; a copied track's containers share their rack type; Tracktion counts a rack as an instrument | `Containers.h` keeps them and drops unplayed ones on opening; `copyContainers()`; `isInstrument()` leaves racks out | a container is a group of devices in the document |

**Modulation and automation (T1, T2)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Modulators updated once a block, under a lock; an assignment's default curve (0.5) not written and read back as 0 | `DeviceModulation` follows LFOs within a block for Phazite's devices; `updateDeviceModulation()` leaves modulations with an offset or a curve to Tracktion | modulators of Phazite's own, engine-neutral |
| Tracktion's defaults for new modulators (LFOs and random sources one way only, step sequencers and envelope followers at no depth), macros only on audio tracks, and parameter IDs such as "biopolar" | Phazite sets its own defaults on each new modulator and renames parameters for display (`Modulation.cpp`) | Phazite's modulators, Phazite's names |
| Automation recorded with its last points written in the same round of messages as the stop | `RecordingStep` ends asynchronously (`Automation.cpp`), and the song takes in what was recorded as it ends (`adoptRecordedAutomation()`) | recording of Phazite's own |
| Tracktion writes into its curves and tracks itself: a curve's one point moves with its parameter's value as a control turns, and a track that wrote goes into latch as a pass ends (both through the undo manager) | the song takes both in, in the same step (`adoptMovedPoint()`, `adoptAutomationMode()`, called by `AutomationWatcher`) | automation of Phazite's own, played by its engine (T2) |
| A control's value shown at the playhead only if the curve is followed while stopped | Phazite calls `updateToFollowCurve()` | the engine's value read by the control |

**Playback, the transport and the engine (T2)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Phazite's devices must be wrapped in a Tracktion plug-in to play | `DevicePlugin`, which glides parameters, sleeps when silent and mixes dry and wet itself | devices are nodes of Phazite's graph |
| The position still reads as the start for about 200 ms after play starts | the position change is sent again (`Playback.cpp`) | the engine's position |
| Low Latency means turning delay compensation off and remaking the playback context | `internal/EngineSetup.h` | the engine changes its own buffer and latency |
| Modulators keep where they were from one render to the next (an LFO's phase, a note follower's last note), so the same Edit rendered twice can differ | the golden renders' candidate engine renders a fresh copy of the project (`tests/RenderHarness.h`) | an engine that starts every render from the song's start |
| A new Edit's master fader at -3 dB, which an export's stems leave out with the master's devices | the stems' golden test adds them up at the master's level (`tests/GoldenMixerTests.cpp`) | the master's level the document's, Phazite's default |
| The playback context freed by renders and kept alive by previews | restored after renders (`Freeze.cpp`), freed after previews (`Preview.cpp`) | offline renders of their own |
| Volume and pan and the click read message-thread values on the audio thread, meters allocate in their first block, the tempo map is swapped under a lock (audit findings 8, 14, 2 and 12) | reported upstream; measured by the audit | values handed over as the rules require |
| The song's length counts only what Tracktion holds | `getSongEnd()`, `getTimelineEnd()` | the document's own extent |
| Guide notes only if the engine behaviour says so | the piano roll forces them | the engine plays what the piano roll asks |
| Blocking jobs run on the calling thread | `runTaskWithProgressBar()` | Phazite's own jobs |

**Inputs and recording (T4)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Live notes timed by JUCE's approximate clock, dropped after a busy UI thread | Phazite keeps that clock current from the audio thread (audit finding 23) | notes timestamped by the engine |
| The MIDI scan answered 4 seconds after startup; MIDI outputs opened that nothing plays | `createEngine()` asks again; Phazite closes them | JUCE's inputs, listed as they change |
| Arming a track remakes the graph, which loses a recording that just started | the live-MIDI track is armed in advance (`LiveInput.cpp`) | arming changes nothing that plays |
| A retrospective buffer on every input | set to 0, and back to 30 seconds for live MIDI (`EngineSetup.cpp`, `LiveInput.cpp`) | Capture's own buffer |
| No punching in while looping, and no loop under two seconds | a punched take turns the loop off; the loop grows while recording (`Recording.cpp`) | recording of Phazite's own |
| Overdub and quantize kept in the MIDI device's settings, as Tracktion's strings | `Recording.cpp` | Phazite's settings |
| Capture's notes made into clips by Tracktion | Phazite turns quantisation off, deletes those clips and places the notes itself (`Capture.cpp`) | Capture writes into the document |
| Inputs remade by any change; a recording's first block mapped a block late; consumers that must be added before playing; the first block from a stop sometimes skipped | `AudioRecorder::update()` retries; the start is taken at the second block; known bug 4 | inputs of the engine's own |

**Hosting and setup (T3, T6)**

| Tracktion-ism | How Phazite works around it now | What replaces it |
|---|---|---|
| Plug-ins hosted in Phazite's process, so that one crash closes Phazite | after a crash, a project can open with its plug-ins off (`setPluginsLoad()`) | sandboxed helpers, with Reload |
| A hosted plug-in's parameters known by their index | `PluginParameters.h` saves their own IDs beside and moves them through updates | parameters known by their own IDs |
| Plug-in windows opened and closed through Tracktion's UI behaviour | `AppUIBehaviour`, `PluginWindow` | windows of Phazite's own host |
| The scanner talks to its child process without a lock | the plug-in list's scan uses one thread (`MainWindow.cpp`) | Phazite's own scanner |
| Phazite's own settings, the plug-in list, the audio setup and the count-in (as Tracktion's enum number) in Tracktion's `Settings.xml` | `PropertyStorage` | a settings file of Phazite's own |
| A preview Edit with three tracks of Tracktion's making | `PreviewLoopPlugin` plays on a fourth (`Preview.cpp`); pitch matching off, as it ignores major and minor | previews from Phazite's engine |
| Freezing: Tracktion renders on the message thread, unfreezes through undo, unfreezes a track whose file isn't in its own folder as a project opens, and leaves deleted tracks' files behind | `Freeze.cpp` flips the frozen flag without its listener, and cleans up after Tracktion | freezing of Phazite's own |
