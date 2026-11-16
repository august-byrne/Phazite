---
title: The browser and plug-ins
description: Finding sounds and presets, filters, previews, favorites and collections, Swap, Phazite Sounds, and plug-ins.
sidebar:
  order: 9
---

The browser finds sounds, presets, instruments and effects, plays a sound before you use it, and puts things into the
song. It is a view of the left panel: click its icon in the activity bar down the window's left edge, or press
**Ctrl+F** to show it with the cursor in its search field.

## The places

The browser is laid out like a file explorer: a search field, the filters, a tree of places, and under the tree the
selected sound and the preview's controls.

| Place | Holds |
|---|---|
| Favorites | What you starred |
| Recent | The last items you put into the song |
| Collections | Your named sets, then **New Collection...** |
| Phazite Sounds | The free sounds, once downloaded; until then a row that downloads them |
| Instruments | Phazite's instruments, and the instrument plug-ins found by scanning |
| Effects | Phazite's effects, and the effect plug-ins found by scanning |
| Phazite Presets | The presets that come with Phazite, by device and by kind of sound |
| Presets | Your own presets |
| Your folders | Each folder you added, as it is on disk |
| Add Folder... | Adds a folder of your sounds |

One click opens a folder. A folder you added has **Remove from the Browser** in its right-click menu.

## Search and tags

Type in the search field to list what matches across the sounds, the presets, the folders and the devices: every word
you type, in any order, in a name, its tags or the names of the folders it is in. The best matches come first: names that
start with what you typed, then names with a word that does, then everything else that matches, with folders last. The
list shows the best 200; where there are more, its last line says how many ("The first 200 of 3,412"), so type more
words to find fewer. Typing never waits for the search, which runs while you type. **Down** or **Enter** moves from the
field to the results of what you typed last.

Phazite tags each sound from the words in its name and its folders' names: what it is (Kick, Snare, Clap, Hat, Cymbal,
Tom, Percussion, Bass, FX, Foley, Vocal, Synth), loop or one-shot, its tempo ("120 BPM") and its key ("F# minor"). WAV
and AIFF files that carry loop data give their key and tempo too. Search finds tags, so "kick loop" finds a kick loop
in a folder called Loops.

## Filters

Under the search field is a chip for each of **Type**, **Loop**, **Tempo** and **Key**. A chip opens a menu of the
values the sounds shown have, each with how many sounds have it, so a filter never leaves you with nothing. A chip
that is set shows its value; its x takes it away, and **Clear** takes away every filter and the search. Filters are
not kept from one start of Phazite to the next.

While a filter is set, each place lists its matching sounds as one list, its folders' sounds within it.

## Preview

| Gesture | Does |
|---|---|
| Click a sound, or move to it with the arrow keys | Plays it |
| Click a loop that is playing | Stops it |
| Click a one-shot again | Plays it again |
| **Escape** | Stops the preview |
| Play or stop the song | Stops the preview |

- **Loops play in time** with the song's tempo, stretched as their clips would be, and keep their own pitch. While the
  song plays, a loop you start plays along on the song's bars.
- **One-shots play as they are.**
- **MIDI files** play on Phazite's synth, in time.
- The headphones switch under the tree turns previews off; the fader beside it sets how loud they play (a
  double-click sets -6 dB).

Under the tree, the selected sound shows its waveform and a line of what it is: a loop, at what tempo and how many
bars, or a one-shot; how long it lasts; its sample rate, bits and channels.

## Putting things into the song

**Drag and drop:**

| Dropped on | What happens |
|---|---|
| Empty space below the tracks | A new track, with the sound as a clip or the device on it |
| A track | A sound becomes a clip at the grid line before the pointer; a device goes on the track |
| A mixer channel | A device goes on the channel |
| One of the drum sampler's pads | The sound becomes the pad's sound |

**Enter** or a double-click puts a sound on the selected track where playback starts (or on a new track), and a
device or a preset onto the selected track. **Add to the Song**, in an item's right-click menu, does the same.

