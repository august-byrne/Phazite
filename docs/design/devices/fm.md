# FM

*Design section 9.*

A four-operator FM synth for electric pianos, bells, basses, brass, mallets, organs and synthesized percussion. Each
operator is an oscillator with its own envelope; an algorithm says which operators are heard and which bend the others'
waves. A new FM plays a soft electric piano, so that it is a finished sound from the start.

## Parameters

Parameter names are the ones automation and modulation list. The editor shortens some of its labels, as noted.

### Algorithm and Timbre

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Algorithm | Stack, Two Into Stack, Chain And One, Three Into One, Two Pairs, Stack And One, One Into Three, Four Carriers | Two Pairs | Which operators modulate which, and which are heard (the carriers). See the list below. |
| Timbre | 0 to 200 % | 100 % | Turns every modulator's depth and every operator's feedback up or down at once: 0 % is none, 100 % as set, 200 % twice as deep. Feedback never goes past its full depth. |

The eight algorithms, with operator 4 the highest (`4 > 3` reads "4 modulates 3", `|` separates what is heard side by
side):

| Algorithm | Shape | Carriers |
| --- | --- | --- |
| Stack | 4 > 3 > 2 > 1 | 1 |
| Two Into Stack | 4 and 3 > 2 > 1 | 1 |
| Chain And One | 4 > 3, and 3 and 2 > 1 | 1 |
| Three Into One | 4, 3 and 2 > 1 | 1 |
| Two Pairs | 4 > 3 \| 2 > 1 | 1 and 3 |
| Stack And One | 4 > 3 > 2 \| 1 | 1 and 2 |
| One Into Three | 4 > 3, 2 and 1 | 1, 2 and 3 |
| Four Carriers | 4 \| 3 \| 2 \| 1 | all four |

### Each operator

Every operator has the same 19 parameters, named "Op 1 ..." to "Op 4 ...". The defaults that differ between operators
are in the second table.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Op N On | Off, On | On | Switches the operator on or off. |
| Op N Wave | Sine, Triangle, Saw, Square | Sine | Its wave. Triangle, saw and square are band-limited. |
| Op N Feedback | -100 to +100 % | see below | Feeds its own wave back into itself: above 0 toward a saw (every harmonic), below 0 toward a square (odd harmonics only). Brightest at 50 % either way; further, rougher, and toward noise at 100 %. |
| Op N Phase | 0 to 360° | 0° | Where in its wave it starts each note while the synth's Phase is Reset. Nothing while Phase is Free. |
| Op N Ratio | 0.5 to 32, in steps of 0.5 | see below | Its pitch as a multiple of the note's. |
| Op N Fine | 0 to 0.49, in steps of 0.01 | 0 | Added to the ratio, for pitches between its steps. |
| Op N Detune | -50 to +50 cents | see below | Pulls it off its ratio, so that two operators a few cents apart beat. |
| Op N Fixed | Off, On | Off | Plays it at Frequency whatever the note, rather than at a ratio of the note's pitch. |
| Op N Frequency | 1 Hz to 10 kHz | 440 Hz | Its frequency while Fixed is on. |
| Op N Level | 0 to 100 % | see below | A carrier's loudness, or a modulator's depth: how strongly it bends what it modulates. |
| Op N Velocity | 0 to 100 % | see below | How much a note's velocity changes its level: on a carrier louder, on a modulator brighter. |
| Op N Key Scale | -100 to +100 % | see below | How its level follows the key. At 100 %, half the level for each octave above middle C and double for each below; below 0 the other way. |
| Op N Attack | 0.5 ms to 10 s | see below | The time its envelope takes to reach its peak. |
| Op N Decay | 0 ms to 10 s | see below | The time it then takes to fall to its sustain. |
| Op N Sustain | 0 to 100 % | 0 % | Where it stays while the key is held. |
| Op N Release | 0.5 ms to 10 s | see below | The time it takes to fall silent after the key is let go. |
| Op N Attack Curve | -100 to +100 % | -15 % | How the attack bends: 0 % is a straight line, below it fast at first and slowing into its target, above it slow at first. |
| Op N Decay Curve | -100 to +100 % | -46 % | The same for the decay. |
| Op N Release Curve | -100 to +100 % | -46 % | The same for the release. |

| Default | Op 1 | Op 2 | Op 3 | Op 4 |
| --- | --- | --- | --- | --- |
| Ratio | 1 | 14 | 1 | 1 |
| Detune | +4 cents | 0 cents | -4 cents | 0 cents |
| Level | 100 % | 30 % | 100 % | 45 % |
| Velocity | 25 % | 90 % | 25 % | 70 % |
| Key Scale | 0 % | 25 % | 0 % | 0 % |
| Feedback | 0 % | 0 % | 0 % | 10 % |
| Attack | 1 ms | 0.5 ms | 1 ms | 1 ms |
| Decay | 3 s | 350 ms | 2.5 s | 1.2 s |
| Release | 400 ms | 300 ms | 400 ms | 300 ms |

