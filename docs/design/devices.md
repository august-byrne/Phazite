# Devices, containers and presets

*Design section 9.*

A device is an instrument, which turns a track's notes into sound, or an effect, which changes the sound that comes
into it. Phazite has twenty devices of its own and hosts plug-ins as devices beside them. A container groups devices
into one device with parallel chains and eight macros, and a preset keeps a device, a container or a whole track in a
file that any project can take in.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Phazite's own devices: 5 instruments, 15 effects | Built | Each has a page of its own, below |
| A sampled-instrument device (the Multisampler) | Planned | Roadmap step 4.10 |
| An instrument first, then any number of effects, on every track | Built | No limit |
| Effects moved by dragging | Built | Within a channel or a container; not from one channel to another |
| A switch and a Mix for every effect, plug-ins included | Built | The Limiter, the Multiband and the Stereo Expander have no Mix |
| A/B comparison of a device's settings | Planned | |
| The Devices view, with its Instrument and Effects pages | Built | |
| One of Phazite's devices in a window of its own | Built | Containers show in the Devices view only |
| Every built-in editor whole in the default dock, without scrolling | Built | A test checks each device |
| A picture in every editor, with handles to drag | Built | |
| Containers: grouping, eight macros | Built | |
| Chains, splits and instrument containers | Built | |
| A wired graph of devices | Declined | Chains and splits do it without wires |
| Sidechain for the Compressor, the Multiband, the Vocoder and plug-ins | Built | Not for a device in a container |
| Presets of devices, containers and tracks | Built | |
| A preset loaded into a device, and Swap | Built | Swap is for Phazite's own devices |
| The presets that come with Phazite | Built | [presets.md](../presets.md) |
| A preset dropped on a device; renaming presets in the browser | Planned | |
| Plug-ins from others built into Phazite | Declined | They may be offered as optional downloads |

## What a device is

Each of Phazite's own devices is DSP that knows nothing of the song or of the engine that plays it (`src/devices/`).
It describes itself in a `DeviceInfo` (`src/devices/core/DeviceInfo.h`): a type ID, a name, whether it is an
instrument or an effect, whether it listens to a sidechain, and its parameters, each with an ID, a name, a range, a
default and a unit. The type ID and the parameter IDs are what projects and presets save; a choice is saved by its
name. The test vector `tests/golden/format/devices.json` lists every device's parameters.

Phazite's engine plays a device through its runner (`src/engine/nodes/DeviceRunner.h`), which hands it the audio, the
notes and its parameters' values, and does four things for every device alike:

- it applies an effect's Mix (below);
- it plays a mono channel in stereo;
- it reports the device's latency, which the engine makes up for on the other tracks ([engine.md](engine.md));
- it lets the device sleep once what comes in and what it plays have been silent for a quarter of a second, for
  longer than its latency and for as long as the tail it reports. A render never sleeps.

Parameters move smoothly, so that turning a knob does not click. Automation, modulators and macros move a device's
parameters ([automation-and-modulation.md](automation-and-modulation.md)). Each device has a guarded scenario in the
real-time audit ([realtime-audit.md](../realtime-audit.md)): steady playback through it must not allocate.

While Low Latency is on, a device that is on and delays its sound by more than 1 ms is left out of what plays, and the
sound passes as it comes: the Limiter, the Multiband, the Exciter, the Vinyl and a Compressor set to Duck.

## Phazite's own devices

Menus list them in this order: the instruments, then the effects by what they do.

