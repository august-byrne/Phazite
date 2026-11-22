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
| 5 | Wide | Wider notes and strips, warped clips heard at once, controllers recorded and edited, MIDI learn, MP3 export, resampling, the release process. Then a beta anyone can download, with no promise that its projects open later |
| 6 | Deep | Every built-in device and its editor brought to the best plug-ins' standard |
| 7 | Professional | MIDI out, note effects, multi-out, comping, a cue mix, the Multisampler |
| 8 | Release | A signed installer, the guide, crash reports, the promise of compatibility. Then 1.0 |

## Instruments and effects

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Modulation and expression in the Synth, the FM synth and the Sampler: more than one free-running LFO with one target, tempo sync, aftertouch, MPE | `src/devices/synth/Synth.cpp` | (XL) | 6 | devices-instruments 4 |
| The Synth's warps past its Quality (S6.7: 2x and 4x, a swept sync 60 and 74 dB under the note at C5): a longer BLEP at sync's reset and a PolyBLAMP for the pulse width's and the mirror's corners, to reach 80 dB at 4x; voices that fade at the old rate through a change of Quality, where they are cut now | `src/devices/synth/Synth.cpp` | (XL) | 6 | devices-instruments 6 |
| The Filter's Driven character for the FM synth's and the Sampler's filters, which share its code and still play Clean (the Synth has it since S6.7); a ladder or a diode model as a third character; the Synth's Clean drive at 1x aliases 61 dB under a note at C5 (its Quality at 2x takes it to 99) | `src/devices/dsp/MultimodeFilter.cpp` | (L) | 6 | devices-instruments 7 |
| A wavetable made from a sound file in the background, not on the message thread: since tables keep 16 samples a cycle (S6.7), a file of 256 frames takes 2.7 times as long to make as it did (it was 84 ms in a release build) and 54 MB | `src/model/devices/Wavetables.cpp` | M | 6 | S6.7's part A |
| Stock effects with no counterpart, each placed by the audit of 2026-11-17 ([decisions.md](design/decisions.md)): the channels' tools in Stereo (S6.12); a gate's hold and a transient mode in the Compressor (S6.3); a de-esser as a dynamic band of the EQ (S6.4); a bitcrusher as a shape of the Saturator (S6.6); tremolo and auto-pan as track presets with a modulator on volume or pan. Left: a pitch shifter ([open question 57](open-questions.md)); a convolution reverb and a tuner, not in 1.0; and an effect of time (half speed, stutter, tape stop), which nothing here can make, after 1.0 | `src/devices/` | — | 6 | devices-effects 2, product-gaps 5 |
| Compressor: stereo link options (unlinked, mid and side), an automatic release, a knee as a number | `src/devices/dynamics/Compressor.cpp` | M | 6 | devices-effects 4; the rest of that row was built in S6.3 |
| Reverb, after its Hall (S6.5): the Damping's top setting as off (the Hall's 2 kHz octave decays 6 to 10 % short at 20 s and the smallest Size); the Room's and the Plate's sways moved on over a sleep, by a closed form on a gap, so that live plays as a render after one; the Room's losses and dampings gliding when the Decay moves; a test of the Diffusion modulated fast under a 20 s tail; the ringing measure averaged over several takes | `src/devices/reverb/Reverb.cpp` | M | 6 | devices-effects 5; S6.5's review |
| EQ: more than eight bands, a sidechain and attack and release controls for its dynamic bands; for its linear phase (S6.4), a choice of kernel lengths (one length now: below 100 Hz a narrow bell or a steep cut is up to 5 dB off its curve), and Phase under automation (it plays as the one set by hand) | `src/devices/eq/Equalizer.cpp` | L | 6 | devices-effects 7; mid/side and dynamic bands were built in S6.4 |
| Delay: reverse, diffusion, a width, a bucket-brigade character, the Tape loop's curve anti-aliased | `src/devices/delay/Delay.cpp` | M | — | devices-effects 8; the rest of that row was built in S6.6 |
| What stage 6 made dearer, to look at with a profiler: a Synth voice (0.19 to 0.27 % of a core; with full unison 4.0 to 6.4: lines worked out for every sample where nothing moves, tables of 16 samples a cycle), the Saturator at its defaults (0.74 to 1.17), the Compressor (0.35 to 0.45), the Delay (0.14 to 0.24) | `src/devices/synth/Synth.cpp` | M | 7 | benchmarks of 2026-11-19 ([performance.md](performance.md)) |
| What each device's verdict names as short of the plug-ins producers own (S6.1): the table at the end of [design/devices.md](design/devices.md) has each device's standing and largest gap, its page's Verdict the rest. Short of the bar: the FM synth (four operators, eight fixed algorithms, stepping modulation), the Sampler (one zone; until the multisamples of 4.10), the Limiter (one release, one style, a fixed look-ahead), the Vocoder (its own carrier plays no chords) | `src/devices/` | L each | 7 | S6.1 |
| Stretch modes: a formant switch, re-pitch, and a mode that slices drums at their hits | `src/devices/stretch/Stretch.cpp` | (L) | 5 | devices-effects 6, recording audit |
| Multiband: its attack, release and curve worked out once a block for each band, not for every sample | `src/devices/dynamics/Multiband.cpp` | — | 6 | devices-effects 13, performance audit |
| Compressor and Multiband: the curve's table of 529 gains made at most once a block while a point of the curve moves (a lane, a modulator or a glide on it makes it again every 32 samples, each entry with two `expm1` calls: 1.5 to 6 % of a core for each moving curve, worked out, not measured) | `src/devices/dynamics/DynamicsCurve.cpp` | S | 6 | threading audit, 2026-11-20 |
| Drum sampler: only the parameters that move walked at each stretch of 32 samples (all 3,329 are, several times, whenever one of them is automated, modulated or gliding: about 1 % of a core, worked out), as the runner already does for a device asleep | `src/engine/nodes/DeviceRunner.cpp` | M | 6 | threading audit, 2026-11-20 |
| A linear-phase Multiband: its crossovers as kernels through the EQ's partitioned convolution (`dsp::PartitionedConvolution`), whose bands then add up to a delay exactly | `src/devices/dynamics/Multiband.cpp` | L | — | S6.4's builder, 2026-11-18 |
| FM synth: four voices at once in SIMD registers, or operators skipped once their envelopes have ended; the Synth's unison voices in SIMD | `src/devices/fm/FmSynth.cpp` | — | — | performance audit |
| A sampler's sound made ready (its hits found, its octaves made) in the background, not on the message thread | `src/model/devices/SamplerSound.cpp` | — | — | performance audit |
| MP3 files: a test of where a click lands, then the encoder's delay and padding trimmed by the LAME tag | `src/model/engine/AudioFiles.cpp` | — | — | DSP audit |
| Vocal pitch correction: a voice pulled to a key and scale or to notes played, as it plays, with its formants kept. After 1.0, and only at the standard stage 6 sets for a device | `src/devices/` | XL | — | comparison with another DAW, 2026-11-17 |

