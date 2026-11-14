# Flanger

*Design section 9.*

The sound mixed with a copy of itself a few milliseconds behind, the copy's delay swept by an LFO, so that a comb of
notches slides through the sound: the whoosh of tape flanging.

## Parameters

The editor groups them as Sweep, LFO and Tone.

### Sweep

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Delay | 0.2 to 15 ms | 3 ms | The longest the copy lags: the top of the sweep. |
| Depth | 0 to 100 % | 70 % | How far the LFO shortens the delay from there: at 100 %, by five octaves. |

### LFO

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sync | Off, On | On | On, a sweep takes the Division at the song's tempo; off, the Rate. |
| Division | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar, 2 bars, 4 bars, 8 bars | 2 bars | How long one sweep takes while Sync is on. The editor calls it Note. |
| Rate | 0.05 to 20 Hz | 0.25 Hz | How fast the sweep is while Sync is off. |
| Shape | Sine, Triangle | Triangle | The LFO's wave. |

### Tone

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Feedback | -100 to +100 % | 30 % | Rings the comb. Above 0 it rings at the multiples of one over the delay (metallic); below 0 it hollows out there instead, which also takes the bass unless Safe Bass keeps it. |
| Stereo | 0 to 100 % | 50 % | How far ahead in the cycle the right side's LFO is, up to half a cycle. |
| Safe Bass | 20 Hz to 1 kHz | 20 Hz | Leaves what is below it as it was: the comb hears only what is above. At its lowest it is off. |
| Mix | 0 to 100 % | 100 % | How much of what plays is the flanger's sound, which already holds the sound and its copy. The device's runner applies it, not the flanger; it is in the device's line in the Devices view ([Devices](../devices.md)). |

## How it works

Each side is a comb filter, the one behind the Comb + and Comb - types of the [Filter](filter.md) and the synths: a
delay line whose output is fed back into it and added to what goes in. The copy's delay is the Delay shortened by up
to five octaves, as the Depth and the LFO say, and never less than 2 samples; it is worked out every 8 samples, glides
between, and is read between samples by 4-point Lagrange interpolation.

The Feedback sets how much the loop loses each time round, from everything at 0 % to 2 % at full. The gains into and
out of the loop follow it, so that the comb's notches are as deep at any feedback and its peaks rise by as many
decibels as what lies between them falls, up to 8.5 dB: the level stays about the same wherever the knob is. When the
Feedback crosses 0, its sign glides through nothing over 10 ms, so a turn across it doesn't click.

Safe Bass splits the sound with a Linkwitz-Riley crossover of 24 dB an octave: the comb hears only the band above the
frequency, and the band below goes around it. The dry sound that the runner mixes back in is split and added up the
same way, so the two stay in phase at any Mix. Turned on, it fades in over 10 ms at 5 Hz, under what is heard, and
glides up to its frequency; turned off, it glides down and fades out, so it never clicks. The [Chorus](chorus.md) and
the [Phaser](phaser.md) share it.

The LFO is the one the chorus and the phaser use. It starts at the bottom of the sweep on the downbeat. In time with
the song, while the song plays, its phase comes from where the song is, so a loop sweeps alike each time round and a
render is the same each time; when the song jumps, the phase glides there over 10 ms; stopped, it runs free.

The flanger has no latency.

## Its editor

The comb over the spectrum of what the flanger plays, a tooth at each notch, sliding as the LFO sweeps: the left
side's response lit, the right side's fainter, what comes in faint behind. Under it is a strip of the delay in
milliseconds, drawn as a tape's heads: how far the sweep reaches and where each side's copy lags now. Under that is the
LFO: one cycle of the sweep with a dot for each side, and its rate.

A dot on the comb sits at the Delay's frequency, one over the Delay (the comb's first peak, or its first notch while
the Feedback is below 0), as high as the Feedback, with 0 in the middle. Dragging it moves both as one undo step, a
press anywhere puts it under the pointer, and the wheel turns the Depth. Beside the picture are Delay and Depth as
large knobs, the LFO's controls, and Feedback, Stereo and Safe Bass.

## Limits

- No through-zero flanging: the copy never lags by less than 2 samples and the sound itself isn't delayed, since that
  would need latency.
- The LFO is a sine or a triangle only. There is no envelope follower inside it; a track's modulators can move its
  Delay instead ([Automation and modulation](../automation-and-modulation.md)).
- No tape or bucket-brigade character: the copy is a clean delay.

## In the code

- DSP: `src/devices/motion/Flanger.h`, `src/devices/motion/Flanger.cpp`; the comb, `src/devices/dsp/CombFilter.h`;
  Safe Bass, `src/devices/motion/SafeBass.h`; the LFO, `src/devices/dsp/SyncedLfo.h`, and its note values,
  `src/devices/dsp/NoteValues.h`.
- Editor: `src/ui/devices/FlangerEditor.h`, `src/ui/devices/FlangerEditor.cpp`; what it shares with the chorus's and
  the phaser's, `src/ui/devices/SweepPicture.h`.
- Tests: `tests/MotionEffectTests.cpp` (with `tests/MotionTestHelpers.h`), `tests/EffectEditorTests.cpp` and
  `tests/DragPointerTests.cpp` (the editor), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Flanger/` ([presets.md](../../presets.md)).
