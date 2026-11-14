# Sampler

*Design section 9.*

One sound played across the keys: held and looped as an instrument, played through as a one-shot, or cut into slices
that each sit on a key of their own. A new sampler plays an 808 bass at C2 that Phazite synthesizes, so that it plays a
finished sound at once.

## Parameters

Parameter names are the ones automation and modulation list. The editor shortens some of its labels, as noted. Places
in the sound (the markers) are parts of its length, from 0 to 100 %.

### Sample

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | Classic, One-Shot, Slice | Classic | How the sound is played: see "How it works". The editor shows it as three buttons. |
| Start | 0 to 100 % | 0 % | Where what the sound plays begins. |
| Start Random | 0 to 100 % | 0 % | How far past Start a note may begin, drawn again at each note: at 100 %, anywhere in what it plays. The editor's label is Random. |
| End | 0 to 100 % | 100 % | Where what the sound plays ends. |
| Reverse | Off, On | Off | Plays the sound backwards. |
| Loop On | Off, On | Off | Classic: loops the sound between Loop Start and Loop End while a note is held. The editor's label is Loop. |
| Loop Start | 0 to 100 % | 67 % | Where the loop begins. |
| Loop End | 0 to 100 % | 100 % | Where the loop ends. |
| Crossfade | 0 to 50 % | 10 % | How much of the loop's length its seam crossfades over, with what comes before Loop Start. |
| Playback | Trigger, Gate | Trigger | One-Shot and Slice: Trigger plays to the end however short the note; Gate plays while the note is held. |
| Fade In | 0 ms to 1 s | 0 ms | One-Shot and Slice: a fade at the start. |
| Fade Out | 0 ms to 1 s | 10 ms | One-Shot and Slice: a fade before the end, and as a gated note is let go. |
| Slice By | Transients, Beats, Equal, Manual | Transients | Slice: where the sound is cut: at its hits, on a beat division, into equal parts, or only where slices were put by hand. |
| Sensitivity | 0 to 100 % | 50 % | Slicing by Transients: how many of the sound's hits slice it, from the strongest only to all of them. |
| Division | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar, 2 bars, 4 bars, 8 bars | 1/16 | Slicing by Beats: the note value of a slice. |
| Slices | 2 to 64 | 8 | Slicing by Equal: how many parts. |
| Play Through | Off, On | Off | Slice: a slice plays on past its end, to the end of the sound. The editor's label is Through. |
| Choke | Off, On | On | Slice: a slice cuts off the one playing. |

### Pitch

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Root | C-1 to G9 (notes 0 to 127) | C2 | The note that plays the sound at its own pitch. |
| Transpose | -48 to +48 semitones | 0 semitones | Moves every note's pitch. |
| Fine | -50 to +50 cents | 0 cents | Fine tuning. Loading a sound sets it to take back how far the sound is off its root note. |

### Voice

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Voices | Poly, Mono | Poly | Poly plays up to 16 notes; Mono plays one, legato. The editor's label is Mode. |
| Glide | 0 ms to 2 s | 0 ms | How long the pitch takes to move from the last note to the new one. |
| Bend Range | 0 to 24 semitones | 2 semitones | How far the pitch wheel bends the pitch. The editor's label is Bend. |
| Velocity | 0 to 100 % | 60 % | How much a softer note is turned down. |
| Level | -36 to +6 dB | -6 dB | The sampler's output level. |

### Amp

The amp envelope shapes Classic's notes. In One-Shot and Slice the fades shape a note instead, and the envelope shows
faint.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Attack | 0.5 ms to 10 s | 1 ms | The time the level takes to reach its peak. |
| Decay | 0 ms to 10 s | 300 ms | The time it then takes to fall to its sustain. |
| Sustain | 0 to 100 % | 100 % | Where it stays while the key is held. |
| Release | 0.5 ms to 10 s | 50 ms | The time it takes to fall silent after the key is let go. |
| Attack Curve | -100 to +100 % | -15 % | How the attack bends: 0 % is a straight line, below it fast at first and slowing into its target, above it slow at first. The editor's label is A Curve. |
| Decay Curve | -100 to +100 % | -46 % | The same for the decay (D Curve). |
| Release Curve | -100 to +100 % | -46 % | The same for the release (R Curve). |
| Key Time | 0 to 100 % | 0 % | How much shorter the envelope's times are up the keys: at 100 %, half for each octave above the root and double for each below. |

