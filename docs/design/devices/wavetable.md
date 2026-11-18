# Wavetable

*Design section 9.*

A wavetable synth with 16 voices, for sounds that move through a table of waves: two oscillators that morph through
their tables' frames, each with a warp, a sub and noise, unison, one resonant filter, three envelopes, two LFOs and
eight modulation slots that move things for each note on its own. It follows Serum's layout with fewer parts.

## Parameters

The sub-headings are the editor's parts. Inside a part the editor shows shorter labels than the names below, given
under each heading. What an oscillator plays, its table, is not a parameter: see "Tables" below.

### Osc 1 and Osc 2

In the editor: Position, Pitch, Fine, Level, Warp, Amount; the switch is at the right end of the part's title.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Osc 1/2 On | Off, On | On for Osc 1, Off for Osc 2 | The oscillator's switch. |
| Osc 1/2 Position | 0 to 100 % | 67 % | Where in its table the oscillator plays, crossfading between the two frames nearest. The default is the saw in the first factory table. |
| Osc 1/2 Pitch | -36 to +36 semitones | 0 semitones | The oscillator's pitch against the note. |
| Osc 1/2 Fine | -100 to +100 cents | 0 cents | Its fine tuning. |
| Osc 1/2 Level | 0 to 100 % | 75 % | Its level. |
| Osc 1/2 Warp | None, Sync, Bend +, Bend -, Pulse Width, Mirror, FM, Ring Mod | None | How its wave is reshaped: see "How it works". |
| Osc 1/2 Warp Amount | 0 to 100 % | 0 % | How far the warp goes. |

### Sub and Noise

In the editor: Sub, Octave, Shape, Noise.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sub Level | 0 to 100 % | 0 % | The level of the sub oscillator, in the middle of the stereo field. |
| Sub Octave | -1 Octave, -2 Octaves | -1 Octave | How far below the note the sub plays. |
| Sub Shape | Sine, Triangle, Saw, Square | Sine | The sub's wave. |
| Noise Level | 0 to 100 % | 0 % | The level of white noise, in the middle of the stereo field. |

### Unison

In the editor: Voices, Detune, Spread, Stack, Drift, Position. The unison is one for both oscillators.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Unison Voices | 1 to 8 | 1 | How many copies of each oscillator a voice plays at once. |
| Unison Detune | 0 to 100 cents | 20 cents | How far the outermost copies are tuned from the note, the others evenly between them. |
| Unison Spread | 0 to 100 % | 80 % | How far the copies spread across the stereo field. |
| Unison Position Spread | 0 to 100 % | 0 % | How far the copies spread across the table, around the oscillator's position: at 100 %, half the table either way. |
| Unison Stack | Off, Octave, Two Octaves, Fifth, Power Chord | Off | Where the copies play, in turn from the first: all on the note; the note and an octave up; the note, one and two octaves up; the note and a fifth up; or the note, a fifth and an octave up. |
| Unison Drift | 0 to 50 cents | 0 cents | How far each copy's pitch wanders on its own. |

### Filter

In the editor: Type, Cutoff, Resonance, Slope, Drive, Key; the switch is at the right end of the part's title.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Filter On | Off, On | On | The filter's switch. Off, the oscillators go to the amp as they are. |
| Filter Type | Low Pass, High Pass, Band Pass, Notch, Comb +, Comb -, Vowel, Phaser, Sample & Hold | Low Pass | The filter's type: those of the [Filter](filter.md) device. |
| Filter Cutoff | 20 Hz to 20 kHz | 2.5 kHz | The filter's frequency before key tracking and modulation move it. |
| Filter Resonance | 0 to 100 % | 10 % | Sharpens the filter at its cutoff (for each type, as on the [Filter](filter.md) page, but a 12 dB section's Q, as the [Synth](synth.md)'s: see "Filter" below). |
| Filter Slope | 12 dB, 24 dB | 24 dB | The slope per octave of Low Pass, High Pass, Band Pass and Notch. |
| Filter Drive | 0 to 24 dB | 0 dB | Saturates what goes into the filter. |
| Filter Key Tracking | 0 to 100 % | 50 % | How far the cutoff follows the key, around middle C: at 100 %, a semitone for each semitone. |

### Amp, Envelope 2 and Envelope 3

