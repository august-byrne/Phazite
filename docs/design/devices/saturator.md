# Saturator

*Design section 9.*

Drives the sound into a curve that rounds its peaks off, adding harmonics that make it warmer, fuller or grittier, or
rounds it to steps, fewer bits the harder it is driven. With its Band on, only a band of the sound goes into the curve
and the rest passes as it came: a band of highs driven and lifted is what an exciter does (the Exciter was merged into
it, roadmap step S6.6), and a band of lows driven warms a bass without touching its top. It is a device of its own
beside the [Compressor](compressor.md), whose Drive only colours what it compresses.

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Drive | -24 to +36 dB | +6 dB | The gain into the curve. Below 0 dB a loud sound is only bent gently. For Crush, how coarse its steps are (below). |
| Shape | Soft, Tape, Tube, Hard, Fold, Crush | Soft | The curve (below). |
| Colour | 0 to 100 % | 0 % | Moves the curve's working point off its middle, as a tube's bias does: the curve gives way sooner on one side than the other, which adds even harmonics. A quiet sound keeps its level at any Colour. Crush ignores it. |
| Tone | -100 to +100 % | 0 % | Tilts the sound after the curve around 1 kHz: darker below 0 %, brighter above, 6 dB at each end at the range's ends. With Band on, the band alone. |
| Output | -24 to +12 dB | 0 dB | A gain after the curve. With Band on, the band's level: above 0 dB it lifts the band, as a shelf or a bell would. |
| Auto Gain | Off, On | On | Takes the Drive back off after the curve, so that turning the Drive changes the colour rather than the loudness: quiet sound, which the curve leaves straight, keeps its level. Crush changes no level, and Auto Gain does nothing to it. |
| Band | Off, On | Off | Only the band between From and To goes into the curve; the rest of the sound passes as it came. |
| From | 20 Hz to 10 kHz | 3 kHz | The band's low end. At its lowest, 20 Hz, the band has no low end. |
| To | 200 Hz to 20 kHz | 20 kHz | The band's high end, kept at least an octave above From. At its highest, 20 kHz, the band has no high end. |
| Quality | 2x, 4x, 8x | 2x | How many times the sound's rate the curve plays at: the higher, the fewer aliases, and the more it costs. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the saturator's sound; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

## How it works

### The curves

The six curves, for a driven sample `x`:

- **Soft:** a soft clip like `tanh`, exactly 1 from 3 on.
- **Tape:** `x / (1 + |x|)`, which rounds off earlier and more gently.
- **Tube:** the soft clip of `x + x^2 / 5`: lopsided, so it adds even harmonics too.
- **Hard:** clipping at 1.
- **Fold:** `sin (pi x / 2)`: smooth up to 1, then the peaks fold back. Its middle is steeper than a straight line, by
  pi / 2, so a quiet sound comes out 3.9 dB louder.
- **Crush:** rounds the sound to steps: `step * round (x / step)`, with `step = 2^-7` times the Drive as a gain. That is
  8 bits (256 steps from -1 to 1) at a Drive of 0 dB, a bit fewer for every 6 dB more, so 2 bits at +36 dB and 12 at
  -24 dB. It changes no level, so Auto Gain does nothing for it and Output is its gain.

Each curve is played by its antiderivative: every output is the curve's average between the last two inputs
(first-order antiderivative anti-aliasing: Parker, Zavalishin and Le Bivic, 2016; Bilbao, Esqueda, Parker and
Valimaki, 2017); Crush's antiderivative is a straight line within each step, `n x - n^2 / 2` for the step
`n = round (x)`, meeting the next at the half step. The harmonics the curve adds then fall away steeply, and the
half-band filters take away what is left above the sound's band, so they don't fold back as aliasing. The averaging
dulls the top octave, so a short linear-phase filter after it makes that up: what a curve leaves straight comes out
level to 20 kHz. See [dsp-audit.md](../../archive/dsp-audit.md).

### Colour