### Filter

The filter is switched by the switch at its title (Filter On).

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter On | Off, On | Off | Switches the filter on or off. |
| Filter Type | Low Pass, High Pass, Band Pass, Notch, Comb +, Comb -, Vowel, Phaser, Sample & Hold | Low Pass | The filter's type, the same nine the [Synth](synth.md) and the [Filter](filter.md) have. |
| Cutoff | 20 Hz to 20 kHz | 3 kHz | The filter's frequency. |
| Resonance | 0 to 100 % | 15 % | How much the filter rings at its cutoff. |
| Filter Slope | 12 dB, 24 dB | 12 dB | The slope per octave. |
| Key Tracking | 0 to 100 % | 50 % | How much the cutoff follows the note. The editor's label is Key. |
| Filter Velocity | 0 to 100 % | 0 % | How far a softer note's cutoff falls: at 100 %, four octaves at velocity 0. |
| Filter Envelope | -48 to +48 semitones | +24 semitones | How far the filter's envelope opens the cutoff, or closes it below 0, while the envelope is on. |

### Filter Envelope

The envelope is switched by the switch at its title (Filter Envelope On).

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter Envelope On | Off, On | Off | Switches the filter's envelope on or off. |
| Filter Attack | 0 ms to 10 s | 1 ms | The envelope's attack. |
| Filter Decay | 0 ms to 10 s | 400 ms | Its decay. |
| Filter Sustain | 0 to 100 % | 20 % | Its sustain. |
| Filter Release | 0 ms to 10 s | 300 ms | Its release. |
| Filter Attack Curve | -100 to +100 % | -15 % | How the attack bends, as the amp's. |
| Filter Decay Curve | -100 to +100 % | -46 % | How the decay bends. |
| Filter Release Curve | -100 to +100 % | -46 % | How the release bends. |

### LFO

The LFO is switched by the switch at its title (LFO On).

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| LFO On | Off, On | Off | Switches the LFO on or off. |
| LFO Shape | Sine, Triangle, Saw Up, Saw Down, Square, Random | Sine | The LFO's wave. |
| LFO Target | Pitch, Cutoff, Level | Pitch | What it moves: the pitch (up to a semitone either way), the cutoff (up to two octaves either way) or the level (tremolo). |
| LFO Rate | 0.05 to 20 Hz | 5 Hz | Its speed. |
| LFO Depth | 0 to 100 % | 25 % | How far it moves its target. |

## How it works

**The three modes.**

- **Classic** plays the sound at the note's pitch against the root key, from Start to End, for as long as the note is
  held, through the amp envelope. With Loop on, a held note goes round between Loop Start and Loop End. The loop's seam
  crossfades at equal power with what comes before Loop Start, over Crossfade's part of the loop's length (at most
  half the loop, and no more than there is before Loop Start), so that it doesn't click.
- **One-Shot** plays the sound at the note's pitch from Start to End however short the note (Trigger), or while it is
  held (Gate), shaped by Fade In and Fade Out.
- **Slice** cuts what lies between Start and End into up to 64 slices, each on its own key from C2 (note 36) up, played
  at the sound's own pitch (Transpose, Fine and the pitch wheel still move it), triggered or gated, with the fades. With
  Choke a slice cuts off the one playing; with Play Through a slice plays on to the end of the sound.

**Slicing.** As a sound loads, the sampler finds where its hits begin by the spectral flux of its log magnitudes over
an adaptive threshold (Dixon, 2006), at least 40 ms apart, each placed just before its rise and on a zero crossing.
Transients slices at those hits, the strongest first as Sensitivity rises. Beats slices on a note value at the sound's
own tempo, where its loop data or its name say how many beats it lasts, and at the song's tempo otherwise. Equal cuts it
into equal parts. Manual keeps only the slices put in by hand. In every way of slicing, a slice put in by hand stays,
and a found slice that was moved or taken away stays away. Two slices closer than 64 samples are one.

