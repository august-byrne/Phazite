# Saturator

*Design section 9.*

Drives the sound into a curve that rounds its peaks off, adding harmonics that make it warmer, fuller or grittier.
It is a device of its own beside the [Compressor](compressor.md), whose Drive only colours what it compresses.

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Drive | -24 to +36 dB | +6 dB | The gain into the curve. Below 0 dB a loud sound is only bent gently. |
| Shape | Soft, Tape, Tube, Hard, Fold | Soft | The curve (below). |
| Tone | -100 to +100 % | 0 % | Tilts the sound after the curve around 1 kHz: darker below 0 %, brighter above, 6 dB at each end at the range's ends. |
| Output | -24 to +12 dB | 0 dB | A gain after the curve. |
| Auto Gain | Off, On | On | Takes the Drive back off after the curve, so that turning the Drive changes the colour rather than the loudness: quiet sound, which the curve leaves straight, keeps its level. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the saturator's sound; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

## How it works

The sound is taken to twice the sample rate through polyphase half-band filters (`juce::dsp::Oversampling`), driven
into the curve, tilted, and brought back down. Drive and Output glide to their new values over about 20 ms.

The five curves, for a driven sample `x`:

- **Soft:** a soft clip like `tanh`, exactly 1 from 3 on.
- **Tape:** `x / (1 + |x|)`, which rounds off earlier and more gently.
- **Tube:** the soft clip of `x + x^2 / 5`: lopsided, so it adds even harmonics too. The DC it leaves is filtered
  out with a 10 Hz high-pass.
- **Hard:** clipping at 1.
- **Fold:** `sin (pi x / 2)`: smooth up to 1, then the peaks fold back.

Each curve is played by its antiderivative: every output is the curve's average between the last two inputs
(first-order antiderivative anti-aliasing: Parker, Zavalishin and Le Bivic, 2016; Bilbao, Esqueda, Parker and
Valimaki, 2017). The harmonics the curve adds then fall away steeply, and the half-band filters take away what is left
above the sound's band, so they don't fold back as aliasing. The averaging dulls the top octave, so a short
linear-phase filter after it makes that up: what a curve leaves straight comes out level to 20 kHz. See
[dsp-audit.md](../../archive/dsp-audit.md).

The Tone is a low shelf and a high shelf at 1 kHz, turned opposite ways, after the curve.

The filters to twice the rate and back delay the sound by a few samples and turn the phase of its top octave. The
saturator reports the delay as its latency, and passes the copy of what came in, which the Mix blends with its sound,
through the same filters with a straight line in the curve's place, so that the two meet in phase at any mix.

## Its editor

- **Saturation:** the curve the sound is driven into, lit as far as the sound reaches into it, so that turning the
  Drive up shows the sound pushing into the bend; beside it the wave coming in, pale, and going out, in the device's
  colour, held still on the wave's start, so that the rounded or folded tops show. A drag up or down on the curve
  turns the Drive, as one undo step; under the pointer the whole curve lights up.
- **Shape:** five buttons, each drawn with its curve.
- **Drive**, as a large knob; then **Tone**, **Output** and **Auto Gain**.

Mix is the knob on the device's row in the Devices view.

## Limits

- One band for the whole sound: no multiband saturation.
- Five fixed curves: no curve to draw, and no bias or asymmetry control.
- No filters before or after the curve other than the Tone's tilt, and no dynamics.
- Twice the sample rate only, with no higher quality setting: a tone driven 24 dB into Hard, Fold or Tube keeps its
  aliases at least 50 dB under it, not out of hearing.
- Auto Gain only undoes the Drive; it does not match the loudness of what comes out to what went in.
- A drag on the curve starts from the Drive as modulation plays it, and sets that as the value by hand.

## In the code

- DSP: `src/devices/saturation/Saturator.h`, `src/devices/saturation/Saturator.cpp`; the curves and their
  anti-aliasing in `src/devices/dsp/Saturation.h`.
- Editor: `src/ui/devices/SaturatorEditor.h`, `src/ui/devices/SaturatorEditor.cpp`.
- Tests: `tests/EffectDeviceTests.cpp`, `tests/AntialiasingTests.cpp`, `tests/EffectEditorTests.cpp`.
- Presets: `resources/presets/Saturator/` (see [presets.md](../../presets.md)).
