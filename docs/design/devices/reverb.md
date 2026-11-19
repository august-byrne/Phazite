# Reverb

*Design section 9.*

A space around the sound, of one of three kinds: a Room, for small and medium spaces, as its Size says; a Plate, the
studios' sheet of steel; or a Hall, a longer and denser network for long tails, which stay smooth at the longest Decay
where the Room's ring. It puts out the tail alone; its Mix blends the tail with the sound.

## Parameters

The editor groups them as Space, Shape and Tone.

### Space

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Decay | 0.2 to 20 s | 2 s | How long the tail takes to fade by 60 dB: the Room's and the Hall's bass, or the Plate's sound below 1.5 kHz. |
| Size | 0 to 100 % | 50 % | How large the space is: the Room's lines from a quarter of their longest to all of it, the Hall's from 35 % of theirs, the Plate from 0.4 to 1.6 times its paper's lengths. A change glides. |

### Shape

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | Room, Plate, Hall | Room | Which of the three algorithms plays. A change fades the one playing out over 30 ms, then the other in, which starts empty. |
| Diffusion | 0 to 100 % | 50 % | How far the all-pass filters smear the sound: none at 0 %, the paper's gains at 50 %, the densest at 100 %. In the Hall, those in its lines' loops too. |
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

**Hall** is the Room's network made longer and denser, for long tails (roadmap step S6.5). Each side goes through the
Pre-Delay and six all-pass filters (the Room's four and two shorter ones, 2.37 and 6.13 ms, the right side's 7 % longer),
into sixteen lines with signs that alternate. The lines' lengths at the largest Size are primes in samples at 48 kHz,
spread evenly on a logarithmic scale from 66 to 264 ms, and add up to 2.31 s, against the Room's 0.53: about 2.3 modes a
hertz, where a tail of 10 s needs 1.5 not to ring (Schroeder: about 0.15 a hertz for each second of decay). At the
smallest Size they are 35 % of that. A Hadamard matrix of order 16 (the fast Walsh-Hadamard transform, scaled by a quarter)
shares each line's sound out to every line alike. Each line loses on every pass what makes it fade by 60 dB over the
Decay, as the Room's do, and its treble through the Damping's one-pole; but where the Room matches its lines' dampings at
the Nyquist frequency, the Hall's take as much a second as each other below the Damping (a one-pole's loss there is nearly
proportional to 2 (1 - c) / c², which each line's takes in proportion to its length), since its lines, four times as long
as each other at the most, came sixty times apart a second at 300 Hz under a Damping of 1 kHz, and the short lines rang on.
In each line's loop, after what the sides hear of it, an all-pass filter a fifth of the line's length at the largest Size
(a third at half of it; it keeps its length as the Size moves, so that a Size turned doesn't click) smears every pass after
the first, at a gain of 0.5 at the Diffusion's middle (none at 0 %, never over 0.6, which it reaches at a Diffusion of 70 %, and less for a Decay too short for it,
as the Plate's all-pass filters). It holds the sound longer than its length on average as the tail fades, which each
line's loss gives back: for energy fading as e^(-st), an all-pass of gain g and length M keeps as much as a delay of
g² + (1 - g²)² e^(sM) / (1 - g² e^(sM)) would. Each line's length sways by 0.25 ms at a rate of its own from 0.3 to 0.9 Hz, along straight lines between
points of its sine 32 samples apart, read by the Room's 4-point Lagrange interpolation; the sways start from the same places
every time, and run on with the song's samples over a sleep, so that what it plays after one is what a render plays. The
losses, dampings and gains glide over a block when a value changes. The two sides hear the lines through two orthogonal
patterns of signs, and its level is matched to the Room's.

What the Hall is for, measured beside the Room at the same settings (`tests/ReverbTests.cpp`; the decays and the ringing
at 24 kHz, Damping open, Low Cut at 20 Hz):

| Measure | Hall | Room | Target |
| --- | --- | --- | --- |
| Decay in the octaves at 250 Hz to 1 kHz, 1 to 20 s, Size 0, 50 and 100 % | 0.98 to 1.02 of it | 0.95 to 1.02 | within 5 % |
| Decay in the octave at 2 kHz, the same | 0.94 to 1.01 (0.94 at 20 s, Size 0) | 0.80 to 0.99 | within 5 % (not met at 20 s, Size 0: Limits) |
| Ringing at 10 and 20 s, Size 50 and 100 % (the largest peak of the spectrum of the tail from 1 to 5 s over its third-octave envelope) | 11.2 to 12.5 dB | 15.9 to 17.5 dB | within 4 dB of what decaying noise itself measures (10.1 to 10.5 dB; the Room is 6 to 7 dB over it) |
| Echo density 0.9 (Abel and Huang) after the Pre-Delay, Size 100 % / 50 % | 95 / 74 ms | 111 / 89 ms | 150 / 80 ms |
| Correlation of the sides from 200 ms, Width 100 % | under 0.02 | under 0.03 | within 0.2 |
| Level in 50 ms frames from 300 ms, from a straight line | 0.5 dB at 2 s, 1.5 dB at 10 s | 0.4 / 1.6 dB | within 2 dB |
| Cost, against the Room's (Debug build) | 1.9 to 2.1 times | 1 | 2.2 times at most |
| Level of the tail from pink noise at the defaults | -0.14 dB | 0 | within 0.5 dB |

All three modes end the same way: the Width through mid and side, the Low Cut, and the High Cut at its slope (12 dB an
octave, or a one-pole 6 dB, which is what the plate's own input bandwidth filter comes to). The reverb reports the
Decay plus the Pre-Delay as its tail and has no latency.

## Its editor

A map of the tail, in the manner of FabFilter's Pro-R: time across and pitch up (50 Hz to 16 kHz), each pitch lit
until it has faded by 60 dB, so the wedge is long in the bass and shorter past the Damping, starts after the
Pre-Delay's gap, and shows nothing below the Low Cut or above the High Cut. It is drawn from the reverb's own decay
formula: for the room, each line's loss and damping at the pitch and how long a pass through it takes there, and how the
energy flows between the lines as the room plays it, which comes within a tenth of how long the room is measured to ring
in a third-octave band (at two dampings and both ends of Size, 300 Hz to 3 kHz). The flow is worked out without stepping
through the tail: what is yet to come out is solved for, and where its curve passes each 5 dB from 5 to 25 dB down is
found by halves, through one step's flow squared again and again, so one pitch costs about the same at any Decay and
Size (under a twentieth of a millisecond in a Debug build). For the hall, where every line feeds every line alike, the
flow has a closed form: the energy going into the lines fades as e^(-st) for the s at which the mean over the lines of what
each keeps on a pass (its loss and its damping at the pitch), times e^(s × its pass) and its all-pass's holding, is 1 (a
renewal equation), found by halves in at most 768 line updates, 0.01 to 0.03 ms a pitch in a Debug build; it comes within a tenth of the hall's measured third-octave decays (at two dampings and both ends of Size, 300 Hz to
3 kHz). The map works its 43 pitches out once when a value changes, not for each paint. Hits coming in drift into the map
and fade: the loudest of what came in at each refresh, kept with its moment and drawn across the time it covers; shown
again after it was hidden, the map has none from meanwhile. The map's top names the mode, and for a plate what a short Decay holds back ("Plate  Diffusion 58%").

