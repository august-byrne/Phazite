# Exciter

*Design section 9.*

Adds the presence and air that an equalizer cannot give a dull sound, since an EQ only raises what is there: it takes
the highs above a frequency, makes harmonics of them, and adds them to the sound, which itself passes untouched.

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Frequency | 1 to 12 kHz | 5 kHz | Where the band that is excited starts. |
| Drive | 0 to +36 dB | +18 dB | How hard the band is pushed into the curve. The same gain comes back off after it, so Drive sets the level from which the band turns into harmonics, never the loudness. |
| Colour | 0 to 100 % | 50 % | Which harmonics the curve makes: odd ones alone at 0 % (the third first: edgy), even ones too as it is turned up (the second first: sweeter); at 100 % the second leads and a quiet sound's third is gone. |
| Amount | 0 to 400 % | 35 % | How much of the excited band is added. At 100 % it is as loud as the band came in, which lifts the top by 6 dB. |
| High Cut | 2 to 20 kHz | 20 kHz (off) | Where the band ends, for a top that mustn't fizz. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the exciter's sound; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

## How it works

The exciter works as the Aphex Aural Exciter does: a side chain of a high-pass and a harmonics generator, mixed back
in with the sound.

**The band.** A high-pass of 24 dB per octave (two Butterworth sections) at the Frequency keeps the notes'
fundamentals out of the curve; while High Cut is below 20 kHz, a low-pass of the same steepness ends the band.

**The curve.** The band is driven into a soft clip and the drive is taken back off. Colour moves the curve's working
point off its middle, as a tube's bias does, which makes the curve lopsided and so adds even harmonics. What the
curve leaves straight is added as it is, exactly in step; only what it bends goes through the curve, at four times
the sample rate (twice from 88.2 kHz on), played by its antiderivative (first-order antiderivative anti-aliasing, as
the [Saturator](saturator.md)'s curves are), through linear-phase half-band filters.

**In phase.** After the curve the band goes through the same filters again, played backwards: a filter of 2 ms whose
taps are the impulse response of the filters before the curve, reversed. The two together turn no phase at all and
pass the band with the square of one's gain, 48 dB per octave. Only what was made above the band is added (the
difference tones a curve makes below it stay out), and the added band sums with the sound in phase at every
frequency: it lifts the top as a shelf does and never cuts a dip beside the band, which a steep high-pass simply
added to the sound would.

**Smoothing.** Drive, Colour and Amount glide over about 20 ms and the band's edges over about 5 ms. High Cut fades
in and out as it turns on or off.

**Latency.** The sound waits for the 2 ms filter and for the curve: about 3.3 ms at 44.1 and 48 kHz. The engine
makes up for it on the other tracks. While Low Latency is on, the exciter is left out of what plays and the sound
passes as it comes.

**Listen** plays what is added alone, in place of the sound with it, whatever the mix, faded over 5 ms, while its
switch is on and the editor shows. It stops half a second after the editor last asked. It is not a parameter, so a
song cannot be saved with it on.

## Its editor

- **Excited band:** the spectrum of what comes in, pale, and of what goes out, in the device's colour, with what the
  exciter adds filled over them, so that the band it takes and the harmonics above it show. Over them its lift as a
  curve: how far it raises the sound at each frequency while the curve inside leaves the band straight. A dot sits
  where the band starts, as high as the top is lifted: a drag moves the Frequency sideways and the Amount up and
  down, as one undo step; a press anywhere on the graph puts the dot under the pointer; the wheel turns the Drive.
  Beside the graph, a meter of how far the driven band reaches into the curve, with a line where the curve bends:
  under the line the band is only lifted, over it it turns into harmonics.
- **Band:** Frequency and Amount, as large knobs.
- **Harmonics:** Drive, Colour and High Cut, and Listen.

Mix is the knob on the device's row in the Devices view.

## Limits

- One band: no separate bands with their own settings, and no choice of excitation mode.
- No bass mode: it works on the highs only.
- The filter after the curve is worked out anew at once when Frequency or High Cut moves, while the filters before
  the curve glide there, so for the few milliseconds of a move the two don't match; automating Frequency is not
  perfectly smooth.
- The 2 ms filter and the curve add about 3.3 ms of latency, so the exciter cannot be played through live with Low
  Latency on.

## In the code

- DSP: `src/devices/saturation/Exciter.h`, `src/devices/saturation/Exciter.cpp`; the curve's anti-aliasing in
  `src/devices/dsp/Saturation.h`.
- Editor: `src/ui/devices/ExciterEditor.h`, `src/ui/devices/ExciterEditor.cpp`; the dot in
  `src/ui/devices/GraphDot.h`.
- Tests: `tests/ExciterTests.cpp`, `tests/EffectEditorTests.cpp`.
- Presets: `resources/presets/Exciter/` (see [presets.md](../../presets.md)).
