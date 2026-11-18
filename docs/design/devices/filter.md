# Filter

*Design section 9.*

A resonant filter to sweep a sound open or closed, with a drive. Besides the low-pass, high-pass, band-pass and notch it
has two combs, a vowel, a phaser and a sample and hold. Its Character is Clean, a linear filter after a drive that gives
back the gain it adds, or Driven, whose drive pushes the sound into the filter's loop, where the resonance saturates and,
at the top of the Resonance, the filter sings on its own. The [Synth](synth.md), the [FM](fm.md) synth and the
[Sampler](sampler.md) use the same filter types, from the same code, so each type sounds the same in all four (Clean: the
instruments have no Character yet).

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Type | Low Pass, High Pass, Band Pass, Notch, Comb +, Comb -, Vowel, Phaser, Sample & Hold | Low Pass | The filter's type. |
| Cutoff | 20 Hz to 20 kHz | 20 kHz | The filter's frequency: the corner of a low-pass or high-pass, the centre of a band-pass or notch, the pitch of a comb's peaks, a vowel's second formant, a phaser's middle notch, half the rate a sample and hold holds at. |
| Resonance | 0 to 100 % | 10 % | Sharpens the filter at its cutoff: a peak there, a narrower band or notch, a comb that rings, narrower formants, a phaser's and a held sound's feedback. Driven, the top of its range makes the filter sing at its cutoff on its own. |
| Slope | 12 dB, 24 dB | 24 dB | The slope per octave of Low Pass, High Pass, Band Pass and Notch. The other types have no slope. |
| Character | Clean, Driven | Clean | Clean: a linear filter after the drive. Driven: the drive in the filter's loop (Low Pass, High Pass, Band Pass and Notch; the other types play Clean). |
| Drive | 0 to 24 dB | 0 dB | Clean: saturates what goes into the filter, quiet sound keeping its level. Driven: how hard the sound is pushed into the loop, half of it given back. |
| Quality | 2x, 4x, 8x | 2x | The rate the drive and the Driven filter play at, as a multiple of the sound's. |
| Mix | 0 to 100 % | 100 % | How much of what comes out is the filter's sound; applied by the device's runner, not by the filter ([devices.md](../devices.md)). |

A new filter changes little: a low-pass open all the way.

## How it works

The sound goes through the drive, then the filter, each channel on its own.

**Low Pass, High Pass, Band Pass and Notch** are state-variable sections integrated with the trapezoidal rule, which
stay stable while the cutoff moves fast. At 12 dB there is one section, its Q rising with Resonance from a
Butterworth's 0.71, flat, to 16. At 24 dB a low-pass or high-pass is the two sections of a fourth-order Butterworth,
3 dB down at the cutoff, the second carrying the resonance; a band-pass or notch is two sections alike, narrower and
deeper.

**Comb + and Comb -** are a short delay fed back into itself and added to what goes in. Comb +'s delay is one period
of the cutoff: it lets through the cutoff and its multiples and cuts what lies halfway between. Comb - is half as long
and fed back with its sign turned: it lets through the cutoff's odd multiples only, hollow as a square wave, at the
same pitch. With no resonance a comb is a gentle row of notches; Resonance feeds back up to 98 %, each step of the
knob dividing what the loop loses by the same factor, until the comb rings at its pitch (about 0.8 s at 440 Hz). As
the peaks narrow they rise by up to 8.5 dB while broadband sound falls as much, so the sound neither vanishes nor
jumps out. The delay is read between samples by four-point Lagrange interpolation, whose gain never exceeds 1, and
glides to a new length, so a sweep neither clicks nor makes the loop unstable.

