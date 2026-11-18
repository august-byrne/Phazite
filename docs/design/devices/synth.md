# Synth

*Design section 9.*

A subtractive synth with 16 voices, for basses, leads, pads, plucks and synthesized drums. Each voice mixes two
band-limited oscillators, a sub and noise, sends them through a resonant filter with an envelope of its own, then
through an amp envelope; one LFO, a pitch envelope, unison and drift move the sound.

## Parameters

The sub-headings are the editor's parts. A part with a switch (Filter, Filter Envelope, LFO, Unison) does nothing while
it is off. Inside a part the editor shows shorter labels than the names below, given under each heading.

### Oscillators

In the editor: Osc 1, Pitch 2, Mix, Sub, Osc 2, Detune 2, Width, Noise.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Osc 1 Wave | Sine, Triangle, Saw, Square | Saw | Oscillator 1's wave. |
| Osc 2 Wave | Sine, Triangle, Saw, Square | Saw | Oscillator 2's wave. |
| Osc 2 Pitch | -24 to +24 semitones | 0 semitones | Oscillator 2's pitch against oscillator 1's. |
| Osc 2 Detune | -100 to +100 cents | 8 cents | Oscillator 2's fine tuning against oscillator 1's. |
| Osc Mix | 0 to 100 % | 50 % | From oscillator 1 alone (0 %) to oscillator 2 alone (100 %). |
| Pulse Width | 5 to 95 % | 50 % | The width of both oscillators' Square wave; 50 % is a square. |
| Sub | 0 to 100 % | 0 % | The level of the sub oscillator, in the middle of the stereo field. |
| Noise | 0 to 100 % | 0 % | The level of white noise, in the middle of the stereo field. |

### Sync and Sub

In the editor: Sync 2, Env 2, Sub Octave, Sub Shape.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Osc 2 Sync | Off, On | Off | Hard sync: oscillator 2 starts its cycle again with each of oscillator 1's. |
| Osc 2 Envelope | -48 to +48 semitones | 0 semitones | How far the filter envelope moves oscillator 2's pitch: with Sync, the sync sweep. Nothing while the Filter Envelope is off. |
| Sub Octave | -1 Octave, -2 Octaves | -1 Octave | How far below oscillator 1 the sub plays. |
| Sub Shape | Sine, Triangle, Saw, Square | Sine | The sub's wave. |

### Pitch

In the editor: Envelope, Decay, Drift.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pitch Amount | -48 to +48 semitones | 0 semitones | The pitch envelope: how far above (or below) its pitch each note starts. |
| Pitch Decay | 1 ms to 2 s | 60 ms | How fast the note falls onto its pitch from there. |
| Drift | 0 to 100 % | 0 % | How far each voice's oscillators and cutoff wander: at 100 %, each oscillator up to 8 cents and the cutoff up to 2 semitones. |

### Filter

In the editor: Type, Cutoff, Resonance, Key, Slope, Drive, Envelope, Velocity.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter On | Off, On | On | The part's switch. Off, the oscillators go to the amp as they are. |
| Filter Type | Low Pass, High Pass, Band Pass, Notch, Comb +, Comb -, Vowel, Phaser, Sample & Hold | Low Pass | The filter's type: those of the [Filter](filter.md) device. |
| Cutoff | 20 Hz to 20 kHz | 3 kHz | The filter's frequency before the envelope, the keys, the velocity, the LFO and the drift move it. |
| Resonance | 0 to 100 % | 15 % | Sharpens the filter at its cutoff (for each type, as on the [Filter](filter.md) page). |
| Filter Slope | 12 dB, 24 dB | 12 dB | The slope per octave of Low Pass, High Pass, Band Pass and Notch. |
| Filter Drive | 0 to 24 dB | 0 dB | Saturates what goes into the filter. |
| Filter Envelope | -48 to +48 semitones | 24 semitones | How far the filter envelope moves the cutoff at its top. |
| Key Tracking | 0 to 100 % | 50 % | How far the cutoff follows the key, around middle C: at 100 %, a semitone for each semitone. |
| Filter Velocity | 0 to 100 % | 0 % | How far a softer note's cutoff falls: at 100 %, four octaves at velocity 0. |

