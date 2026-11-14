---
title: The mixer and devices
description: Channels, faders and meters, sends and returns, folders as buses, the sidechain, the Devices view, containers, freezing and bouncing.
sidebar:
  order: 6
---

The mixer shows every channel of the song side by side: a strip for each track, folder and return, and the master on
the right. Press **F9** to show it in place of the playlist, and again to go back. The selected track's instrument and
effects show in the lower panel's **Instrument** and **Effects** tabs, the Devices view (**F6** and **Shift+F6**).

## A strip, top to bottom

A strip reads in the order the sound goes:

1. **The name,** and under it what the channel is and what feeds it. Beside the name are the track's switch for mute
   and solo and, while it records, its arm switch. A folder has its **Bus** switch here.
2. **The instrument** (tracks only), in a slot of its own above the effects. A track with notes and no instrument says
   **Choose instrument**, tinted, so you can see why it makes no sound.
3. **The effects,** each one line: its on/off switch and its name. **+ Effect** adds one. Drag an effect's name up or
   down to move it in the chain.
4. **The sends:** a knob for every return the track can send to. **+ Send** makes a new return with a send to it.
5. **Out:** where the sound goes, "Out: Master" or the bus the track is in.
6. **Pan, the fader and the meter.**
7. **A CPU bar** along the bottom: how much of each block's time the strip's devices take, while the song plays.

| Gesture | Does |
|---|---|
| Click a strip | Selects its channel |
| Click a device's name | Shows the device in the Devices view |
| **Ctrl**+click or **Shift**+click a device's name | Selects devices together, to group them |
| Right-click a device's name | Opens its menu |
| Drag an effect's name up or down | Moves it in the chain |
| Drop a device or a preset from the browser | Adds it to the channel |

Selecting a channel outlines the channels it feeds and those that feed it, through sends, buses and sidechains.

## Faders, pan and meters

- The fader shows and takes decibels, with a scale beside it. 0 dB is about three quarters of the way up, and the top
  is +6 dB.
- The meter beside the fader reads on the same scale: a sound at -12 dB lights it up to the scale's -12, wherever the
  fader is.
- Pan keeps a sound's loudness wherever it is panned.
- Every fader and knob takes a typed value: right-click it and choose **Type a Value...**. A double-click puts it back
  where it started.
- **Escape** while you drag a fader or a knob puts it back where it was when you pressed, leaving nothing to undo.

## Mute and solo

Each track has one small switch, a light, for both mute and solo: in its playlist header, its mixer strip and the
inspector.

| Gesture | Does |
|---|---|
| Click | Mutes the track (the light goes out to a hollow ring); again to hear it |
| Right-click | Solos the track alone; again to end the solo |
| **Ctrl**+right-click | Solos it along with the tracks soloed already |

A soloed track's light is ringed; a track that other tracks' solos silence shows its light faint. A solo never changes
the mute switches, soloing a folder solos its tracks, and returns stay audible while a track that sends to them is
soloed. Neither mute nor solo is an undo step. **Mute Track** and **Solo Track** are in the track's right-click menu
too.

## Sends and returns

A return holds effects that several tracks share, such as a reverb.

- **+ Send** on a strip offers **With an Effect...** and **Empty**: it makes the return and the track's send to it in
  one step. The **+** at the end of the returns adds an empty return, and **Add > Add Return Track** does too.
- Turn a send's knob to send to that return. Once a send is made, its **Pre** / **Post** switch says whether it takes
  the sound before or after the fader.
- A send gives the return a copy: the track still plays through its own output. To run several tracks' whole sound
  through one effect, put them in a bus instead.

## Folders and buses

- A folder is a bus: it mixes its tracks through its own effects, fader and meter.
- Its **Bus** switch can make it a plain folder, which has no sound of its own and whose fader only scales its tracks'
  volumes. A plain folder becomes a bus again when it gets an effect. Either way its volume keeps its automation and
  modulation; a plain folder has no pan, so its pan's go.
- **Out** on a track's strip offers **Master**, the buses, and **New Bus from the Selected Tracks**. Choosing a bus
  moves the track into that folder.

## Sidechain