In the editor: Attack, Decay, Sustain, Release, A Curve, D Curve, R Curve. Envelope 1 is the part named Amp and shapes
the level; Envelopes 2 and 3 do nothing until a modulation slot uses them.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Envelope 1 Attack | 0.5 ms to 10 s | 2 ms | The time to full level. Never instant, so that a note doesn't click. |
| Envelope 1 Decay | 0 ms to 10 s | 300 ms | The time from full level to the sustain level. |
| Envelope 1 Sustain | 0 to 100 % | 80 % | The level held while the key is down. |
| Envelope 1 Release | 0.5 ms to 10 s | 200 ms | The time to silence once the key is up. It is also the tail the synth reports. |
| Envelope 2/3 Attack | 0 ms to 10 s | 1 ms | The time to the top. |
| Envelope 2/3 Decay | 0 ms to 10 s | 400 ms for Envelope 2, 800 ms for Envelope 3 | The time from the top to the sustain level. |
| Envelope 2/3 Sustain | 0 to 100 % | 20 % for Envelope 2, 0 % for Envelope 3 | The level held while the key is down. |
| Envelope 2/3 Release | 0 ms to 10 s | 300 ms | The time to nothing once the key is up. |
| Envelope 1/2/3 Attack Curve | -100 to +100 % | -15 % | How the attack bends: below 0 fast at first, 0 straight, above 0 slow at first. |
| Envelope 1/2/3 Decay Curve | -100 to +100 % | -46 % | How the decay bends, likewise. |
| Envelope 1/2/3 Release Curve | -100 to +100 % | -46 % | How the release bends, likewise. |

### LFO 1 and LFO 2

In the editor: Shape, Rate, Sync, Division, Retrigger, Fade In. An LFO has no depth of its own: the slot that uses it has.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| LFO 1/2 Shape | Sine, Triangle, Saw Up, Saw Down, Square, Random | Sine for LFO 1, Triangle for LFO 2 | The LFO's wave. Random holds a new value each cycle. |
| LFO 1/2 Rate | 0.05 to 20 Hz | 2 Hz for LFO 1, 0.5 Hz for LFO 2 | Its speed while Sync is off. |
| LFO 1/2 Sync | Off, On | Off | Whether it runs on a note value at the song's tempo. |
| LFO 1/2 Division | 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2T, 1/2, 1/2D, 1 bar, 2 bars, 4 bars, 8 bars | 1/4 | The length of its cycle while Sync is on. |
| LFO 1/2 Retrigger | Off, On | On for LFO 1, Off for LFO 2 | On, each note has an LFO of its own that starts with it; off, one LFO runs on for every note. |
| LFO 1/2 Fade In | 0 ms to 10 s | 0 ms | How long each note's share of the LFO takes to come in, from nothing, counted from the note's start, whether the LFO retriggers or not, as the [Synth](synth.md)'s LFO Fade In. |

### Matrix

Eight rows, each a slot: its source, its target and its amount.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mod 1 to 8 Source | None, Envelope 2, Envelope 3, LFO 1, LFO 2, Velocity, Note, Mod Wheel, Random, Drift | Envelope 2 for Mod 1, None for the others | What moves the target. Note is the key around middle C; Random is a value drawn for each note; Drift is a slow wander of each voice's own for each slot, from -1 to 1. |
| Mod 1 to 8 Target | None, Osc 1 Position, Osc 1 Warp, Osc 1 Pitch, Osc 1 Level, Osc 2 Position, Osc 2 Warp, Osc 2 Pitch, Osc 2 Level, Pitch, Sub Level, Noise Level, Cutoff, Resonance, Volume, Pan, Unison Detune, Mod 1 Amount to Mod 8 Amount | Cutoff for Mod 1, None for the others | What is moved. Pitch is both oscillators' and the sub's. Mod N Amount is another slot's amount: a slot's own is shown in its list but can't be chosen. |
| Mod 1 to 8 Amount | -100 to +100 % | 25 % for Mod 1, 0 % for the others | How far. At 100 % a slot moves a position, a warp amount, a level, the resonance or another slot's amount across its whole range, a pitch by 24 semitones, the cutoff by eight octaves, the unison detune by 100 cents, the volume from silence to twice as loud and the pan all the way to a side. |

A new synth's first slot is Envelope 2 on the cutoff at 25 % (two octaves), so it starts as a saw that opens with each
note.

### Voice

In the editor: Mode, Glide, Glide Mode, Phase, Bend, Velocity, Level.

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Voices | Poly, Mono | Poly | Sixteen voices, or one that plays legato. |
| Glide | 0 ms to 2 s | 0 ms | How long a note takes to glide from the one before; 0 is none. |
| Glide Mode | Always, Legato | Always | Whether every note glides, or only one played while another is held, as the [Synth](synth.md)'s. |
| Phase | Reset, Free | Reset | Reset starts every oscillator (each unison copy and the sub) at the start of its wave at each note, so that a note sounds the same every time; Free lets each voice's oscillators run on from where they were when it last played, as an analog synth's do, in Poly and in Mono (a voice that has never played starts as on Reset), as the [FM](fm.md) synth's Phase. |
| Bend Range | 0 to 24 semitones | 2 semitones | How far the pitch wheel bends, either way. |
| Velocity | 0 to 100 % | 60 % | How much of the level depends on the note's velocity, which counts squared. |
| Level | -36 to +6 dB | -6 dB | The synth's output level. |

