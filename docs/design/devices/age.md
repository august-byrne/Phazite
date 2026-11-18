# Age

*Design section 9.*

What a physical medium does to a sound: a record or a tape. Its Medium picks one; each of its parts (Wobble, Noise,
Drive, Stereo and Band) has a switch, so that a part can be heard on and off, or left out. It is the lo-fi, hip-hop and
synthwave staple, in the manner of iZotope's Vinyl, XLN's RC-20 and Ableton's Vinyl Distortion, one device for an aged
sound in place of one for each medium (the owner, 2026-11-17: "an effect that adds an aged effect (from different
physical media), and you can enable/disable different parts"). It grew out of the Vinyl, whose record it keeps as it
was: a record with every part on plays what the Vinyl played, sample for sample.

## Parameters

The editor has a tab for each medium, Record and Tape, each a page of the parts: the picture with the band's two cuts
and the Band's switch, then Wobble, Noise, Drive and Stereo, each with its switch at the right end of its title. A
medium ignores the parameters that are the other's alone: the record its Dropouts and Head Bump, the tape its Crackle
and Density.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Medium | Record, Tape | Record | The medium: the editor's two tabs. |

### Wobble

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Wobble | on, off | on | The part's switch. |
| Wow | 0 to 25 cents | 4 cents | How far the pitch swings: once a turn, as a warped or off-centre record's does, or as a tape's transport drifts. |
| Wow Rate | 0.25 to 4 Hz | 0.56 Hz | How fast: a record's turns a second (0.56 Hz is 33 1/3 turns a minute, 0.75 Hz is 45 and 1.3 Hz is 78; the record shows its turns a minute), or the tape's drift's rate. The editor calls it Rate. |
| Flutter | 0 to 10 cents | 0 cents | The drive's quicker, unsteady wobble: noise below 6 Hz on a record's pitch, between 4 and 14 Hz on a tape's. |

### Noise

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Noise | on, off | on | The part's switch. |
| Hiss | -96 to -30 dB | -64 dB | The medium's steady hush, as its RMS level: a record's groove's, or a tape's. At its lowest it is off. |
| Crackle | -96 to -6 dB | -30 dB | The record's: the dust's clicks and pops, the largest ones' peak. At its lowest it is off. |
| Density | 0.5 to 500 Hz | 5 Hz | The record's: how many clicks come a second, on average (a rate, not a pitch). |
| Dropouts | 0 to 100 % | 0 % | The tape's: how often and how deep the sound dips where the tape loses the head. |

### Drive

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Drive | on, off | on | The part's switch. |
| Distortion | 0 to 100 % | 15 % | A record's groove's tracing distortion, the loud highs breaking up and the bass staying clean; or a tape's saturation of the whole sound, the highs first. |
| Head Bump | 0 to 6 dB | 0 dB | The tape's: a lift of the lows around 60 Hz, as a playback head gives. |

### Stereo

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Stereo | on, off | on | The part's switch. |
| Width | 0 to 100 % | 90 % | How far apart the two sides are: 100 % as they come, 0 % mono. |
| Bass Mono | 20 to 500 Hz | 100 Hz | Below it the lows are summed to mono, as records are cut. At its lowest it is off. |

### The band and the mix

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Band | on, off | on | The band's switch, under the picture with the band. |
| Low Cut | 20 Hz to 1 kHz | 30 Hz | The low end of the band the medium keeps, 12 dB an octave. |
| High Cut | 1 to 20 kHz | 14 kHz | The high end of the band the medium keeps, 12 dB an octave. |
| Mix | 0 to 100 % | 100 % | How much of what plays is the medium's sound. The device's runner applies it, not Age; it is in the device's line in the Devices view ([Devices](../devices.md)). |

## How it works

The sound goes through the medium in the same order in both: the Stereo part's cut, the wobble, the drive, the noise,
the width, the band.