The map's edges drag, each as one undo step: the wedge's end (or a drag anywhere in the tail) for the Decay, the gap
for the Pre-Delay, the slant's corner for the Damping, the floor for the Low Cut and the ceiling for the High Cut, each
as far as the pointer moves from where it is set (a press changes nothing; Escape takes a drag back). The edge a press
would take lights up, the pointer shows which way it moves, and the hint bar says what it does. Beside the
map are Decay and Size as large knobs, then Mode, Diffusion and Pre-Delay, then the Tone controls.

## Limits

- Three algorithms, against the dozen or more of Valhalla VintageVerb: no chamber, spring, non-linear or convolution
  reverb.
- The Room's eight lines add up to 531 ms at the largest Size (332 ms at the default): thin for the long end of its
  Decay, where it rings (its tail's spectrum peaks 16 to 17.5 dB over its envelope at 10 and 20 s, against noise's 10 to
  10.5): the Hall is for those. No mode's sway has a control.
- The Hall's treble in the octave at 2 kHz fades a little sooner than its Decay at the longest Decays, the more the
  smaller it is (0.94 of 20 s at Size 0 and 0.95 at 50 %, measured at 24 kHz; 0.90 at Size 0 at 48 kHz): the Damping at its top is a one-pole at 20
  kHz, which still takes a little of 2 kHz on each pass (0.025 dB at 48 kHz), and a small hall's tail of 20 s goes round
  400 times. The Room's comes to 0.80 there. A Damping whose top is off would end it.
