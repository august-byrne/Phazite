# Vinyl

*Design section 9.*

What a record and its player do to a sound: the pitch that swings as the record turns, the groove's distortion of the
loud highs, the lows in mono, the surface's hiss and crackle, and the player's narrower band and stereo. It is the
lo-fi, hip-hop and synthwave staple, in the manner of iZotope's Vinyl, XLN's RC-20 and Ableton's Vinyl Distortion.

## Parameters

The editor groups them as Wow, Noise and Groove, with the band's two cuts on the picture.

### Wow

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Wow | 0 to 25 cents | 4 cents | How far the pitch swings once a turn, as a warped or off-centre record's does. |
| Wow Rate | 0.25 to 4 Hz | 0.56 Hz | How fast the record turns: 0.56 Hz is 33 1/3 turns a minute, 0.75 Hz is 45 and 1.3 Hz is 78. The editor calls it Rate and shows the turns a minute on the record. |
| Flutter | 0 to 10 cents | 0 cents | The drive's quicker, unsteady wobble: noise below 6 Hz on the pitch. |

### Noise

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Hiss | -96 to -30 dB | -64 dB | The groove's steady hush, as its RMS level. At its lowest it is off. |
| Crackle | -96 to -6 dB | -30 dB | The dust's clicks and pops: the largest ones' peak. At its lowest it is off. |
| Density | 0.5 to 500 Hz | 5 Hz | How many clicks come a second, on average: a rate, not a pitch. |

### Groove

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Distortion | 0 to 100 % | 15 % | The groove's tracing distortion: the loud highs break up and the bass stays clean. |
| Width | 0 to 100 % | 90 % | How far apart the player's two sides are: 100 % as the record has them, 0 % mono. |
| Bass Mono | 20 to 500 Hz | 100 Hz | Below it the lows are summed to mono, as records are cut. At its lowest it is off. |

### The band and the mix

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Low Cut | 20 Hz to 1 kHz | 30 Hz | The low end of the band the player keeps, 12 dB an octave. |
| High Cut | 1 to 20 kHz | 14 kHz | The high end of the band the player keeps, 12 dB an octave. |
| Mix | 0 to 100 % | 100 % | How much of what plays is the record's sound. The device's runner applies it, not the vinyl; it is in the device's line in the Devices view ([Devices](../devices.md)). |

## How it works

The sound goes through the record in the order a record makes it.

**The cut.** The side's lows below Bass Mono are taken off by a Linkwitz-Riley crossover, and the middle is kept in
phase with what is left.

**Wow and flutter.** Both move one delay line, which runs at twice the sample rate and is read by 4-point Lagrange
interpolation, so that the top keeps its level as the delay moves. The wow swings once a turn, with a quarter as much
again at twice the rate, as a warp's second hump, so the record is flat for longer than it is sharp. The flutter is
noise below 6 Hz. The wow's depth and rate glide over 50 ms, and how far they swing the delay moves no faster than
would bend the pitch 7 cents, so a deep wow turned up never swoops. The delay line's middle is the device's latency,
about 11 ms.

**The groove's distortion.** What is above 2 kHz (a high-pass of 12 dB an octave) is driven into a lopsided curve,
which gives a second harmonic first and a third as it is pushed; at 100 % the highs are pushed 12 dB into it. Only
what the curve bends is added to the sound, which itself passes untouched, so at 0 % there is no curve at all. The
curve plays at twice the rate by its antiderivative, so that it doesn't alias.

**The surface noise.** The hiss is pink from 60 Hz to 2 kHz and falls 6 dB an octave more from 2.1 kHz, the RIAA
curve's corner, half alike on the two sides. The crackle is clicks at random times: most small and bright, a few large
and dull, each one the slope of a bump, as a stylus thrown over a speck plays. Whether a moment of the song crackles,
and how, comes from a hash of where the song is, in steps of a 4096th of a beat, so a passage crackles the same
however it is reached, and playback, a render and a freeze play the same clicks. The noise plays while the song plays,
from a silent start too. It fades out over 20 ms when the song stops, and 2 s after the sound coming in stops, as a
run-out groove ends, so an export ends with the song.

**The player.** After the noise, the two sides are narrowed by the Width, and the Low Cut and the High Cut, Butterworth
filters, roll the band's ends off, the noise's too.