**The parts' switches.** A part that is off does none of its work and passes the sound as the part before it left it;
its values stay, so that it can be compared on and off. A switch fades its part over 20 ms: the wobble's depth glides
to nothing as it does when the Wow is turned down, so the pitch never jumps; the noise fades; the drive's amount glides
to nothing (and the head bump with it); the width glides to 100 % and Bass Mono goes as it does at its lowest; the
band's filters fade in and out, as the Exciter's High Cut does. A change of medium crossfades each part from one
medium's way to the other's: the drive and the noise over 20 ms, the flutter over a quarter of a second, and the wow's
depth glides to nothing in the old medium's shape, then up again in the new one's, no faster than would bend the pitch
7 cents. The latency stays the same in both media and with any part off, so that switching nothing moves the sound in
time.

**The cut.** The side's lows below Bass Mono are taken off by a Linkwitz-Riley crossover, and the middle is kept in
phase with what is left.

**Wow and flutter.** Both move one delay line, which runs at twice the sample rate and is read by 4-point Lagrange
interpolation, so that the top keeps its level as the delay moves. A record's wow swings once a turn, with a quarter as
much again at twice the rate, as a warp's second hump, so the record is flat for longer than it is sharp; its flutter
is noise below 6 Hz. A tape's wow is its transport's slow, uneven drift: 0.63 of a sine at the rate and 0.27 of noise
low-passed at half the rate (as loud as a sine of the depth), so that it stays within the Wow but for a moment now and
then when the drift and the sine swing the same way (over a minute, its 99th percentile just inside the depth and its
furthest a seventh past it), held inside the record's reach (so that at the deepest wow and the slowest rate its
drift's furthest swings are softly rounded off); its flutter, a capstan's and its rollers', is noise between 4 and 14 Hz,
of a whole unit's depth, gathered into a delay and then made as deep as the Flutter, so that it is nothing the
moment the Flutter is. The wow's depth and rate glide over 50 ms, and how far they swing the delay moves no faster
than would bend the pitch 7 cents, so a deep wow turned up never swoops; and the rate rises only as fast as that
swing comes down, so that a slow, deep wow turned fast bends the pitch no more than twice the Wow on its way. The
flutters' noises come from fixed seeds, the same after every reset, so a render plays as live. The delay line's middle is the device's latency, about
11 ms.

**A record's drive: the groove's distortion.** What is above 2 kHz (a high-pass of 12 dB an octave) is driven into a
lopsided curve, which gives a second harmonic first and a third as it is pushed; at 100 % the highs are pushed 12 dB
into it.

**A tape's drive: its saturation and head bump.** A tape is recorded with its top lifted, so its highs saturate first:
a high shelf of 6 dB at 4 kHz, then a symmetric soft curve (odd harmonics only), up to 9 dB into it at 100 % (the
highs 15 dB, with the shelf) with as much taken back off, then the same shelf turned down again on what the curve
bent. A loud 8 kHz tone is turned down more than a loud 200 Hz one. The curve goes no higher than full scale, so at
100 % the lows come out no louder than -9 dBFS, however loud they come in. Pushed further, the curve's harmonics of a
high tone at -6 dBFS fold back at twice the rate louder than the record's do (18 dB in, 39 dB under the tone; at 9 dB,
62 dB under, where the record's stay more than 60 dB under); a high tone nearer full scale is bent harder, and its
aliases are louder. The Head Bump is a bell at 60 Hz with a Q of 1.4, as many decibels as it says, after the curve;
at 0 dB it isn't played.

In both, only what the curve bends is added to the sound, which itself passes untouched, so at 0 % there is no curve
at all and a quiet sound keeps its level at any Distortion; what is added goes through a high-pass (the record's at 20 Hz, its curve taking the highs alone; the tape's at 2 Hz, so that a bass keeps its saturation), so that a
lopsided wave, which even the tape's symmetric curve bends unevenly, leaves no DC. The curves play at twice the rate by
their antiderivatives, so that they don't alias.

**A record's noise.** The hiss is pink from 60 Hz to 2 kHz and falls 6 dB an octave more from 2.1 kHz, the RIAA
curve's corner, half alike on the two sides. The crackle is clicks at random times: most small and bright, a few large
and dull, each one the slope of a bump, as a stylus thrown over a speck plays. Whether a moment of the song crackles,
and how, comes from a hash of where the song is, in steps of a 4096th of a beat, so a passage crackles the same
however it is reached, and playback, a render and a freeze play the same clicks.

