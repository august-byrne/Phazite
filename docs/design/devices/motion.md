# Motion

*Design section 9.*

The effects that sweep the sound with an LFO, as one device in three modes: **Chorus**, copies of the sound a few
milliseconds behind, each copy's delay swept so that its pitch wavers a little, as several players never quite in tune
sound; **Flanger**, the sound mixed with one copy of itself whose delay is swept, so that a comb of notches slides
through it, the whoosh of tape flanging; and **Phaser**, the sound mixed half and half with itself through all-pass
filters whose phase the LFO sweeps, so that notches move up and down the spectrum, as the classic four-stage phaser
pedals do. One device means one line in the effects' menu, and Swap, which lists one device's presets, tries a chorus,
a flanger and a phaser on a sound in place.

Each mode is played by an engine of its own (`src/devices/motion/Chorus.h`, `Flanger.h`, `Phaser.h`), which were the
Chorus, the Flanger and the Phaser before roadmap step S6.10 made them one device. The three share their LFO, Feedback,
Stereo and Safe Bass; a mode ignores the parameters it doesn't use. A new Motion is a chorus at the defaults of the
Juno-60's first mode.

## Parameters

The editor shows each mode's parameters on its own page, under the modes' tabs: Sweep, LFO, then the mode's own parts,
then Stereo and Bass.

### Mode

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | Chorus, Flanger, Phaser | Chorus | Which effect plays. A change fades out over 10 ms and the new mode in over 10 ms (below). |

### Sweep

| Parameter | Range | Default | Modes | What it does |
| --- | --- | --- | --- | --- |
| Delay | 0.2 to 25 ms | 3.5 ms | Chorus, Flanger | The chorus's: the middle of the sweep, how far behind the copies are on average. The flanger's: the longest the copy lags, the top of the sweep. |
| Frequency | 50 Hz to 10 kHz | 800 Hz | Phaser | The middle of the sweep. |
| Depth | 0 to 100 % | 50 % | All | The chorus's: how far the sweep goes each way, as a part of the Delay (at the defaults, from 1.75 to 5.25 ms). The flanger's: how far the LFO shortens the delay from the Delay, by five octaves at 100 %. The phaser's: how far the LFO moves the notches from the Frequency, two octaves each way at 100 %. |

### LFO

The same in every mode.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sync | Off, On | Off | On, a sweep takes the Division at the song's tempo; off, the Rate. |
| Division | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar, 2 bars, 4 bars, 8 bars | 1 bar | How long one sweep takes while Sync is on. The editor calls it Note. |
| Rate | 0.05 to 20 Hz | 0.5 Hz | How fast the sweep is while Sync is off. |
| Shape | Sine, Triangle | Triangle | The LFO's wave. |

### The modes' own

| Parameter | Range | Default | Modes | What it does |
| --- | --- | --- | --- | --- |
| Feedback | -100 to +100 % | 0 % | All | How far the mode's loop rings, 100 % as far as its engine goes: the chorus's copies sent back into the line up to 90 %, toward a flanger's ring, or below 0 upside down, the peaks where the notches were; the flanger's comb, from no loss to 2 % lost each time round, above 0 ringing at the multiples of one over the delay (metallic), below 0 hollowing out there instead, which also takes the bass unless Safe Bass keeps it; the phaser's up to 85 %, ringing as a filter's resonance does, above 0 at the peaks between the notches, below 0 at the notches themselves, which fill in and then turn into peaks. |
| Notches | 1 to 6 | 2 | Phaser | How many notches: a second-order all-pass section each, which is two of a classic phaser's stages. |
| Spacing | 0 to 100 % | 50 % | Phaser | Draws the notches together or spreads them apart: the all-pass sections' Q, from 2 at 0 % through 0.5 at 50 % to 0.125 at 100 %. |
| High Cut | 1 to 20 kHz | 20 kHz | Chorus | Darkens the copies and what they feed back, as a bucket-brigade line does. At its top it is off. |
| Voices | 1 to 8 | 1 | Chorus | How many copies each side has. Together they are as loud as one. A change crossfades over 10 ms. |
| Voice Phase | 0 to 100 % | 100 % | Chorus | How far apart in the LFO's cycle a side's copies are. At 100 % the copies of both sides are spread evenly round the cycle (three a side, a third of a cycle apart); closer, they swirl together. The editor calls it Phase. |
| Spread | 0 to 100 % | 0 % | Chorus | Gives each copy a part of the sweep to itself, the first the shortest delays and the last the longest: at 0 % every copy sweeps all of it, at 100 % the parts lie end to end. |
| Shimmer | 0 to 100 % | 0 % | Chorus | Adds to the sweep a sine ten times as fast and, at 100 %, a tenth as deep, as a string machine's second LFO does. |