| Device | Kind | What it is |
| --- | --- | --- |
| [Synth](devices/synth.md) | Instrument | A subtractive synth: two oscillators, a sub and noise, unison, a filter, two envelopes and an LFO |
| [Wavetable](devices/wavetable.md) | Instrument | Two oscillators that morph through tables of waves, with warps, three envelopes, two LFOs and eight modulation slots |
| [FM](devices/fm.md) | Instrument | Four operators in eight algorithms, for electric pianos, bells, basses and brass |
| [Sampler](devices/sampler.md) | Instrument | One sound across the keys: held and looped, played through as a one-shot, or cut into slices |
| [Drum Sampler](devices/drum-sampler.md) | Instrument | A pad on every note, 128 in all, each with up to four layers, an envelope, a filter and a choke group |
| [EQ](devices/eq.md) | Effect | Eight bands over a spectrum of what comes in and what goes out |
| [Filter](devices/filter.md) | Effect | A resonant filter of nine types, with drive |
| [Compressor](devices/compressor.md) | Effect | Two modes: Compress, by a drawn curve, and Duck, under a sidechain |
| [Multiband](devices/multiband.md) | Effect | Three bands of dynamics and saturation, then a master band |
| [Limiter](devices/limiter.md) | Effect | A look-ahead limiter that keeps true peaks under a ceiling |
| [Saturator](devices/saturator.md) | Effect | Drive into one of five curves, with a tone tilt |
| [Exciter](devices/exciter.md) | Effect | Adds harmonics of the highs above a frequency |
| [Vinyl](devices/vinyl.md) | Effect | What a record and its player do to a sound |
| [Delay](devices/delay.md) | Effect | Echoes on a note value or a time, with ping-pong and cuts in the loop |
| [Reverb](devices/reverb.md) | Effect | A room or a plate |
| [Phaser](devices/phaser.md) | Effect | Notches swept by an LFO |
| [Flanger](devices/flanger.md) | Effect | A swept comb |
| [Chorus](devices/chorus.md) | Effect | Swept copies of the sound, one to eight voices |
| [Vocoder](devices/vocoder.md) | Effect | Makes a carrier speak with the voice on its track |
| [Stereo Expander](devices/stereo-expander.md) | Effect | Width, bass in mono, polarity |

The synths and the Filter share one set of filter types, described on the [Filter](devices/filter.md)'s page. The
Phaser, the Flanger and the Chorus share an LFO that follows the song's place, so that a sweep is the same each time a
part plays.

## Devices on tracks and the master

- A track has one instrument, then its effects in the order the sound goes through them. A return and a folder that is
  a bus have effects only, and so has the master. A plain folder has none ([mixer.md](mixer.md)).
- A track holds as many effects as it needs.
- **Adding:** the **+ Instrument** and **+ Effect** buttons, in the Devices view and the mixer's strips, open menus of
  Phazite's devices and then the plug-ins found by scanning, by maker. Add Instrument or Effect (F8) opens the plug-in
  picker, and the browser's devices drag onto tracks ([browser.md](browser.md)). An instrument chosen for a track that
  has one replaces it. Each is one undo step ("Add Reverb", "Replace Instrument with Synth").
- **A device's menu** (right-click its name): Replace..., Move Up and Move Down (Move Left and Move Right where devices
  show in a row), Move to Chain for a device in a container, Group into Container or Ungroup Container, Save Device
  Preset... and Remove.
- **Moving:** an effect's name dragged up or down the Devices view's column, or along a mixer strip's slots, moves it
  there as one undo step ("Move Device"), with a line where it would go.
- **On and off:** every effect has a switch. An instrument has none: its track's mute silences it.
- **Selecting:** a click on a device's name shows it; Ctrl-click or Shift-click selects devices together, to group
  them. Devices are selected on their own: selecting one deselects tracks and clips, and the other way round.
- A device on a frozen track shows faint, with its controls off, until the track is unfrozen.

## The Devices view

The lower panel's **Instrument** (F6) and **Effects** (Shift+F6) tabs are two pages of one view of one channel's
devices ([window.md](window.md)). The channel is the selected track's, or, with none selected, the track live MIDI
plays; a click on a device's name in the mixer shows that device's channel, the master's too.

### The column

Down the left side of both pages, a narrow column holds the channel's name, its devices, and its modulators and
macros.

- On the **Instrument** page: the instrument's name, its **Presets** button and its window's button. A track without
  an instrument has a button that chooses one, in the warning colour while its notes make no sound. A folder, a
  return and the master say why they have none.