## How it works

**Tables.** A table is up to 256 single-cycle frames. Each frame is kept band-limited at every pitch: 21 copies, one
each half octave, each with only the harmonics that level may play, made with an FFT when the table is made, with 16
samples for each cycle of its highest harmonic, at least 64 and at most 8192 (so the two copies of the most harmonics,
which only notes below 39 Hz play, keep 8 and 11). An oscillator reads the copy whose highest harmonic's
alias stays above 20 kHz at the pitch it plays, with four-point cubic interpolation, and crossfades between the two
frames each side of its position. So the images the interpolation leaves are far down: Basic Shapes' saw, square and
triangle, and the Pulse Width table at 25 %, alias more than 90 dB under the note from A0 to A7, as far down as the
[Synth](synth.md)'s own waves or further ([tests/AntialiasingTests.cpp](../../../tests/AntialiasingTests.cpp)). A frame
takes about 212 KB, 2.6 times what four samples a cycle took: Basic Shapes 850 KB, a factory table of 32 frames 6.8 MB,
a sound file of 256 frames 54 MB, which takes about 2.7 times as long to make.

Thirteen tables come with the synth, made from formulas when first needed, so that no sound files ship: Basic Shapes
(sine, triangle, saw, square, each the [Synth](synth.md)'s own wave in phase and polarity: a sine rising from 0, a triangle from
its peak, a saw rising, a square high for the first half of its cycle, so that they sum with the other oscillator and the
sub as the Synth's do; since roadmap step S6.7), Pulse Width (pulses high for the first part of their cycle, from 50 % down
to 2 %, as the Synth's pulse), Saw Build, Square Build, Vowels, Sync, FM, FM Hollow, Fold, Digital, Comb,
Growl and Spectra. A sound file becomes a table too. A WAV file whose `clm ` chunk gives its frames' length, as Serum
writes it, is cut into frames of that length; any other sound is frames of 2048 samples when it is a whole number of
them, one frame when it is shorter than two (a single cycle of any length), and otherwise its first frames of 2048, up
to 256. Its channels are mixed, any constant offset is taken out and the loudest frame peaks at 1.

The song keeps what each oscillator plays with its device: a factory table by its name, or a sound file, of which the
project plays its own copy in its Audio folder. Choosing a table is one undo step. A preset carries a file to the
project that loads it ([devices.md](../devices.md)).

**Warps.** Each oscillator has one:

- **Sync**: hard sync. The wave is read 2^(3 a) times in each of the note's cycles, a the Warp Amount, starting again
  with each; the jump back is smoothed by a two-point PolyBLEP. So the amount is even in pitch, 36 semitones across the
  knob (1 to 8 times): a slot from an envelope to the warp at N / 36 sweeps it N semitones, as the [Synth](synth.md)'s Osc
  2 Envelope sweeps its synced oscillator. A slot can take it a third past the knob's end, to 16 times (48 semitones).
- **Bend +** and **Bend -**: the wave's start squeezed and its end stretched, or the other way.
- **Pulse Width**: the whole wave squeezed into the first part of the cycle, down to a twentieth of it, its start held
  for the rest.
- **Mirror**: the wave read forward, then back, mixed in by the amount.
- **FM**: the other oscillator moves this one's phase.
- **Ring Mod**: the other oscillator multiplies this one.

A warp that reads the wave faster than the note picks the copy with fewer harmonics that its fastest reading allows.
Every frame is centred over its whole cycle, but not over a part of it: Sync reads a part of its last cycle, the Bends read
the cycle unevenly, and Pulse Width holds the wave's start. Each takes what that leaves off centre away as it plays: the
table keeps, for each frame and each of its band-limited copies, its area from its start across its cycle and its mean
read at bends from 1/8 to 8, worked out as the table is made (a fifth more memory, and about a sixth more time: both
exactly from the frame's harmonics), so that a warped note
carries no constant offset.

**Unison.** Both oscillators play up to eight copies, tuned evenly across the detune, panned evenly across the spread
at equal power and, with Position Spread, spread across the table, each at a level that keeps the note as loud. The
stack's steps are taken by the copies in turn, and each step reads the copy of the table its own pitch allows. With
Drift, each copy's pitch wanders toward a new place every so often at a pace of its own, from half to twice the
middle pace, drawn again each note. On Reset (Phase), the first copy starts each note at the start of its wave, the others at places
drawn from a fixed seed.

**Filter.** One of the nine types of `dsp::MultimodeFilter`, the code the [Filter](filter.md) device and the
[Synth](synth.md) share. Resonance makes a 12 dB section's Q by the synth's own curve, from 0.5, softer than a
Butterworth, to 25, as the [Synth](synth.md)'s does (`dsp::getSynthResonanceQ()`; since roadmap step S6.7, whose presets
from the Synth needed its reach), where the Filter device's runs from 0.71 to 16: a 12 dB low-pass or high-pass and the
band-pass and the notch at either slope; a 24 dB low-pass or high-pass, and the other types, take the resonance alike. The cutoff a voice plays is Filter Cutoff moved by key tracking and the slots aimed at it,
kept between 20 Hz and 0.45 of the sample rate. Filter Drive soft-clips what goes into the filter at twice the sample
rate through a per-voice oversampler, the curve played by its antiderivative so that what it adds doesn't fold back as
aliasing; it fades in and out over two thirds of a millisecond, at any rate.

**Modulation.** The slots are worked out for each voice on its own, so an envelope sweeps every note's table from its
start. Several slots on one target add up. Each LFO comes into each note over its Fade In, the note's share growing in a
straight line from nothing, as the Synth's does, counted from the note's start (a legato note in Mono goes on with the
voice's). Drift wanders on its own for each slot that plays it, as the Synth's Drift moves each of its two oscillators and its
cutoff on its own (since roadmap step S6.7): each note from a place of its own, toward a new one every 0.45 s, gliding
there with a time constant of 0.3 s, from a fixed seed for each voice and slot, moved only while its slot plays it; slots
from Drift to Osc 1 Pitch and Osc 2 Pitch at 0.33 % (8 cents) and to Cutoff at 2.08 % (two semitones) are the Synth's Drift
at full: the two oscillators wander against each other, and beat. A slot aimed at Mod N
Amount adds its source, times its own amount, to slot N's amount, so the mod wheel on an LFO slot's amount brings its
vibrato in. So that nothing loops, those slots are worked out first, each by its own amount as set, which nothing moves;
one aimed at its own amount does nothing; and an amount moved stays within -100 and +100 %. Envelopes 2 and 3, the LFOs, the slots, the pitch, the frames read and the
filter's coefficients are worked out at the control rate, every 16 samples, on the song's own grid while it plays and on
a running count while it is stopped, as the [Synth](synth.md)'s; the oscillators, the filter and Envelope 1 run at every sample. A track's modulators and macros
([automation-and-modulation.md](../automation-and-modulation.md)) still move any of its knobs for the whole track.

**Notes.** As the [Synth](synth.md)'s: in Poly every note takes a voice of its own, a key played again too; with all
16 busy a new note takes the voice ringing out longest, then the oldest, whose sound fades out over 3 ms in a place of
its own (one for each voice, so a chord that takes several at once fades each); Mono plays
one voice legato and returns to the key still held. Glide works in both modes, for every note, or with Glide Mode on
Legato only for a note played while another key is held. Portamento and slide
notes ([piano-roll.md](../piano-roll.md)) glide as on the Synth. Pitch bend, the sustain pedal (controller 64) and the
mod wheel (controller 1, a modulation source) are the controllers it answers.

**The same every time.** Noise, the unison's starting phases, the drift and the Random and Drift sources come from fixed seeds,
so a render repeats exactly. A table chosen while notes play is read from the next control step on, which takes what was
given last; one given and replaced before a step took it is freed at once, so that a synth that plays nothing (on a muted
track) holds at most the table it plays and the newest. One a step took is freed once the count of steps begun has moved
on past where it was when the next was given, by the playback session's timer (30 times a second) or the next table
given, whichever comes first. The noise is as the Synth's, alike at every rate. A DC blocker on each side of its sum (12
dB at 5 Hz) takes away what ring modulation and FM leave at whole ratios. It plays only while an oscillator that is on
has one of those warps: it changes the waveform of whatever it plays, so every other patch, whose waves are centred as
they are made (the other warps by their areas and bent means), plays without it, bit for bit. Taken on and let go in
mid-note, it fades in and out over 5 ms; once playback has stopped and no note sounds, what it still rings with fades
out over 3 ms, so that nothing sounds on after Stop. Envelopes 2 and 3, the LFOs and both drifts move a control step at a
time; where the grid cuts a step short (a loop going round, a jump), they move on only when that keeps them nearer the
samples played, so that they keep their pace, and a comb's delay set at a step of one sample glides over the next step
rather than jumping.

## Its editor

Seven columns, built on the frame every device editor shares ([devices.md](../devices.md)): Osc 1; Osc 2; Filter;
Amp; Sub and Noise over Voice; Unison; and Modulation, a list down its side of Envelope 2, Envelope 3, LFO 1, LFO 2
and Matrix, one showing at a time. In the dock they show in four pages: the oscillators; Filter and Amp; Sub and Noise
over Voice; Unison and Modulation.

- **Each oscillator** draws its table as its frames one behind another, with the frame the newest note plays lit in
  the device's colour, moving as modulation moves it. Dragging across the drawing, right or up, moves the position:
  the drawing's width is the whole table. A button above it names the table and opens a menu of the factory tables
  and Load Wavetable File.... A sound dropped on the drawing, from the browser or the file manager, becomes the
  table. An oscillator that is off is drawn faint.
- **Filter** draws the filter's response, which follows the newest note's cutoff, with a dot at the cutoff, as high as
  the resonance, to drag.
- **Amp, Envelope 2 and Envelope 3** draw their envelopes with a line at the newest note's level; their points and
  curves drag as the [Synth](synth.md)'s do.
- **LFO 1 and LFO 2** draw one cycle with a dot where the LFO is (the newest note's, for one that retriggers). The
  wheel over the picture turns its rate, or its division while Sync is on.
- **Matrix** is a row for each slot: two lists and a knob. In a slot's list of targets its own amount is shown but can't
  be chosen.

## Limits

- Thirteen factory tables, and no way to draw or edit a table.
- One warp for each oscillator, and none that works on the spectrum.
- Two oscillators and one filter; no third oscillator, no second filter, no filter routing.
- The unison is shared: both oscillators have the same number of copies, detune, spread, stack and drift.
- Eight slots and nine sources; no aftertouch or MPE; modulation is set in the Matrix's lists, not dragged onto the
  knobs it moves. A slot that moves another's amount can't itself be moved.
- The LFOs' shapes are fixed; none can be drawn. No arpeggiator.
- No oversampling of the oscillators and no quality setting: Sync's jump is smoothed by a two-point PolyBLEP only, and
  Bend, Pulse Width, Mirror and FM only read a copy with fewer harmonics, so warped sounds alias on high notes.
- A frame's copies keep 16 samples for each cycle of their highest harmonic (up to 8192 a copy), 2.6 times the memory
  four took: a sound file of 256 frames takes 54 MB, and about 2.7 times as long to make, on the message thread.
- Position, pitch, levels and cutoff move in steps of 16 samples without interpolation, and Envelopes 2 and 3 and the
  LFOs run at that rate: a fast sweep of a table or a resonant filter can be heard stepping.
- The filter is linear: it never self-oscillates, there is no ladder or diode model, and Filter Drive, before the
  filter, gives back the gain it adds, so a loud voice driven hard comes out quieter.
- A held note that is silent for longer than the release lets the synth sleep ([known bug 37](../../known-bugs.md)).
- No effects of its own: Phazite's devices follow it.

## In the code

- DSP: `src/devices/wavetable/WavetableSynth.h`, `WavetableSynth.cpp`; a table in `src/devices/wavetable/Wavetable.h`;
  the factory tables in `src/devices/wavetable/FactoryWavetables.h`; the filter in `src/devices/dsp/MultimodeFilter.h`;
  the drive's oversampler in `src/devices/dsp/Oversampling.h`; the LFOs' note values in `src/devices/dsp/NoteValues.h`.
- Model: what an oscillator plays, and reading a sound file as a table, in
  `src/model/devices/internal/Wavetables.h`; the editor reaches it through `src/model/devices/DeviceHandle.h`.
- Editor: `src/ui/devices/WavetableEditor.h`, `WavetableEditor.cpp`; the envelope, LFO and filter pictures in
  `src/ui/devices/SynthParts.h`.
- Presets: `resources/presets/Wavetable/` ([presets.md](../../presets.md)).
- Tests: `tests/WavetableTests.cpp`, `tests/WavetableModelTests.cpp`, `tests/WavetableEditorTests.cpp`,
  `tests/RealtimeAuditTests.cpp` (guarded scenarios), `tests/GoldenRenderTests.cpp` with
  `tests/golden/renders/wavetable.json`, and `tests/FactoryPresetTests.cpp`.