**The mix.** The device's runner blends what the record plays with what came in, which the vinyl passes the record's
linear way itself, at every Mix, 0 % too: through the oversampling and the delay line read at its middle, as the sound
is, and through Bass Mono's all-pass while Bass Mono is on (as it comes on or goes off, as far as the sound's own
split has come in, over 20 ms), but not what moves or colours the sound (the wow and flutter, the distortion, the
side's lows, the noise, the width and the band). Merely delayed, the copy cancelled the sound where those turn its
phase: half and half, the middle 17 dB down at Bass Mono's frequency and the top octave 5 to 18 dB down at 15 and 18
kHz; and a copy merely delayed at 0 % but turned above it set the two half a cycle apart at Bass Mono's frequency at a
Mix resting a little over 0 %. Now what is left between the two is the record's own doing, at any Mix. So at a Mix of
0 % what comes out is what came in with its phase turned (by Bass Mono's all-pass while it is on, and in the top
octave by the oversampling), its level within a hundredth of a decibel to 5 kHz; at the top the delay line's read between
samples, the sound's own, takes 0.04 dB off at 10 kHz, 0.19 at 15 and 0.37 at 18 kHz (at 44.1 kHz; 0.03, 0.13 and 0.27
at 48 kHz). The half-band filters themselves cost nothing in level, only phase. At 100 % the vinyl doesn't make the copy, which nothing
hears, once the Mix has been there a second (so that a Mix moved through 100 % and back, or automation touching it,
goes on with the copy it had): it keeps only the copy's way into its delay line, at twice the rate. As the Mix leaves 100 %
after that, it makes the way back again from the line's last 128 samples, the same work at any rate (a few 32-sample
calls' worth), so that the copy comes in without a click.

Because its noise must start with the song whether sound comes in or not, its runner puts the vinyl to sleep only while
the song is stopped, and wakes it as the song plays (`Processor::playsWithTheSong()`). It rests on its own as well while
the song is stopped, nothing comes in and its delay line has emptied.

## Its editor

The record turning at its speed, drawn off its centre as far as the wow swings it, its label's mark turning with it,
the stylus lit as a crackle comes, and a speck where each crackle came, which turns away with the record and fades.
The record shows its turns a minute. Beside it is the pitch the record played over the last three seconds, with the
pitch now. Under them is the band the player keeps, between the Low Cut and the High Cut, with the two cuts' knobs at
its ends.

A drag up or down on the record or the pitch turns the Wow, as one undo step, and the wheel turns the Wow Rate. The
band's ends drag sideways, the nearer one lit. Beside the picture are Wow, Rate and Flutter; Hiss, Crackle and Density;
and Distortion, Width and Bass Mono.

## Limits

- At a Mix below 100 % the wow's moving delay against what came in sweeps a gentle comb through the sound, as two
  copies at different speeds do (a chorus); the band's filters turn its edges' phase against what came in too.
- With Bass Mono on, the phase of the lows is turned at every Mix, 0 % too: its all-pass is part of the sound's way,
  and the copy for the mix takes it so that the two meet in phase. So the vinyl at 0 % is not a bypass: an untouched
  copy of the sound played beside it (a parallel track, a send) cancels around Bass Mono's frequency, as against any
  all-pass. The device's switch bypasses it.
- It adds about 11 ms of latency, which Phazite makes up for on the other tracks.
- No rumble or hum, no spin-down, no scratches that tick once a turn, and no wear or year control that sets the band,
  the noise and the stereo together, where iZotope's Vinyl has them.
- It is the record alone: none of the other modules that RC-20 puts beside its noise and wobble.

## In the code

- DSP: `src/devices/vinyl/Vinyl.h`, `src/devices/vinyl/Vinyl.cpp`; the hiss and the crackle,
  `src/devices/vinyl/SurfaceNoise.h`, `src/devices/vinyl/SurfaceNoise.cpp`; the curve and its oversampling,
  `src/devices/dsp/Saturation.h`, `src/devices/dsp/Oversampling.h`; the delay line, `src/devices/dsp/DelayLine.h`.
- Editor: `src/ui/devices/VinylEditor.h`, `src/ui/devices/VinylEditor.cpp`.
- Tests: `tests/VinylTests.cpp`, `tests/EffectEditorTests.cpp` and `tests/DragPointerTests.cpp` (the editor),
  `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Vinyl/` ([presets.md](../../presets.md)).