- On the **Effects** page: the effects one under another, in the order the sound goes through them. Each is one line:
  its switch, its name and its Mix. A click on a name shows that effect beside the column and lights its line, with
  its Presets and its window's button under it. **+ Effect** comes after them, and the column scrolls when it holds
  more than fits.
- A container is one line, as an effect is.
- Modulators and macros are chips under the devices: a grip to drag onto a knob, a name, and a meter of the
  modulator's output or a macro's knob. A click on a chip pops its editor out over the window, beside it, so that the
  knobs it moves stay in view ([automation-and-modulation.md](automation-and-modulation.md)).

Which effect shows is remembered for each channel while Phazite runs, and each page keeps its own while the other
shows. An effect just added shows, and so does a device shown from the mixer.

### Beside the column

The device that shows takes the whole rest of the panel:

- one of Phazite's own devices shows its editor, whole;
- a plug-in shows up to eight knobs for its key parameters, its own interface being in its window
  ([plugin-hosting.md](plugin-hosting.md));
- a container shows its macros beside its chains (below).

### A device's own window

Any of Phazite's own devices can also show in a window of its own: its window's button opens one named after the
device and its channel ("EQ - Bass"), above the main window. The window opens as large as the editor; made narrower,
the editor wraps its parts into rows. Every shortcut and the computer keyboard work in it as in the main window.
Meanwhile the Devices view says where the editor is, its button lit, so that one editor shows the device at a time;
closing the window puts the editor back. A container has no window.

### Fitting the panel

At the default window size (1280 by 800), every built-in device shows whole in the panel, with no scroll bar
(`tests/DevicesViewTests.cpp` checks each one). The editors share a layout that makes this hold
(`src/ui/devices/DeviceLayout.h`):

- an editor is columns of titled parts, in the order the sound goes through them;
- it is as wide at any height: a column whose parts do not fit the height shows one at a time, under tabs of their
  titles;
- in a narrower space its graphs narrow first; narrower still, the columns wrap into rows where the height allows, or
  else show in pages, as many columns on each as fit, under a bar of the pages' names;
