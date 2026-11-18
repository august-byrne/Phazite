# Delay

*Design section 9.*

Echoes of the sound, on a note value at the song's tempo or at a time in milliseconds, each side its own or both the same,
each quieter, darker and thinner than the one before, as a tape echo's are, or clean. It puts out the echoes alone; its
Mix blends them with the sound.

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sync | Off, On | On | On, each side's time is its Note at the song's tempo; off, its Time. |
| Note | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar | 1/8D | The left's time while Sync is on, and both sides' while Link is on: T is a triplet, D dotted, and a bar is as long as the song's time signature makes it. |
| Time | 1 ms to 2 s | 375 ms | The left's time while Sync is off, and both sides' while Link is on. |
| Link | Off, On | On | On, the right side takes the left's time; off, it has its own. |
| Note Right | as Note | 1/8D | The right's time while Link is off and Sync on. |
| Time Right | 1 ms to 2 s | 375 ms | The right's time while Link is off and Sync off. |
| Feedback | 0 to 110 % | 35 % | How much of each echo comes back as the next. Above 100 % the echoes grow each time round until the loop's curve holds them: the dub delay's swell. |
| Ping-Pong | Off, On | Off | Bounces the echoes between the left and the right. |
| Character | Tape, Clean | Tape | What the loop does to an echo each time round: Tape bends it with a soft saturation; Clean gives it back as it left, but for the Feedback and the cuts. |
| Mod Rate | 0.05 to 10 Hz | 0.5 Hz | How fast the modulation wavers the times. |
| Mod Depth | 0 to 100 % | 0 % | How far: up to 4 ms either way at 100 %, so that short times chorus and long ones wow. |
| Duck | 0 to 100 % | 0 % | Turns the echoes down while the sound plays, up to 24 dB at 100 %. |
| Freeze | Off, On | Off | Holds what is in the lines, going round unchanged, and lets nothing new in. |
| Transition | Repitch, Fade | Repitch | How a change of time is made: Repitch glides to it, bending the echoes' pitch; Fade fades from the old time to the new. |
| Low Cut | 20 Hz to 2 kHz | 100 Hz | A high-pass in the loop, 12 dB an octave: each echo is thinner than the one before. |
| High Cut | 1 to 20 kHz | 8 kHz | A low-pass in the loop, 12 dB an octave: each echo is darker than the one before. |
| Mix | 0 to 100 % | 25 % | How much of what plays is the echoes. The device's runner applies it, not the delay; it is in the device's line in the Devices view ([Devices](../devices.md)). |

A return made for a delay sets its Mix to 100 % ([Mixer](../mixer.md)). With every parameter added in step S6.6 at its
default (Link on, Tape, no modulation, no duck, not frozen, Repitch), it plays what it played before, sample for sample.

## How it works

Each side has a delay line up to 5 s long (and the modulation's 4 ms beyond), which is the longest echo whatever the Note
and the tempo ask for. What comes out of the line passes the Low Cut and the High Cut and is the echo; multiplied by the
Feedback and bent by the character's curve, it goes back into the line with the sound coming in.

- **Tape** bends every echo with a soft saturation (a rational approximation of tanh), at every level, so that the loop
  can never run away. The curve isn't anti-aliased: it is as it was before step S6.6.
- **Clean** is a straight line up to full scale, so that an echo up to full scale, quiet or loud, comes back exactly as it
  left but for the Feedback's gain and the cuts, with nothing added. Above full scale it rounds off toward 2 (1 plus the
  tanh of what is over 1, as steep and as straight as the line where they join), which is what holds a Feedback above
  100 %. The same curve holds what it puts out, so that the cuts, ringing a loop held at the curve's top, never take the
  echoes past +6 dBFS (they reached +9 dBFS at 110 % without it): the first echo of a sound hotter than full scale comes
  out bent by it too, at any Feedback.
- A change of character crossfades the two curves over 20 ms.