Colour moves where the curve works: the driven sample goes into the curve at `x + 0.7 * Colour`, what the curve makes of
that working point alone is taken off, and the result is scaled by the curve's slope at its middle over its slope at the
working point, so that a quiet sound keeps its level at any Colour (within 0.1 dB). The curve is then lopsided, giving
way sooner on one side, and adds even harmonics, the second first. The working point glides as the drive does, and a
second copy of the curve, fed the working point alone, plays beside it while it moves, so that what is taken off is as
late and as smooth as the sound.

A lopsided curve (Tube, or any curve with Colour above 0) leaves an offset, which a 10 Hz high-pass takes off after the
curve. It comes in and goes over 5 ms as the curve becomes lopsided or even, without a click.

### Quality and latency

The sound is taken to 2, 4 or 8 times its rate through polyphase IIR half-band filters (`juce::dsp::Oversampling`, one
stage for each doubling, with a whole latency), driven into the curve, tilted, and brought back down. Every Quality's
filters are made ready when the device is prepared, so that a change of Quality while the song plays allocates nothing:
the new one plays unheard beside the one playing while its filters settle (5 ms, or 150 ms where a lopsided curve's
offset filter or the band's filters must settle), and the sound then fades from one to the other over 10 ms.

The filters and the curve delay the sound by 6 samples at 2x and 7 at 4x and 8x, at any rate (the curve's 4 samples at
8 times the rate are half of one, and 4 more make it whole). The latency follows the Quality set by hand, as the
Compressor's follows its mode: automation that moves the Quality while the song plays changes how the curve plays,
never how late the sound comes. A Quality that comes sooner than the one set waits for it; one that would come later
than the latency set by hand plays as the best Quality below it that comes in time, so a saturator set to 2x stays at
2x whatever its automation asks (4x and 8x come a sample later).

How far each Quality keeps its aliases down, measured on a 5 kHz and a 12 kHz sine driven 24 dB into Hard, Fold and
Tube, at 44.1 and 48 kHz (`tests/AntialiasingTests.cpp`), the loudest alias under the tone at the worst of those:

| Quality | Loudest alias under the tone | Where |
| --- | --- | --- |
| 2x | 15.4 dB | 12 kHz into Fold at 44.1 kHz; 30 to 51 dB on the 5 kHz tone |
| 4x | 36.9 dB | 12 kHz into Fold at 44.1 kHz; 62 to 80 dB on the 5 kHz tone |
| 8x | 61.8 dB | 5 kHz into Tube at 48 kHz; 62 to 74 dB on the 12 kHz tone |

The test holds each Quality to its figure less 3 dB, and each to doing better than the one below it.

### The Band

While Band is on, the sound is split at From and To and only the band between them goes into the curve:

- **The split:** two Linkwitz-Riley crossovers of 24 dB an octave (`src/devices/dsp/Crossover.h`), at the rate the curve
  plays at, after the sound has been taken up to it, so that one oversampler serves the whole device. What is below
  From goes through the other crossover's all-pass, so that it stays in phase with the band and what is above To (as
  the Multiband keeps its bands in phase), and what is outside the band goes through a straight line played as the curve
  is, so that it is as late and levelled alike. All of it is added up before the sound comes down again. With the curve
  bending nothing, Band on changes nothing but the sound's phase: flat within 0.1 dB from 20 Hz to 20 kHz.
