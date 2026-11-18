# src/devices

The DSP of Phazite's own instruments and effects, engine-neutral and compiled into `phazite_core`. It uses JUCE's
`juce_core`, `juce_audio_basics` and `juce_dsp` only, and Rubber Band, and nothing of the other layers, so that a device
could run on another engine; Phazite's engine plays each through `engine::DeviceRunner` (`src/engine/nodes/`). Every
other layer may include it ([docs/architecture.md](../../docs/architecture.md)).

One folder per area, with `core/` below the others. Each device's parameters, described in its header, are what the
model saves and automation and modulation move.

## core/

| File | What it holds |
|---|---|
| `DeviceInfo.h` | How a device and its parameters are described, and each knob's travel (`Scale`: logarithmic for every frequency from above 0, else linear or skewed around a midpoint; `Travel`, the same as plain numbers for the audio thread). |
| `Processor.h` | What a device does with a block: what plays it prepares it, then hands it each block's audio, notes and parameter values. |
| `Clock.h` | The clock Phazite reads where a test must steer time (the timeouts that something must not end before, the CPU meters' measure, the engine's block times and playhead stamps): JUCE's high-resolution clock, unless a test holds it. |
| `AudioThreads.h` | How Phazite's own threads that play audio (the engine's workers, the helpers' threads that play plug-ins) get the audio device's priority, never raising the whole process's. |
| `DeviceScope.h` | What goes into a device and what comes out of it, for its editor's displays, at no cost until an editor listens. |
| `SampleTap.h` | What a device just played, each channel apart, for a display on the message thread (an EQ's analyzer); lock-free, one writer and one reader, holding a quarter of a second at 192 kHz. |
| `Spectrum.h` | The spectrum of what a device plays, for an analyzer. |
| `LiveValue.h`, `MeterValue.h`, `FallingValue.h` | Values a device sets on the audio thread for its editor to show: the latest (`LiveValue`), the most since the editor last looked (`MeterValue`), or a peak that falls back at a steady rate, for several displays at once (`FallingValue`). |

## The devices

| Folder | What it holds |
|---|---|
| `synth/` | `Synth.h`: the synth ("Synth"), two wavetable oscillators, a sub and noise, unison, a filter, three envelopes, two LFOs and eight modulation slots. |
| `wavetable/` | `Wavetable.h`: a wavetable, its frames band-limited at every pitch, with what a part of a frame's cycle adds up to, for the warps that read one; `FactoryWavetables.h`: the tables that come with it, made from formulas the first time each is needed. |
| `fm/` | `FmSynth.h`: the FM synth ("FM"), four operators. |
| `drums/` | `DrumSampler.h`: the drum sampler, a pad on every note, each with its envelope, sweep, filter and layers, its parameters found by their IDs at once. `FactoryDrums.h`: its factory sounds, the default kit and each of Phazite's kits' own, synthesized when first needed, from `DrumSynthesis.h` (with the 80s' gated reverb, a room that isn't gated, `addRoom()`, struck sounds by their modes, `makeStruck()`, and the Simmons SDS-V's voice, which `tools/soundkit/` shares) and `DrumMachines.h` (the classic drum machines' voices as their circuits make them: the 808's, the 909's and a CR-78's). |
| `sampler/` | `Sampler.h`: the sampler, one sound played across the keys. `SampleAnalysis.h`: what it finds in a sound as it loads (its pitch, where its hits begin, its zero crossings). `FactorySamples.h`: its factory sounds, an 808 bass, a saw ensemble made from its spectrum (Wide Saw), and the drum sampler's. |
| `vocoder/` | `Vocoder.h`: the vocoder. |
| `eq/` | `Equalizer.h`: the EQ, eight bands matched to their analog shapes, each on the whole sound, its middle, its side, its left or its right, and each bell or shelf dynamic if wanted. |
| `filter/` | `FilterEffect.h`: the Filter device, with a drive. |
| `dynamics/` | `Compressor.h`: the compressor, in its Compress (keyed or not, with a look-ahead, automatic makeup and a hold), Duck and Transient modes. `Limiter.h`: the limiter, which looks ahead and listens for true peaks. `Multiband.h`: the multiband, in the layout of FL Studio's Maximus. `DynamicsCurve.h`: a dynamics curve, through up to eight points, which the compressor and the multiband share. `TruePeak.h`: true peaks as ITU-R BS.1770 finds them, for the limiter and the loudness meter. |
| `saturation/` | `Saturator.h`: the saturator: six curves (Crush among them), a colour, a band that only it drives (which makes it an exciter too), at 2, 4 or 8 times the rate. |
| `delay/` | `Delay.h`: the delay: a time for each side, a Tape or Clean loop with a feedback to 110 %, modulation, ducking, a freeze, and a change of time that glides or fades. |
| `reverb/` | `Reverb.h`: the reverb, a room's eight-line feedback delay network, Jon Dattorro's plate (JAES 1997) or a hall's sixteen-line network, as its Mode says, the plate's decay from a table measured on it. |
| `motion/` | `Motion.h`: Motion, the effects that sweep as one device in three modes, its values mapped to its engines and a change of mode faded; `Chorus.h`, `Flanger.h`, `Phaser.h`: its engines, no devices of their own; `SafeBass.h`: Safe Bass, which they share, the lows going around the effect. |
| `stereo/` | `Stereo.h`: Stereo, the device for a channel's two sides (width, bass in mono, balance, mono, swap, polarity, a DC filter and gain). |
| `age/` | `Age.h`: Age, what a medium does to a sound, a record or a tape, each part with a switch: its wow and flutter through one moving delay line, the drive (a record's groove distortion of the loud highs, a tape's saturation and head bump), its lows in mono, its narrower sides and its band. `SurfaceNoise.h`: a record's surface noise, hiss and crackle, each click drawn from where the song is. `TapeWear.h`: a tape's hiss and its dropouts, each drawn from where the song is. |

## What the devices share

| Folder | What it holds |
|---|---|
| `modulation/` | `Modulation.h`: modulation as devices take it, an LFO's waves and how modulation moves a parameter. `Modulators.h`: the six modulators (LFO, envelope follower, step sequencer, random source, breakpoint envelope, note follower), each worked out on the song's control points. |
| `stretch/` | `Stretch.h`: how audio clips are stretched, pitched and reversed: Rubber Band Library's R3 engine, offline, over a whole file, along a time map for a clip under the song's tempo changes, in real-time mode with its stretch set at each hop to land on the map. `RubberBandLibrary.cpp`: Rubber Band's single-file build, with its warnings off. |
| `dsp/` | The building blocks: `Oscillators.h` (band-limited oscillators, read from band-limited tables as the synth's wavetables are, for the synth's sub and the vocoder's buzz, and a low-frequency one), `Filters.h` (the state-variable filter, and the EQ's matched bands), `MultimodeFilter.h` (the filter types every Phazite filter offers), `CombFilter.h`, `Crossover.h` (Linkwitz-Riley crossovers), `DcBlocker.h` (an instrument's DC blocker, 12 dB at 5 Hz), `DelayLine.h`, `Envelopes.h` (the ADSR and the level follower), `Noise.h` (the synths' white noise, alike at every rate), `Saturation.h` and `Oversampling.h` (curves played by their antiderivatives at twice the rate, so that drive doesn't alias), `AlignedShaper.h` (such a curve whose delay a first-order all-pass makes up to a whole number of samples, so that a plain delay lines up with it), `ControlGrid.h` (the effects' and the synths' grid of control steps, counted from the song's place while it plays, so that a loop, a start anywhere and a wake from sleep play as a render does, and how far a step the song cut short got, for the glides to go on from there), `SincReader.h` (a sound read between its samples by a windowed sinc whose cutoff follows its pitch), `SpectralLoop.h` (a sound made from its spectrum, each harmonic a band so many cents wide, as one seamless loop, by Paul Nasca's PADsynth algorithm), `SyncedLfo.h` (the sweeping effects' LFO, in time with the song), `NoteValues.h` (note values and their lengths), `Random.h` (random numbers that come out the same from the same seed) and `Math.h`. |