### Filter Envelope

In the editor: Attack, Decay, Sustain, Release, A Curve, D Curve, R Curve.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter Envelope On | Off, On | On | The part's switch. Off, the envelope moves neither the cutoff nor oscillator 2's pitch. |
| Filter Attack | 0 ms to 10 s | 1 ms | The time to the top. |
| Filter Decay | 0 ms to 10 s | 400 ms | The time from the top to the sustain level. |
| Filter Sustain | 0 to 100 % | 20 % | The level held while the key is down. |
| Filter Release | 0 ms to 10 s | 300 ms | The time to nothing once the key is up. |
| Filter Attack Curve | -100 to +100 % | -15 % | How the attack bends: below 0 fast at first, 0 straight, above 0 slow at first. |
| Filter Decay Curve | -100 to +100 % | -46 % | How the decay bends, likewise. |
| Filter Release Curve | -100 to +100 % | -46 % | How the release bends, likewise. |

### Amp

In the editor: Attack, Decay, Sustain, Release, A Curve, D Curve, R Curve.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Attack | 0.5 ms to 10 s | 2 ms | The time to full level. Never instant, so that a note doesn't click. |
| Decay | 0 ms to 10 s | 300 ms | The time from full level to the sustain level. |
| Sustain | 0 to 100 % | 80 % | The level held while the key is down. |
| Release | 0.5 ms to 10 s | 200 ms | The time to silence once the key is up. It is also the tail the synth reports. |
| Attack Curve | -100 to +100 % | -15 % | How the attack bends, as the filter envelope's curves do. |
| Decay Curve | -100 to +100 % | -46 % | How the decay bends. |
| Release Curve | -100 to +100 % | -46 % | How the release bends. |

### LFO

In the editor: Shape, Rate, Depth, Target, Fade In.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| LFO On | Off, On | Off | The part's switch. Off, the mod wheel does nothing either. |
| LFO Shape | Sine, Triangle, Saw Up, Saw Down, Square, Random | Sine | The LFO's wave. Random holds a new value each cycle. |
| LFO Target | Pitch, Cutoff, Level, Pulse Width | Pitch | What the LFO moves: at full depth the pitch by a semitone either way, the cutoff by two octaves either way, the level down to silence, or the pulse width by 45 % either way. |
| LFO Rate | 0.05 to 20 Hz | 5 Hz | The LFO's speed. |
| LFO Depth | 0 to 100 % | 25 % | How far the LFO moves its target. The mod wheel adds to it. |
| LFO Fade In | 0 ms to 10 s | 0 ms | How long each note's LFO takes to come in, from nothing to its depth. |

### Unison

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Unison On | Off, On | Off | The part's switch. |
| Unison | 1 to 7 | 3 | How many copies of each oscillator a voice plays at once. |
| Unison Detune | 0 to 100 cents | 25 cents | How far the outermost copies are tuned from the note, the others evenly between them. |
| Unison Spread | 0 to 100 % | 80 % | How far the copies spread across the stereo field. |

### Voice

In the editor: Mode, Glide, Glide Mode, Bend, Velocity, Level.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Voices | Poly, Mono | Poly | Sixteen voices, or one that plays legato. |
| Glide | 0 ms to 2 s | 0 ms | How long a note takes to glide from the one before; 0 is none. |
| Glide Mode | Always, Legato | Always | Whether every note glides, or only one played while another is held. |
| Bend Range | 0 to 24 semitones | 2 semitones | How far the pitch wheel bends, either way. |
| Velocity | 0 to 100 % | 60 % | How much of the level depends on the note's velocity, which counts squared. |
| Level | -36 to +6 dB | -6 dB | The synth's output level. |

## How it works

**Oscillators.** The saw and the triangle are read from band-limited tables, a copy each half octave that keeps only the
harmonics whose aliases stay above 20 kHz, sampled finely enough to be read by straight lines
([dsp-audit.md](../../archive/dsp-audit.md), section 3.2). The square is one such saw taken from another a pulse width behind
it, so its width can move and it carries no constant offset at any width. The sine comes from a polynomial. With Sync
on, oscillator 2 is read at a place that oscillator 1's cycle gives, and its jump back to its start is smoothed by a
two-point PolyBLEP; the part of its last cycle that each of oscillator 1's cuts short isn't centred, so what it adds up to over
oscillator 1's cycle is worked out from its wave and taken away, and a synced oscillator carries no constant offset either.
The sub runs at a half or a quarter of oscillator 1's frequency.

