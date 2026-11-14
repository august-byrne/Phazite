# Filter

*Design section 9.*

A resonant filter to sweep a sound open or closed, with a drive that saturates what goes into it. Besides the low-pass,
high-pass, band-pass and notch it has two combs, a vowel, a phaser and a sample and hold. The [Synth](synth.md) and the
[Wavetable](wavetable.md) synth use the same filter types, from the same code, so each type sounds the same in all
three.

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Type | Low Pass, High Pass, Band Pass, Notch, Comb +, Comb -, Vowel, Phaser, Sample & Hold | Low Pass | The filter's type. |
| Cutoff | 20 Hz to 20 kHz | 20 kHz | The filter's frequency: the corner of a low-pass or high-pass, the centre of a band-pass or notch, the pitch of a comb's peaks, a vowel's second formant, a phaser's middle notch, half the rate a sample and hold holds at. |
| Resonance | 0 to 100 % | 10 % | Sharpens the filter at its cutoff: a peak there, a narrower band or notch, a comb that rings, narrower formants, a phaser's and a held sound's feedback. |
| Slope | 12 dB, 24 dB | 24 dB | The slope per octave of Low Pass, High Pass, Band Pass and Notch. The other types have no slope. |
| Drive | 0 to 24 dB | 0 dB | Saturates what goes into the filter; quiet sound keeps its level. |
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

**The drive** multiplies the sound by its gain, soft-clips it and divides by the gain again, at twice the sample rate
through JUCE's polyphase IIR half-band oversampler. The curve is played by first-order antiderivative anti-aliasing
(Parker, Zavalishin and Le Bivic; Bilbao, Esqueda, Parker and Välimäki), with a short linear-phase filter that makes
up the top octave the averaging dulls ([dsp-audit.md](../../archive/dsp-audit.md), section 3.4). Undriven, the sound takes a
plain delay as long instead, so the filter's latency, which it reports, stays the same; the two are crossfaded over
64 samples as the drive comes on or goes off.

**The dry copy** that Mix blends in is delayed as the sound is. While the sound goes through the drive and Mix is
below 100 %, the copy goes through the same half-band filters with a straight line in the curve's place, so that the
two meet in phase at any mix.

**The tail.** The filter reports how long a comb rings, so that a render waits for it. The other types fall silent
within a few milliseconds.

## Its editor

Two columns, built on the frame every device editor shares ([devices.md](../devices.md)).

- **Response**: the filter's response drawn over the spectrum of what it plays, with what comes in fainter behind it,
  so each type looks like itself: a slope, a peak, a notch, a comb's teeth, a vowel's formants or a phaser's notches.
  Sample & Hold, which is not a linear filter, draws as a flat line. A dot sits at the cutoff, as high as the
  resonance: dragging it moves both, as one undo step, and a press anywhere puts it under the pointer. The wheel over
  the graph turns the drive, a decibel a notch. The graph names the type, the cutoff and the resonance, and the drive
  while there is any.
- **Sweep** (Cutoff, large, and Resonance) over **Character** (Type, Slope and Drive).

Mix is not in the editor: it is with the device in the Devices view ([devices.md](../devices.md)).

## Limits

- No circuit models: the filter is clean and linear, with no ladder, diode or other analog character, and resonance
  never saturates.
- No self-oscillation: the resonant section's Q stops at 16 at 12 dB and at about 21 at 24 dB.
- The drive sits before the filter, not in its loop, and gives back the gain it adds, so a loud sound driven hard comes
  out quieter rather than louder.
- Slopes of 12 and 24 dB per octave only.
- No LFO or envelope follower of its own: a track's modulators
  ([automation-and-modulation.md](../automation-and-modulation.md)) move its cutoff.
- No stereo offset between the two channels' cutoffs, and no mid/side mode.
- The drive is oversampled twice, with no higher quality setting, also in a render.
- Not built: a filter that morphs between types, a peak type (the [EQ](eq.md)'s bell does that) and a DJ-style
  filter that is a low-pass one way and a high-pass the other.

## In the code

- DSP: `src/devices/filter/FilterEffect.h`, `FilterEffect.cpp`; the types in `src/devices/dsp/MultimodeFilter.h` and
  `MultimodeFilter.cpp`; the sections in `src/devices/dsp/Filters.h`; the combs in `src/devices/dsp/CombFilter.h`; the
  drive's curves in `src/devices/dsp/Saturation.h`.
- Editor: `src/ui/devices/FilterEditor.h`, `FilterEditor.cpp`; the dot in `src/ui/devices/GraphDot.h`.
- Presets: `resources/presets/Filter/` ([presets.md](../../presets.md)).
- Tests: `tests/EffectDeviceTests.cpp` (the device, its types and its mix), `tests/DspTests.cpp` (the sections, the
  combs and each type), `tests/AntialiasingTests.cpp` (the drive), `tests/EffectEditorTests.cpp` (the editor),
  `tests/RealtimeAuditTests.cpp` (guarded scenarios), and `tests/GoldenMixerTests.cpp` with
  `tests/golden/renders/effect-filter.json`.