- a tab or a page chosen stays chosen;
- a device with modes (the Compressor's Compress and Duck) has its modes as tabs along its top, each a page of parts.

### What the editors share

- **A picture of what the device does,** drawn as the sound plays, in the device's colour, which its knobs' arcs take
  too. The colour is by kind (`DeviceFamily` in `src/ui/theme/DesignTokens.h`): dynamics; tone (the EQ and the
  Filter); drive (the Saturator, the Exciter and the Vinyl); space (the Delay and the Reverb); motion (the Phaser, the
  Flanger and the Chorus); the Stereo Expander's; the Vocoder's. Instruments take the accent.
- **Pictures are controls.** A handle on a picture is a second grip on a parameter that also has a knob, so that
  everything also works from the keyboard and for screen readers. A drag is one undo step. Envelopes drag by their
  points, and each stage bends by a handle in its middle. The wheel bends no line: over a picture it scrolls the view
  the picture is in, except where a device's page says it turns something.
- **Mix and the switch are not in the editor:** they are the device's line's in the column, so that a window of its
  own shows the editor and the line keeps them.
- **What a sidechain listens to is chosen in the editor** (below).
- **Displays cost nothing while hidden.** One timer refreshes every editor that shows, 30 times a second, with the
  level meters (`src/ui/components/DisplayTicker.h`), and a picture repaints only when what it draws moved. What goes
  in and out of a device reaches its editor through a lock-free scope that costs nothing while no editor listens
  (`src/devices/core/DeviceScope.h`).

Each device's page says what its own editor shows and what can be dragged there.

## Mix

Every effect has a Mix, from 0 % (only the sound coming in) to 100 % (only the effect's), for Phazite's own effects, a
plug-in and a container alike. The engine applies it, not the device: a linear crossfade of the sound coming in and
the effect's, with the sound coming in delayed as much as the effect delays its own and turned as the effect turns its
phase, and a change ramped so that a fast move does not click.

- Most effects start at 100 %. The Delay and the Reverb start at 25 % and the Chorus at 50 %; a double-click on the
  knob goes back to the device's own default.
- A return made with an effect has the effect at 100 %: the dry sound reaches the master through the track itself.
- The Limiter, the Multiband and the Stereo Expander have no Mix: a limiter mixed with what came in would let peaks
  past its ceiling, and the Multiband has a blend of its own before its master band.
- A drag of the knob is one undo step ("Change Mix").

## Sidechain

A device with a sidechain can listen to another track in place of the sound passing through it. Of Phazite's own
devices, three have one:

- the [Compressor](devices/compressor.md) ducks under the track chosen, in its Duck mode;
- the [Multiband](devices/multiband.md) has its bands' compressors hear it, band by band;
- the [Vocoder](devices/vocoder.md) plays it as its carrier.

A plug-in has one if it has inputs for one. What a device listens to is chosen inside the device: in the editor of
one of Phazite's own, or in a plug-in's window. The picker, what it offers and what turns a sidechain off are in
[mixer.md](mixer.md#sidechain). In short:

- the source is a track, a return or a bus, but never one whose sound comes from the device's own track, which would
  make a loop;
- choosing a source is one undo step, and deleting the source turns the sidechain off in the same step as the delete;
- a muted source still feeds the sidechain, so that a kick that is never heard can duck a bass, and a solo elsewhere
  leaves it feeding too;
- a device in a container has no sidechain: it hears what passes through it. Grouping a device whose sidechain
  listened to a track turns that off.

## Containers

A container is devices of one channel grouped into one device. It plays them in chains side by side, and it has eight
macros, knobs that modulate the parameters of its devices and chains. A container of one chain is a plain group: its
devices one after another, under one Mix and eight macros.

### Making one

- **Group into Container** (Ctrl+G, or a selected device's right-click) puts the selected effects of one channel into
  a container in the first one's place, in their order, with their settings, their automation and what the track's
  modulators do to them. A device right-clicked without being selected is grouped alone.
- **Ungroup Container** (Ctrl+Shift+G) puts the devices of a container of one chain back on the channel. A container
  of several chains cannot be ungrouped, since chains side by side would sound otherwise in a row.
- **Container** is the first item of every effects' menu: an empty container after the channel's effects ("Add
  Container"), with one chain. An empty container passes the sound as it came.
- Each is one undo step.

### Chains

- A chain's devices play one after another. A container's chains play side by side, each through its own volume, pan
  and switch for mute and solo, each delayed to line up with the latest, and they add up. The container's Mix blends
  that with what came in.
- **+ Chain** adds one, up to 16. A chain's menu offers Rename, Duplicate, Move Left, Move Right and Remove; a
  container's only chain cannot be removed.
- A chain's switch works as a track's: a click mutes it, a right-click solos it alone, Ctrl+right-click along with
  others. These are not undo steps.
- An effect moves among its chain's devices, or into another chain of its container, by its name dragged there or by
  Move to Chain.
- A chain's volume, its pan and its crossover are parameters, as a device's are: macros and the track's modulators
  move them, and automation lanes show them under the track.

### Splits

A container of effects says what part of the sound coming in each chain plays:

| Split | Chains | What each plays |
| --- | --- | --- |
| Layers | Up to 16 | All of it. Two empty chains are twice as loud; a dry chain beside a compressor is parallel compression |
| Frequency Bands | Up to 16 | A band, low to high in the chains' order, each from its chain's crossover |
| Mid and Side | Two | The first what the sides share, the second what they do not |
| Left and Right | Two | The first the left side, the second the right |

But for layers, the parts add up to the sound, so that empty chains pass it as it came. The bands are split by
Linkwitz-Riley crossovers of the fourth order, the lower bands kept in phase with the higher. Two bands start at
250 Hz; three at 120 Hz and 2.5 kHz; four at 120 Hz, 800 Hz and 5 kHz; a chain added gets a band above the last. A
crossover cannot pass its neighbours', and the chains of a container split into bands stay in their bands' order.

### Instrument containers

An instrument container is a track's instrument. Each chain plays an instrument of its own, then its effects, on the
notes in its keys and its velocities.

- It is made from **Container**, the first item of the instruments' menu (the instrument the track had goes into its
  first chain), or by grouping the instrument with effects.
- A chain's **+ Instrument** chooses its instrument.
- A chain's Keys and Velocities are two bars dragged by their ends, each drag one undo step. Every chain plays every
  note at first, which layers them; keys apart make a split; velocities apart, a soft and a hard sound.
- A note ends in the chain it started in, whatever that chain's keys are by then.
- An instrument container has no split, and cannot go into another's chain.

### Macros

- Eight, always there: "Macro 1" to "Macro 8", each at 0 when made, so that a parameter it modulates does not move
  until the macro is turned.
- A macro's grip is dragged onto a knob of one of the container's devices or chains, or the knob's right-click offers
  Modulate. A macro moves only its own container's devices and chains.
- The track's modulators reach the devices in its containers too.

### In the Devices view

A container clicked in the column shows its name over its macros, and beside them its chains side by side. Each chain
is a card, as a mixer strip is: its switch, name and menu's button; what part of the sound it plays; its crossover (a
band's), volume and pan; an instrument container's chain's keys and velocities; then its devices one under another,
each with its switch, name and Mix, and its own + Effect. The split and + Chain are along the top. The chains scroll
when the panel is smaller than they are; the macros stay.

A device clicked in a chain shows in the chains' place, beside the macros, under a line that says where it is: the
container, the chain, then the device, with its Presets and its window's button. The container's name and the
chain's each lead back to the chains.

### Rules

- A container cannot go in a container.
- A container's devices are devices: they switch on and off, move, save presets, are automated in lanes under the
  track and are modulated as any device is.
- A duplicated track's containers are copies, which the copy's own modulators and macros modulate.

## Presets

### Files

A preset is one file, `.phazitepreset`: JSON that names one of Phazite's devices by its type and every parameter by
its ID, a plug-in by what identifies it with its own state, and containers, modulators and macros as Phazite names
them. The model makes the song's parts from it as it loads, with new IDs, so that a change in how the song keeps a
device cannot break a saved preset. Presets carry no format version.

- The user's presets are in the `Phazite/Presets` folder of the user's documents: a folder for each device ("EQ", or
  a plug-in's name), `Containers` and `Tracks`.
- The presets that come with Phazite are in a `presets` folder next to the app, by device and kind of sound. Nothing
  writes to them.
- The browser shows the two as **Phazite Presets** and **Presets**, and search finds both
  ([browser.md](browser.md)).

### Saving

- **Save Device Preset...** (a device's or a container's right-click) and **Save Track Preset...** (a track's) open a
  save dialog in the preset's folder, named after what is saved.
- A device's **Presets** button, and a plug-in's window's Plug-in menu, have **Save Preset...** too.
- A track dragged from the playlist into the browser is saved at once in `Tracks`, named after it.

### What a preset holds

| Preset | Holds | Leaves out |
| --- | --- | --- |
| Device | Its settings and its Mix: every parameter of one of Phazite's own, a plug-in's whole state | Its automation, what modulates it from outside, its sidechain's source, whether it is on |
| Container | Its name and Mix, its chains with their devices, settings and ranges, its split and its kind, and its macros with what they modulate in it | The same |
| Track | Its name, colour, volume and pan, its instrument, effects and containers, and its modulators and macros with what they modulate on it | Its clips, automation and sends, and where its sound goes |

The sound files a device plays (a Drum Sampler's pads', a Sampler's sound, a Wavetable's tables) go with a preset: they
are copied into a folder beside it named after it ("Kit Sounds"), and the preset names each by its path from its own
folder ("Kit Sounds/Boom.wav"), as a sampler's patch keeps its samples beside it. Copied with that folder, a preset
works on any computer, and it holds no path of the computer it was saved on. A sound that can't be copied there (a
full disk, a file where the folder goes) fails the save, which says which sound and leaves nothing behind. A project
that takes the preset copies them into its own folder; the sounds it can't take (on a network share, not there, or not
beside the preset: named by a full path, or out of its folder by "..") are told by name. The sounds and tables that
come with Phazite go by their names.

### Loading

- **Into the song:** a preset dragged from the browser onto a track or a mixer channel, or Enter or a double-click on
  it for the selected track, goes where a device would. An effect's or a container's goes after the channel's
  effects, or on the master when dropped below the tracks. An instrument's takes the place of the track's, or makes a
  new track. A track's makes a new track after the one it is dropped on, or at the end. Each is one undo step, "Add"
  and the preset's name, with new IDs throughout, so that a container's macros modulate the devices that came with
  them.
- **Into a device:** a device's **Presets** button lists Phazite's presets for it by kind of sound, then the user's
  under **Your Presets**. The one chosen replaces the device's settings and Mix as one undo step ("Load Preset Warm
  Pad"). Every parameter the preset does not name goes to its default, so that a preset is the same starting point
  whatever the device was set to. A plug-in's window's **Load Preset** does the same for the plug-in. A preset of
  another device, of a container or of a track is refused.

### Swap

**Swap: Try Them in Place**, first in the Presets menu of one of Phazite's own devices, tries presets in the device
itself. The browser lists the device's presets alone, and each one it moves to is put in place and heard at once: in
the beat while the song plays, through the track's devices; while it is stopped, an instrument plays a short note.
Enter or Keep
keeps the one tried as one undo step; Escape or Put Back puts the old settings back, leaving nothing to redo. Anything
else that changes the song meanwhile keeps what is tried, as a step of its own, and ends the swap.

A Drum Sampler's pad swaps its sound the same way ([drum-sampler.md](devices/drum-sampler.md),
[browser.md](browser.md)).

### The presets that come with Phazite

Every device has presets: the instruments across kinds of sound, the effects the settings producers reach for. Each
names every parameter of its device. [presets.md](../presets.md) says what they cover, how they are named (by their
sound, never by a trademark), how they are leveled and how they are checked; `tests/FactoryPresetTests.cpp` loads and
plays each one.

## Modulators

A track's modulators and macros, the ring a modulated knob shows, and what Modulate offers are in
[automation-and-modulation.md](automation-and-modulation.md). This page covers only where they sit in the Devices
view and a container's macros.

## Plug-ins

A plug-in is a device as Phazite's own are: it sits in the same slots, has a switch and a Mix, goes into containers,
is automated and modulated, and saves presets. How plug-ins are found, hosted in helper processes and recovered after
a crash, and their windows with the keys that work over them, are in [plugin-hosting.md](plugin-hosting.md).

## Decisions

- No device from others is built into Phazite, however good or freely licensed, so that its own devices keep one
  design language.
- A track's devices show in one place, the Devices view, and a window is something the user asks for: no editor
  floats by default.
- Effects stack in a column and one shows at a time in the rest of the panel, so that adding effects never leaves the
  open one less room.
- Modulators' editors pop out over the window rather than open in line, so that they do not squeeze the devices and
  the knobs they move stay in view.
- Every built-in editor fits the default panel, with one level of tabs or pages at most.
- Every part of an editor that works in its own way has a title.
- The engine applies Mix, to Phazite's own effects and to plug-ins alike, as a linear crossfade.
- Lines in pictures bend by handles, not by the wheel, which scrolls the view under it.
- Containers have chains and splits, and no wired graph: a graph would be a second way to route sound, beside the
  mixer's.
- Layers add up without being turned down, so that a dry chain beside an effect is parallel processing.
- Eight macros, always there, starting at 0.
- A preset is written in Phazite's own names rather than as a piece of the song, so that the song's format can change
  without breaking presets.
- A preset loaded into a device sets every parameter, so that it sounds the same whatever was there before.
- A preset does not hold automation, a sidechain's source or whether the device is on: those belong to the song it
  was saved from.
- A track holds any number of devices.

## Not built

- **The Multisampler,** a device for instruments of many recordings (pianos, guitars, strings) from SFZ files:
  Planned, roadmap step 4.10.
- **Note effects** (an arpeggiator, a chord or scale device before the instrument): not built; no plan yet. A device is
  an instrument or an effect.
- **A/B comparison of a device's settings:** Planned.
- **Moving, copying or duplicating a device to another track:** not built; no plan yet. A preset, or a copy of the
  whole track, does it.
- **Containers in containers, a chain picked by a control, fades between key or velocity ranges, and a container's
  split as a parameter:** not built; no plan yet.
- **Sidechains for devices in containers:** Planned.
- **Automating a macro:** not built ([automation-and-modulation.md](automation-and-modulation.md)).
- **A wired graph of devices:** Declined.
- **A preset dropped on a device of its kind, to replace its settings:** Planned.
- **Renaming presets in the browser:** Planned.
- **Swap for a plug-in's presets:** not built; a plug-in's window lists and loads them.
- **A preset's sounds kept beside it:** not built. A user's preset names sound files by their paths, so a preset whose
  files have moved loads without them.
- **An equal-loudness Mix** for reverbs and delays: not built; no plan yet.

## In the code

- `src/devices/`: each device's DSP, one folder per area; `core/DeviceInfo.h` (how a device and its parameters are
  described), `core/Processor.h` (what a device does with a block), `core/DeviceScope.h` (what its editor sees).
- `src/engine/nodes/DeviceRunner.h`, `DeviceNode.h`: a device as the engine plays and keeps it; `Container.h`: a
  container's chains, split and mix.
- `src/model/devices/internal/Devices.h`: the list of Phazite's devices. `src/model/devices/DeviceHandle.h`,
  `ParameterHandle.h`: a device and a parameter as editors reach them. `src/model/devices/SwapModel.h`: Swap.
- `src/model/mixer/MixerModel.h`: devices in slots, adding, moving, replacing and removing them, Mix (`MixHandle.h`),
  sidechains. `src/model/mixer/internal/Mixer.h`, `Sidechain.h`: the song's side.
- `src/model/mixer/ContainerModel.h`, `ContainerValues.h`, `internal/Containers.h`: containers, chains, splits and
  macros.
- `src/model/presets/PresetModel.h`: saving and loading presets. `resources/presets/`: the presets that come with
  Phazite.
- `src/ui/mixer/DevicesView.h`: the Devices view; `ContainerEditor.h`: a container's macros and chains;
  `DeviceSlots.h`: a strip's slots and a device's menu; `ModulatorSlots.h`: the chips.
- `src/ui/devices/`: `DeviceEditors.h` (which editor a device gets), `DeviceEditorBase.h`, `DeviceLayout.h`,
  `ParameterControls.h`, `DeviceGraph.h`, `GraphDot.h`, `DeviceTint.h` (the shared frame), `DeviceMenus.h` (the menus
  that add devices), `SidechainMenus.h` (the sidechain's picker), `DeviceWindowContent.h` (an editor in a window), and
  an editor for each device.
- `src/app/PluginWindow.h`: devices' windows, a plug-in's and one of Phazite's own.
- `src/ui/browser/Browser.h`: the Presets menu, the save dialogs and Swap's strip.
- Tests: `tests/DeviceInfoTests.cpp`, `tests/EffectDeviceTests.cpp`, `tests/DeviceEditorTests.cpp`,
  `tests/DevicesViewTests.cpp`, `tests/ContainerTests.cpp`, `tests/ContainerChainTests.cpp`,
  `tests/ContainerUiTests.cpp`, `tests/SidechainTests.cpp`, `tests/PresetTests.cpp`, `tests/FactoryPresetTests.cpp`,
  `tests/SwapTests.cpp`, and the golden list of every device's parameters, `tests/golden/format/devices.json`.