With Ping-Pong on, the two sides of the sound go into the left line as one, the left line's echo feeds the right line
and the right's feeds the left, so each echo crosses to the other side; with Link off it bounces between the two times.

**A change of time.** With Repitch, a time glides to a new one over about 50 ms, as a tape's would, which bends the
echoes' pitch on the way instead of clicking. With Fade, a change of time never bends the pitch by more than 10 cents:
the time read follows the time set by a glide no faster than 10 cents (0.58 % of real time), and once it is more than
1 ms behind, the line is read at the time set at once while the read as it is fades out over 50 ms and the new one fades
in (two reads of the line for that long), at equal power (a sine and a cosine), so that sound unrelated between the two
reads keeps its level. A change that comes while a fade is under way waits for it to end, then glides or fades on from
what is heard. So a nudge glides in (half a millisecond takes about 90 ms), a jump or a note value switched fades, and a
time that moves faster than 10 cents fades again and again, one fade at a time: the time ramped from 375 to 400 ms over
2 s fades 18 times a side, gliding between; a tempo ramped from 120 to 150 BPM over 2 s with Sync on (the time moving 4 %
of real time) fades 40 times a side, back to back (the time moves 2 ms during each fade's 50 ms), which combs noise by
0.3 dB in thirds of an octave from 1 to 8 kHz for as long as the ramp lasts: the price of no bend, which is what Fade is
chosen for. Each side's time changes on its own. An echo that falls between samples is read by 4-point Lagrange
interpolation, so that it keeps its treble pass after pass.

**The modulation** wavers each line's time as a sine at the Mod Rate, by up to 4 ms either way at 100 % Mod Depth, the
right side a quarter of a cycle ahead, never taking a line under its shortest time (1 ms: nearer it, the swing is less;
while Fade crosses, each read is swung by its own time's swing). Its phase runs on a sample at a time from the Mod Rate,
as an LFO in hertz does, so that a move of the rate, by hand or by automation, changes how fast it runs and never where
it is. It counts the song's samples, not its beats, so a change of tempo moves nothing. As playback starts it is set at
once from where the song is, its sample times the rate: the engine resets the device at every start and at every jump of
the playhead while the song plays, and a render starts so too, so a passage reached by a jump plays as one played from
there, and every render of a song plays the same. While the song plays on it runs on; a block that comes later than the
last one ended (the runner let the delay sleep on silence, as live playback does and a render never does) moves it on
by the samples it missed, at the rate it has then, as if it had played them, so that after a sleep live playback swings
as the render does. A loop going round (the engine doesn't reset the device at a loop's wrap) leaves it running on, live
as in a render of the loop. Stopped, it runs free. The modulation always bends the pitch, whatever the Transition. Its depth glides over 20 ms, and at 0 % it does
nothing at all.

**The duck** follows the sound coming in, the loudest of the louder side's peaks over the last 20 ms (so that the level
it hears doesn't fall between the half-cycles of a bass note, as the compressor's Duck holds its key), rising 60 dB in
5 ms and falling 60 dB in 250 ms, and
turns down what the device puts out (the echoes, not what goes round the loop, so that the tail comes back after the
phrase): by 24 dB times the Duck once the sound is at -12 dBFS or louder, by nothing under -36 dBFS, in a straight line in
decibels between. Its gain glides with a time constant of 5 ms.

**The freeze**, while on, lets nothing new into the lines, makes the feedback exactly 1 and takes the cuts and the curve
out of the loop, so that what is in the lines goes round unchanged for as long as it is on: the loop copies whole samples,
at the time rounded to a whole number of samples and without the modulation, so that nothing of it is dulled pass after
pass, as a read between samples pushed back would dull it. What is heard is still read as before, at the time set (between
samples, once), swung by the modulation, through the cuts: each pass repeats the echo heard as the freeze came on, so
that freezing changes nothing of what is heard but that it goes on, and the sound's lows, highs and any offset, which the
lines hold whole, stay out of it. The Feedback and the Character do nothing meanwhile. While the time moves (a glide, or a
fade), what goes round is read at it, between samples, so that the loop is stretched with it rather than cut by a sample
here and there (cut so, a tempo automated by 1 BPM left some 150 ticks in the loop for good); that dulls only what goes
round during the move, once, and when the time rests again, what goes round moves to the nearest whole sample, no faster
than 10 cents, and copies whole samples from there. Switched on and off it crossfades over 20 ms; off, the loop goes on from what the lines hold, with its Feedback and
cuts back.

The delay reports as its tail the time its echoes take to fall by 60 dB at the longer side's time (at most 60 s), and 60 s
at a Feedback of 100 % or more and while frozen. It has no latency. Every crossfade and glide is counted in samples, so it
plays the same in blocks of any size.

## Its editor

The sound coming in as a bar, then each echo a bar at its time after it, on a scale where the left's first echo is a step
along (on the song's beats, or in milliseconds), each lower than the last by the Feedback, in the manner of FabFilter's
Timeless. While the sides differ (Link off) or bounce (Ping-Pong), the left's echoes stand above the middle line and the
right's hang below it, each at its own time; in ping-pong at the times they cross at. Frozen, every echo is as tall as the
first and the readout says Frozen; ducked, they are as much lower as the duck takes them down now. What plays lights the
first bar, then each echo in its turn.

A drag over the echoes moves the first echo from where its values are set by hand, as far as the pointer moves: across
for the time, in proportion to how far along from the sound coming in it is taken (the nearest note value while Sync is
on), and up or down for the Feedback, to 100 % at most (past it is the knob's, or a value typed, so that no drag swells
the echoes by accident), as one undo step; with Link off, a press under the middle line takes the right's
first echo and its time. A press changes nothing, and Escape takes the drag back. Under the echoes is the band they keep,
between the Low Cut and the High Cut; its ends drag sideways from where they are, the nearer one lit, and the two cuts'
knobs sit at its ends.

Beside the picture: the Time (Sync, Note, Time and Transition; Link, with Note Right and Time Right beside it, faint while
Link is on), the Feedback, large, with Ping-Pong and Freeze; the Character, with Mod Rate and Mod Depth; and the Duck.

## Limits

- No reverse, no diffusion, no width control and no swing.
- Two characters only: no bucket-brigade or other character, and Tape's curve isn't anti-aliased.
- A synced echo is a bar at most, and any echo 5 s.
- Fade crosses at equal power, which keeps the level of sound unrelated between the two reads (the usual case: a note value
  switched); where the two reads match (a steady tone changed by whole cycles, or a low sound whose time moves just over
  1 ms), the sum is up to 3 dB louder at the fade's middle.
- Repitch's glide comes to rest a little short of the time set at long times, where its last steps are too small for the
  float it is kept in: about 3 samples at 375 ms and 48 kHz, some 10 (0.2 ms) at 2 s.
- A press over the echoes sets the time and the feedback together from where the pointer is, and a right-click presses
  as a left-click does.

## In the code

- DSP: `src/devices/delay/Delay.h`, `src/devices/delay/Delay.cpp`; the line and its interpolation,
  `src/devices/dsp/DelayLine.h`; the Tape curve, `src/devices/dsp/Math.h` (`softClip()`).
- Editor: `src/ui/devices/DelayEditor.h`, `src/ui/devices/DelayEditor.cpp`.
- Tests: `tests/DelayTests.cpp` (the echoes, each side's time, the characters, the feedback above 100 %, the modulation,
  the duck, the freeze, the transitions, blocks of any size), `tests/AntialiasingTests.cpp` (the treble an echo keeps),
  `tests/EffectEditorTests.cpp`, `tests/GraphDragTests.cpp` and `tests/DragPointerTests.cpp` (the editor),
  `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Delay/` ([presets.md](../../presets.md)).