### Pitch

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pitch Amount | -48 to +48 semitones | 0 semitones | How far above (or below) its pitch each note starts. The editor's label is Amount. |
| Pitch Decay | 1 ms to 2 s | 60 ms | How fast the pitch falls back to the note's. The editor's label is Decay. |
| Key Time | 0 to 100 % | 25 % | How much shorter every envelope is up the keys. At 100 %, half the times for each octave above middle C and double for each below, between an eighth and eight times. |

### Voice

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Voices | Poly, Mono | Poly | Poly plays up to 16 notes; Mono plays one, legato. The editor's label is Mode. |
| Glide | 0 ms to 2 s | 0 ms | How long the pitch takes to move from the last note to the new one. |
| Bend Range | 0 to 24 semitones | 2 semitones | How far the pitch wheel bends the pitch. The editor's label is Bend. |
| Phase | Reset, Free | Reset | Reset starts every operator at its own Phase at each note, so that a note sounds the same every time; Free lets the operators run on from where they were. |
| Spread | 0 to 50 cents | 0 cents | Above 0, each note plays twice, that far apart, the lower on the left and the higher on the right. |
| Level | -36 to +6 dB | -6 dB | The synth's output level. |

### Filter

The filter is switched by the switch at its title (Filter On).

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter On | Off, On | Off | Switches the filter on or off. |
| Filter Type | Low Pass, High Pass, Band Pass, Notch, Comb +, Comb -, Vowel, Phaser, Sample & Hold | Low Pass | The filter's type, the same nine the [Synth](synth.md) and the [Filter](filter.md) have. |
| Cutoff | 20 Hz to 20 kHz | 5 kHz | The filter's frequency. |
| Resonance | 0 to 100 % | 10 % | How much the filter rings at its cutoff. |
| Filter Slope | 12 dB, 24 dB | 24 dB | The slope per octave. |
| Filter Envelope | -48 to +48 semitones | 0 semitones | How far the filter's envelope opens the cutoff, or closes it below 0. At 0 the envelope does nothing. |
| Key Tracking | 0 to 100 % | 50 % | How much the cutoff follows the note. The editor's label is Key. |
| Filter Velocity | 0 to 100 % | 0 % | How far a softer note's cutoff falls: at 100 %, four octaves at velocity 0. |

### Filter Envelope

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter Attack | 0 ms to 10 s | 1 ms | The envelope's attack. Key Time shortens the filter's envelope with the operators'. |
| Filter Decay | 0 ms to 10 s | 400 ms | Its decay. |
| Filter Sustain | 0 to 100 % | 20 % | Its sustain. |
| Filter Release | 0 ms to 10 s | 300 ms | Its release. |
| Filter Attack Curve | -100 to +100 % | -15 % | How the attack bends, as an operator's. |
| Filter Decay Curve | -100 to +100 % | -46 % | How the decay bends. |
| Filter Release Curve | -100 to +100 % | -46 % | How the release bends. |

### LFO

The LFO is switched by the switch at its title (LFO On).

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| LFO On | Off, On | Off | Switches the LFO on or off. |
| LFO Shape | Sine, Triangle, Saw Up, Saw Down, Square, Random | Sine | The LFO's wave. |
| LFO Target | Pitch, Timbre, Level, Cutoff | Pitch | What it moves: the pitch (up to a semitone either way), Timbre, the level (tremolo) or the cutoff (up to two octaves either way). |
| LFO Rate | 0.05 to 20 Hz | 5 Hz | Its speed. |
| LFO Depth | 0 to 100 % | 25 % | How far it moves its target. The mod wheel adds to it. |

## How it works

Each operator is a phase-modulated oscillator, as in every FM synth since the DX7: the operators that modulate it add
to its phase. Operators are only ever modulated by higher-numbered ones, so a voice works them out from operator 4 down.
The eight algorithms are Yamaha's 4-operator ones.

A modulator's depth is the square of its Level, up to 4π radians, and a carrier's gain is the square of its Level, so
that the knob turns evenly to the ear. Feedback at 100 % is 2π radians for what the operator puts out at its full
level, and it is averaged over the operator's last two samples so that it doesn't hunt. Negative feedback feeds back
the wave squared, which is the same in both halves of the cycle and so leaves only odd harmonics.

The operators run at twice the sample rate and come back down through polyphase IIR half-band filters
(`src/devices/dsp/Oversampling.h`), always, live and in renders alike, so that the sidebands FM makes above hearing
don't fold back down as aliasing. An operator whose frequency is above what that rate can play is silent. The saw and
triangle are read from band-limited tables, and the square is two such saws half a cycle apart. What is heard is the
carriers' sum, turned down by the square root of their number and by 5 dB more, so that changing the algorithm keeps
the level and chords stay under full scale. A DC blocker at 5 Hz then takes away the offset that feedback and
equal-ratio modulators leave.

