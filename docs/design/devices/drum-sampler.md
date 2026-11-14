# Drum Sampler

*Design section 9.*

A pad on every MIDI note, 128 in all, each playing up to four sounds with its own envelope, tuning, filter and choke
group. A new drum sampler plays a default kit that Phazite synthesizes, on General MIDI's drum notes from the kick at
note 36, so that it needs no files.

## Parameters

Every pad has the same 25 parameters, named after its note: "Pad 38 Level". The tables below write "Pad N". The
editor shows the selected pad's controls under the short labels given in its part's name ("Level", "Pan"). The only
default that differs between pads is Choke: pads 42, 44 and 46 (the closed, pedal and open hats) start in group 1.
Automation's menus list a pad's parameters under the pad, for the pads that play a sound.

### Sound

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pad N Start | 0 to 95 % | 0 % | Where in its sound the pad starts, from its end while reversed. |
| Pad N Reverse | Off, On | Off | Plays the sound backwards. |
| Pad N Attack | 0 to 200 ms | 0 ms | A fade in from silence. |
| Pad N Hold | 0 ms to 10 s | 10 s | How long it stays at full level after the attack. At its longest, 10 s, the whole sound plays. |
| Pad N Decay | 1 ms to 5 s | 200 ms | The time it takes to fall to silence (-60 dB) after the hold, or, gated, after the note's end. |
| Pad N Mode | One Shot, Gate | One Shot | One Shot plays on however short the note; Gate starts the decay at the note's end. |

### Pad

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pad N Level | -48 to +12 dB | 0 dB | The pad's level. |
| Pad N Pan | Left to right | Center | The pad's place between left and right. |
| Pad N Velocity | 0 to 100 % | 100 % | How much a softer hit turns the pad down: at 0 % not at all, at 100 % as the velocity says. |
| Pad N Choke | Off, 1 to 8 | Off (1 for pads 42, 44 and 46) | Its choke group: a hit on a pad stops the other pads in its group. |
| Pad N Mute | Off, On | Off | A muted pad plays nothing, and fades out if it rings. |
| Pad N Solo | Off, On | Off | While any pad is soloed, only the soloed pads play. |

### Pitch

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pad N Tune | -24 to +24 semitones | 0 semitones | The pad's tuning. |
| Pad N Fine | -50 to +50 cents | 0 cents | Fine tuning. |
| Pad N Sweep | -48 to +48 semitones | 0 semitones | How far above (or below) its pitch a hit starts, sweeping back to it as a synthesized kick or tom does. |
| Pad N Sweep Time | 1 ms to 2 s | 50 ms | How fast it sweeps back: the sweep's time constant. |

### Layers

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pad N Layer Mode | Stack, Velocity, Round Robin, Random | Stack | How the pad's sounds share its hits: all at once; one by how hard it is hit, the softest hits the first's, the range split evenly; each in turn; or one at random, never the same twice in a row. The editor's label is Mode. |
| Pad N Layer 1 to 4 Level | -48 to +12 dB | 0 dB | Each layer's level against the others'. |

### Filter

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Pad N Filter | Off, Low Pass, High Pass, Band Pass | Off | The pad's filter. The editor's label is Type. |
| Pad N Cutoff | 20 Hz to 20 kHz | 1 kHz | The filter's frequency. |
| Pad N Resonance | 0 to 100 % | 0 % | How much the filter rings at its cutoff. |
| Pad N Velocity to Cutoff | 0 to 48 semitones | 0 semitones | How far a hit at velocity 0 closes the filter; a harder hit closes it less. The editor's label is Velocity. |

### Output

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Output | -24 to +6 dB | 0 dB | The level of everything the sampler plays on its own track. A pad on a track of its own doesn't go through it. |

## How it works

