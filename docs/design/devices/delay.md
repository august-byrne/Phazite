# Delay

*Design section 9.*

Echoes of the sound, on a note value at the song's tempo or at a time in milliseconds, each quieter, darker and thinner
than the one before, as a tape echo's are. It puts out the echoes alone; its Mix blends them with the sound.

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sync | Off, On | On | On, the echo's time is the Note at the song's tempo; off, it is the Time. |
| Note | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar | 1/8D | The echo's time while Sync is on: T is a triplet, D dotted, and a bar is as long as the song's time signature makes it. |
| Time | 1 ms to 2 s | 375 ms | The echo's time while Sync is off. |
| Feedback | 0 to 95 % | 35 % | How much of each echo comes back as the next. |
| Ping-Pong | Off, On | Off | Bounces the echoes between the left and the right. |
| Low Cut | 20 Hz to 2 kHz | 100 Hz | A high-pass in the loop, 12 dB an octave: each echo is thinner than the one before. |
| High Cut | 1 to 20 kHz | 8 kHz | A low-pass in the loop, 12 dB an octave: each echo is darker than the one before. |
| Mix | 0 to 100 % | 25 % | How much of what plays is the echoes. The device's runner applies it, not the delay; it is in the device's line in the Devices view ([Devices](../devices.md)). |

A return made for a delay sets its Mix to 100 % ([Mixer](../mixer.md)).

## How it works

Each side has a delay line up to 5 s long, which is the longest echo whatever the Note and the tempo ask for. What
comes out of the line passes the Low Cut and the High Cut and is the echo; multiplied by the Feedback and bent by a
soft saturation (a rational approximation of tanh), it goes back into the line with the sound coming in, so that the
loop can never run away.

With Ping-Pong on, the two sides of the sound go into the left line as one, the left line's echo feeds the right line
and the right's feeds the left, so each echo crosses to the other side.

A change of time glides to the new one over about 50 ms, as a tape's would, which bends the echoes' pitch on the way
instead of clicking. An echo that falls between samples is read by 4-point Lagrange interpolation, so that it keeps its
treble pass after pass. The delay reports as its tail the time its echoes take to fall by 60 dB (at most 60 s) and has
no latency.

## Its editor

The sound coming in as a bar, then each echo a bar after it on the song's beats (or in milliseconds), each lower than
the last by the Feedback, above and below the middle line in turn while they ping-pong, in the manner of FabFilter's
Timeless. What plays lights the first bar, then each echo in its turn.

A press or a drag over the echoes puts the first echo under the pointer: across for the time (the nearest note value
while Sync is on) and up or down for the Feedback, as one undo step. Under the echoes is the band they keep, between
the Low Cut and the High Cut; its ends drag sideways, the nearer one lit, and the two cuts' knobs sit at its ends.
Beside the picture are Sync, Note, Time and Ping-Pong, and the Feedback as a large knob.

## Limits

- One time for both sides: no separate left and right times, no offset and no swing.
- No modulation of the time, no ducking, freeze, reverse, diffusion or width, and no tape or bucket-brigade character
  beyond the two cuts: well below Valhalla Delay or FabFilter Timeless.
- A change of time can only glide, which re-pitches the echoes; there is no mode that fades or jumps to the new time.
- The feedback always passes the saturation, which bends at every level and isn't anti-aliased, so no echo is ever
  entirely clean; and it stops at 95 %, so the echoes can't be held.
- The echo is one sample longer than set, since the line is read before the sound is pushed into it: a comb made with
  a very short Time is slightly flat.
- A synced echo is a bar at most, and any echo 5 s.
- A press over the echoes sets the time and the feedback together from where the pointer is, and a right-click presses
  as a left-click does.

## In the code

- DSP: `src/devices/delay/Delay.h`, `src/devices/delay/Delay.cpp`; the line and its interpolation,
  `src/devices/dsp/DelayLine.h`; the saturation, `src/devices/dsp/Math.h` (`softClip()`).
- Editor: `src/ui/devices/DelayEditor.h`, `src/ui/devices/DelayEditor.cpp`.
- Tests: `tests/EffectDeviceTests.cpp` (the echoes), `tests/AntialiasingTests.cpp` (the treble an echo keeps),
  `tests/EffectEditorTests.cpp` and `tests/DragPointerTests.cpp` (the editor), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Delay/` ([presets.md](../../presets.md)).