After the operators come the filter (`src/devices/dsp/MultimodeFilter.h`, with its own envelope, key tracking and
velocity, named and ranged as the [Synth](synth.md)'s), and the output level. With Spread above 0 a voice plays two
complete sets of operators, detuned apart, one on each side, at twice the work.

Pitch, operator levels, the filter and the LFO are updated every 16 samples. An operator's level follows the
square of a note's velocity, as far as its Velocity says, and what velocity and the key make of it is fixed as the
note starts.

Poly mode has 16 voices. Every note takes a voice of its own, a key played again too, so that what still rings of it
rings out over its release. With all voices busy, a new note takes the one that has rung out longest, or else the
oldest, which fades out over 3 ms. Mono mode
plays one voice, legato: it glides between held keys without starting the envelopes again and goes back to a key still
held when the newest is let go. The piano roll's slide and portamento notes glide in both modes. The sustain pedal holds
notes, and the mod wheel adds to the LFO's depth.

## Its editor

`src/ui/devices/FmEditor.h`. The algorithm and the operators are one part, Operators; a dock narrower than all the
parts shows them in pages (see [Devices](../devices.md) for the frame every editor shares).

- **Algorithm**, down the left: its name between two arrows that step through the eight (the arrow keys too), and its
  picture drawn large. Each operator is a box with its number: the carriers in the device's colour along the line to
  the output, the modulators in modulation's colour over what they modulate, an operator that is fed back with a loop
  at its side. The boxes glow with the newest note's envelopes. A click on the picture shows all eight algorithms, each
  with what it is good for; a click on one plays it, as one undo step.
- **Harmonics**, under the picture: a bar for each half harmonic up to the 16th of what the operators make together,
  as a note strikes or as the newest note plays. Under them, Timbre.
- **Operators**, four columns side by side. Each is headed by its switch, its number in its role's colour and what it
  does in this algorithm ("Heard", or what it modulates, "→ 1"). Then its envelope, whose points drag and whose stages
  bend, its Level, and its Ratio and Fine (or its frequency while it is fixed). A column shows faint while its operator
  is off, and the operator under the pointer lights its box in the picture.
- **Tabs** under the columns show one group of all four operators' remaining controls at a time: Envelope (the times,
  sustain and bends), Tone (wave, feedback and phase, the phase faint while the phases run free), Pitch (Fixed and
  Detune) and Scaling (Velocity and Key).
- Then **Pitch** over **Voice**, the **Filter** with its response drawn and its **Filter Envelope** beside it (faint
  while the filter is off), and the **LFO** with its wave drawn.

Each operator control's tooltip says what it does in the operator's present role: a carrier's Level is how loud it
plays, a modulator's how bright it makes what it bends.

## Limits

- Four operators and eight fixed algorithms: no six-operator sounds, no import of DX7 or other FM patches, no routing
  matrix and no algorithms of the user's own. FM8, Dexed and Sytrus have six operators and 32 algorithms or a matrix.
- Envelopes are ADSRs whose stages bend: no multi-stage or looping envelopes, no DX-style rates and levels.
- Key scaling is one slope around middle C: no breakpoint and no separate curves below and above it.
- One LFO for the whole synth, free-running, with one target: no tempo sync, no restart at each note, no amount for each
  operator, and at most a semitone of pitch.
- Expression is the pitch wheel, the mod wheel (LFO depth) and the sustain pedal: no aftertouch, no MPE, no modulation
  matrix inside the synth. The track's [modulators](../automation-and-modulation.md) reach every parameter, but not per
  voice.
- Unison is Spread's two voices only; operators have no pan of their own, and waves can't be drawn.
- Pitch, operator levels and the cutoff are set once every 16 samples and not smoothed between, so fast modulation
  moves in steps.
- Oversampling is fixed at twice the rate: there is no higher quality setting for very bright sounds.
- The filter is linear: it doesn't self-oscillate and has no drive.
- When several notes each take a busy voice on the same sample, only the last taken fades out; the others stop at once
  and can click.
- An operator's settings can't be copied onto another, and there is no randomizer.
- The editor shows three of the four groups of operator controls only behind their tabs.

## In the code

- DSP: `src/devices/fm/FmSynth.h`, `src/devices/fm/FmSynth.cpp`. Shared: `src/devices/dsp/Oversampling.h`,
  `src/devices/dsp/MultimodeFilter.h`, `src/devices/dsp/Envelopes.h`, `src/devices/dsp/Oscillators.h`,
  `src/devices/wavetable/Wavetable.h` (the band-limited tables).
- Editor: `src/ui/devices/FmEditor.h`, `src/ui/devices/FmEditor.cpp`, with the envelope, filter and LFO views of
  `src/ui/devices/SynthParts.h`.
- Presets: `resources/presets/FM/`, described in [presets.md](../../presets.md).
- Tests: `tests/FmSynthTests.cpp`, `tests/FmEditorTests.cpp`, `tests/FactoryPresetTests.cpp`; the parameters in the
  test vector `tests/golden/format/devices.json`.
