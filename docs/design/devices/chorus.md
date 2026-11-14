# Chorus

*Design section 9.*

The sound with copies of itself a few milliseconds behind, each copy's delay swept by an LFO so that its pitch wavers a
little, as several players never quite in tune sound. With one copy a side, the sides opposite, it is a classic two-line
chorus; with three and Shimmer, a string machine's ensemble.

## Parameters

The editor groups them as Sweep, LFO, Voices and Tone.

### Sweep

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Delay | 1 to 25 ms | 3.5 ms | The middle of the sweep: how far behind the copies are on average. |
| Depth | 0 to 100 % | 50 % | How far the sweep goes each way, as a part of the Delay: at the defaults, from 1.75 to 5.25 ms. |

### LFO

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sync | Off, On | Off | On, a sweep takes the Division at the song's tempo; off, the Rate. |
| Division | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar, 2 bars, 4 bars, 8 bars | 1 bar | How long one sweep takes while Sync is on. The editor calls it Note. |
| Rate | 0.05 to 20 Hz | 0.5 Hz | How fast the sweep is while Sync is off. |
| Shape | Sine, Triangle | Triangle | The LFO's wave. |

### Voices

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Voices | 1 to 8 | 1 | How many copies each side has. Together they are as loud as one. A change crossfades over 10 ms. |
| Voice Phase | 0 to 100 % | 100 % | How far apart in the LFO's cycle a side's copies are. At 100 % the copies of both sides are spread evenly round the cycle (three a side, a third of a cycle apart); closer, they swirl together. The editor calls it Phase. |
| Spread | 0 to 100 % | 0 % | Gives each copy a part of the sweep to itself, the first the shortest delays and the last the longest: at 0 % every copy sweeps all of it, at 100 % the parts lie end to end. |
| Shimmer | 0 to 100 % | 0 % | Adds to the sweep a sine ten times as fast and, at 100 %, a tenth as deep, as a string machine's second LFO does. |

### Tone

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Stereo | 0 to 100 % | 100 % | How far ahead in the cycle the right side's LFO is, up to half a cycle: at 100 % the two sides sweep opposite ways. |
| Feedback | 0 to 90 % | 0 % | Sends the copies back into the line, toward a flanger's ring. |
| High Cut | 1 to 20 kHz | 20 kHz | Darkens the copies and what they feed back, as a bucket-brigade line does. At its top it is off. |
| Safe Bass | 20 Hz to 1 kHz | 20 Hz | Leaves what is below it as it was: the copies are made only from what is above. At its lowest it is off. |
| Mix | 0 to 100 % | 50 % | How much of what plays is the copies. The device's runner applies it, not the chorus; it is in the device's line in the Devices view ([Devices](../devices.md)). |

At 50 % Mix the sound and its copies are as loud as each other. Above it the copies are the louder, and at 100 % only
the copies play, which with one voice is a vibrato.

## How it works

Each side has one delay line that every copy of that side reads, by 4-point Lagrange interpolation. A copy's delay is
the Delay times one plus the Depth times the sweep, where the sweep is the LFO's shape, with Shimmer's fast sine on it,
read at the copy's own place in the cycle (the right side ahead by Stereo, each voice after the one before by Voice
Phase) and, with Spread, squeezed into the copy's own part of the whole sweep. New delays are worked out every 8
samples and glide between, and no copy lags by less than 2 samples. At the defaults it is the first mode of the
Juno-60's chorus: one copy a side, swept from 1.75 to 5.25 ms by a triangle at 0.5 Hz, the sides opposite.

Each copy's share of the output is one over the square root of the number of voices, so that several copies together
are as loud as one wherever they don't line up, which is most of the spectrum; in the deep bass, where they do, they
are louder, which Safe Bass keeps out. What goes back into the line is the copies' average times the Feedback, so the
loop never gains, and the output is scaled so that the comb's peaks rise by as many decibels as what lies between them
falls. High Cut is a 12 dB an octave low-pass on what goes into the line.

When Voices changes, the copies that play fade out where they are while a set of the new number fades in, so that no
copy jumps to a new place.

Safe Bass splits the sound with a Linkwitz-Riley crossover of 24 dB an octave: the copies hear only the band above the
frequency, and the band below goes around them. The dry sound that the runner mixes back in is split and added up the
same way, so the two stay in phase at any Mix. Turned on, it fades in over 10 ms at 5 Hz, under what is heard, and
glides up to its frequency; turned off, it glides down and fades out, so it never clicks. The [Flanger](flanger.md)
and the [Phaser](phaser.md) share it.

The LFO is the one the flanger and the phaser use. It starts at the bottom of the sweep on the downbeat. In time with
the song, while the song plays, its phase comes from where the song is, so a loop sweeps alike each time round and a
render is the same each time; when the song jumps, the phase glides there over 10 ms; stopped, it runs free.

The chorus has no latency.

## Its editor

The comb that the copies make with the sound now, over the spectrum of what the chorus plays, the left side's lit and
the right side's fainter, sliding as the copies sweep. Under it is a strip of the delay in milliseconds, drawn as a
tape's heads: how far the sweep reaches and where each copy lags now, each voice on a line of its own with the part of
the sweep that Spread gives it. Under that is the LFO: one cycle of the sweep, with Shimmer's sine on it, and a dot for
each copy where Voice Phase puts it.

A dot on the comb sits at the Delay's frequency (one over the Delay), as high as the Depth. Dragging it moves both as
one undo step, a press anywhere puts it under the pointer, and the wheel turns the Rate (while Sync is on, the
Division). Beside the picture are Delay and Depth as large knobs, the LFO's controls, the Voices part and the Tone
part. Phase and Spread show faint while there is one voice a side, which has nothing to keep apart.

## Limits

- No bucket-brigade character beyond the High Cut: no noise, companding or clock aliasing, where a modelled chorus
  has them.
- No vibrato mode of its own (one voice at 100 % Mix is one) and no level for the copies apart from the Mix.
- No pan or width for single voices: every copy of a side plays on that side.
- The LFO is a sine or a triangle only, and one LFO sweeps every voice.
- Where the copies line up, in the deep bass, several voices are louder than one unless Safe Bass keeps the bass out.

## In the code

- DSP: `src/devices/motion/Chorus.h`, `src/devices/motion/Chorus.cpp`; Safe Bass, `src/devices/motion/SafeBass.h`;
  the LFO, `src/devices/dsp/SyncedLfo.h`, and its note values, `src/devices/dsp/NoteValues.h`; the line,
  `src/devices/dsp/DelayLine.h`; the crossover, `src/devices/dsp/Crossover.h`.
- Editor: `src/ui/devices/ChorusEditor.h`, `src/ui/devices/ChorusEditor.cpp`; what it shares with the flanger's and
  the phaser's, `src/ui/devices/SweepPicture.h`.
- Tests: `tests/ChorusTests.cpp`, `tests/MotionEffectTests.cpp` (with `tests/MotionTestHelpers.h`),
  `tests/EffectEditorTests.cpp` and `tests/DragPointerTests.cpp` (the editor), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Chorus/` ([presets.md](../../presets.md)).
