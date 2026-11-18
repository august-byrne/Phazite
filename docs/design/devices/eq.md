# EQ

*Design section 9.*

An eight-band parametric equalizer: it raises or lowers parts of the spectrum and cuts off its ends. Each band plays on
the whole sound, on its middle or its sides alone, or on its left or its right alone, and a bell or a shelf can be
dynamic: its gain moves with the level of the sound in its band, which makes a de-esser of it, or a lift that lets go
when its band is already loud. A new EQ changes nothing: its two cuts are off and its other bands are at 0 dB.

## Parameters

Each of the eight bands has the same nine parameters, named "Band 1 On" to "Band 8 Threshold".

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Band 1 to 8 On | Off, On | On for bands 2 to 7, Off for bands 1 and 8 | Whether the band plays. |
| Band 1 to 8 Type | Low Cut, Low Shelf, Bell, Notch, High Shelf, High Cut | Band 1 Low Cut, band 2 Low Shelf, bands 3 to 6 Bell, band 7 High Shelf, band 8 High Cut | The band's shape. Any band can take any shape. |
| Band 1 to 8 Frequency | 20 Hz to 20 kHz | 30 Hz, 100 Hz, 250 Hz, 800 Hz, 2.5 kHz, 6 kHz, 10 kHz, 18 kHz | Where the band sits. |
| Band 1 to 8 Gain | -24 to +24 dB | 0 dB | How far a bell or a shelf raises or lowers the sound. A cut and a notch ignore it. |
| Band 1 to 8 Q | 0.1 to 18 | 0.71 for bands 1, 2, 7 and 8, 1 for bands 3 to 6 | A bell's or a notch's width and a shelf's steepness. On a cut, 0.71 is flat and more makes a resonant bump at its frequency. |
| Band 1 to 8 Slope | 12 dB/oct, 24 dB/oct, 36 dB/oct, 48 dB/oct | 24 dB/oct | How steep a cut is. The other shapes ignore it. |
| Band 1 to 8 Placement | Stereo, Mid, Side, Left, Right | Stereo | What of the sound the band plays on: both sides alike, the middle (L + R) / 2 or the side (L - R) / 2 alone, or the left or the right alone. |
| Band 1 to 8 Dynamic | -24 to +24 dB | 0 dB | How far a bell's or a shelf's gain moves, beyond its Gain, as the sound in its band gets loud. At 0 the band doesn't move. A cut and a notch ignore it. |
| Band 1 to 8 Threshold | -60 to 0 dB | -24 dB | The level of the sound in the band from which its gain starts to move. |
| Output | -24 to +24 dB | 0 dB | A gain after the bands. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the EQ's sound; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

## How it works

The bands play one after another, on up to two channels. A band on Stereo plays on each channel through filters of its
own; a band on Mid or Side plays on the middle (L + R) / 2 or the side (L - R) / 2 alone, leaving the other as it is
(so a Mid band leaves a sound that is opposite on the two sides untouched, sample for sample, and a Side band one that is
alike on them; the mono sum through a Side band is what it was, to a float's last bit); a band on Left or Right plays on
that channel alone, leaving the other untouched. Each band keeps its own filters for what it plays on. On a mono channel
the Stereo, Mid and Left bands play on the channel and the Side and Right bands do nothing. A change of a band's placement
while it plays starts the band on its new placement from rest and crossfades to it over 10 ms from the band as it played,
both playing meanwhile, so that it doesn't click (a band that isn't played, a flat bell or shelf, just moves). A change
while a crossfade is under way waits for its end, as the placement is read again at each step: a placement automated
through several in a row moves through them one 10 ms crossfade after another, never cutting one short.

A shelf, a bell or a notch is one second-order section. A cut is a Butterworth cascade of one to four second-order
sections (12 to 48 dB per octave), and the band's Q scales the Q of its steepest section, relative to the flat 0.71.

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