- **Its edges:** From at its lowest, or To at its highest, leaves that crossover out. An edge coming in fades in over
  10 ms out of hearing (at 5 Hz, or at 90 % of the sound's rate for To) and then glides to its place; going out, it glides
  there and fades out; and it glides from one frequency to another, 150 octaves a second, however far its value jumps,
  as Safe Bass does, so that none of these clicks.
- **Nothing under the band:** what the curve adds below the band is kept out, as the Exciter kept it out. The band as a
  straight line would play it (times the gain a quiet sound gets: 1 with Auto Gain, the Drive without) is taken from what
  the curve plays; the difference, what the curve bent, goes through a 12 dB an octave high-pass at From, and the two are
  added again. A tone in a band of highs driven hard into a lopsided curve adds harmonics above it and nothing under From
  but what its crossover's slope leaves there (80 dB under the band at 8x). Two tones in the band still make a
  difference tone under it, which the high-pass takes 12 dB an octave off.
- **Tone and Output** act on the band alone: `out = rest + output * tilt (straight + highPassed (bent))`. So Output above
  0 dB lifts the band, which, with harmonics above it, is what an exciter does.
- **The switch** crossfades over 10 ms between the whole sound in the curve and the band in it.
- **Listen** plays the band alone as it goes out (`output * tilt (...)`, without the rest), whatever the Mix, faded over
  5 ms, while its switch in the editor is on and the editor shows. The editor asks for it at each refresh, and the
  saturator stops half a second after it was last asked, so nothing plays the band alone while its editor is hidden.
  Listen is not a parameter and is not saved. With Band off it plays the saturated sound whole, whatever the Mix.

### Moving it

Drive (in decibels), Auto Gain, Colour, Output and Tone glide to their new values through two smoothing steps of 10 ms,
so that a glide starts and ends without a corner. A change of shape starts the new curve beside the one playing,
unheard while it settles (5 ms, or 150 ms for a lopsided curve, whose offset filter must settle), and the sound then fades
from one to the other over 10 ms. Every fade (shape, Quality, Band, Listen, an edge, the offset filter) follows half a
cosine, so that it starts and ends smoothly.

### The copy for the mix

The filters to the curve's rate and back delay the sound by a few samples and turn the phase of its top octave, and the
band's crossovers turn the phase around From and To. The saturator reports the delay as its latency, and passes the
copy of what came in, which the Mix blends with its sound, through the same filters, with a straight line in the curve's
place and, while Band is on, the crossovers' all-passes, so that the two meet in phase at any mix: with the curve left
straight, the sound at 50 % is the sound at 100 %, to 60 dB under it.

## Its editor

- **Saturation:** the curve the sound is driven into, as it plays (lopsided with Colour, in steps with Crush), lit as far
  as the sound reaches into it, so that turning the Drive up shows the sound pushing into the bend; beside it the wave
  coming in, pale, and going out, in the device's colour, held still on the wave's start, so that the rounded or folded
  tops show. A drag up or down on the curve turns the Drive, as one undo step; under the pointer the whole curve lights
  up.
- **Shape:** six buttons, each drawn with its curve.
- **Drive**, as a large knob; then **Colour**, **Tone**, **Output** and **Auto Gain**.
- **Band:** its switch at the right end of the part's title, as a synth's parts have theirs; while it is off the part
  shows faint. A picture of the spectrum of what comes in, pale, and of what goes out, in the device's colour, with the
  band lit between From and To as it plays; its two edges drag sideways from where they are, the nearer one lit under
  the pointer, each drag one undo step that Escape takes back. **From**, **To** and **Listen** under it.
- **Quality**, a choice, last.

Mix is the knob on the device's row in the Devices view.

## Presets

Seven of its own (Distortion: Hard Clip, Wave Fold, Bit Crush; Warmth: Subtle Drive, Tube Glow, Warm Tape, Bass Warmth),
and the Exciter's eight, converted, in its Exciter kind ([presets.md](../../presets.md)). The five it had before Colour,
Crush, the Band and Quality came play as they did: those parameters' defaults change nothing (a test played each against
the saturator as it was, to 129 dB under its peak).

The Exciter's presets became these (Band on, Soft, Auto Gain on, Tone 0, Quality 2x, Mix as it was). Each was played
through both devices on one signal (pink noise at -18 dB RMS with sines at 2 and 6 kHz at -12 dB, stereo, 4 s) and compared
in third-octave bands from 100 Hz to 16 kHz; the Drive, and From by up to a third of an octave, were tuned until the bands
met, and the worst band's difference is the last column. The Exciter's band fell 48 dB an octave and this one falls 24, so
the bands beside From are where they differ: From moved a third of an octave up for most of them. Where tuning the Drive
changed the worst band by little, the Drive nearer the start was kept.

