# Phaser

*Design section 9.*

The sound mixed half and half with itself through all-pass filters whose phase an LFO sweeps, so that notches move up
and down the spectrum, as the classic four-stage phaser pedals do.

## Parameters

The editor groups them as Sweep, LFO, Tone, and Stereo and Bass.

### Sweep

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Frequency | 50 Hz to 10 kHz | 800 Hz | The middle of the sweep. |
| Depth | 0 to 100 % | 60 % | How far the LFO moves the notches from there: at 100 %, two octaves each way. |

### LFO

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sync | Off, On | On | On, a sweep takes the Division at the song's tempo; off, the Rate. |
| Division | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar, 2 bars, 4 bars, 8 bars | 1 bar | How long one sweep takes while Sync is on. The editor calls it Note. |
| Rate | 0.05 to 20 Hz | 0.5 Hz | How fast the sweep is while Sync is off. |
| Shape | Sine, Triangle | Sine | The LFO's wave. |

### Tone

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Notches | 1 to 6 | 2 | How many notches: a second-order all-pass section each, which is two of a classic phaser's stages. |
| Feedback | -85 to +85 % | 30 % | Makes it ring, as a filter's resonance does: above 0 at the peaks between the notches; below 0 at the notches themselves, which fill in and then turn into peaks. |
| Spacing | 0 to 100 % | 50 % | Draws the notches together or spreads them apart: the all-pass sections' Q, from 2 at 0 % through 0.5 at 50 % to 0.125 at 100 %. |

### Stereo and Bass

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Stereo | 0 to 100 % | 50 % | How far ahead in the cycle the right side's LFO is, up to half a cycle. |
| Safe Bass | 20 Hz to 1 kHz | 20 Hz | Leaves what is below it as it was: the notches sweep only what is above. At its lowest it is off. |
| Mix | 0 to 100 % | 100 % | How much of what plays is the phaser's sound, which already holds the sound and its phased copy half and half. The device's runner applies it, not the phaser; it is in the device's line in the Devices view ([Devices](../devices.md)). |

## How it works

Each side has up to six second-order all-pass sections in a row, all at the same frequency: the Frequency moved by the
LFO, up to two octaves each way, and kept between 20 Hz and 0.45 times the sample rate. At 50 % Spacing each section
has a Q of a half, which makes it two first-order stages alike, the classic design. What comes out of the sections is
added to the sound and the sum halved: where the copy is in phase the sound keeps its level, and where it is upside
down, at the notches, nothing is left.

The Feedback sends the sections' output back into their input. The loop is solved within each sample: before the
sections take a sample, each says what it would put out for any input (a gain and an offset), and the input that
satisfies the loop is worked out from those, so there is no one-sample delay in the feedback to detune the notches,
and it can't run away.

The sections' coefficients are worked out every 8 samples, on a grid counted from the song's place while it plays
(on from where it was while it is stopped), so that it plays the same however the samples are cut into blocks, round a
loop, from any start and after a sleep, as a render of the song does.

Safe Bass splits the sound with a Linkwitz-Riley crossover of 24 dB an octave: the sections hear only the band above
the frequency, and the band below goes around them. The dry sound that the runner mixes back in is split and added up
the same way, so the two stay in phase at any Mix. Turned on, it fades in over 10 ms at 5 Hz, under what is heard, and
glides up to its frequency; turned off, it glides down and fades out, so it never clicks. The [Flanger](flanger.md)
and the [Chorus](chorus.md) share it.

The LFO is the one the flanger and the chorus use. It starts at the bottom of the sweep on the downbeat. In time with
the song, while the song plays, its phase comes from where the song is, so a loop sweeps alike each time round and a
render is the same each time; when the song jumps, the phase glides there over 10 ms; stopped, it runs free.

The phaser has no latency.

## Its editor

The notches over the spectrum of what the phaser plays, moving as the LFO sweeps them: the left side's response lit
and glowing down to the bottom, so that each notch reads as a valley carved in the sound, the right side's fainter,
and what comes in faint behind. Along the bottom is how far the sweep reaches, and under it the LFO: one cycle of the
sweep with a dot for each side, and its rate.

A dot sits at the Frequency, as high as the Feedback, with 0 in the middle. Dragging it moves both as one undo step, a
press anywhere puts it under the pointer, and the wheel turns the Depth. Beside the picture are Frequency and Depth as
large knobs, the LFO's controls, then Notches, Feedback and Spacing, and Stereo and Safe Bass.

## Limits

- The all-pass sections move once every 8 samples, not every sample, so a very fast, deep sweep moves in small steps.
- Every section sits at the same frequency: no modelled spread between stages, and no barber-pole or doubler mode.
- The LFO is a sine or a triangle only. There is no envelope follower inside it; a track's modulators can move its
  Frequency instead ([Automation and modulation](../automation-and-modulation.md)).
- The Feedback stops at 85 % either way, so it can't be pushed into self-oscillation.

## In the code

- DSP: `src/devices/motion/Phaser.h`, `src/devices/motion/Phaser.cpp`; the all-pass sections,
  `src/devices/dsp/Filters.h`; Safe Bass, `src/devices/motion/SafeBass.h`; the LFO, `src/devices/dsp/SyncedLfo.h`,
  and its note values, `src/devices/dsp/NoteValues.h`.
- Editor: `src/ui/devices/PhaserEditor.h`, `src/ui/devices/PhaserEditor.cpp`; what it shares with the flanger's and
  the chorus's, `src/ui/devices/SweepPicture.h`.
- Tests: `tests/MotionEffectTests.cpp` (with `tests/MotionTestHelpers.h`), `tests/EffectEditorTests.cpp` and
  `tests/DragPointerTests.cpp` (the editor), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Phaser/` ([presets.md](../../presets.md)).