**Vowel** is three band-passes side by side at the first three formants of a sung vowel, at a tenor's frequencies,
levels and widths (the Csound manual's table of formant values). The cutoff is where the second formant is: from U
(600 Hz) through O, A and E to I (1870 Hz) the formants glide from vowel to vowel, and beyond U or I they move with
the cutoff. Resonance narrows the formants from twice the table's width to half of it; a gain after them, larger as
they narrow, keeps the level.

**Phaser** is six first-order all-pass stages mixed with what goes in. Its three notches sit at about a quarter of the
cutoff, at the cutoff and at nearly four times it. Resonance feeds the stages back, up to 85 %, which raises the peaks
between the notches; the feedback loop is solved within each sample.

**Sample & Hold** holds the sound at twice the cutoff's rate, so that what lies above the cutoff folds back below it,
aliasing on purpose. Resonance feeds the held sample back with its sign turned, up to 60 %, which rings at the cutoff.

**The cutoff glides** in octaves, over about 10 ms, so automation, modulators and a turned knob sweep it evenly; the
filter takes new coefficients every 8 samples while it glides. Switching to another kind of type starts the filter
from silence.

**The Clean drive** multiplies the sound by its gain, soft-clips it and divides by the gain again, at the Quality's
rate (2, 4 or 8 times the sample rate) through JUCE's polyphase IIR half-band oversampler. The curve is played by
first-order antiderivative anti-aliasing (Parker, Zavalishin and Le Bivic; Bilbao, Esqueda, Parker and Välimäki), with a
short linear-phase filter that makes up the top octave the averaging dulls ([dsp-audit.md](../../archive/dsp-audit.md),
section 3.4). Undriven, the sound takes a plain delay as long instead; the two are crossfaded over 64 samples as the
drive comes on or goes off. Then the filter, at the sound's rate.

**Driven** (Low Pass, High Pass, Band Pass and Notch) plays the state-variable section with a curve inside its loop, as
Vadim Zavalishin's "The Art of VA Filter Design" builds the nonlinear state-variable filter: the band-pass state's
feedback, the damping, is a Butterworth's less the resonance's share, and that share passes through the soft clip, so
that a loud sound tames the peak and the resonance can't grow past the curve's ceiling. The loop is solved within each
sample, never a sample late: a guess from the line that touches the curve where the last sample's last step was taken,
then two Newton steps (the loop's equation rises steadily with the band-pass state, so they converge from any guess).
That is five divisions (six for a high-pass, a band-pass or a notch, whose output takes the curve once more) and about forty
multiplications and additions per channel per sample at its rate, against Clean's ten multiplications and additions
at the sound's rate: at 2x some ten times Clean's filter, besides the half-band filters. At 24 dB a
linear section goes before the loop, as Clean's first section.

- **Resonance** up to 90 % gives Clean's Q (a patch switched to Driven with little drive keeps its shape: for a quiet
  sound the two are within 0.5 dB wherever Clean is within 12 dB of full, and within 1.5 dB down to 24 dB under, where
  Clean's bilinear transform at the sound's rate squeezes its far skirt and Driven's, at its Quality's rate, keeps the
  analog shape). From 90 % to 100 % the damping falls in a straight line to a little under nothing, crossing it at about
  98 %: there the filter sings at its cutoff on its own, its band-pass and low-pass states at 0.25, where the curve's
  bend balances the loss: -12 dBFS at a Drive of 0 dB, and lower as the Drive rises, whose half given back turns the
  loop's output down (-18 dBFS at 12 dB, -24 dBFS at 24 dB). It sings in tune from 50 Hz to 5 kHz at every Quality, at
  44.1 and 96 kHz: the tests measure 0.04 cents flat and hold it within 5 cents. A loop set to sing that is at rest takes
  a kick 80 dB under full scale, as an analog filter starts from its own noise, so that it sings from silence too (in
  about half a second at 440 Hz).
- **The band-pass** puts out the loop's damping term and the rest of Clean's gain on its state, so that at its cutoff it
  follows what comes in at any level: where a loud sound saturates the damping, the damping term still carries it. A
  tone at the cutoff at Drive 0 comes out as loud as through Clean at 50 and 85 % whether at -40, -12 or -6 dBFS (within
  0.01 dB). At 95 % the Driven damping has left Clean's on its way to singing, so a quiet tone there comes out 6.9 dB
  louder than through Clean, and the louder the tone the nearer Clean (5.4 dB louder at -40 dBFS, 1.2 dB at -12, 0.8 dB
  at -6). The Drive makes it louder: a -12 dBFS tone at the cutoff 8.9 dB louder at 18 dB. The notch, what comes in less
  the same damping term, passes a tone an octave from its cutoff at the same gain at -40 and at -12 dBFS (within 0.003 dB
  at 90 %).
- **Drive** multiplies the sound into the loop and gives half of it back after, in decibels, so a quiet sound comes out
  louder as it is driven: as loud at 0 dB, 6 dB louder at 12 dB and 12 dB louder at 24 dB. What comes out passes a
  ceiling at full scale that is a straight line up to half of it (-6 dBFS) and rounds off from there to full scale,
  0.5 + 0.5 tanh (2 (|x| - 0.5)), its slope continuous at the knee (`dsp::KneeCurve`, played by its antiderivative). So
  at no Drive the filter leaves a sound under -6 dBFS as Clean plays it: a sine at -7 dBFS in the pass band within 0.01
  dB of Clean, its harmonics more than 150 dB under full scale. A loud sound comes out thicker: a sine at full scale
  (open, 1 kHz) peaks at -1.1 dBFS at 0 dB, rounded, at full scale at 12 dB and at +0.15 dBFS at 24 dB, nearly a square,
  which the half-band filters back to the sound's rate overshoot a little (+0.36 dBFS as it starts). Its harmonics are
  odd (the third first): on a sine at -12 dBFS, none at 0 and 12 dB (it comes out at -12 and -6 dBFS, under the knee),
  the third 49 dB under full scale at 18 dB and 28 dB under at 24 dB.
- **The Driven filter plays at the Quality's rate**, through the same half-band filters as the drive, its coefficients
  worked out for that rate, and the ceiling's curve by its antiderivative.

**The Quality**: every rate's filters are made ready as the device is, and every Quality comes 7 samples late (8x's
latency, which the others wait for), so that automation can move it anywhere while the song plays. Clean at 2x plays
sample for sample what it played before Character and Quality came, a sample later (Sample & Hold excepted, whose holds,
counted from its start, then fall on other samples of the sound: as loud).