### Stereo and Bass

The same in every mode.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Stereo | 0 to 100 % | 100 % | How far ahead in the cycle the right side's LFO is, up to half a cycle: at 100 % the two sides sweep opposite ways. |
| Safe Bass | 20 Hz to 1 kHz | 20 Hz | Leaves what is below it as it was: the effect hears only what is above. At its lowest it is off. |
| Mix | 0 to 100 % | 50 % | How much of what plays is the effect (below). The device's runner applies it, not Motion; it is in the device's line in the Devices view ([Devices](../devices.md)). |

## One meaning of Mix

In every mode, what Motion puts out is the effect alone, which its runner blends with the sound coming in: half and
half at 50 %, its default, and only the effect at 100 %.

- **Chorus:** the effect is the copies. At 50 % the sound and its copies are as loud as each other; above it the
  copies are the louder, and at 100 % only the copies play, which with one voice is a vibrato.
- **Flanger and Phaser:** their engines put out the sound and its copy already mixed, half and half, `y`. Motion puts
  out `2 y - x`, `x` being the dry copy its runner mixes back in (after Safe Bass's split, so that the two stay in
  phase): at 50 % Mix the runner plays `0.5 x + 0.5 (2 y - x) = y`, the full flanger or phaser, and a mode switched at
  the default Mix sounds whole. Below 50 % the notches are shallower, as less of the copy meets the sound; above it the
  copy outweighs the sound and they grow shallower again, until at 100 % only the copy plays: without feedback, the
  phaser's all-pass sections alone, flat, and the flanger's swept copy alone, a vibrato.

## Switching modes

The Mode is a choice parameter like any other: automation moves it, and Undo puts it back. When it changes, the effect
fades out over 10 ms toward the sound as it came in, the new mode's engine is reset (its lines empty, its LFO at the
start of its cycle) and fades in over 10 ms, as the Reverb changes its Mode. What each engine is fed fades with it, so
that no sound cut in at full level clicks in its lines; what it puts out and the dry copy for the Mix fade alike, so
that at the bottom of the fade the sound passes untouched, at any Mix. Since what an engine plays is already faded with
what it is fed, it is faded a second time only near the bottom (by four times the fade's gain, up to whole), so that
the level holds through the change: where the effect is in phase with the sound, no more than 0.6 dB down. The fade is
counted in samples, so that it plays the same however the samples are cut into blocks. A mode asked for again as it
fades out fades back in from where it was. The tail Motion reports, which its runner waits for before it lets it
sleep, is the tail of the mode that plays.

## How each mode works

The three share the LFO and Safe Bass.

The LFO starts at the bottom of the sweep on the downbeat. In time with the song, while the song plays, its phase comes
from where the song is, so a loop sweeps alike each time round and a render is the same each time; when the song
jumps, the phase glides there over 10 ms; stopped, it runs free. Each mode works out its delays or coefficients every
8 samples, on a grid counted from the song's place while it plays (on from where it was while it is stopped), so that
it plays the same however the samples are cut into blocks, round a loop, from any start and after a sleep, as a render
of the song does.

Safe Bass splits the sound with a Linkwitz-Riley crossover of 24 dB an octave: the effect hears only the band above
the frequency, and the band below goes around it. The dry sound that the runner mixes back in is split and added up
the same way, so the two stay in phase at any Mix. Turned on, it fades in over 10 ms at 5 Hz, under what is heard, and
glides up to its frequency; turned off, it glides down and fades out, so it never clicks.

Motion has no latency.

### Chorus

Each side has one delay line that every copy of that side reads, by 4-point Lagrange interpolation. A copy's delay is
the Delay times one plus the Depth times the sweep, where the sweep is the LFO's shape, with Shimmer's fast sine on it,
read at the copy's own place in the cycle (the right side ahead by Stereo, each voice after the one before by Voice
Phase) and, with Spread, squeezed into the copy's own part of the whole sweep. New delays glide between their control
steps (from where a glide got to, where the song jumps in the middle of a step), and no copy lags by less than 2
samples. At the defaults it is the first mode of the Juno-60's chorus: one copy a side, swept from 1.75 to 5.25 ms by
a triangle at 0.5 Hz, the sides opposite.

