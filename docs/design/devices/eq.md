# EQ

*Design section 9.*

An eight-band parametric equalizer: it raises or lowers parts of the spectrum and cuts off its ends. Each band plays on
the whole sound, on its middle or its sides alone, or on its left or its right alone, and a bell or a shelf can be
dynamic: its gain moves with the level of the sound in its band, which makes a de-esser of it, or a lift that lets go
when its band is already loud. It plays in minimum phase, or in linear phase at a latency. A new EQ changes nothing: its
two cuts are off and its other bands are at 0 dB.

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
| Phase | Minimum, Linear | Minimum | How it plays its bands: each turning the phase as its analog shape does, at no latency (Minimum), or the same curve with every frequency as late as every other, at a latency (Linear; see [Linear phase](#linear-phase)). |
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

In Minimum the EQ runs at the sound's own rate and reports no latency; everything above is how Minimum plays.

## Linear phase

With its Phase on Linear the EQ plays the same curve with every frequency as late as every other, so that its sound stays
in phase with what wasn't equalized: on a master, on one of several microphones of the same source, on a copy mixed back
with its original, and at any Mix (the dry copy is delayed as much, so a Mix of 50 % is an EQ of half the depth; in Minimum
the phase a bell of +12 dB turns cancels some of what came in beside it, 0.4 dB at 590 Hz beside a bell at 1 kHz). It costs a
latency and a ring before a sharp sound, which is why it is a choice. Its Phase is the one set by hand as the EQ was
prepared, as its latency is: a Phase that automation moves plays as the one set by hand, as a compressor's Look-Ahead beyond
its latency does. Its buttons follow the lane; the sound and the latency's text beside them don't.

**Its sizes.** With r = 1 for rates up to 50 kHz, 2 up to 100 kHz and 4 above: a latency of L = 4096 r samples (85 ms at
48 kHz, 93 ms at 44.1 kHz), which it reports, so that the engine delays the other tracks to match; kernels of L - P taps to
each side of their middle tap, convolved in partitions of P = 256 r samples (uniformly partitioned, overlap-save, in the
frequency domain), so that a partition's wait and a kernel's half add up to L. A kernel reaches L - P samples to each side of
its middle (80 ms at 48 kHz): a sharp sound's ring starts as much before it and ends as much after. Silence in gives silence
out after 2 L, the tail it reports. Its FFT is Phazite's own (`src/devices/dsp/Fft.h`, radix 2, single precision), which
takes no lock and computes the same on every system.

**Its kernels.** Each band that is on is, at each frequency, a real gain on what it is placed on: the magnitude of its sections
as Minimum plays them, so both modes draw the same curve, cuts at every slope, shelves, bells and notches alike. The bands in
their order are a 2 x 2 matrix from (left, right) in to (left, right) out, whose four entries are four kernels: one for both
sides while every band is on Stereo; while none is on Left or Right, two: the middle's and the side's where one of them is
a delay alone (bands on Mid alone, or on Side alone), else the sides' and the crossings' from each side to the other; else
four; on a mono channel one, of the bands that play on it in Minimum. A kernel that is nothing is skipped, and one that is 1
everywhere (every band off or flat, or the side under bands on Mid alone) plays what came in as late as L, to the bit: so a
Side band leaves a sound alike on both sides as it came, and a Mid band one opposite on them, as in Minimum, and a Left band
the right. A kernel is its response on a grid of 2 L frequencies, turned into time by an inverse FFT and cut to its taps by a
rectangle; what the cut dropped of its sum (its gain at 0 Hz) is then spread evenly over its taps, so that it passes 0 Hz as
its curve does; then it is cut into partitions, each transformed. The rectangle was chosen by measuring (`tests/EqualizerTests.cpp`,
"the window and the grid"): a Kaiser window smears the curve, the more the longer a band rings (beta 8 put a bell at 200 Hz,
Q 4, 0.6 dB off; the rectangle puts it 0.005 dB off); a raised cosine over the last 2, 3 or 5 % of each end left the curve
from 200 Hz up as it was but put a 48 dB cut's level at 10 Hz 3 to 6 dB higher at 30 Hz and 9 to 13 dB at 80 Hz; and a grid
of 4 L halves only the hardest of the low end's errors (a 48 dB cut at 30 Hz, 2.0 dB against 4.3) for twice a design's work.
So a kernel ends in a step: its end taps are 68 dB under its middle one for a bell of +12 dB at 100 Hz with a Q of 10 (71 dB
at 30 Hz), so a click through it brings a faint tone at the bell's frequency in at once 80 ms before the click, and cuts it
off 80 ms after. The Output gain plays after the kernels.

**How true its curve is.** From 200 Hz up, a bell or a shelf with a Q up to 4 plays within 0.01 dB of the curve the editor
draws. Below, what rings longer than the kernel (160 ms at 48 kHz) is cut short; the largest error where the curve is above
-60 dB, measured at 48 kHz:

| Band | Q 1 | Q 4 | Q 10 |
| --- | --- | --- | --- |
| Bell, +12 dB at 30 Hz | 0.11 dB | 3.5 dB | 5.3 dB |
| Bell, +12 dB at 60 Hz | 0.002 dB | 1.0 dB | 3.0 dB |
| Bell, +12 dB at 100 Hz | 0.00001 dB | 0.20 dB | 2.1 dB |
| Bell, -12 dB at 30 Hz | 0.016 dB | 1.2 dB | 3.8 dB |
| Bell, -12 dB at 60 Hz | 0.0001 dB | 0.27 dB | 2.7 dB |
| Bell, -12 dB at 100 Hz | 0.000005 dB | 0.028 dB | 0.44 dB |

| Low cut | Its error above -60 dB | At 10 Hz (its curve's) | At 0 Hz |
| --- | --- | --- | --- |
| 12 dB/oct at 30 Hz | 0.0001 dB | -19.1 dB (-19.1 dB) | -160 dB |
| 12 dB/oct at 80 Hz | 0.00001 dB | -36.1 dB (-36.1 dB) | |
| 48 dB/oct at 30 Hz | 4.3 dB | -61.4 dB (-76.3 dB) | -146 dB |
| 48 dB/oct at 80 Hz | 0.06 dB | -110 dB (-144 dB) | |

The spread that puts each kernel's gain at 0 Hz on its curve costs the steep cuts a little at 10 Hz: without it, a 48 dB cut
at 30 Hz passed 0 Hz at -45 dB and was at -71.5 dB at 10 Hz (-118 dB at 80 Hz). A low shelf of +12 dB at 30 Hz passes 0 Hz
within 0.01 dB of its curve, with a Q of 0.71 and of 4 (a Q of 10 was 0.6 dB off without it). A notch of Q 1 is 39 dB deep
at 100 Hz and 50 dB deep at 1 kHz. A longer kernel would bring the low end closer, at more latency.

**A change.** A change of any band (a knob turned, automation) changes the kernels on a grid of L / 2 samples counted from the
song's place (as the 32-sample grid is): at a line the values are taken (themselves, not the 32-sample glide's), and where
they differ from those of the kernels designed last a design starts, spread evenly over the next L / 2 samples played by a
count of its work, the same every time, never by a clock or the block's size, no sample's share more than a 64th of it. At the
first line after it is done the kernels played glide to it over L / 2 samples, both convolved meanwhile and their outputs
crossfaded in a straight line. So the values taken at a line are wholly in the sound L samples later, when the sound that came
in with them comes out, and automation stays in time with the song: a bell swept from 200 Hz to 2 kHz in a second plays at
each moment as the automation stood L samples before, within a step of the grid, without a click; a step of a band's gain is
half way in L samples later, within half a step. A change while a design or a glide is under way waits for it; while nothing
changes, one set of kernels is convolved and nothing is designed. What it plays doesn't depend on the block's size: blocks of
7, 32 and 512 samples play the same to the bit.

**A start, a jump and a sleep.** As it is prepared its kernels are designed whole, for the values set by hand. After a reset
the history is silence, as a render's is after silence; where the values at the first block differ from the kernels' (an
automated band at the song's start), a design starts at once and is put in whole, without a glide, L / 2 samples later: until
then only a kernel's leading half of what came in since the reset can sound, the pre-ring alone. Measured with full-scale
noise starting at the reset through the steepest low cut, against the cut's own kernels from the start: at most -58 dBFS
apart in those L / 2 samples at 30 Hz (-92 dBFS at 100 Hz), and the same after. Where the song jumps (a loop
going round, a start elsewhere), a design under way for the values before it is dropped, and one for the values there
starts at once, spread over the samples to the grid's next line so that it glides in there, as a render's design from the
line before does (where fewer than L / 8 samples are left, which would crowd its work, the next line's design follows as at
any line); a glide under way ends as it would. With its bands standing still, it plays after a jump and after a sleep on
silence as a render does (within 3 x 10^-7, the partitions falling elsewhere). A band automated across a jump plays the
kernels of the values before it until the new ones are in, where a render has those the jump never played: measured with a
bell swept under noise and the jump at 16 places along a step of the grid, up to -21 dBFS apart from the render, for 1.45 L
at the longest (5,920 samples, the jump just after a line), L at the shortest (the jump on a line). Across a sleep, up to
-73 dBFS apart, for L. In a loop shorter than the grid's step (43 ms at 48 kHz) a design done comes in at the loop's wrap
where no line of the grid lies inside the loop, and of two designs in a row that a wrap finds made for other values only the
first is dropped, so that the kernels still follow the knobs and the lanes there, a step or two later than elsewhere.

**Dynamic bands.** A dynamic band's shape at its Gain is in the kernels like any band's. What its Dynamic moves is played after
the kernels by the band's own sections, as Minimum plays them but made with the moved amount alone as their gain, on the
band's placement, its ear hearing what came in as late as the kernels play it, so that its gain moves in time with what comes
out: a de-esser set in Minimum does the same in Linear (De-Ess takes a burst 8 dB down in both, its deepest within a
millisecond of where Minimum's is, counted from the burst as it comes out). A band with both a Gain and a Dynamic is a hair
off Minimum's curve while it moves (two bells in a row are not exactly one of their sum): a sine through a bell of +6 dB
moved 8 dB down comes out of Linear at most 0.07 dB from what Minimum plays.

**What it costs.** Each kernel that a side's output sums takes 31 partitions of 257 complex points, four real multiply-adds
each, every 256 samples: 249 multiply-adds a sample with one kernel for both sides, 127 with bands on Mid alone or on Side
alone (the middle's or the side's kernel, the other a delay), 498 with two or four, and an FFT of 512 points in and one out
for each side every 256 samples; with every band flat, a delay alone, 5. A design is about 480,000 units of work (each about
five floating-point operations) for eight bands on one kernel, 750,000 for four kernels, spread so that no sample's share is
more than a 120th of it (after a jump, as much as four times as fast: see above).

For its analyzer it taps what it plays, only while an editor shows it. The analyzer (`src/devices/core/Spectrum.h`,
shared by every device that shows a spectrum) keeps each level at the loudest any frame gave it since the last
refresh, so short sounds show, and works in two resolutions: long frames of the sound at about 12 kHz for the lows, up
to 150 Hz, and frames of about 43 ms for the highs, from 300 Hz, with an octave between them over which one gives way
to the other.

## Its editor

A graph from 20 Hz to 20 kHz and from -24 to +24 dB (the analyzer behind it from -90 to +6 dB) shows the EQ's curve, the selected band's own curve filled in its
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
The spectrum of what the EQ plays is then drawn as a trace for each of those ways, the middle's and the side's (or the
left's and the right's), each in its curve's colour, faint as the spectra are; with every band on Stereo, one trace, the
louder of the two sides, as before. The analyzer's levels go from -90 to +6 dB, as every device's do.

A dynamic band's dot has a line from it up or down to where its gain goes at its whole Dynamic, with a hollow handle at
the line's end that drags up and down for the Dynamic (one undo step; Escape takes it back; over it the pointer shows
that it moves up and down; a press takes the nearer of the dot and the handle, so that a small Dynamic's handle, within
the dot's reach, can be taken), and a mark across the line where the band's gain is now, which moves with the sound. The
curves are drawn with the gains as they play now, so a de-esser's dip is seen to open and close; the graph repaints
when a mark moves by a hundredth of a decibel or more.

Beside the graph are eight numbered buttons that select a band, and the selected band's controls: its On switch, its type
(titled Band Shape), its slope and its Placement, and knobs for Frequency, Gain, Q, Dynamic and Threshold, with the EQ's
Output knob. Under the Output, the Phase is two buttons, Minimum and Linear, with the latency in milliseconds beside them
while the EQ plays Linear ("85 ms"), as the EQ reports it. The Threshold is faint while the Dynamic is 0, and both are off
for a cut or a notch. These work from the keyboard and have screen-reader names. Mix is the knob on the device's row in the
Devices view. The curves are what is heard, the Mix among them: with H a way's response (its bands' in their order, and
the Output) and m the Mix, |(1 - m) + m H| in Minimum, where each band turns the phase, and (1 - m) + m |H| in Linear,
whose kernels turn none, so that under 100 % a cut is filled in by what came in; the selected band's own curve is the band
alone. The analyzer is the same in both phases.

The EQ's editor is the one device editor not built on the shared frame (`DeviceEditorBase`): its graph narrows and
shrinks to fit the dock, and it never wraps or turns into pages.

## Limits

- Eight bands. Linear phase has one kernel length (L = 4096 samples at 48 kHz): below about 100 Hz a narrow bell or a steep
  cut is cut short (the table above), and no longer kernel is offered. Linear rings before a sharp sound (a kick through a
  steep low cut), as every linear-phase EQ does, and plays a dynamic band's moving part in minimum phase. No "natural phase"
  mode (the curve's phase without the latency). A Phase that automation moves plays as the one set by hand: its buttons
  follow the lane, the sound and the latency's text don't.
- A dynamic band hears its own input only: no sidechain, no automatic threshold, and no attack or release controls (its
  level rises in 3 ms, holds 25 ms and falls in 60 ms). Its gain moves in steps of 32 samples.
- Cuts are 12 to 48 dB per octave: no 6 or 96 dB slopes, and no tilt or band-pass shape.
- No band solo (listening to one band alone) and no automatic gain.
- Turning a band on or off, or changing its type or slope, takes effect at once, without a fade.
- The analyzer has no settings (range, tilt, freeze). With every band on Stereo it shows both channels as one trace,
  each level the louder channel's; the left, right, mid and side traces show only while those curves do, and what
  comes in is always one trace.
- A dot has no fine drag.

## In the code

- DSP: `src/devices/eq/Equalizer.h`, `src/devices/eq/Equalizer.cpp`; its linear phase in `src/devices/eq/LinearPhase.h`,
  with the partitioned convolution in `src/devices/dsp/PartitionedConvolution.h` and the FFT done in small parts that spreads
  a design's work in `src/devices/dsp/SplitFft.h`, both on Phazite's own FFT in `src/devices/dsp/Fft.h`; the filters and
  their matched coefficients in `src/devices/dsp/Filters.h`; the analyzer in `src/devices/core/Spectrum.h` and
  `src/devices/core/SampleTap.h`.
- Editor: `src/ui/devices/EqEditor.h`, `src/ui/devices/EqEditor.cpp`; the spectrum's drawing in
  `src/ui/devices/DeviceGraph.h`.
- Tests: `tests/EqualizerTests.cpp`, `tests/EqEditorTests.cpp`, and `tests/ConvolutionTests.cpp` for the FFT and the
  convolution.
- Presets: `resources/presets/EQ/` (see [presets.md](../../presets.md)).