A dynamic band (a bell or a shelf whose Dynamic isn't 0; a cut or a notch ignores its Dynamic) hears the sound as it comes
into the EQ, before any band, on its placement (the middle for a Mid band, the louder of the two sides for a Stereo one),
through a filter that is its own region: for a bell a band-pass at its frequency with its Q (at least 1), for a low shelf
a 12 dB per octave low-pass at its frequency, for a high shelf a high-pass. The level it hears is the loudest of that
sound over the last 25 ms (whole milliseconds of it, as the Compressor's Duck holds its key: so a steady sine reads at its
peak, and the level doesn't fall between the peaks of a bass down to 30 Hz, which would move the gain at twice its
frequency), followed up toward a louder one 63 % of the way in 3 ms and down toward a quieter one 63 % of the way in
60 ms, in decibels. So after an "s" 20 dB over its Threshold, a de-esser's band of -8 dB is back within 1 dB of its Gain
about 150 ms after it. A level that has fallen 24 dB under the band's Threshold, with the loudest it holds, is none, so that
what comes next finds it as after any silence. At each step of 32 samples the band plays its
Gain plus its Dynamic times how far that level is over its Threshold, from none at the Threshold to all of it 12 dB above:
`gain + dynamic * clamp ((level - threshold) / 12, 0, 1)`. Its Dynamic and Threshold glide as its Gain does. A band at
0 dB with a Dynamic is played whenever its moving gain isn't 0, and starts from rest as it leaves 0, where it was a
straight wire, so that it comes in without a click. Nothing of it depends on the block size, a band whose Dynamic is 0
costs nothing more, and with every band on Stereo and none dynamic each sample goes through the same work as before
step S6.4. How far each band's Dynamic moves its gain now is published once a block for the editor.

While any band is dynamic, the EQ reports a tail (`getTailSeconds()`): as long as its level takes to fall from 12 dB over
full scale to where it is none under the lowest of their Thresholds, 26 ms held and then 60 ms for each 8.7 dB (half a
second for a Threshold of -32 dB, 0.69 s at the lowest, -60 dB). Its runner lets it sleep on silence only after that, so
that a dynamic band doesn't wake still turned by what it heard before the silence and the song plays live as it renders.
Without a dynamic band it reports none and sleeps as soon as any effect does.

The EQ's response for each way through it (`Equalizer::getMagnitude()`) is the product of the bands that act on that
way: on the middle, the bands on Stereo and those on Mid; on the side, those on Stereo and on Side; on the left, those on
Stereo and on Left; on the right, those on Stereo and on Right; and Stereo's, the bands on Stereo alone, which act on all
of it. A Left or a Right band acts on half of the middle and of the side and mixes them, which no single curve shows, so
the middle's and the side's curves leave it out, as the left's and the right's leave out the Mid and Side bands.

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
- a right-click offers the band's type, a cut's slope, its placement, Make Dynamic for a bell or a shelf that isn't
  dynamic (its Dynamic at -6 dB, which gives it a handle to drag), and Turn On or Turn Off.

Over a dot the pointer becomes a hand and the dot lights up. A band that isn't on Stereo has its placement's letter after
its number in its dot (M, S, L or R). While any band that is on is on Mid or Side, the graph draws two curves in place of
one, the middle's in the device's colour and the side's dashed in the analyzer's second colour (the faint trace's), and
while any is on Left or Right, the left's and the right's alike; their names are along the graph's top at its right.

A dynamic band's dot has a line from it up or down to where its gain goes at its whole Dynamic, with a hollow handle at
the line's end that drags up and down for the Dynamic (one undo step; Escape takes it back; over it the pointer shows
that it moves up and down; a press takes the nearer of the dot and the handle, so that a small Dynamic's handle, within
the dot's reach, can be taken), and a mark across the line where the band's gain is now, which moves with the sound. The
curves are drawn with the gains as they play now, so a de-esser's dip is seen to open and close; the graph repaints
when a mark moves by a hundredth of a decibel or more.

Beside the graph are eight numbered buttons that select a band, and the selected band's controls: its On switch, its type
(titled Band Shape), its slope and its Placement, and knobs for Frequency, Gain, Q, Dynamic and Threshold, with the EQ's
Output knob. The Threshold is faint while the Dynamic is 0, and both are off for a cut or a notch. These work from the
keyboard and have screen-reader names. Mix is the knob on the device's row in the Devices view.

The EQ's editor is the one device editor not built on the shared frame (`DeviceEditorBase`): its graph narrows and
shrinks to fit the dock, and it never wraps or turns into pages.

## Limits

- Eight bands, minimum phase only: no linear-phase or natural-phase mode.
- A dynamic band hears its own input only: no sidechain, no automatic threshold, and no attack or release controls (its
  level rises in 3 ms, holds 25 ms and falls in 60 ms). Its gain moves in steps of 32 samples.
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