Each copy's share of the output is one over the square root of the number of voices, so that several copies together
are as loud as one wherever they don't line up, which is most of the spectrum; in the deep bass, where they do, they
are louder, which Safe Bass keeps out. What goes back into the line is the copies' average times the Feedback (90 %
of it at the knob's 100 %, either way), so the loop never gains, and the output is scaled by the Feedback's square, so
that the comb's peaks rise by as many decibels as what lies between them falls, whichever its sign. High Cut is a
12 dB an octave low-pass on what goes into the line.

When Voices changes, the copies that play fade out where they are while a set of the new number fades in, so that no
copy jumps to a new place.

### Flanger

Each side is a comb filter, the one behind the Comb + and Comb - types of the [Filter](filter.md) and the synths: a
delay line whose output is fed back into it and added to what goes in. The copy's delay is the Delay shortened by up
to five octaves, as the Depth and the LFO say, and never less than 2 samples, gliding between its control steps and
read between samples by 4-point Lagrange interpolation.

The Feedback sets how much the loop loses each time round, from everything at 0 % to 2 % at full. The gains into and
out of the loop follow it, so that the comb's notches are as deep at any feedback and its peaks rise by as many
decibels as what lies between them falls, up to 8.5 dB: the level stays about the same wherever the knob is. When the
Feedback crosses 0, its sign glides through nothing over 10 ms, so a turn across it doesn't click.

### Phaser

Each side has up to six second-order all-pass sections in a row, all at the same frequency: the Frequency moved by the
LFO, up to two octaves each way, and kept between 20 Hz and 0.45 times the sample rate. At 50 % Spacing each section
has a Q of a half, which makes it two first-order stages alike, the classic design. What comes out of the sections is
added to the sound and the sum halved: where the copy is in phase the sound keeps its level, and where it is upside
down, at the notches, nothing is left.

The Feedback sends the sections' output back into their input, 85 % of it at the knob's 100 %, either way. The loop is
solved within each sample: before the sections take a sample, each says what it would put out for any input (a gain
and an offset), and the input that satisfies the loop is worked out from those, so there is no one-sample delay in the
feedback to detune the notches, and it can't run away.

## Its editor

The three modes are tabs along the top, each with a tooltip that says what it is; a click on one sets the Mode as one
undo step and shows its page, and a Mode set elsewhere (an undo, automation) shows its page as the editor refreshes.
The editor keeps its size from mode to mode. The LFO's part and the Stereo and Bass part are the same controls on
every page.

| Mode | Its page, in order |
| --- | --- |
| Chorus | its picture; Sweep (Delay, Depth, large); LFO (Sync, Note, Rate, Shape); Voices (Voices, Phase, Spread, Shimmer); Tone (Feedback, High Cut); Stereo and Bass (Stereo, Safe Bass) |
| Flanger | its picture; Sweep (Delay, Depth, large); LFO; Tone (Feedback); Stereo and Bass |
| Phaser | its picture; Sweep (Frequency, Depth, large); LFO; Tone (Notches, Feedback, Spacing); Stereo and Bass |

Each picture shows the response of what Motion plays, the mode's effect mixed with the sound at the Mix set (so that it
moves when the Mix does), the left side's lit and the right side's fainter, over the spectrum of what Motion plays,
with what comes in faint behind, and under it the LFO: one cycle of the sweep with a dot for each side's copy, and its
rate. Each reads Motion's values through the same mapping to its engine's that plays them.