| Preset | The Exciter's Frequency, Drive, Colour, Amount, High Cut | From, To | Output | Drive: from the rule, tuned | Worst band: at the rule's settings, tuned |
| --- | --- | --- | --- | --- | --- |
| Saturated Highs | 2.1 kHz, 16 dB, 35 %, 180 %, off | 2646 Hz, none | +8.75 dB (measured at +8.94) | 14.08, 11.33 dB | 4.19, 2.24 dB at 3.2 kHz |
| Gentle Sheen | 10 kHz, 8 dB, 75 %, 25 %, off | 10 kHz, none | +1.94 dB | 1.01, kept | 0.46 dB at 10.2 kHz |
| Acoustic Presence | 3 kHz, 10 dB, 70 %, 40 %, 12 kHz | 3780 Hz, 12 kHz | +2.92 dB | 4.56, -4.19 dB | 2.55, 0.92 dB at 12.8 kHz |
| Drum Bus Snap | 5 kHz, 14 dB, 50 %, 40 %, off | 6300 Hz, none | +2.92 dB | 8.56, 10.06 dB | 0.91, 0.28 dB at 5.1 kHz |
| Snare Crack | 3 kHz, 20 dB, 30 %, 60 %, 14 kHz | 3780 Hz, 14 kHz | +4.08 dB | 15.74, 12.99 dB | 2.80, 1.67 dB at 12.8 kHz |
| Synth Bite | 2.5 kHz, 20 dB, 35 %, 60 %, 16 kHz | 3150 Hz, 16 kHz | +4.08 dB | 15.74, 13.24 dB | 2.39, 1.82 dB at 16.1 kHz |
| Top End Restore | 3.5 kHz, 24 dB, 50 %, 80 %, off | 4410 Hz, none | +5.11 dB | 20.48, 17.73 dB | 3.09, 2.51 dB at 5.1 kHz |
| Vocal Air | 8 kHz, 10 dB, 60 %, 50 %, off | 10 kHz (From's highest), none | +3.52 dB | 5.23, -3.52 dB | 0.88, 0.34 dB at 12.8 kHz |

Two stay over 2 dB in one band beside From: Saturated Highs by 0.24 dB and Top End Restore by 0.51 dB. Saturated Highs ships with its Output 0.19 dB under the rule's lift, so that on the presets test's loud beat it peaks under 6 dB over full scale (at +8.94 dB it reached 6.05).

## Limits

- One band: no separate bands with their own settings (the [Multiband](multiband.md) saturates three).
- Six fixed curves: no curve to draw.
- No dynamics, and no filters before or after the curve other than the Tone's tilt and the Band.
- The difference tones two tones in the band make under it are taken off 12 dB an octave, not kept out: the Exciter's
  filter after its curve fell 48 dB an octave, at 2 ms of latency.
- At 2x a loud tone high in the band, driven hard into Fold, aliases 15 dB under itself; 4x or 8x are for that.
- A Quality set to 2x by hand can't be raised by automation, since 4x and 8x come a sample later than its latency.
- Auto Gain only undoes the Drive; it does not match the loudness of what comes out to what went in.
- A drag on the curve starts from the Drive as modulation plays it, and sets that as the value by hand.

## In the code

- DSP: `src/devices/saturation/Saturator.h`, `src/devices/saturation/Saturator.cpp`; the curves and their
  anti-aliasing in `src/devices/dsp/Saturation.h`; the crossovers in `src/devices/dsp/Crossover.h`.
- Editor: `src/ui/devices/SaturatorEditor.h`, `src/ui/devices/SaturatorEditor.cpp`.
- Tests: `tests/SaturatorTests.cpp`, `tests/AntialiasingTests.cpp`, `tests/EffectEditorTests.cpp`,
  `tests/GraphDragTests.cpp`.
- Presets: `resources/presets/Saturator/` (see [presets.md](../../presets.md)).