**A change of Character or Quality** starts the new way unheard, from rest (a Driven filter taking the loop of the Driven
filter that plays, where there is one, so that a filter that sings goes on singing), lets it settle (5 ms, and four
cycles of the cutoff up to 100 ms), and crossfades to it over 10 ms; a change meanwhile waits for that to end. The Clean
filter plays all the while, on the plain delay while no Clean way does, so that it is ready. A filter that sings, switched
to Clean, fades out with no click: Clean's Q is bounded, and it hears what comes in, not the singing.

**The aliases**, the loudest under a tone at half of full scale driven 24 dB, at 2.5, 5 and 12 kHz, at 44.1 and 48 kHz
(the worst of them, held by `tests/FilterTests.cpp`):

| Quality | Clean, Drive 24 dB | Driven, Drive 24 dB, Resonance 50 % |
| --- | --- | --- |
| 2x | 34.3 dB under | 45.9 dB under |
| 4x | 60.1 dB under | 63.0 dB under |
| 8x | 87.4 dB under | 99.5 dB under |

The worst is always the 12 kHz tone; at 2.5 kHz Clean is 73 dB under at 2x.

**The dry copy** that Mix blends in is delayed as the sound is. While the sound goes through the drive or the Driven
filter and Mix is below 100 %, the copy goes through the same half-band filters with a straight line in the curve's
place, so that the two meet in phase at any mix.

**The tail.** The filter reports how long a comb rings, and how long the Driven filter's ring takes to fall 70 dB, so that
a render waits for it and its runner doesn't let it sleep sooner; where it sings, 60 s, the longest it reports. A render
of a filter that sings to the song's end goes on for the export's own longest tail, 10 s, and is cut there. A Driven
filter never sleeps on silence, while its last block played Driven: it can start to sing by itself as its Resonance rises,
which a filter asleep wouldn't hear, so it plays its loop on a silent track too, which is the price of a filter that can
sing on its own. A Clean filter sleeps on silence as other devices do, through any lane or modulator on it, but wakes when its values
make it Driven (a lane that switches its Character at a Resonance that sings), as a render, which never sleeps, plays it. Switched off, it isn't
played at all. Clean's other
types fall silent within a few milliseconds.