- **Chorus:** the comb that the copies make with the sound now, sliding as the copies sweep. Under it is a strip of
  the delay in milliseconds, drawn as a tape's heads: how far the sweep reaches and where each copy lags now, each
  voice on a line of its own with the part of the sweep that Spread gives it; the LFO shows Shimmer's sine on its curve
  and a dot for each copy where Voice Phase puts it. A dot on the comb sits at the Delay's frequency (one over the
  Delay), as high as the Depth; the wheel turns the Rate (while Sync is on, the Division). Phase and Spread show faint
  while there is one voice a side, which has nothing to keep apart.
- **Flanger:** the comb, a tooth at each notch, sliding as the LFO sweeps, and under it the strip of the delay: how far
  the sweep reaches and where each side's copy lags now. The teeth are as strong as the notches are deep as heard: at
  a notch the copy cancels the sound and the dry copy alone is heard, |1 - 2 m| of it, so the teeth are whole at 50 %,
  half at 25 and 75 %, and gone at 100 %, where the copy plays alone and has no notches (the curve is flat there without
  Feedback, and peaks with it). A dot sits at the Delay's frequency, one over the Delay (the
  comb's first peak, or its first notch while the Feedback is below 0), as high as the Feedback, with 0 in the middle;
  the wheel turns the Depth.
- **Phaser:** the notches, moving as the LFO sweeps them, the left side's response glowing down to the bottom, so that
  each notch reads as a valley carved in the sound, and along the bottom how far the sweep reaches. A dot sits at the
  Frequency, as high as the Feedback, with 0 in the middle; the wheel turns the Depth.

A drag from anywhere on the picture moves the dot's two values as one undo step, as far as the pointer goes (a press
beside the dot doesn't move it there), a fifth as far with Shift; a double-click puts back both, and a right-click shows
both menus.

## Limits

- No through-zero flanging: the flanger's copy never lags by less than 2 samples and the sound itself isn't delayed,
  since that would need latency.
- No bucket-brigade or tape character beyond the chorus's High Cut: no noise, companding or clock aliasing, and no
  High Cut in the flanger.
- The LFO is a sine or a triangle only, and one LFO sweeps every voice. There is no envelope follower inside it; a
  track's modulators can move the Delay or the Frequency instead
  ([Automation and modulation](../automation-and-modulation.md)).
- The chorus has no pan or width for single voices: every copy of a side plays on that side. Where its copies line up,
  in the deep bass, several voices are louder than one unless Safe Bass keeps the bass out.
- The phaser's all-pass sections move once every 8 samples, not every sample, so a very fast, deep sweep moves in
  small steps; every section sits at the same frequency, with no modelled spread between stages and no barber-pole
  mode; and its Feedback, -100 to +100 % on the knob, sends back at most 85 % of what the sections put out (the
  knob's 100 %), so it can't be pushed into self-oscillation.
- A change of Mode takes 20 ms, the effect passing the sound untouched at its middle: it is for switching between
  sections of a song, not for a sweep between two modes.

## In the code

- DSP: `src/devices/motion/Motion.h`, `src/devices/motion/Motion.cpp` (the device, its parameters, the mapping to its
  engines and the switch between them); its engines, `src/devices/motion/Chorus.h`, `Flanger.h`, `Phaser.h` (with
  their `.cpp`); Safe Bass, `src/devices/motion/SafeBass.h`; the LFO, `src/devices/dsp/SyncedLfo.h`, and its note
  values, `src/devices/dsp/NoteValues.h`; the chorus's line, `src/devices/dsp/DelayLine.h`; the flanger's comb,
  `src/devices/dsp/CombFilter.h`; the phaser's all-pass sections, `src/devices/dsp/Filters.h`; the crossover,
  `src/devices/dsp/Crossover.h`.
- Editor: `src/ui/devices/MotionEditor.h`, `src/ui/devices/MotionEditor.cpp`; what its three pictures share,
  `src/ui/devices/SweepPicture.h`.
- Tests: `tests/MotionTests.cpp` (each mode plays what its device played, the switch, the editor);
  `tests/ChorusTests.cpp` and `tests/MotionEffectTests.cpp` (with `tests/MotionTestHelpers.h`), the engines' DSP;
  `tests/DragPointerTests.cpp` and `tests/GraphDragTests.cpp` (its pictures' dots), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Motion/` ([presets.md](../../presets.md)).