**A tape's noise.** The hiss is brighter than a record's hush, and its two tracks share nothing: white noise from fixed
seeds, each side its own, as loud for each hertz at every sample rate (the synths' noise, `dsp/Noise.h`), high-passed at
200 Hz (12 dB an octave) with a high shelf of 7 dB at 5 kHz. Its level is its RMS at 48 kHz after the filters, the rate
Phazite's presets are leveled at ([presets.md](../../presets.md)), and the same in each hertz at every rate: 0.37 dB
less at 44.1 kHz, and at 96 and 192 kHz the band above 24 kHz holds none of it. The dropouts are the moments the tape
loses contact with the head: the level dips as a raised cosine down and up, both sides together, and while it is down
by d dB a low-pass of 12 dB an octave closes from 20 kHz toward 3 kHz in proportion to d / 18 (on a log scale), so the
top goes first. At 0 % there are none; from 10 % they come once every 8 seconds, 3 dB deep at most, to twice a second,
18 dB deep at most, at 100 % (the rate on a log scale, the depth straight between; under 10 % the depth falls to
nothing at 0 % and the rate goes on down its log scale). Each lasts 30 to 250 ms and dips from half as deep to as deep
as the amount says, both drawn for each. Where each comes, how long it lasts and how deep it dips are drawn from a hash
of where the song is, in steps of a 256th of a beat, as the record's crackle is: a passage drops out the same however
it is reached (after a jump, the steps of the last 250 ms are drawn again, so that a dip under way is there too), live
and in a render. The dips fall on the sound after the drive and before the hiss, which is the playback's and doesn't
dip; a dip of silence is silence, so dropouts to come don't keep Age from resting in silence.


Both media's noise plays while the song plays, from a silent start too, and as late as the sound. It fades out over
20 ms when the song stops (there are no dropouts while it is stopped), and 2 s after the sound coming in stops, as a
run-out groove or a tape's leader ends, so an export ends with the song.

**The stereo part and the band.** After the noise, the two sides are narrowed by the Width, and the Low Cut and the
High Cut, Butterworth filters, roll the band's ends off, the noise's too.

**The mix.** The device's runner blends what the medium plays with what came in, which Age passes the medium's linear
way itself, at every Mix, 0 % too: through the oversampling and the delay line read at its middle, as the sound is,
and through Bass Mono's all-pass while Bass Mono is on (as it comes on or goes off, as far as the sound's own split has
come in, over 20 ms), but not what moves or colours the sound (the wobble, the drive, the side's lows, the noise, the
width and the band). Merely delayed, the copy cancelled the sound where those turn its phase: half and half, the middle
17 dB down at Bass Mono's frequency and the top octave 5 to 18 dB down at 15 and 18 kHz; and a copy merely delayed at
0 % but turned above it set the two half a cycle apart at Bass Mono's frequency at a Mix resting a little over 0 %. Now
what is left between the two is the medium's own doing, at any Mix. So at a Mix of 0 % what comes out is what came in
with its phase turned (by Bass Mono's all-pass while it is on, and in the top octave by the oversampling), its level
within a hundredth of a decibel to 5 kHz; at the top the delay line's read between samples, the sound's own, takes
0.04 dB off at 10 kHz, 0.19 at 15 and 0.37 at 18 kHz (at 44.1 kHz; 0.03, 0.13 and 0.27 at 48 kHz). The half-band
filters themselves cost nothing in level, only phase. At 100 % Age doesn't make the copy, which nothing hears, once the
Mix has been there a second (so that a Mix moved through 100 % and back, or automation touching it, goes on with the
copy it had): it keeps only the copy's way into its delay line, at twice the rate. As the Mix leaves 100 % after that,
it makes the way back again from the line's last 128 samples, the same work at any rate (a few 32-sample calls' worth),
so that the copy comes in without a click.

Because its noise must start with the song whether sound comes in or not, its runner puts Age to sleep only while the
song is stopped, and wakes it as the song plays (`Processor::playsWithTheSong()`). It rests on its own as well while the
song is stopped, nothing comes in and its delay line has emptied.

## Its editor

