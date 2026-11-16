# EQ

*Design section 9.*

An eight-band parametric equalizer: it raises or lowers parts of the spectrum and cuts off its ends. A new EQ changes
nothing: its two cuts are off and its other bands are at 0 dB.

## Parameters

Each of the eight bands has the same six parameters, named "Band 1 On" to "Band 8 Slope".

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Band 1 to 8 On | Off, On | On for bands 2 to 7, Off for bands 1 and 8 | Whether the band plays. |
| Band 1 to 8 Type | Low Cut, Low Shelf, Bell, Notch, High Shelf, High Cut | Band 1 Low Cut, band 2 Low Shelf, bands 3 to 6 Bell, band 7 High Shelf, band 8 High Cut | The band's shape. Any band can take any shape. |
| Band 1 to 8 Frequency | 20 Hz to 20 kHz | 30 Hz, 100 Hz, 250 Hz, 800 Hz, 2.5 kHz, 6 kHz, 10 kHz, 18 kHz | Where the band sits. |
| Band 1 to 8 Gain | -24 to +24 dB | 0 dB | How far a bell or a shelf raises or lowers the sound. A cut and a notch ignore it. |
| Band 1 to 8 Q | 0.1 to 18 | 0.71 for bands 1, 2, 7 and 8, 1 for bands 3 to 6 | A bell's or a notch's width and a shelf's steepness. On a cut, 0.71 is flat and more makes a resonant bump at its frequency. |
| Band 1 to 8 Slope | 12 dB/oct, 24 dB/oct, 36 dB/oct, 48 dB/oct | 24 dB/oct | How steep a cut is. The other shapes ignore it. |
| Output | -24 to +24 dB | 0 dB | A gain after the bands. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the EQ's sound; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

## How it works

The bands play one after another, on up to two channels, each channel through its own filters. A shelf, a bell or a
notch is one second-order section. A cut is a Butterworth cascade of one to four second-order sections (12 to 48 dB
per octave), and the band's Q scales the Q of its steepest section, relative to the flat 0.71.

Each section is a state-variable filter integrated with the trapezoidal rule, which stays stable while its frequency
moves. Its coefficients are matched to the analog shape rather than taken from the bilinear transform (M. Vicanek,
"Matched Second Order Digital Filters", 2016): the poles sit where the analog section's map to, and the zeros are
chosen so that the gain is the analog section's at 0 Hz, at the Nyquist frequency and at the band's own frequency. A
bell or a shelf near the top of the spectrum therefore keeps its width and symmetry. See
[dsp-audit.md](../../archive/dsp-audit.md).

Parameters move smoothly: the coefficients are worked out again every 32 samples, on a grid counted from
the song's place while it plays (so that what it plays doesn't depend on how the samples are cut into blocks, and a
loop's second pass, a start anywhere and a wake from sleep play as a render does), while a band's frequency, gain or
Q glides to its new value over about 20 ms, frequency and Q on a logarithmic scale. A shelf or a bell at 0 dB is not
played at all. The output gain ramps the same way; where the song jumps (a loop going round, the song starting after a
count-in) in the middle of a step, the ramp goes on from where it got to.

The EQ is minimum phase, runs at the sound's own rate and reports no latency.

For its analyzer it taps what it plays, only while an editor shows it. The analyzer (`src/devices/core/Spectrum.h`,
shared by every device that shows a spectrum) keeps each level at the loudest any frame gave it since the last
refresh, so short sounds show, and works in two resolutions: long frames of the sound at about 12 kHz for the lows, up
to 150 Hz, and frames of about 43 ms for the highs, from 300 Hz, with an octave between them over which one gives way
to the other.

## Its editor

A graph from 20 Hz to 20 kHz and from -24 to +24 dB shows the EQ's curve, the selected band's own curve filled in its
colour, the spectrum of what the EQ plays and, fainter behind it, the spectrum of what comes in. Each band has a dot:

- drag a dot to move the band's frequency and, for a bell or a shelf, its gain, as one undo step: it moves as far as
  the pointer, from where it is set (a click changes nothing), and Escape takes the drag back;
- the wheel over a dot changes its band's Q, a turn of several notches one undo step; elsewhere it scrolls the view;
- a double-click turns the band on or off;
- a right-click offers the band's type, a cut's slope, and Turn On or Turn Off.

Over a dot the pointer becomes a hand and the dot lights up. Beside the graph are eight numbered buttons that select a
band, and the selected band's controls: its On switch, its type (titled Band Shape) and slope, and knobs for
Frequency, Gain and Q, with the EQ's Output knob. These work from the keyboard and have screen-reader names. Mix is the
knob on the device's row in the Devices view.

The EQ's editor is the one device editor not built on the shared frame (`DeviceEditorBase`): its graph narrows and
shrinks to fit the dock, and it never wraps or turns into pages.

## Limits

- Eight bands, minimum phase only: no linear-phase or natural-phase mode.
- Every band treats both channels alike: no mid/side or left/right placement.
- No dynamic bands.
- Cuts are 12 to 48 dB per octave: no 6 or 96 dB slopes, and no tilt or band-pass shape.
- No band solo (listening to one band alone) and no automatic gain.
- Turning a band on or off, or changing its type or slope, takes effect at once, without a fade.
- The analyzer has no settings (range, tilt, freeze), and shows both channels as one trace, each level the louder
  channel's: no left, right, mid or side traces of their own.
- A dot has no fine drag.

## In the code

- DSP: `src/devices/eq/Equalizer.h`, `src/devices/eq/Equalizer.cpp`; the filters and their matched coefficients in
  `src/devices/dsp/Filters.h`; the analyzer in `src/devices/core/Spectrum.h` and `src/devices/core/SampleTap.h`.
- Editor: `src/ui/devices/EqEditor.h`, `src/ui/devices/EqEditor.cpp`; the spectrum's drawing in
  `src/ui/devices/DeviceGraph.h`.
- Tests: `tests/EqualizerTests.cpp`, `tests/EqEditorTests.cpp`.
- Presets: `resources/presets/EQ/` (see [presets.md](../../presets.md)).