A device with a sidechain input (Phazite's Compressor, Multiband and Vocoder, or a plug-in that has one) can listen to
another track instead of the sound passing through it. You choose what it listens to inside the device: under
**Sidechain** in the editor of one of Phazite's devices, or at the right end of a plug-in window's header.

- Its menu lists **None** and the other tracks, returns and buses, each with its colour. One whose sound comes from the
  device's own track would make a loop, so it is greyed out, with why.
- Choosing a source turns the sidechain on. A muted source still feeds the sidechain, so a kick you never hear can
  still duck a bass.
- In the mixer, a device that listens to a track has a stripe of that track's colour down its left.

**Duck Under**, in a track's right-click menu, does "duck the bass under the kick" in one step: choose the track to
duck under, and Phazite adds its Compressor after the track's effects, listening to it.

## The master

The master strip has its effects, fader and meter, and a loudness meter: momentary, short-term and integrated loudness
in LUFS, and the true peak. **Reset** starts the integrated loudness and the true peak again.

## The Devices view

The lower panel's **Instrument** (**F6**) and **Effects** (**Shift+F6**) tabs are two pages of one view of the selected
track's devices.

- **A column down the left** holds the channel's devices. On the **Instrument** page: the instrument's name, its
  **Presets** button and its window's button, or **+ Instrument** (**Choose instrument** while the track's notes make
  no sound). On the **Effects** page: the effects one under another, in the order the sound goes through them, each
  with its switch, its name and its **Mix**, then **+ Effect**.
- **Beside the column** the device you clicked takes the rest of the panel: one of Phazite's own devices shows its
  editor; a plug-in shows knobs for its main parameters, and its own interface opens in its window.
- **Modulators and macros** are chips under the devices, with **+ Modulator** to add one
  ([Automation](automation.md#modulators-and-macros)).

**Adding devices.** **+ Instrument** and **+ Effect** list Phazite's devices, then the plug-ins found by scanning.
**Add Instrument or Effect** (**F8**) finds one by name ([The browser](browser.md#the-plug-in-picker)), and devices and
presets drag in from the browser. An instrument chosen for a track that has one replaces it.

**Mix.** Every effect has a **Mix**, from 0 % (only the sound coming in) to 100 % (only the effect's), for Phazite's
effects, plug-ins and containers alike.

**A device's menu** (right-click its name) has, as they apply: **Reload** (for a plug-in that crashed), **Open
Window**, **On**, **Automate**, **Replace...**, **Move Up** and **Move Down**, **Move to Chain**, **Group into
Container** or **Ungroup Container**, **Save Device Preset...** and **Remove**.

**Presets.** A device's **Presets** button lists the presets that come with Phazite for it, by kind of sound, then
yours under **Your Presets**, and has **Save Preset...**. For Phazite's own devices its first item, **Swap: Try Them
in Place**, tries presets in the song as it plays ([The browser](browser.md#swap)).

## Containers

A container groups devices of one channel into one device, with eight macro knobs (**Macro 1** to **Macro 8**) that
turn several of their knobs at once, and chains that play side by side.

- **Group into Container** (**Ctrl+G**, or a device's right-click menu) puts the selected devices into a container, in
  their order, with their settings and automation. **Ungroup Container** (**Ctrl+Shift+G**) puts them back, for a
  container of one chain.
- **Container** is the first item of the **+ Effect** and **+ Instrument** menus: an empty container. An instrument
  container takes the track's instrument into its first chain.
- **+ Chain** adds a chain, up to 16. A chain's menu has **Rename**, **Duplicate**, **Move Left**, **Move Right** and
  **Remove**. A chain's switch works as a track's: click to mute, right-click to solo.
- **The split** says what part of the sound each chain of an effects container plays: **Layers** (all of it, so a dry
  chain beside a compressor is parallel compression), **Frequency Bands** (a band each, from its crossover), **Mid and
  Side**, or **Left and Right**.
- **An instrument container** plays a different instrument in each chain. Each chain's **Keys** and **Velocity** bars,
  dragged by their ends, set which notes it plays: apart for a split keyboard, or soft and hard sounds.
- **Macros:** drag a macro's grip onto a knob of one of the container's devices, or right-click the knob, **Modulate**,
  then the macro. Each starts at 0, so nothing moves until you turn it.

In the Devices view a container shows its macros beside its chains, each chain a card with its devices one under
another. Click a device in a chain to show it; the line over it leads back to the container.

## Freezing and bouncing

- **Freeze Track** (in the track's right-click menu) plays the track from a file of its sound up to its fader, so that
  its instrument and effects take no CPU. A snowflake in its header shows it; click it, or run **Freeze Track** again,
  to play the track live again. Changing what it froze (its clips or a frozen device) unfreezes it. Freezing is not an
  undo step.
- **Bounce in Place** (in the track's right-click menu) renders the track through its instrument and effects onto a
  new audio track under it, and mutes the original. It is one undo step.
- **Consolidate** joins clips into one ([Audio clips](audio-clips.md#consolidating)).

## Not yet

- A routing view with cables: not planned. Routing is set on the strips.
- An output to several places at once: not planned; sends do it.
- Moving or copying a device to another track: not planned yet. A preset, or a copy of the whole track, does it.
- Sidechains for devices inside containers: planned.
- Automating a macro: not yet.
- A/B comparison of a device's settings: planned.