**The sound.** A sampler plays one of the factory sounds or a sound file. The factory sounds are synthesized the first
time each is needed, so Phazite ships no sound files: an 808 bass, "Wide Saw" (a saw ensemble made from its spectrum by
Paul Nasca's PADsynth algorithm, each harmonic a band of frequencies with random phases, as one seamless stereo loop;
`src/devices/dsp/SpectralLoop.h`), and every factory drum of the [Drum Sampler](drum-sampler.md). A sound file is
copied into the project's Audio folder and its first two minutes are read into memory once, however many samplers play
it. The project keeps a factory sound by its name and a file by its path.

Putting a sound on the sampler is one undo step, named after the sound ("Put Vox on the Sampler"). It keeps the mode
and every setting but those that belong to the sound: the root key and Fine, the markers (set to the whole sound), the
loop (the file's own, switched on, or off) and the slices changed by hand (none).

**The root key** comes from, in this order: the note the file's own data name (a WAV file's `smpl` chunk); a note in
its name ("808 Long F#"), in the octave its pitch is in; its pitch, found on its steady part past its attack by YIN (de
Cheveigné and Kawahara, 2002); or else middle C. Fine is set to the cents the sound is off that note, within half a
semitone, so that a sound tuned between notes plays in tune. Find Root Key does this again for the sound playing.

**Reading the sound.** A voice reads between the sound's samples with a Kaiser-windowed sinc of 16 zero crossings a
side whose cutoff follows how fast it is read (`src/devices/dsp/SincReader.h`), forward or reversed, so that a note
pitched up doesn't alias and the top octave keeps its level. A note an octave or more above the sound's pitch reads
from a copy of the sound at half, a quarter or an eighth of its rate, made as the sound loads, which costs less for the
same result. A seamless factory sound reads round from its end into its start. A start or an end in the middle of the
sound fades over 32 samples.

**Voices.** Poly mode has 16 voices. Every note takes a voice of its own, a key played again too, so that a release,
a one-shot or a slice that Choke leaves playing rings on under the new note. With all voices busy a new note takes
another's voice, which fades out over 3 ms. Mono mode plays one voice, legato: it glides between held keys without
starting the sound or its envelopes again. The piano roll's slide and portamento notes glide too. The sustain pedal
holds notes. Start Random draws its places from a fixed seed, so a render is the same every time.

After the sound come the filter (`src/devices/dsp/MultimodeFilter.h`, one for each side), the amp envelope or the
fades, and the level. Pitch, the filter and the LFO are updated every 16 samples. The sampler swaps a sound in while it
plays without the audio thread waiting or freeing memory; a voice still on the old sound fades out.

## Its editor

`src/ui/devices/SamplerEditor.h`. A dock narrower than its parts shows them in pages (see [Devices](../devices.md) for
the frame every editor shares).

- **Sample**, first: the three modes as buttons; a button with the sound's name, which opens a menu of the factory
  sounds (the drums grouped by kit, the one playing ticked), Load Sound File... and Find Root Key; Reverse; and the
  sound drawn, with its length in its corner (and its root key, outside Slice mode). A sound dropped on the drawing
  from the browser or the system's file manager is put on the sampler.
- On the drawing: what plays lies between the Start and End markers; Classic's loop is a band between its own two
  markers; One-Shot's fades are ramps; Slice's slices are lines, those put in by hand in the accent colour. Each voice's
  place in the sound shows as it plays.
- Every marker drags, as one undo step, snapping to the zero crossing within 10 ms of the pointer; Alt places it
  anywhere, Shift moves it in fine steps, and a double-click puts it back. The loop's band drags the loop whole.