**Unison.** Each oscillator plays up to seven copies, tuned evenly from one side of the detune to the other and panned
evenly across the spread at equal power, each at a level that keeps the voice as loud (one over the square root of
their number). The first copy starts each note at the start of its wave, the others at places drawn from a fixed seed.
With unison on and a stereo output, a voice runs two filters, one a side; the sub and the noise stay in the middle.

**Filter.** One of the nine types of `dsp::MultimodeFilter`, the code the [Filter](filter.md) device and the
[Wavetable](wavetable.md) synth share, so a type sounds the same in all three. The synth turns Resonance into a 12 dB
section's Q by a curve of its own, from 0.5 to 25. The cutoff a voice plays is Cutoff moved, in semitones, by the
filter envelope, the key, the velocity, the LFO and the drift, and kept between 20 Hz and 0.45 of the sample rate.
Filter Drive soft-clips what goes into the filter at twice the sample rate, the curve played by its antiderivative so
that what it adds doesn't fold back as aliasing; it fades in and out over two thirds of a millisecond, at any rate.

**Envelopes.** Each stage takes exactly its time along its curve and lands on its target; a note that starts or ends in
the middle of a stage goes on from the level it has reached.

**Control rate.** Pitch, the filter's coefficients, the drift and the LFO's moves on pitch and cutoff are worked out every
16 samples, on the song's own grid while it plays (a step at every multiple of 16 samples from the song's start) and on
a running count while it is stopped (`src/devices/dsp/ControlGrid.h`), as the [Wavetable](wavetable.md), the FM and the
Sampler do: a note that starts between two steps gets its values for what is left of the step, and where the song's
place leaves the count (playback starting, a jump, a loop's wrap, the device waking from sleep) a step begins there. So
what it plays doesn't depend on how the samples come in blocks, nor on how a passage was reached: after a count-in, in a
loop's second pass and after a sleep it plays what a render plays. The oscillators, the filter, the envelopes, tremolo
and pulse-width modulation run at every sample. The noise is random samples, as they are at 48 kHz, the rate the
presets are leveled at ([presets](../../presets.md#leveling)); at 44.1 kHz the square root of 44.1 / 48 quieter, so
that each hertz of the band holds as much as at 48 kHz; and from twice those rates, the same noise made at 44.1 or
48 kHz and raised to the rate by the drives' half-band filters, so that nothing of it lies above 24 kHz
(`src/devices/dsp/Noise.h`). So its level in the band that is heard, its peaks and what the drive does to it are alike
at every rate. The envelopes follow a new sustain level over the same time at any rate. A DC blocker on each side of the
synth's sum (12 dB at 5 Hz, `src/devices/dsp/DcBlocker.h`) takes away what the drive leaves off centre of a wave that is
centred but lopsided. It plays only while the drive is on: it changes the waveform of whatever it plays (it takes out a
note's net area and puts it back as a slow tail), so a clean patch, whose waves are centred as they are made, plays
without it, bit for bit. Taken on and let go in mid-note, it fades in and out over 5 ms; once playback has stopped and
no note sounds, what it still rings with fades out over 3 ms, so that nothing sounds on after Stop.

**Notes.** In Poly every note takes a voice of its own. A key played again while its last note rings out leaves that
tail ringing and starts a new voice; played again while its key or the sustain pedal still holds a voice, that voice
lets go first, so a key never holds two. With all 16 voices busy, a new note takes the voice that has been ringing out
longest, then the oldest; what the taken voice played fades out over 3 ms, from a copy of it in a place of its own (one
for each voice, so a chord that takes several at once fades each). In Mono one voice plays: a key pressed over
a held one moves the pitch without starting the envelopes again, and letting it go returns to the key still held. A
switch between Poly and Mono lets go of every note.

**Glides.** Glide moves a note's pitch from the last note's toward its own, in Poly as in Mono. A portamento note
glides in evenly over Glide's time (over 0.1 s while Glide is 0), and a slide note moves every held voice evenly to
its pitch over its length ([piano-roll.md](../piano-roll.md)).

**Controllers.** Pitch bend, the sustain pedal (controller 64) and the mod wheel (controller 1, which adds to the LFO's
depth) are the controllers it answers.

**The same every time.** Noise, the unison's starting phases and the drift come from fixed seeds, so a render repeats
exactly.

## Its editor

Seven columns as the sound goes through them, built on the frame every device editor shares
([devices.md](../devices.md)): Oscillators; Filter; Amp; Filter Envelope; LFO; Pitch over Sync and Sub; Unison over
Voice. A dock narrower than all of them shows them in pages, two or three whole parts to a page.

- **Oscillators** draws one cycle of what the two oscillators, the sub and the noise play together.
- **Filter** draws the filter's response, which follows the newest note's cutoff as its envelope and LFO move it. A dot
  sits at the cutoff, as high as the resonance: dragging it sets both, as one undo step.
- **Amp** and **Filter Envelope** draw their envelopes with a line at the newest note's level. The peak drags sideways
  for the attack, the corner where the decay ends sideways for the decay and up or down for the sustain, the end
  sideways for the release. A hollow handle in the middle of each stage that rises or falls drags up or down to bend
  it. A double-click puts a point or a curve back. Shift drags in fine steps.
- **LFO** draws one cycle of its wave, as tall as its depth, with a dot where it is: a drag up or down sets the depth,
  the wheel the rate.
- The four parts that switch on and off have their switch at the right end of their title; a part that is off is drawn
  faint.

## Limits

- One LFO for the whole synth, not one a voice: it runs free, has one target at a time, no tempo sync and no restart
  with each note, and moves the pitch by a semitone at the most.
- No modulation matrix, aftertouch or MPE: only velocity, pitch bend, the mod wheel and the sustain pedal reach it. A
  track's modulators ([automation-and-modulation.md](../automation-and-modulation.md)) move its knobs for the whole
  track, not for each note.
- The filter is linear state-variable sections: it never self-oscillates (its Q stops at 25), there is no ladder or
  diode model, and resonance doesn't saturate.
- Filter Drive sits before the filter, not in its loop, and gives back the gain it adds, so a loud voice driven hard
  comes out quieter, not louder.
- Hard sync is smoothed by a two-point PolyBLEP only and the oscillators aren't oversampled, so a synced oscillator
  aliases on high notes.
- Pitch and cutoff move in steps of 16 samples without interpolation: a fast LFO at full depth on a resonant filter can
  be heard stepping.
- Sixteen voices, fixed; oscillators always start each note at the start of their waves (no free-running phase); the
  noise is white only.
- A held note that is silent for longer than the release lets the synth sleep ([known bug 37](../../known-bugs.md)).
- No effects of its own, a chorus included: [Motion](motion.md)'s Chorus and Phazite's other devices follow it.

## In the code

- DSP: `src/devices/synth/Synth.h`, `Synth.cpp`; the oscillators and LFO in `src/devices/dsp/Oscillators.h`; the
  filter in `src/devices/dsp/MultimodeFilter.h`, `Filters.h` and `CombFilter.h`; the envelopes in
  `src/devices/dsp/Envelopes.h`; the drive in `src/devices/dsp/Saturation.h` and `Oversampling.h`.
- Editor: `src/ui/devices/SynthEditor.h`, `SynthEditor.cpp`; the envelope, LFO and filter pictures it shares with the
  other synths in `src/ui/devices/SynthParts.h`.
- Presets: `resources/presets/Synth/` ([presets.md](../../presets.md)).
- Tests: `tests/SynthTests.cpp`, `tests/SynthEditorTests.cpp`, `tests/GraphEditingTests.cpp` (the pictures' drags),
  `tests/AntialiasingTests.cpp` (the oscillators' aliases and the pulse's offset), `tests/DspTests.cpp` (the filter
  types), `tests/RealtimeAuditTests.cpp` (guarded scenarios), `tests/GoldenRenderTests.cpp` with
  `tests/golden/renders/synth.json`, and `tests/FactoryPresetTests.cpp`.