**A hit.** A note starts a voice for the pad's sound, or one for each of its sounds in Stack mode. The voice reads the
sound from Start, forwards or reversed, at the pad's tuning, through an envelope: the attack, a hold at full level,
then an exponential decay to -60 dB. In Gate mode the note's end starts the decay, even during the attack. Sweep bends
the pitch at the start and lets it fall back exponentially. Then comes the pad's filter, a 12 dB per octave
state-variable filter (Cytomic's form) whose band-pass is at full level at its centre, and the pad's level, its layer's
level, velocity and pan.

A pad's level, pan and filter follow their values as it rings, so automation and a turned knob are heard at once.
Everything else is set as the pad is hit.

**Reading the sound.** A pad that is untuned and whose sound is at the engine's sample rate plays its samples as they
are. Otherwise it reads between them with a Kaiser-windowed sinc of 16 zero crossings a side whose cutoff follows how
fast it is read (`src/devices/dsp/SincReader.h`), so that the top octave keeps its level and a pad tuned up doesn't
alias.

**Voices.** There are 32. A hit when all are busy takes the oldest. A choked or muted voice fades out over 10 ms.

**Sounds.** Each pad has four layers. A layer plays a factory sound, kept in the project by its name, or a sound file,
which the project copies into its Audio folder and of which the pad plays the first 30 seconds. A sound dropped on a
pad, or tried with Swap and kept, plays over the pad's kit sound; Reset puts the kit's sound back, and Clear takes a
sound off another layer. The sampler swaps a sound in while it plays without the audio thread waiting or freeing
memory.

**Factory sounds.** Every factory sound is synthesized the first time it is needed, with a fixed seed so that it comes
out the same every time, and with its peak at -6 dB (`src/devices/drums/FactoryDrums.h`). There is the default kit and
a kit of sixteen sounds for each of the twelve kit presets. The general drums follow Sound On Sound's "Synth Secrets"
on drums (`src/devices/drums/DrumSynthesis.h`); the classic drum machines' voices are made as their circuits make them,
after Werner, Abel and Smith's analyses of the 808's bass drum and cymbal and the machines' service notes
(`src/devices/drums/DrumMachines.h`). [presets.md](../../presets.md) describes the kits and how they are named and
leveled.

**A pad on a track of its own.** Own Track gives the selected pad a track right after the sampler's, named after what
the pad plays and in the sampler's track's colour. The pad's sound then leaves the sampler's track and plays through
its own track's effects, sends and fader, not through the sampler's Output, and any device's
[sidechain](../devices.md) can listen to it as to any track but the sampler's own, whose sound it comes from. The
engine has the sampler play that pad into a buffer of its own, which the pad's track adds up (`src/engine/nodes/PartOutput.h`). The sampler's track's mute and solo cover its
pads on tracks of their own, and a pad's track has its own mute and solo too. Clicking Own Track again brings the pad
back and deletes its track. Each is one undo step. Removing or replacing the sampler, or deleting its track, deletes
its pads' tracks in the same step, as a Drum Rack's chains go with it in Ableton Live; a pad's track that holds clips
stays, a plain track. A track whose drum sampler has pads on tracks of their own can't be
frozen or bounced, since that would leave those pads out.

**In the piano roll,** a drum track's rows are the sampler's pads that play a sound, named after what they play, and
any other note its clips play (see [the piano roll](../piano-roll.md)).

## Its editor

`src/ui/devices/DrumSamplerEditor.h`. A dock too narrow for all of it shows the parts in pages, with the pads on each
(see [Devices](../devices.md) for the frame every editor shares).

- **Pads:** sixteen at a time, in four rows of four, the lowest at the bottom left; at first the default kit's sixteen
  from note 36. Beside them is a map of all 128, four across with the lowest at the bottom: pads that play a sound are
  filled and light as they play, the selected pad is outlined and the sixteen shown are framed. A click or a drag on
  the map, its up and down arrow keys, or the mouse wheel over the pads, show others a row at a time; Page Up and Page
  Down on the map move by sixteen.
- Each pad shows its note, what it plays (an empty pad shows, faintly, the drum General MIDI puts on its note), its
  choke group in its corner ("C1"), M while it is muted and S while it is soloed. A pad with more than one layer says
  how many more ("Snare +1"). Pads light as they play.
- Clicking a pad, or Space or Enter on it, plays it and selects it; the rest of the editor shows the selected pad's
  controls. A sound dropped on a pad from the browser or the system's file manager plays there.
- **Sound:** the selected pad's name with Reverse, Swap and Reset; its sound drawn as it plays (reversed, from its
  end) with its envelope over it and a line where it is as it plays. Its start, and the ends of its attack, hold and
  decay, are handles dragged sideways, each drag one undo step. Under the drawing: Start, Attack, Hold, Decay and Mode.