A track dragged from the playlist into the browser is saved as a track preset.

## The plug-in picker

**Add Instrument or Effect** (**F8**) opens a small search over every device: Phazite's own and your plug-ins. Type
part of a name or a maker's name, move with the arrow keys, and press **Enter** (or click) to add it to the selected
track, or to a new one. **Escape** closes it.

## Favorites and collections

- **Favorites:** click the star at the end of a row, or choose **Add to Favorites** in its right-click menu.
- **Collections** are your own named sets of sounds, presets, folders and devices. Right-click an item, **Add to
  Collection**, then a collection (ticked where the item is in it already; choose it again to take it out) or **New
  Collection...**. A collection's own menu has **Rename...** and **Delete Collection**; its sounds stay where they are
  on disk.
- A sound renamed or moved on disk stays in its collections and among the favorites, once the browser finds it again.

## Swap

Swap tries sounds or presets in place in the song, so you hear each one in the mix before you choose.

- **On a drum pad:** the **Swap** button in the drum sampler's editor.
- **On one of Phazite's own devices:** **Swap: Try Them in Place**, the first item of its **Presets** menu.

While a swap is open, a strip over the browser's search says what is being swapped, with **Keep** and **Put Back**. Move
through the browser with the arrow keys or click: each sound (for a pad) or preset (for a device) plays in its place,
in the beat while the song plays, or on its own while it is stopped. A pad's swap lists sounds of the same type; a
device's swap lists its presets alone.

| To | Do |
|---|---|
| Keep what you are hearing | **Enter**, a double-click, or **Keep**: one undo step |
| Go back to what was there | **Escape** or **Put Back** |

Any other change to the song while a swap is open keeps what you were trying and ends the swap.

## Phazite Sounds

Phazite Sounds are 85 free drums, loops, effects and foley sounds to start with, in the public domain (CC0), so songs
made with them owe nothing to anyone. They are a separate download, only when you ask: from the Phazite Sounds row in
the browser, the Welcome window, or **Help > Phazite Sounds**. On a computer without a connection, download the zip from
the [phazite-sounds releases](https://github.com/august-byrne/phazite-sounds/releases) elsewhere and use **Install
from a File...** on the Welcome window's Phazite Sounds page.

## Plug-ins

**Scan for Plug-ins**, in the menu of the gear at the bottom left, finds your VST®3 plug-ins. They then show under
Instruments and Effects, in the **+ Instrument** and **+ Effect** menus and in the plug-in picker.

- **Each plug-in plays in a helper process of its own,** so a plug-in that crashes or hangs can't take Phazite down. A
  crashed plug-in's slot reads "*name* - Crashed" and plays nothing (an effect's sound fades out); click the slot,
  or choose **Reload** in its menu, to open it again as it was. The Project view lists crashed plug-ins too
  ([The window](window.md#the-left-panel)).
- **A plug-in's window** opens from its slot's menu (**Open Window**) or its window button in the Devices view. Its
  **Plug-in** menu has **Save Preset...**, **Load Preset**, and **Send All Keys to the Plug-in**, for a plug-in that
  needs the keys Phazite otherwise keeps for itself (play and stop, and the computer keyboard).
- A plug-in that isn't installed on this computer stays in the song, silent, until a scan finds it.

[Troubleshooting](troubleshooting.md) says what to do when a plug-in is missing or keeps crashing.

VST is a registered trademark of Steinberg Media Technologies GmbH.

## Not yet

- Sounds known by listening to them (type, tempo, key), and Show Similar: planned.
- Loops previewed in the song's key, and tempo found for loops whose names don't give one: planned.
- Previews through the selected track's devices: planned (Swap does it for a pad or a device).
- Swap for a plug-in's presets, for wavetables, and for a pad's layers after the first: planned.
- Ratings and sample maps: not planned; favorites and collections cover them.