Two tabs along its top, Record and Tape: a click on one sets the Medium, as one undo step, and shows its page; the
Medium set elsewhere (an undo, automation) shows its page as the editor refreshes. The two pages are the same
components but for the picture and the parts' controls that are one medium's alone, so the editor keeps its size from
one to the other.

**The record's picture.** The record turning at its speed, drawn off its centre as far as the wow swings it, its
label's mark turning with it, the stylus lit as a crackle comes, and a speck where each crackle came, which turns away
with the record and fades. The record shows its turns a minute.

**The tape's picture.** In the record's place, the tape's two reels turning at the Wow Rate (so that the wow shows),
the tape running between them over the head, its line breaking at the head for a moment as a dropout comes, the gap as
wide as the dip is deep; in the same plain shapes and the device's colour as the record. It shows the Wow Rate.

Beside either is the pitch it played over the last three seconds, with the pitch now; under either is the band it
keeps, between the Low Cut and the High Cut, with the two cuts' knobs at its ends and the Band's switch beside the Low
Cut's knob (the band faint while it is off).

A drag up or down on the record, the reels or the pitch turns the Wow, as one undo step, and the wheel turns the Wow
Rate. The band's ends drag sideways, the nearer one lit. Beside the picture are the parts, each with its switch at the
right end of its title, faint while off: Wobble (Wow, Rate and Flutter), Noise (Hiss, then Crackle and Density on the
record, Dropouts on the tape), Drive (Distortion, and Head Bump on the tape) and Stereo (Width and Bass Mono). Each page
is whole in the default dock.

## Limits

- At a Mix below 100 % the wow's moving delay against what came in sweeps a gentle comb through the sound, as two
  copies at different speeds do (a chorus); the band's filters turn its edges' phase against what came in too.
- With Bass Mono on, the phase of the lows is turned at every Mix, 0 % too: its all-pass is part of the sound's way,
  and the copy for the mix takes it so that the two meet in phase. So Age at 0 % is not a bypass: an untouched copy of
  the sound played beside it (a parallel track, a send) cancels around Bass Mono's frequency, as against any all-pass.
  The device's switch bypasses it.
- It adds about 11 ms of latency, which Phazite makes up for on the other tracks.
- A change of medium at a deep wow and a slow rate takes a while: the wow glides down in the old medium's shape and up
  in the new one's no faster than would bend the pitch 7 cents (up to two seconds each way at 25 cents and 0.25 Hz).
- The tape is a sound, not a model of one: no hysteresis model of the tape's magnetism, no tape speeds or formulations,
  no print-through, azimuth or bias, and no tape echo. Its saturation is a soft symmetric curve rather than the
  hysteresis loop, so its odd harmonics don't change with the frequency as a real tape's do.
- The record has no rumble or hum, no spin-down, no scratches that tick once a turn, and no wear or year control that
  sets the band, the noise and the stereo together, where iZotope's Vinyl has them.
- Two media only: none of the other modules that RC-20 puts beside its noise and wobble (a radio, a telephone, a
  sampler's converters).

## In the code

- DSP: `src/devices/age/Age.h`, `src/devices/age/Age.cpp`; the record's hiss and crackle,
  `src/devices/age/SurfaceNoise.h`, `src/devices/age/SurfaceNoise.cpp`; the tape's hiss and dropouts,
  `src/devices/age/TapeWear.h`, `src/devices/age/TapeWear.cpp`; the curves and their oversampling,
  `src/devices/dsp/Saturation.h`, `src/devices/dsp/Oversampling.h`; the delay line, `src/devices/dsp/DelayLine.h`.
- Editor: `src/ui/devices/AgeEditor.h`, `src/ui/devices/AgeEditor.cpp`.
- Tests: `tests/AgeTests.cpp`, `tests/EffectEditorTests.cpp` and `tests/DragPointerTests.cpp` (the editor),
  `tests/DeviceConformanceTests.cpp` (each medium as a variant), `tests/RealtimeAuditTests.cpp`, and the golden renders
  `effect-age`, `effect-age-half-mix` and `effect-age-tape` (`tests/GoldenMixerTests.cpp`).
- Presets: `resources/presets/Age/` ([presets.md](../../presets.md)).