- **Pad** (Level, Pan, Velocity, Choke, Mute, Solo) and **Pitch** (Tune, Fine, Sweep, Sweep Time).
- **Layers:** the layer mode, and for each layer its number and what it plays, or "Drop a sound", with its level and a
  Clear button. A sound dropped on a layer replaces what it plays.
- **Filter** (Type, Cutoff, Resonance, Velocity).
- **Output**, with **Own Track** under it, lit while the selected pad plays on a track of its own.

Swap tries sounds from the [browser](../browser.md) on the selected pad in place.

## Limits

- A hit when all 32 voices are busy takes the oldest voice at once, without a fade, so a long sound cut that way can
  click. Stack mode uses a voice for each layer, so rolls over long sounds get there.
- A pad can't choke itself: its own earlier hits always ring on under a new one, and a choke group only stops the
  other pads in it.
- Velocity layers split the range evenly: a layer has no velocity range of its own and layers don't crossfade.
- A pad's sound has a start but no end marker and no loop, and nothing stretches it to the song's tempo.
- The pad's filter has three types at one slope and no envelope of its own.
- No effects or sends for a pad inside the sampler: a pad that needs its own goes on a track of its own.
- A pad is fixed to its note: pads can't be dragged to move or copy them.
- A track whose pads play on tracks of their own can't be frozen or bounced.
- Every kit is synthesized: no recorded kit comes with it.
- Sounds read from floating-point files aren't checked for values that aren't numbers (known bug 35 in
  [known-bugs.md](../../known-bugs.md)).
- In the editor a pad sounds when the mouse button is let go, not when it is pressed, and always at full velocity.
- A press anywhere on the sound's drawing takes the nearest envelope handle and moves it to the pointer.
- Pads light as they play but have no level meters.

## In the code

- DSP: `src/devices/drums/DrumSampler.h`, `src/devices/drums/DrumSampler.cpp`; the factory sounds,
  `src/devices/drums/FactoryDrums.h`, made by `src/devices/drums/DrumSynthesis.h` and
  `src/devices/drums/DrumMachines.h`. Shared: `src/devices/dsp/SincReader.h`.
- Engine: `src/engine/nodes/PartOutput.h` (a pad's sound on a mixer channel of its own).
- Model: `src/model/devices/internal/DrumKit.h` (what each pad's layers play, sounds put on pads, a pad on a track of
  its own), `src/model/devices/internal/DeviceFiles.h` (the project's copy of a sound file),
  `src/model/devices/SwapModel.h` (Swap).
- Editor: `src/ui/devices/DrumSamplerEditor.h`, `src/ui/devices/DrumSamplerEditor.cpp`; the piano roll's drum rows,
  `src/ui/pianoroll/DrumRows.h`.
- Presets: `resources/presets/Drum Sampler/Kits/`, described in [presets.md](../../presets.md).
- Tests: `tests/DrumSamplerTests.cpp`, `tests/DrumSamplerEditorTests.cpp`, `tests/DrumSynthesisTests.cpp`,
  `tests/DrumRowTests.cpp`, `tests/AntialiasingTests.cpp`, `tests/SwapTests.cpp`, `tests/FactoryPresetTests.cpp`; the
  parameters in the test vector `tests/golden/format/devices.json`.