- In Slice mode a click on the strip under the drawing adds a slice there, a drag moves a slice and a double-click
  takes it away, each one undo step. **Slices to Clip** writes each slice's note, as long as its slice, into a new MIDI
  clip on the track at the play start (after the track's last clip where one is in the way), as one undo step, so that
  the clip plays the sound in time with the song at any tempo.
- A press on the sound plays it at its root, or in Slice mode the slice under the pointer, while the button is held.
- Under the drawing, one row of the controls of the mode it plays in: Classic's (Start, Random, End, Loop, Loop Start,
  Loop End, Crossfade), One-Shot's (Start, Random, End, Playback, Fade In, Fade Out) or Slice's (Slice By, the knob of
  the way it slices, Playback, Fade In, Fade Out, Choke, Through).
- Then **Pitch** over **Voice**; the **Amp** envelope, drawn, with its points to drag; the **Filter** with its response
  drawn; its **Filter Envelope**; and the **LFO** with its wave drawn.

## Limits

- One sound in one zone: no multisampled instruments, velocity layers or round robin. Those are planned as a device of
  their own, the Multisampler (see [Devices](../devices.md)).
- No time-stretching: a loop or a slice plays at the speed its pitch gives it and doesn't follow the song's tempo.
  Slices to Clip is the way to play a sliced loop in time.
- Loops run forward only: no ping-pong loop, and no choice of what the loop does at a note's release.
- No pitch envelope.
- Slices share every setting: no level, pitch, reverse or envelope for each slice.
- Swap tries presets on the sampler, not sounds.
- A sound file's first two minutes are read, into memory: longer sounds are cut, and nothing streams from disk.
- Every factory sound is synthesized: no recorded instrument comes with it.
- One LFO for the whole sampler, free-running, with one target: no tempo sync, no restart at each note, and at most a
  semitone of pitch.
- Expression is the pitch wheel and the sustain pedal: the mod wheel does nothing, and there is no aftertouch, MPE or
  modulation matrix inside the sampler. The track's [modulators](../automation-and-modulation.md) reach every
  parameter, but not per voice.
- Pitch and the cutoff are set once every 16 samples and not smoothed between.
- The filter is linear: it doesn't self-oscillate and has no drive.
- When several notes each take a busy voice on the same sample, only the last taken fades out; the others stop at once
  and can click.
- Sounds read from floating-point files aren't checked for values that aren't numbers (known bug 35 in
  [known-bugs.md](../../known-bugs.md)).
- The drawing can't be zoomed or scrolled, so markers on a long sound are placed to the nearest pixel of the whole
  sound; a reversed sound is drawn forwards, marked "Reversed".
- A right-click on the drawing plays the sound as a left click does, and a double-click removes a slice where it resets
  a marker.

## In the code

- DSP: `src/devices/sampler/Sampler.h`, `src/devices/sampler/Sampler.cpp`; what is found in a sound as it loads,
  `src/devices/sampler/SampleAnalysis.h`; the factory sounds, `src/devices/sampler/FactorySamples.h`. Shared:
  `src/devices/dsp/SincReader.h`, `src/devices/dsp/SpectralLoop.h`, `src/devices/dsp/MultimodeFilter.h`,
  `src/devices/dsp/Envelopes.h`.
- Model: `src/model/devices/internal/SamplerSound.h` (choosing and loading the sound, the root key, the slices changed
  by hand, Slices to Clip), `src/model/devices/internal/DeviceFiles.h` (the project's copy of a sound file).
- Editor: `src/ui/devices/SamplerEditor.h`, `src/ui/devices/SamplerEditor.cpp`, with the envelope, filter and LFO views
  of `src/ui/devices/SynthParts.h`.
- Presets: `resources/presets/Sampler/`, described in [presets.md](../../presets.md).
- Tests: `tests/SamplerTests.cpp`, `tests/SamplerModelTests.cpp`, `tests/SamplerEditorTests.cpp`,
  `tests/SpectralLoopTests.cpp`, `tests/FactoryPresetTests.cpp`; the parameters in the test vector
  `tests/golden/format/devices.json`.
