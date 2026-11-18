# src/engine

Phazite's own playback engine, engine-neutral like `src/devices/` and compiled into `phazite_core`. It uses JUCE's
`juce_core`, `juce_audio_basics` and `juce_dsp`, r8brain-free-src and `src/devices/`, and nothing of the layers above;
the model compiles the song into what it plays (`src/model/playback/internal/SongPlayer.h`;
[docs/architecture.md](../../docs/architecture.md)).

## core/

| File | What it holds |
|---|---|
| `Engine.h` | The engine: it plays a plan, handed to it whole, and its transport, which the message thread drives through the state it last asked for (atomic values) and reads back through its playhead, with fades at a stop, a jump, the loop's end and a plan whose delay compensation changed. Live, the model's playback host calls it from the audio device's callback; a render makes an engine of its own. |
| `Plan.h` | What the engine plays, built on the message thread from the song and never changed once built. What sounds on from one plan to the next (a device's voices, a reverb's tail, a channel's fader) is pointed at, kept by the song's IDs. |
| `Handover.h` | How the audio thread is handed what it plays, whole, and frees nothing: what it replaces is freed on the message thread, but for what the audio thread says it keeps playing a while longer. |
| `Workers.h` | The threads that help the engine play a block, each strip a task. |
| `Recording.h` | What the engine records of a take: the armed inputs, and where the take started, known to the sample; the take after a jump, started where the playhead lands. |
| `PlayheadClock.h` | Where the playhead was as each of the last blocks started, on a clock smoothed over them, which the window reads at its frames' own times. |
| `TempoMap.h` | The song's tempo map in samples, a beat a quarter note whatever the time signature, its ramps played exactly and bent ones in straight pieces, with its bars and the time signature's beats that the count-in and the metronome click on. |
| `EventQueue.h` | Live notes from any thread into the engine: a lock-free queue for several threads into one. |

## nodes/

| File | What it holds |
|---|---|
| `DeviceRunner.h` | One of Phazite's devices, or a plug-in in its helper, as any engine plays it: what every device gets around its DSP (its parameters glided, automated and modulated on the song's control points, its mix). |
| `DeviceNode.h` | A device as Phazite's engine keeps it from plan to plan, with its values set by hand. |
| `NoteSequence.h` | A track's notes and controllers' changes, in beats, with an index of the notes sounding at a place and each controller's changes, made with it, which a start, a jump and the loop's wrap find them and the controllers' values (the chase) by. |
| `PartOutput.h` | A part of a device's sound on a mixer channel of its own (a drum sampler's pad on a track of its own): the buffer the device plays it into, which the pad's track adds up. |
| `Channel.h`, `LevelMeter.h` | A channel's fader, pan, mute and meter. |
| `Strip.h` | A track's, bus's, return's or the master's state, with what joins paths: the rings of what came out of a strip at a point (its taps, which the paths from there share), the delay that lines a path up and a send's level. |
| `Container.h` | A container's chains, each in a buffer of its own, its split of what comes in and its mix. |
| `LoudnessMeter.h` | The loudness of what goes out, by EBU R128. |
| `Comparison.h` | A version's bounce heard against the song (A/B) after the master, each measured as it is and the louder turned down to the other's level at each switch. |

## audio/

| File | What it holds |
|---|---|
| `AudioFile.h`, `FileReader.h` | Audio files, read ahead of where they play by a reading thread, so that the audio thread never waits for the disk, and where a jump lands first, the message thread never waiting for a read either; what isn't a number in a file of floats read as silence. |
| `AudioRegion.h` | An audio clip as the engine plays it: a part of a file at its gain, through its fades; one of a file at another rate with a second resampling, made ready a little a block for where it plays from next (the loop's start, its own start ahead). |
| `Resampler.h` | A file at another rate than the engine's, resampled as it plays by r8brain-free-src, header-only (`external/r8brain-free-src`): `Resampler.cpp` turns its warnings off, and its folder is a system include for GCC and Clang. |

## Other folders

| Folder | What it holds |
|---|---|
| `automation/` | `Curve.h`: a parameter's automation as the engine plays it, its points positions along the control's travel turned into the parameter's units by its range, and the control points, one every 32 samples from the song's start, that automation and modulation move what they move on. |
| `modulation/` | `ModulatorNode.h`: a track's modulator or macro as the engine plays it, kept by its ID, its values on a block's control points, and its history for its editor. |
| `transport/` | `Click.h`: the metronome's click, synthesized. |
