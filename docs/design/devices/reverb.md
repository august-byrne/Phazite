# Reverb

*Design section 9.*

A space around the sound, of one of two kinds: a Room, which is small or a hall as its Size says, or a Plate, the
studios' sheet of steel. It puts out the tail alone; its Mix blends the tail with the sound.

## Parameters

The editor groups them as Space, Shape and Tone.

### Space

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Decay | 0.2 to 20 s | 2 s | How long the tail takes to fade by 60 dB: the Room's bass, or the Plate's sound below 1.5 kHz. |
| Size | 0 to 100 % | 50 % | How large the space is: the Room's lines from a quarter of their longest to all of it, the Plate from 0.4 to 1.6 times its paper's lengths. A change glides. |

### Shape

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | Room, Plate | Room | Which of the two algorithms plays. A change fades the one playing out over 30 ms, then the other in, which starts empty. |
| Diffusion | 0 to 100 % | 50 % | How far the all-pass filters smear the sound: none at 0 %, the paper's gains at 50 %, the densest at 100 %. |
| Pre-Delay | 0 to 250 ms | 10 ms | The gap before the tail starts. A change glides. |

### Tone

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Low Cut | 20 Hz to 1 kHz | 100 Hz | A high-pass on the tail, 12 dB an octave, which keeps rumble out of it. |
| High Cut | 1 to 20 kHz | 20 kHz | A low-pass on the tail. At its top it is off. |
| Damping | 1 to 20 kHz | 6 kHz | The frequency past which the treble fades sooner than the bass, on each pass through the space. |
| Width | 0 to 100 % | 100 % | How far apart the tail's two sides are, from mono to wide stereo. |
| High Cut Slope | 6 dB, 12 dB | 12 dB | The High Cut's steepness, an octave. The editor calls it Slope. A change crossfades over 10 ms. |
| Mix | 0 to 100 % | 25 % | How much of what plays is the tail. The device's runner applies it, not the reverb; it is in the device's line in the Devices view ([Devices](../devices.md)). |

A return made for a reverb sets its Mix to 100 %, since the sound itself reaches the master through its own track
([Mixer](../mixer.md)).

## How it works

**Room** is a feedback delay network. Each side is delayed by the Pre-Delay and smeared by four all-pass filters, with
the lengths of the input diffusers of Dattorro's plate (the right side's 7 % longer, so that the sides differ). Eight
delay lines, 44.3 to 89.5 ms long at the largest Size and sharing no common period, feed each other through a
Householder matrix, which keeps the sound's energy. Each line loses on every pass as much level as makes it fade by
60 dB over the Decay, and its treble through a one-pole low-pass at the Damping, as much on a pass as the line's length
is of the lines' mean (Jot and Chaigne, 1991), so that the tail keeps its colour as it fades. Each line's length sways
by 0.12 ms at a slow rate of its own (0.31 to 1.13 Hz), read between samples by 4-point Lagrange interpolation, so that
the tail doesn't ring like metal. The sways start from the same places every time, so a render comes out the same. The
two sides hear the lines through two orthogonal patterns of signs, which leaves them uncorrelated.

**Plate** is Jon Dattorro's plate as his paper draws it ("Effect Design, Part 1: Reverberator and Other Filters", J.
Audio Eng. Soc. 45 (9), 1997, figure 1 and table 2). Both sides go in as one, through the Pre-Delay and four all-pass
filters, into a tank of two halves that feed each other in a figure of eight: each half is an all-pass filter whose
length sways (0.385 ms at 0.6 Hz, the halves a quarter turn apart), a delay, the damping and the decay's gain, another
all-pass filter and a delay. Each side hears seven taps of the tank. The lengths are the paper's at the Size's middle,
scaled to the sample rate, and whole numbers of samples except while the Size glides. The tank's gain for a Decay comes
from a table measured on the plate, since its all-pass filters hold the sound longer than its loop alone would; a
Decay too short for the plate's Size or Diffusion holds them back, and the editor says by how much. Its two damping
low-passes sit two octaves under the Damping, so that the knob means about the same in both modes, and its level is
matched to the Room's.

Both modes end the same way: the Width through mid and side, the Low Cut, and the High Cut at its slope (12 dB an
octave, or a one-pole 6 dB, which is what the plate's own input bandwidth filter comes to). The reverb reports the
Decay plus the Pre-Delay as its tail and has no latency.

## Its editor

A map of the tail, in the manner of FabFilter's Pro-R: time across and pitch up (50 Hz to 16 kHz), each pitch lit
until it has faded by 60 dB, so the wedge is long in the bass and shorter past the Damping, starts after the
Pre-Delay's gap, and shows nothing below the Low Cut or above the High Cut. It is drawn from the reverb's own decay
formula. Hits coming in drift into the map and fade. The map's top names the mode, and for a plate what a short Decay
holds back ("Plate  Diffusion 58%").

The map's edges drag, each as one undo step: the wedge's end (or a drag anywhere in the tail) for the Decay, the gap
for the Pre-Delay, the slant's corner for the Damping, the floor for the Low Cut and the ceiling for the High Cut, each
as far as the pointer moves from where it is set (a press changes nothing; Escape takes a drag back). The edge a press
would take lights up, the pointer shows which way it moves, and the hint bar says what it does. Beside the
map are Decay and Size as large knobs, then Mode, Diffusion and Pre-Delay, then the Tone controls.

## Limits

- Two algorithms, against the dozen or more of Valhalla VintageVerb: no hall, chamber, spring, non-linear or
  convolution reverb.
- The Room's eight lines add up to 531 ms at the largest Size (332 ms at the default): thin for the long end of its
  Decay, where a reverb with more or longer lines stays dense. Its sway is fixed and has no control.
- No early reflections apart from the tail, no bass decay multiplier, no freeze, shimmer, gate or ducking, and the
  Pre-Delay doesn't follow the song's tempo.
- The Plate takes both sides in as one, as the paper does, so a sound panned left reverberates the same as one panned
  right.
- Its sound is checked by measurement (decay times, level between the modes), not by listening tests against other
  reverbs ([dsp-audit.md](../../archive/dsp-audit.md)).

## In the code

- DSP: `src/devices/reverb/Reverb.h`, `src/devices/reverb/Reverb.cpp`; the delay line and its interpolation,
  `src/devices/dsp/DelayLine.h`; the cuts, `src/devices/dsp/Filters.h`.
- Editor: `src/ui/devices/ReverbEditor.h`, `src/ui/devices/ReverbEditor.cpp`.
- Tests: `tests/ReverbTests.cpp`, `tests/ReverbEditorTests.cpp`, `tests/DragPointerTests.cpp` (the map's edges),
  `tests/RealtimeAuditTests.cpp`, `tests/FactoryPresetTests.cpp`.
- Presets: `resources/presets/Reverb/` ([presets.md](../../presets.md)).