## Its editor

Two columns, built on the frame every device editor shares ([devices.md](../devices.md)).

- **Response**: the filter's response drawn over the spectrum of what it plays, with what comes in fainter behind it,
  so each type looks like itself: a slope, a peak, a notch, a comb's teeth, a vowel's formants or a phaser's notches.
  Sample & Hold, which is not a linear filter, draws as a flat line. Driven draws its response to a quiet sound, the
  curve straight, whose peak goes off the top where it sings. A dot sits at the cutoff, as high as the
  resonance: dragging it moves both, as one undo step, and a press anywhere puts it under the pointer. The wheel over
  the graph turns the drive, a decibel a notch. The graph names the type, "Driven" while it is, the cutoff and the
  resonance, and the drive while there is any.
- **Sweep** (Cutoff, large, and Resonance) over **Character** (Clean and Driven as two buttons, over Type, Slope and
  Drive) and **Quality**, a list.

Mix is not in the editor: it is with the device in the Devices view ([devices.md](../devices.md)).

## Limits

- No ladder, diode or Sallen-Key model as such: Driven is the state-variable filter with its resonance saturating.
- Driven only for Low Pass, High Pass, Band Pass and Notch; the combs, the vowel, the phaser and the sample and hold play
  Clean. A notch that sings is silent at its cutoff, as a notch is, and a band-pass sings quietly.
- A full-scale sound driven hard comes out a little past 0 dBFS (+0.15 dBFS for a sine at 24 dB, +0.36 as it starts):
  the ceiling holds full scale at the Quality's rate, and the half-band filters back to the sound's rate overshoot it.
- Clean's Q stops at 16 at 12 dB and at about 21 at 24 dB: Clean doesn't self-oscillate.
- Slopes of 12 and 24 dB per octave only.
- The synths' filters (the [Synth](synth.md)'s, the FM synth's and the sampler's) are
  still Clean: Driven lives in their shared filter, for them to take later.
- No LFO or envelope follower of its own: a track's modulators
  ([automation-and-modulation.md](../automation-and-modulation.md)) move its cutoff.
- No stereo offset between the two channels' cutoffs, and no mid/side mode.
- Not built: a filter that morphs between types, a peak type (the [EQ](eq.md)'s bell does that) and a DJ-style
  filter that is a low-pass one way and a high-pass the other.

## In the code

- DSP: `src/devices/filter/FilterEffect.h`, `FilterEffect.cpp`; the types, and Driven's loop (`dsp::DrivenCoefficients`,
  `MultimodeFilter::processDriven()`), in `src/devices/dsp/MultimodeFilter.h` and `MultimodeFilter.cpp`; the sections in
  `src/devices/dsp/Filters.h`; the combs in `src/devices/dsp/CombFilter.h`; the drive's curves in
  `src/devices/dsp/Saturation.h`.
- Editor: `src/ui/devices/FilterEditor.h`, `FilterEditor.cpp`; the dot in `src/ui/devices/GraphDot.h`.
- Presets: `resources/presets/Filter/` ([presets.md](../../presets.md)).
- Tests: `tests/FilterTests.cpp` (the device: its types, Clean against the old device's code, Driven's response, singing,
  drive, the band-pass's and the notch's level, the ceiling's curve, the loop's bounds and the device's stability, switches,
  its aliases and latency at each Quality, blocks, and sleeping), `tests/DspTests.cpp`
  (the sections, the combs and each type), `tests/AntialiasingTests.cpp` (the drive at 2x), `tests/EffectEditorTests.cpp`
  (the editor), `tests/RealtimeAuditTests.cpp` (guarded scenarios), and `tests/GoldenMixerTests.cpp` with
  `tests/golden/renders/effect-filter.json`.