## Playback engine

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Sound files written again in their place (known bug 198's fix), what is left: the disk is asked only when a plan is made, so a file written again while nothing is edited plays on from the old reader; a sampler's sounds are read once; the record of files' sizes and times is never pruned; the waveform's `findStretch()` may still read a file's size and time in a paint (not checked) | `src/model/mixer/Freeze.cpp`, `src/model/project/DawProject.cpp`, `src/model/engine/AudioFiles.cpp`, `src/app/ClipWaveforms.cpp` | M | — | known bug 198's reviews |
| A muted track's send knobs look the same whether the send still goes on (before the fader) or is silent with the track (after it): show which sends still sound | `src/ui/mixer/ChannelStrip.cpp` | S | 6 | known bug 78's fix |
| Files at another rate rendered to the engine's rate in the background, as stretched clips are. Today a clip of a 44.1 kHz file in a 48 kHz project is resampled as it plays (0.24 % of a core and about 276 KB of state for each stereo clip), and its resampler must take 832 samples of the file (18.9 ms) before it gives anything out, so a jump among such clips is held up to 20 ms of silence while they are made ready. Cost: disk space, and a render each time the project's rate changes | `src/engine/audio/AudioRegion.cpp` | L | 7 | known bug 85's fix |
| A sequence keeps its controllers' part when only notes change: each track's sequence is rebuilt whole at every compile, controllers included, so a note edit on a track that holds a five-minute pressure take at the bound (3 million values) takes 353 ms and makes an 80 MB sequence again (one minute of ten keys at 200 values a second: 23.7 ms). Split the controllers' events and index into a shared read-only part that `compileNotes()` reuses while no clip of the track changed a controller | `src/engine/nodes/NoteSequence.h` | M | 7 | known bug 173's fix |
| Stretched, pitched and reversed clips heard at once, stretched as they play, instead of silent until their render is done | `src/model/arrangement/ClipRenders.cpp` | L | 5 | model-arrangement 4, product-gaps 3 |
| Time-signature changes along the song | `src/model/transport/Tempo.cpp` | M | 5 | model-playback 12, product-gaps 14 |
| Wider strips: mono tracks, more than one output pair, a cue mix | `src/engine/nodes/Strip.cpp` | XL | 7 | plan, product-gaps 10 |
| Scale: the benchmarks on a laptop, a scheduler that doesn't spin on what it waits for (the plan lists each folder's tracks and then its bus, depth first, and threads take strips in that order, so one that takes a bus spins for its tracks while the next folder's tracks wait untaken: with the plan ordered by depth, the strips that take nothing in first, fewer wait, the sound the same to the bit), strips played in smaller blocks than the device's, read-ahead before a jump. The 100-track benchmark is built and measured ([performance.md](performance.md)): one thread can't play 100 busy tracks, four can at 128 samples with little room | `src/engine/core/Workers.cpp` | XL | 7 | engine 11, performance audit |
| Parameter smoothing that lasts longer than one block, and channel meters that keep less than 64 blocks | `src/engine/nodes/DeviceRunner.cpp` | — | — | engine 14 |
| A strip's modulators follow the song as late as the sound comes into each device they move: a device after latency gets the place of its sound and its automation there (since 2026-11-18), but a modulator's values are worked out once for its strip, at the strip's own place | `src/engine/core/Engine.cpp` | M | — | S6.7's review, 2026-11-18 |
| The click: a level to set and an output to choose; audio files of more than two channels played whole | `src/engine/transport/Click.cpp` | — | — | engine 15 |
| Files at another rate than the engine's converted once into the song's cache, if many such clips at once ever weigh on a song | `src/engine/audio/Resampler.cpp` | — | — | performance audit, DSP audit |
| A key to its sound measured with a wired output (the target is under 15 ms), and the whole start timed, by hand | `docs/hand-checks.md` | — | 3 | performance audit |
| A MIDI input that closes ends only the notes it held: live notes don't carry their input, so every note held live ends | `src/engine/core/Engine.cpp` | S | — | plan |
| An export started while a block is stalled fails after a limit, saying that playback is busy, where it now waits for the block | `src/model/playback/PlaybackSession.cpp` | S | — | plan |
| A sound file's rate bounded wherever a file is read, through one function that makes a reader: the sampler refuses a file that says more than 768 kHz (a WAV's header takes any number), but audio clips, the drum sampler's pads, previews, imports and the transients take what the file says, and a clip of such a file would ask the resampler for a ratio of thousands | `src/model/engine/AudioFiles.cpp` | S | — | threading audit, 2026-11-20 |
| A render lets go of the Ogg Vorbis files it has played too (WAV, AIFF and FLAC are let go of; MP3 can't be: its reader decodes otherwise after a seek), once JUCE's reader is shown to give the same samples for a place read again | `src/model/playback/SongPlayer.cpp` | S | — | threading audit, 2026-11-20 |
| A delay line's reset that clears only what the line took in since its last reset, counted for each block by its device (no cost for each sample; a count inside the line was measured and taken back: [performance.md](performance.md)), if Play is ever heard to drop out with dozens of Delays and Reverbs at 64 samples: their resets take 0.35 and 0.6 % of a block of 128 each, at Play and at every jump while the playhead is dragged | `src/devices/dsp/DelayLine.h` | M | — | threading audit, 2026-11-20 |

## Plug-in hosting

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Linux: a helper's process number kept after the process was reaped: `Helper::kill()` and JUCE's `ChildProcess` send a signal to that number whoever holds it by then (Linux gives numbers out in turn, so it would take a full round of them) | `src/sandbox/client/Helper.cpp` | S | — | known bug 191's review |
| Renders name the plug-ins they rendered without: an export, a freeze, a bounce or a DAWproject export plays a plug-in that is missing on this computer, kept off after a crash, or crashed while loading as if it weren't there, and says nothing (it names only plug-ins still loading); the report should name them, for the open song and for an old version's own instances alike | `src/model/project/Export.cpp` | S | 5 | known bug 166's review |
| The helper's platform code for macOS, and its Linux code tried by a person: the watch of the parent that ends a helper with Phazite, and shared memory unlinked once opened, are built and run only by the tests | `src/host/core/Platform.cpp` | S | — | plugin-hosting 15 |
| About twenty VST3s tried by hand | `docs/hand-checks.md` | M | 3 | product-gaps 7 |
| SysEx to plug-ins, and MIDI out of them (channels, the bend, controllers, pressure and program changes reach them) | `src/model/plugins/SandboxedProcessor.cpp` | L | 7 | plugin-hosting 7, product-gaps 2 |
| A track's own MIDI channel: every note and controller of the track sent on it, or each on its own as now, and the channels a keyboard's notes are taken from (roadmap steps S7.3 and S7.6). Today a channel is kept as it was played or written, so a plug-in that listens to one channel is silent for a keyboard on another | `src/model/playback/SongPlayer.cpp` | M | 7 | roadmap step S5.2 |
| Notes drawn on a channel chosen beforehand, and notes shown or coloured by their channel: today a new note is on channel 1 and the inspector's Channel field sets it | `src/ui/pianoroll/PianoRollNoteTools.cpp` | S | 7 | roadmap step S5.2 |
| A plug-in's program changed by hand after the song set one is not set again by the chase, which sends a program only where it differs from the one the song sent last: forget the program sent last when the plug-in's program changes otherwise (in its window, by Undo, by a preset) | `src/engine/core/Engine.cpp` | S | — | roadmap step S5.2's review |
| Multi-out plug-ins, and plug-ins that aren't stereo | `src/host/core/Instance.cpp` | L | 7 | plugin-hosting 7, product-gaps 4 |
| A merge of two edits inside one plug-in, offered and never done by itself | `src/model/versions/SongMerge.cpp` | — | — | format audit |
| The knob values of a plug-in that wasn't playing as a version was saved | `src/model/plugins/PluginInstances.cpp` | — | — | format audit |
| The files a plug-in loads by itself kept with the project. Not planned: nothing outside the plug-in knows them | `src/model/plugins/PluginInstances.cpp` | — | — | format audit |
| "Loading N plug-ins" in the status bar while a project's plug-ins load (`getPluginsLoading()`); the Project view lists them now | `src/ui/shell/StatusBar.cpp` | S | — | plan |
| A plug-in's two-step parameters shown as switches: the helper says which they are, and `ParameterHandle::getInfo()` gives nothing for plug-ins | `src/model/devices/ParameterHandle.cpp` | S | — | plan |
| The round trip of a plug-in's block: a short spin before the sleep on both sides (each block is two wake-ups across processes now, 10 to 100 microseconds each from an idle core, one after another for effects in series); an automated plug-in called once a block with its changes at their samples, or no finer than every 128 samples (the helper calls it once for each run between changes, so about 16 times in a block of 512) | `src/host/core/Instance.cpp` | M | 7 | threading audit, 2026-11-20 |
| To check on Windows: a helper's editor window parented into Phazite's attaches the two threads' input queues, so that keys and the mouse in Phazite could stall while the helper's message thread is busy (a plug-in's own window stuck, a long state); if so, detach them after parenting | `src/host/core/Editor.cpp` | S | — | threading audit, 2026-11-20 (suspected, not tried) |
| A plug-in opened again with a saved program and state waits up to a minute for each on its loading thread, the plug-in showing as loading and renders refusing to start meanwhile: one limit for both | `src/model/plugins/PluginHost.cpp` | S | — | threading audit's review, 2026-11-20 |

## Song model

### Saving, undo and files

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| A project and its versions keep no full path that can hold the user's name: an audio file whose copy into the project failed, a plug-in's file in a per-user folder | `src/model/base/internal/Schema.h` | — | — | format audit |
| The formats named and versioned when compatibility starts: a file that names the store's format, its version and its hash, room for a pack's entries to grow, a format name and version on the project file | `src/model/versions/PieceStore.cpp` | (L) | 8 | format audit, product-gaps 12 |
| Save As copies the project's folders in the background with progress, as the first save does | `src/model/project/Project.cpp` | S | — | plan |
| A preset's Sounds folder swept of the files its preset no longer names | `src/model/presets/Presets.cpp` | S | — | plan |
| The undo history's memory measured in a large song, as it keeps its 1000 steps whatever their size | `src/ui/shell/SettingsPanel.cpp` | S | — | plan |
| Auto Save's work on the message thread: the song is copied whole twice for each save in the background (one copy is enough: the writer's isn't changed while it writes), and every ten minutes the backup compares the two project files byte for byte, copies one and checks every backup's date there; a Save by hand waits for a background write under way | `src/model/project/Project.cpp` | S | — | threading audit, 2026-11-20 |
| Quitting says that it is still writing versions: the process stays, with no window, while the versions' queued writes finish, the SHA-256 of all changed audio among them (minutes after a first version of a project with gigabytes of audio), and an update's installer waits for it | `src/app/Main.cpp` | S | — | threading audit, 2026-11-20 |
| `ClipRenders::update()` asks the disk about each stretched clip's source and render on the message thread, 300 ms after every clip or tempo edit: what is made kept from the jobs' results, the sources' identities asked on a job. The same for `Transients::find()`, which asks a file's size and time on the message thread at each call | `src/model/arrangement/ClipRenders.cpp` | M | — | threading audit, 2026-11-20 |
| A clip's mark for a render that failed follows a change 300 ms after the last one (through a whole drag, which starts the wait again at each step): a failed clip whose settings now need another render stays marked that long, and a pasted or duplicated clip whose render is known to have failed is marked only then. Tell the list at once from what the clips' listener names, without a walk of the song | `src/model/arrangement/ClipRenders.cpp` | S | — | S5.20's review, 2026-11-21 |
| A bounce cancelled while its frozen copy is made waits for that one copy (115 MB for five minutes of stereo): copied in parts, with a look at the stop between them | `src/model/mixer/Freeze.cpp` | S | — | threading audit, 2026-11-20 |

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
| Resampling: an audio track's input can be another track, a bus or the master, recorded as it is heard and placed to the sample (roadmap step S5.17) | `src/model/transport/AudioRecorder.cpp` | M | 5 | comparison with another DAW, 2026-11-17 |
| A key that drops the take being recorded and records it again from where it began, with no clip or file left of the dropped one | `src/model/transport/Take.cpp` | (M) | 5 | comparison with another DAW, 2026-11-17 |

### Arrangement

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| MIDI import of every track of a file, and MIDI export | `src/model/arrangement/Import.cpp` | M | 5 | model-arrangement 2, model-arrangement 11, product-gaps 11 |
| Notes and clips found by an index, and linked clips that share their notes instead of each keeping a copy | `src/model/notes/Notes.cpp` | L | 5 | model-arrangement 5 |
| Duplicate Track makes clips of its own, not clips linked to the original's (a decision to take) | `src/model/arrangement/Tracks.cpp` | — | — | model-arrangement 9 |
| Warp markers written into and read from DAWproject files | `src/model/project/DawProject.cpp` | — | — | recording audit |
| DAWproject import of what it now reports as left out: automation, sends, groups as folders, time-signature changes, clips with more than two warps | `src/model/project/DawProject.cpp` | M | — | plan |
| Linked tracks: a folder's switch that makes a move, a trim, a split or a deletion of one track's clip happen to the clips at the same place on its other tracks, as one undo step, for takes recorded on several microphones (roadmap step S7.10) | `src/model/arrangement/Tracks.cpp` | L | 7 | comparison with another DAW, 2026-11-17 |

### Mixer, automation and presets

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The travel of the knobs that are not in hertz, looked at one device at a time as stage 6 finishes it: times in milliseconds (attacks, releases, delays, envelopes, glides), the reverb's decay, Q, and the levels in decibels still turn on a power curve around a midpoint, where the best plug-ins' time knobs are logarithmic; a change moves what modulations and macros of them reach | `src/devices/core/DeviceInfo.h` | M | 6 | S4.7's review |
| Devices moved, copied and duplicated between tracks, and deleted, duplicated and dragged in the mixer and the Devices view | `src/model/mixer/MixerModel.h` | M | 5 | model-mixer 2, ui-core-mixer-app 4 |
| The strip: input trim, polarity, mono, width, a choice of pan law, a track delay | `src/model/mixer/MixerModel.h` | M | 5 | plan, ui-core-mixer-app 1 |
| Presets that carry their own samples, and say which of their parts are missing | `src/model/presets/Presets.cpp` | M | 5 | model-mixer 13 |
| Buses send to returns, and returns go elsewhere than the master | `src/model/mixer/MixerModel.cpp` | (L) | 7 | model-mixer 9 |
| A device keeps its sidechain when it goes into a container | `src/model/mixer/Containers.cpp` | (L) | 7 | model-mixer 10 |
| Macros automated; macros and modulators modulated | `src/model/modulation/Modulation.cpp` | (L) | 7 | model-mixer 12 |

### Versions

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Versions numbered otherwise than the song (`idNumbering`, known bug 186's fix), what is left: the changes list against such a version lists every track given anew as removed and added (match by an ID below 2^61 where both hold it, or one line saying the two can't be compared by track); a merge waiting whose pieces are read again in a session past the mark (`findMergeAgain`, only once its kept copy is dropped) lists clashes that match nothing; a whole song put in the song's place from a version from before takes its count of IDs near the mark, which Undo doesn't put back, so a later session crosses again | `src/model/versions/SongDiff.cpp`, `VersionHistory.cpp` | M | — | known bug 186's reviews |
| The store swept of what nothing names any more: built, and off until it is sound ([known bug 196](known-bugs.md)); to be turned on for histories one computer alone has written | `src/model/versions/StoreSweep.cpp` | (M) | 4 | model-versions 7 |
| Another computer's newer versions noticed while the project is open (today they show as it opens again; a version saved meanwhile goes its own way beside them) | `src/model/versions/VersionHistory.cpp` | M | — | S4.5 |
| A version saved over newer versions that only this computer holds (a project file put back from a backup), as a line of its own; today Save Version is greyed there and Create Branch is the way on | `src/model/versions/VersionHistory.cpp` | S | — | S4.5 |
| The comparison of the open song with its version on a thread of its own, so that the badge and the changes list don't wait behind Verify History or a version being written with a bounce | `src/model/versions/ChangesWorker.cpp` | S | 5 | S4.5 |
| The benchmark of an edit's versions work on the 100-track song run in `windows-profile` and entered in performance.md (its debug numbers: 0.35 ms on the message thread, where the whole comparison took 550 ms) | `tests/Benchmarks.cpp` | S | 5 | S4.5 |
| One undo for every versions operation: a log of operations (a branch switch, a deletion) to take back | `src/model/versions/VersionHistory.cpp` | — | — | source control audit |
| The undo history kept across a branch switch | `src/model/versions/VersionsModel.cpp` | — | — | source control audit |
| One old version reverted while the later ones are kept | `src/model/versions/SongMerge.cpp` | — | — | source control audit |
| Undo Last Version on a merge's version | `src/model/versions/VersionHistory.cpp` | — | — | source control audit |
| Backup of the history to another folder, then the cloud | `src/model/versions/VersionStore.cpp` | — | — | source control audit |
| Only the named versions shown, once histories grow long | `src/ui/versions/VersionsView.cpp` | — | — | source control audit |
| A version's changes marked on the song in the playlist | `src/ui/versions/VersionsView.cpp` | — | — | source control audit |
| Exports tied to the versions they were made from | `src/model/project/Export.h` | — | — | source control audit |

## UI and UX

### Timeline, playlist, piano roll and audio clip editor

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Automation lanes' drags scroll the view at its edges, and points are selected across lanes | `src/ui/playlist/PlaylistAutomation.cpp` | — | — | ui-timeline-editors 11 |
| A drag held at the view's edge scrolls it once a frame, as scrolls by hand do (`TimelineEditor::scrollViewTo()`): `autoScroll()` moves the view at each of the mouse's reports that takes it a whole pixel on, up to its speed in pixels a second, and what is dragged is placed from the view as it is then | `src/ui/timeline/TimelineEditor.cpp` | S | — | the owner's scroll report, 2026-11-16 |
| A clip dragged or drawn onto a track of the other kind is refused, as the design's rule says that audio and notes don't share a track | `src/ui/playlist/PlaylistTools.cpp` | — | — | ui-timeline-editors 8 |
| The audio clip editor zooms and scrolls | `src/ui/audioclip/AudioClipEditor.cpp` | L | 5 | ui-timeline-editors 4 |
| The piano roll's lane, after the controller lanes: several lanes shown at once; values selected, copied and moved, and moved with their notes; a ramp or a curve kept as one shape that stays editable; a note's pressure drawn; then note chance and grooves | `src/ui/pianoroll/PianoRollControlLane.cpp` | L | 7 | roadmap step S5.3, product-gaps 9, model-arrangement 11 |
| Snapping to clips' edges and markers as well as the grid; trim and slice on several items at once | `src/ui/timeline/TimelineEditor.cpp` | M | 5 | ui-timeline-editors 10 |
| A wheel notch and a mouse move over the playlist look through every clip of the song (`layOutClips()`, `getClipAt()`: three quarters of a notch's cost in a song of 2,400 clips): the clips in view found by row | `src/ui/playlist/Playlist.cpp` | — | — | performance page, 2026-11-16 |
| Drags that edit the song applied once a frame, as scrolls by hand are: a move with Alt, a fine grid, a fade, a tempo ramp's bend, an automation point and a velocity change something at every report of the mouse, and each change makes a plan (1.5 to 5.5 ms each by the benchmarks of 2026-11-16), so a mouse that reports a thousand times a second keeps the message thread full. The latest place kept in the gesture and applied at the display's frame; or the session's plan made at most once a frame while a gesture is under way | `src/ui/playlist/PlaylistTools.cpp` | M | 5 | threading audit, 2026-11-20 (likely, not measured) |
| A change of automation refreshes only the lanes it changed: any change re-reads every track's lanes and repaints every lane in view (`trackHeightChanged()`, `refreshLanes()`), once a turn of the message loop while a point is dragged | `src/ui/playlist/PlaylistAutomation.cpp` | M | 5 | threading audit, 2026-11-20 |
| A drum pad's sound read in the background, as other audio is: it is read on the message thread when Swap tries a sound or a pad's sound changes (`loadPadSound()`, as the song's player takes the value) | `src/model/devices/DrumKit.cpp` | — | — | performance page, 2026-11-16 |
| A waveform's picture kept while a long clip crosses the view's edge, and clips not repainted for a knob turned while playing | `src/app/ClipWaveforms.cpp` | — | — | performance audit |
| The displays ticked at the display's frames, with the views' own timers folded into that tick (each mixer strip and each automation lane without points has one of its own) and the playback session's stopped while nothing plays (about 1 % of a core at rest); `DrawnNumbers::changed()` given its numbers without a copy at each tick | `src/ui/components/DisplayTicker.h` | — | — | performance audit |
| Frames that JUCE shows a frame late reported to JUCE with the measurements, if they bother anyone | `src/ui/timeline/FrameLog.cpp` | — | — | performance audit |
| Tools that write notes in the piano roll: Euclidean rhythms, a seeded melody in the clip's scale, stacked chords, each with the transforms' preview. After 1.0 | `src/model/notes/Notes.cpp` | — | — | comparison with another DAW, 2026-11-17 |

### Device editors

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Graphs after S6.9's grammar: keyboard use (Tab to a graph, arrows to move a handle); the Synth's wavetable picture on the grammar; Space on a drum pad (Enter plays it); the handles no test drives yet (the review's list: band ends, bend handles, the Flanger's and the Phaser's dots...) | `src/ui/devices/DeviceGraph.h` | L | 6 | ui-device-editors 9 |
| Automate and Modulate on graphs' handles (switches and choices have them since S6.9); the top bar's tempo and position fields' wheel as one burst (a plan a notch now); a touch or a pen press ending a wheel's burst before what is pressed takes it | `src/ui/devices/DeviceGraph.cpp` | M | 6 | ui-device-editors 10 |
| Displays left from S6.9's pass: Age's and the Vocoder's don't start afresh when their editor is shown again; the Saturator's two views share one scope, so one's fresh start drops a tick of the other's; Stereo's line under Mono doesn't lean with a Balance; the EQ's curve in Linear at a Mix under 100 % is approximate while a dynamic band moves; the quietest value between two ticks isn't kept (the meters give the loudest) | `src/ui/devices/LevelHistoryView.cpp` | — | 6 | ui-device-editors 7 |
| The EQ's editor on the shared frame; no private timers, pixel sizes or alphas in single editors | `src/ui/devices/EqEditor.h` | — | 6 | ui-device-editors 15 |
| The editors against their rules ([design/devices.md](design/devices.md), "Where an editor puts things"), what S6.9 left: choices of two or three as buttons (`ParameterControls` makes every choice a list); the output as the last control of the last part on the Synth, the FM synth, the Sampler, the Saturator and the Compressor's Compress page; Age's Band switch out of its picture; the Compressor's picture first on Compress and Duck; a ring for Phazite's modulators drawn dashed too; a check that no editor shows a More tab in the default dock | `docs/design/devices.md` | M | 6 | comparison with another DAW, 2026-11-17 |

### Mixer and controls

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The wheel over a knob or a fader: one undo step for a burst, and no change by a wheel that only scrolls past | `src/ui/components/Controls.cpp` | (S) | 5 | ui-core-mixer-app 2 |
| Typed values take a decimal comma, and a bare number's unit shows | `src/ui/components/ValueText.cpp` | (S) | 5 | ui-core-mixer-app 9 |
| Several strips at once: linked faders, strips selected together and reordered; strip widths, a taller fader, RMS meters and a clip light that holds | `src/ui/mixer/ChannelStrip.cpp` | L | 5 | ui-core-mixer-app 1 |
| Right-click menus, and the rows' actions in the browser and the Versions view, reached from the keyboard | `src/ui/components/Controls.cpp` | M | 5 | ui-core-mixer-app 7, ui-shell-browser 12 |
| Fine drag switched by its key during a drag, and knob drags that go on past the screen's edge | `src/ui/components/Controls.cpp` | — | — | ui-core-mixer-app 12 |
| Small ones: a theme colour that goes stale, a menu action that holds its component by a bare pointer, the system's settings for less motion and transparency followed | `src/ui/mixer/DeviceSlots.cpp` | — | — | ui-core-mixer-app 13 |
| Numbers written one way everywhere, and checked by a test: a true minus sign, a thin space before the unit, an en dash in a range, and a fixed count of decimals for each unit | `src/ui/components/ValueText.cpp` | S | 5 | comparison with another DAW, 2026-11-17 |
| Right-click menus in one grammar: a first row that names the object, groups in a fixed order with the clipboard's commands always in the same order, and what deletes last | `src/ui/components/Controls.cpp` | M | 5 | comparison with another DAW, 2026-11-17 |

### Shell

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The Account button, which does nothing, taken out of the activity bar until it does something | `src/ui/shell/ActivityBar.cpp` | — | — | ui-shell-browser 14 |
| The top bar's readouts (position, tempo, time signature, grid) as plain large numbers with a small unit and no box of their own, and the transport's buttons in one well: it is the most boxed part of the window | `src/ui/shell/TopBar.cpp` | M | 5 | comparison with another DAW, 2026-11-17 |
| What is switched on or picked drawn apart from what is playing or recording: a soft tint of the accent for the first (Snap, the tool, Ghost Notes), the solid accent for the second alone, so that the eye finds what runs | `src/ui/theme/DesignTokens.h` | M | 5 | comparison with another DAW, 2026-11-17 |
| Secondary windows that don't block the main one: Settings, the plug-in scan, Chord Progressions and the rest | `src/app/MainWindow.cpp` | M | 5 | ui-shell-browser 2, ui-core-mixer-app 6 |
| The Settings window: one refresh a turn of the message loop, not one for each setting written (a device change makes seven); a row greyed while its command is (Count-In without a song) | `src/ui/shell/SettingsPanel.cpp` | S | — | owner's round 2026-11-19 |
| Plug-in instances that opening a version removes kept alive until Back to Now, so that the way back is instant with heavy plug-ins | `src/model/plugins/` | M | — | owner's round 2026-11-19 |
| Rename and Delete for the user's own presets, from their rows in the browser (there are none; found 2026-11-19) | `src/ui/browser/Browser.cpp` | S | — | owner's round 2026-11-19 |
| The Devices view's Presets menu as the browser lists a device's presets: one list, no "Your Presets" heading | `src/ui/mixer/DevicesView.cpp` | S | — | owner's round 2026-11-19 |
| The device menus' list of devices built once a menu, not once for each menu and submenu; a pen's or a touch's press read for where a right-click's menu opens; the transport bar's widths as tokens | `src/ui/devices/DeviceMenus.cpp` | S | — | owner's round 2026-11-19 |
| The plug-in window's Plug-in menu lists the preset folders as it opens, on the message thread: from the library's listing | `src/app/PluginWindow.cpp` | S | — | threading audit, 2026-11-20 |
| The Inspector's track, section and song pages take a new target as its clips, notes and points pages do (they are made anew at each change while it shows); closing a project never waits for a waveform's file being opened on a drive that doesn't answer (two at most are waited for) | `src/ui/mixer/Inspector.cpp` | S | — | threading audit's fixes, 2026-11-20 |
| The dynamics curve's readout at its bottom: the first point reads "Out silence" beside a 1.0:1 line, and a level out typed as -60 becomes silence | `src/ui/devices/DynamicsCurve.cpp` | S | — | owner's round 2026-11-19 |
| The Limiter's history from readings every half millisecond, as the Compressor's (`devices/core/LevelTrail.h`) | `src/devices/dynamics/Limiter.cpp` | S | — | owner's round 2026-11-19 |
| The text the user reads in a strings table, with one term and one spelling for each thing; with it the design tokens in a file a designer edits, and a gallery app (architecture phase A4) | `src/ui/theme/DesignTokens.h` | L | 8 | ui-shell-browser 11, docs 5 |
| A session with Narrator, and what it finds fixed | `docs/hand-checks.md` | M | 3 | plan |
| Clips, notes, automation points and graphs' handles in a form a screen reader finds | `src/ui/playlist/ClipComponent.cpp` | L | 7 | tests-ui 5 |

## App and services

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| Updates and the installer signed: the update file's signature checked with a key built into Phazite, the installer's with Windows, and the file held from its check to its start | `src/services/Updates.cpp` | M | 8 | model-project-services 7, tests-model 14, product-gaps 15 |
| The rescan of the MIDI inputs every four seconds taken off the message thread; what the autosave costs on the message thread measured apart | `src/model/engine/AudioIO.cpp` | — | — | performance audit |
| The browser library's loop check keyed by volume and file identifier: an inode or an NTFS file index alone can be shared by folders on different volumes, so a linked folder can be skipped | `src/services/Library.cpp` | S | — | plan |
| The browser's small leftovers of folders read in the background: a key held while a folder's rows are read moves one row (queue a count of moves, dropped after the moment); the scroll after a Swap ends is set before a folder still being read has its rows (scroll again as they come, unless the user scrolled); the details of a selected sound stay as the index's first publish had them until the row is looked at again (refresh them when the second lands); a subfolder deleted outside shows "No sounds here" until its parent is read again (remove its row and read the parent); and the name field of a new collection isn't placed again when rows come in above it | `src/ui/browser/Browser.cpp` | S | 6 | the known-bugs round's reviews |

## Tests

Stage 2 built the audit's test nets on Windows ([tests/README.md](../tests/README.md), "The nets"); the rows here are
what is left of them, and what came up while they were built.

| Item | Where | Size | Stage | Status | Source |
|---|---|---|---|---|---|
| The long run over songs of several configurations (audio clips, plug-ins in helpers, containers, a tempo map), not the one busy song of eight tracks | `tests/LongRun.cpp` | S | — | Open | tests-infrastructure 14 |
| The benchmark gates' bounds for GCC's optimised builds, so that they can run on Linux too: today they hold for MSVC's alone | `tests/BenchmarkGates.cpp` | S | — | Open | plan |
| A new project's first save that fails (known bug 160), and a write cut short in the middle, tested through a seam in `writeWholeFile()` | `tests/ProjectTests.cpp` | — | — | Open | model-project-services 14, tests-model 8 |
| Waits for a helper or a timer that still pump a fixed time (SandboxedPluginTests' parameter and key pumps, PlaylistTests' playhead checks) made to wait for a condition; the autosave timer given a clock the tests drive; the files that measure call lengths kept apart from those that load every core | `tests/RealTimePlayback.h` | — | — | Open | tests-infrastructure 12, tests-audio 14, tests-model 15 |
| The drum machines' short and noise voices (rims, claves, maracas, guiro, tambourine, metal, the Trap and Percussion kits' own) pinned by their bands and lengths as the tuned voices are | `tests/DrumSynthesisTests.cpp` | — | — | Open | tests-audio 13 |
| Mouse-driven tests of the mixer strip's controls (fader, pan, sends, Pre/Post, arm), the settings panel's MIDI and Shortcuts tabs, and the render panel | `tests/MixerViewTests.cpp` | — | — | Open | tests-ui 9, tests-ui 15 |
| Golden renders regenerated one at a time, each with its reason, not all at once inside large commits | `tests/GoldenFiles.cpp` | — | — | Open | tests-infrastructure 10 |
| A gate on instructions counted, for every push: each device and a few mixes rendered offline under Valgrind's Callgrind, for the commit `main` is at and for the push's, failing above a small rise (1 %, say). It gives the same count on every machine, where the benchmark gates measure time and run nightly | `tests/BenchmarkGates.cpp` | M | 7 | Open | comparison with another DAW, 2026-11-17 |
| What the audio thread calls marked `[[clang::nonblocking]]` and checked as it compiles by Clang's `-Wfunction-effects` in the `linux-clang` build, beside the real-time audit's counts as it runs. Needs Clang 20 or newer; CI's is 18 | `src/engine/core/Engine.h` | M | — | Open | comparison with another DAW, 2026-11-17 |

## Docs

Stage 2 put the docs in order: pages that say what is, with the history in `archive/`. What is left is the guide's.

| Item | Where | Size | Stage | Status | Source |
|---|---|---|---|---|---|
| The guide: a page for each device, once stage 6 has finished it; the views have their pages | `docs/guide/` | L | 6 | Open | product-gaps 15 |
| The guide's pictures listed in one file (a name, the screen, the part to cut out, both themes) and painted from it by the site-pictures test, with a check that every picture a page names exists; each device's page on one template, opened from the device's menu | `tests/SitePictures.cpp` | M | 8 | Open | comparison with another DAW, 2026-11-17 |
| A map from each feature to its guide page, with the version the page was checked against, and a check that fails when a feature has no page: today nothing but the rule keeps a page from going stale | `docs/guide/` | M | 8 | Open | comparison with another DAW, 2026-11-17 |
| The website's download button for the visitor's system, with the version, the file's name and its size, beside the SHA-256 the page already shows; the version and its date at the top of the home page (the `phazite-site` repository, once 4.5 makes releases) | `docs/README.md` | S | 5 | Open | comparison with another DAW, 2026-11-17 |

## Product gaps

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The checks by hand: recording through an ASIO® interface, a MIDI keyboard, a DAWproject opened in another DAW | `docs/hand-checks.md` | — | 3 | product-gaps 7, docs 2 |
| MIDI learn, what is left: a link that picks the control up where it is rather than jumping, a range for a link, relative encoders, 14-bit controllers and NRPN, pads and notes as switches, links kept for every project, Escape to cancel learning, and Delete on a row of the Project view's MIDI Links | `src/model/midi/MidiLinks.cpp` | M | 7 | roadmap step S5.4 |
| Export of a range or the loop, and templates to start a project from | `src/model/project/Export.h` | M | 5 | product-gaps 11 |
| MP3 export, beside WAV and FLAC (roadmap step S5.16): an encoder under a compatible licence chosen (LAME is LGPL), added to the notices and [third-party.md](third-party.md), a bit rate to choose, and the song's start kept where it is | `src/model/project/Export.h` | M | 5 | the owner, 2026-11-17; product-gaps 11 |
| Presets heard in the browser before they go into the song: an instrument's preset plays a short phrase that fits its kind, an effect's plays on a loop, both rendered beside the song and never through it (roadmap step S5.18) | `src/ui/browser/Browser.cpp` | M | 5 | comparison with another DAW, 2026-11-17 |
| A catalogue of free plug-ins and sound packs, installed, updated and removed from inside Phazite: each an archive pinned by its address, size and SHA-256 as Phazite Sounds are, never an installer that runs, and a project that misses one offers it. The plug-ins run in helpers as all do. After 1.0 | `src/services/SoundPack.cpp` | XL | — | comparison with another DAW, 2026-11-17 |
| Note effects that play as notes come in (an arpeggiator, chords, a scale), and MIDI from plug-ins | `src/devices/core/DeviceInfo.h` | L | 7 | product-gaps 2 |
| MIDI output, clock out, and a MIDI input for each track, which can be another track's notes, so that two instruments play one part | `src/model/engine/AudioIO.cpp` | L | 7 | product-gaps 1; comparison with another DAW, 2026-11-17 |
| Any track or bus as a track's output | `src/model/mixer/MixerModel.h` | (M) | 7 | product-gaps 14 |
| The Multisampler as a plain SFZ player with one piano; with it, sampled acoustic instruments | `src/devices/sampler/Sampler.cpp` | XL | 7 | product-gaps 6 |
| Release work: crash reports with symbols, Linux tried, compatibility of projects from 1.0 on | `packaging/windows/Phazite.iss` | L | 8 | product-gaps 15, product-gaps 12 |
| Out of 1.0: video, notation, a clip launcher, scripting, CLAP, ARA, macOS, translations, Ableton Link, control surfaces | `docs/roadmap.md` | — | — | plan, product-gaps 4, product-gaps 14 |

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

## Structure and build

| Item | Where | Size | Stage | Source |
|---|---|---|---|---|
| The `windows-asan` preset built and run on a PC with the x64 ASan component (`tools\setup-windows.cmd` installs it), and what it finds fixed; the analyzer checks turned off in `.clang-tidy`, and Clang's own warnings, triaged and then made errors; after them macOS and a measure of coverage | `CMakePresets.json` | M | — | build-ci-tools 3, tests-infrastructure 13 |
| Each layer a library of its own, so that the build proves the layers and the architecture check doesn't only count them | `CMakeLists.txt` | L | 5 | build-ci-tools 7 |
| The size of files counted by the architecture check, as phase A2's limit of 1,000 lines isn't | `tools/check-architecture.sh` | S | 5 | docs 10 |
| The 33 colours, font sizes and corner radii written as literals in `src/ui` and `src/app` moved into the design tokens, taking the `ui-design-literals` budget to 0 (`sh tools/check-architecture.sh --list ui-design-literals` lists them) | `src/ui/theme/DesignTokens.h` | M | — | tests-ui 8 |
| The solo rule in one place, where the track list and the song player each have it; with it the previews' sync and the song player's size | `src/model/playback/SongPlayer.cpp` | M | 5 | model-playback 15 |
| Release builds with symbols kept, and a dump left by a user's crash | `CMakeLists.txt` | M | 8 | build-ci-tools 2 |
| Linux for a beta: PipeWire or JACK beside ALSA, HTTPS, install rules | `CMakeLists.txt` | L | 8 | build-ci-tools 5 |
| The installer's silent update tested, and an uninstaller that minds a helper still running | `tools/test-installer.ps1` | S | 8 | build-ci-tools 6 |
| The pins made whole: clang-format by its hash, `format.sh` warning of another version, action and submodule pins refreshed by a bot, a SECURITY.md | `tools/format.sh` | — | — | build-ci-tools 9 |
| The real-time audit's allocation stacks under the sanitizers, where it only counts | `tests/AudioThreadMonitor.cpp` | S | — | plan |