- The Room's and the Plate's sways stop while the device sleeps on silence, so what they play after a sleep differs a
  little from a render (as heard, the same); the Hall's run on.
- No early reflections apart from the tail, no bass decay multiplier, no freeze, shimmer, gate or ducking, and the
  Pre-Delay doesn't follow the song's tempo.
- The Plate takes both sides in as one, as the paper does, so a sound panned left reverberates the same as one panned
  right.
- Its sound is checked by measurement (decay times, level between the modes), not by listening tests against other
  reverbs ([dsp-audit.md](../../archive/dsp-audit.md)).

## Listening set

Ten minutes, on a snare loop, a vocal and one clap. Hand check E12 goes further.

| What to play | The setting | Listen for |
| --- | --- | --- |
| A snare loop, then a vocal | A new Reverb (Room, Decay 2 s, Mix 25 %) | A natural space behind the sound; the tail fades without a flutter or a metallic ring |
| A snare loop, then a vocal | `Reverb/Plates/Vocal Plate`, then `Reverb/Plates/Snare Plate` | A dense, bright plate that blooms quickly; the same tail whether the sound is panned left or right (the Limits) |
| A snare loop, then a vocal | `Reverb/Halls/Large Hall` | A smooth, wide tail, no flutter and no metal |
| One clap, then silence: the test of its ringing | `Reverb/Halls/Endless` (20 s) | The clap fades for 20 s without a pitch, a beat or a flutter in it (its peak 12.5 dB over its envelope, as decaying noise's 10.5, `tests/ReverbTests.cpp`) |
| The same clap | Room, Decay 20 s | It rings like a pipe (17 dB over its envelope): the Limits' weakest place, which the Hall is for; whether a Room this long is ever wanted |
| A snare | `Reverb/Halls/Chamber` | Whether it is a small bright space, or too large for its name (its lines are longer than the Room's) |
| A tail ringing | Change Mode between Room, Plate and Hall | A fade without a click (30 ms out, then in) |
| A vocal | `Reverb/Rooms/Drum Ambience`, then a Room at Size 0 % and Decay 0.4 s | Whether the voice sits in a room: there are no early reflections apart from the tail |

## Verdict

- Its Hall holds its decay within 2 % from 250 Hz to 1 kHz at 1 to 20 s, rings 11.2 to 12.5 dB over its envelope where
  decaying noise measures 10.1 to 10.5, keeps its sides' correlation under 0.02 and its level within 0.14 dB of the
  Room's, at about twice the Room's cost (the page's table, `tests/ReverbTests.cpp`).
- Its Hall's 2 kHz octave decays 6 % short at 20 s and the smallest Size, and the Room and Plate play a little
  differently after a sleep (backlog: "Reverb, after its Hall...").
- Against a flagship algorithmic reverb: three algorithms where it has a dozen or more, no early reflections, no bass
  decay multiplier, no freeze, shimmer, gate or duck, a Pre-Delay that doesn't follow the tempo, no control of the
  sways (no backlog row).
- **Near it: early reflections, a bass decay multiplier, and more spaces than three** (by measurement; to be confirmed
  by ear).

## In the code

- DSP: `src/devices/reverb/Reverb.h`, `src/devices/reverb/Reverb.cpp`; the delay line and its interpolation,
  `src/devices/dsp/DelayLine.h`; the cuts, `src/devices/dsp/Filters.h`.
- Editor: `src/ui/devices/ReverbEditor.h`, `src/ui/devices/ReverbEditor.cpp`.
- Tests: `tests/ReverbTests.cpp` (the Room and the Plate beside a copy of their code before the Hall came, and the Hall's
  measures beside the Room's), `tests/ReverbEditorTests.cpp`, `tests/DragPointerTests.cpp` (the map's edges),
  `tests/RealtimeAuditTests.cpp`, `tests/DeviceConformanceTests.cpp` (as a Room and as a Hall), `tests/FactoryPresetTests.cpp`.
- Presets: `resources/presets/Reverb/` ([presets.md](../../presets.md)).
