---
title: Keyboard shortcuts
description: Every command that has a key, by area, in Phazite's keymap and beside it in the FL Studio and Ableton Live keymaps.
sidebar:
  order: 13
---

Phazite comes with three keymaps: **Phazite** (the default), **FL Studio** and **Ableton Live**, the last two after
those apps' manuals. You choose one on the first page of the Welcome window, and change it later in **Keymap**, in the
menu of the gear at the bottom left.

**Keyboard Shortcuts** (in the Help menu and the gear's menu) lists every command with its keys and changes them. Each
keymap keeps its own changes. **Ctrl+K** finds any command by name and shows its keys, so a command without a key is
always one search away.

## How the keys work

- **Keys that depend on what is selected.** Some commands share a key, and the one that can act runs. With notes
  selected, the arrow keys move the notes; with automation points selected, they move the points; otherwise **Up** and
  **Down** select tracks and **Left** and **Right** nudge clips. **Ctrl+G** groups the selected devices into a
  container, or else the selected tracks into a folder. **Ctrl+E** chops the selected notes, or else splits clips, and
  **Ctrl+L** makes the selected notes legato, or else loops the selected time.
- **Escape** closes an open transform in the piano roll, then deselects the notes, then picks the Smart Tool.
- **Held keys.** A held key runs its command once, except the commands that take a step (nudging, zooming, transposing,
  moving through tracks, undo), which repeat.
- **The time line's commands** (zoom, grid, tools, nudge, quantize) act on the editor you used last: the playlist, the
  piano roll or the audio clip editor.
- **Over a plug-in's window,** Play / Stop, Play / Pause, A/B Compare and Automate Last Touched Parameter still work,
  and so does the computer keyboard.
- **In a text field,** the keys type. A click anywhere outside the field gives them back to the shortcuts.

## The computer keyboard plays notes

**Play Notes with the Computer Keyboard** (**M**; **Ctrl+T** in the FL Studio keymap, or the keyboard button in the
transport bar) turns your typing keyboard into a piano that plays the selected track's instrument. It starts off, so
that the single-letter shortcuts work.

| Keys | Play |
|---|---|
| **A S D F G H J K L ;** | The white keys |
| **W E T Y U O P** | The black keys |
| **Z**, **X** | An octave down, an octave up |
| **C**, **V** | Quieter, louder |

While it plays notes, **Shift** and a letter run that letter's shortcut (**Shift+P** for the Draw tool, for example).
The digits **1** to **9** pick the tools either way, in the keymaps that have them.

## The keys, by area

In the tables, "–" means the keymap gives the command no key. Commands not listed have no key in any keymap: find them
with **Ctrl+K** or give them one in Keyboard Shortcuts.

### Transport

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Play / Stop | **Space** | **Space** | **Space** |
| Play / Pause | **Shift+Space** | **Ctrl+Space** | **Shift+Space** |
| Go to Start | **Home** | **Home** | **Home** |
| Go to End | **End** | – | **End** |
| Loop | **L** | – | – |
| Loop Selection | **Ctrl+L** | **Ctrl+L** | **Ctrl+L** |
| Metronome | **O** | **Ctrl+M** | **O** |
| Record | **R** | **R** | **F9** |
| Capture | **Ctrl+Shift+C** | **Ctrl+Shift+C** | – |
| Count-In | – | **Ctrl+P** | – |

### Editing

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Undo | **Ctrl+Z** | **Ctrl+Alt+Z** | **Ctrl+Z** |
| Redo | **Ctrl+Shift+Z**, **Ctrl+Y** | – | **Ctrl+Y**, **Ctrl+Shift+Z** |
| Delete | **Delete**, **Backspace** | **Delete** | **Delete**, **Backspace** |
| Duplicate | **Ctrl+D** | **Ctrl+B** | **Ctrl+D** |
| Mute | **0** | **Alt+M** | **0** |
| Split | **Ctrl+E** | **Insert** | **Ctrl+E** |
| Insert Time | **Ctrl+I** | **Ctrl+Insert** | **Ctrl+I** |
| Remove Time | **Ctrl+Shift+Delete** | **Ctrl+Delete** | **Ctrl+Shift+Delete** |
| Select All | **Ctrl+A** | **Ctrl+A** | **Ctrl+A** |
| Cut | **Ctrl+X** | **Ctrl+X** | **Ctrl+X** |
| Copy | **Ctrl+C** | **Ctrl+C** | **Ctrl+C** |
| Paste | **Ctrl+V** | **Ctrl+V** | **Ctrl+V** |
| Nudge Left | **Left** | **Shift+Left** | **Left** |
| Nudge Right | **Right** | **Shift+Right** | **Right** |
| Quantize | **Ctrl+U** | **Ctrl+Alt+Q**, **Shift+Q** | **Ctrl+U** |
| Rename | **Ctrl+R**, **F2** | **F2** | **Ctrl+R** |
| Deselect All | – | **Ctrl+D** | – |
| Undo or Redo | – | **Ctrl+Z** | – |

### Clips

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Consolidate | **Ctrl+J** | – | **Ctrl+J** |
| Next Take | – | – | **Ctrl+Down** |
| Previous Take | – | – | **Ctrl+Up** |
| Reverse | – | **9** | **R** |

### Tracks

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Add Track | **Ctrl+T** | – | **Ctrl+T** |
| Group into Folder | **Ctrl+G** | – | **Ctrl+G** |
| Ungroup Folder | **Ctrl+Shift+G** | – | **Ctrl+Shift+G** |
| Collapse Track | **U** | – | **U** |
| Collapse All | **Alt+U** | – | **Alt+U** |
| Select Track Above | **Up** | **Ctrl+Up** | **Up** |
| Select Track Below | **Down** | **Ctrl+Down** | **Down** |
| Taller Tracks | **Alt+=** | – | **Alt+=** |
| Shorter Tracks | **Alt+-** | – | **Alt+-** |
| Add Return Track | – | – | **Ctrl+Alt+T** |
| Arm Track | – | – | **C** |
| Bounce in Place | – | **Ctrl+Alt+C** | **Ctrl+B** |
| Freeze Track | – | – | **Ctrl+Shift+Alt+F** |
| Move Track Down | – | **Alt+Down** | – |
| Move Track Up | – | **Alt+Up** | – |
| Solo Track | – | – | **S** |

### Tools

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Smart Tool | **Escape**, **1** | **Escape** | **Escape**, **1** |
| Draw Tool | **P**, **2** | **P** | **B**, **2** |
| Paint Tool | **B**, **3** | **B** | **3** |
| Slice Tool | **C**, **4** | **C** | **E**, **4** |
| Slip Tool | **S**, **5** | **S** | **5** |
| Mute Tool | **T**, **6** | **T** | **6** |
| Delete Tool | **D**, **7** | **D** | **7** |
| Select Tool | **E**, **8** | **E** | **8** |
| Zoom Tool | **Z**, **9** | **Z** | **9** |

### Grid

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Snap to Grid | **Ctrl+4** | **Backspace** | **Ctrl+4** |
| Adaptive Grid | **Ctrl+5** | – | **Ctrl+5** |
| Finer Grid | **Ctrl+1** | – | **Ctrl+1** |
| Coarser Grid | **Ctrl+2** | – | **Ctrl+2** |
| Triplet Grid | **Ctrl+3** | – | **Ctrl+3** |

### Notes

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Transpose Up | **Up** | **Shift+Up** | **Up** |
| Transpose Down | **Down** | **Shift+Down** | **Down** |
| Octave Up | **Shift+Up** | **Ctrl+Up** | **Shift+Up** |
| Octave Down | **Shift+Down** | **Ctrl+Down** | **Shift+Down** |
| Lengthen Notes | **Shift+Right** | – | **Shift+Right** |
| Shorten Notes | **Shift+Left** | – | **Shift+Left** |
| Legato | **Ctrl+L** | **Ctrl+L** | – |
| Chop | **Ctrl+E** | **Ctrl+U** | **Ctrl+E** |
| Select All Notes | **Ctrl+A** | **Ctrl+A** | **Ctrl+A** |
| Deselect All Notes | **Escape** | **Escape** | **Escape** |
| Quantize... | **Ctrl+Shift+U** | **Alt+Q** | **Ctrl+Shift+U** |
| Apply Transform | **Return** | **Return** | **Return**, **Ctrl+Return** |
| Cancel Transform | **Escape** | **Escape** | **Escape** |
| Arpeggiate... | – | **Alt+A** | – |
| Chop... | – | **Alt+U** | – |
| Strum... | – | **Alt+S** | – |

### Automation

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Automate Last Touched Parameter | **Ctrl+Shift+A** | **Ctrl+Shift+A** | – |
| Show Automation | **A** | – | **A** |
| Raise Automation Points | **Up** | **Shift+Up** | **Up** |
| Lower Automation Points | **Down** | **Shift+Down** | **Down** |
| Raise Automation Points More | **Shift+Up** | **Ctrl+Up** | **Shift+Up** |
| Lower Automation Points More | **Shift+Down** | **Ctrl+Down** | **Shift+Down** |

### Devices

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Group into Container | **Ctrl+G** | – | **Ctrl+G** |
| Ungroup Container | **Ctrl+Shift+G** | – | **Ctrl+Shift+G** |
| Add Instrument or Effect | **F8** | **F8** | **F8** |

### Projects

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| New Project | **Ctrl+N** | – | **Ctrl+N** |
| Open Project | **Ctrl+O** | **Ctrl+O** | **Ctrl+O** |
| Save Project As | **Ctrl+Shift+S** | **Ctrl+Shift+S** | **Ctrl+Shift+S** |
| Save Project | **Ctrl+S** | **Ctrl+S** | **Ctrl+S** |
| Export Song | **Ctrl+Shift+R** | **Ctrl+R** | **Ctrl+Shift+R** |

### Source Control

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Commit | **Ctrl+Alt+S** | **Ctrl+N** | **Ctrl+Alt+S** |
| A/B Compare | **N** | **N** | **N** |

### View

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Search Commands | **Ctrl+K** | **Ctrl+K** | **Ctrl+K** |
| Open the Menus | **F10** | – | **F10** |
| Search the Browser | **Ctrl+F** | – | **Ctrl+F** |
| Show or Hide the Left Panel | **Ctrl+Alt+B** | **Alt+F8** | **Ctrl+Alt+B** |
| Show or Hide the Lower Panel | **Ctrl+Alt+L** | – | **Ctrl+Alt+L** |
| Show or Hide the Inspector | **Ctrl+Alt+I** | – | **Ctrl+Alt+I** |
| Maximize the Panel in Use | **Shift+F11** | – | **Shift+F11** |
| Piano Roll | **F7** | **F7** | **Ctrl+Alt+3** |
| Instrument | **F6** | **F6** | **Ctrl+Alt+4** |
| Effects | **Shift+F6** | **Shift+F6** | – |
| Playlist | **F5** | **F5** | – |
| Mixer | **F9** | **F9** | **Ctrl+Alt+M** |
| Zoom In | **=**, **Numpad +** | **Page up** | **=**, **Numpad +** |
| Zoom Out | **-**, **Numpad -** | **Page down** | **-**, **Numpad -** |
| Follow Playback | **Ctrl+Shift+F** | – | **Ctrl+Shift+F** |
| Center on Playhead | – | **Shift+0** | – |
| Full Screen | **F11** | – | **F11** |
| Hover Help | – | – | **Shift+/** |
| Larger Interface | **Ctrl+=**, **Ctrl+Numpad +** | – | **Ctrl+=**, **Ctrl+Numpad +** |
| Normal Interface Size | **Ctrl+0**, **Ctrl+Numpad 0** | – | **Ctrl+0**, **Ctrl+Numpad 0** |
| Smaller Interface | **Ctrl+-**, **Ctrl+Numpad -** | – | **Ctrl+-**, **Ctrl+Numpad -** |
| Maximize the Upper Panel | – | **F12** | – |
| Zoom Back | – | – | **X** |
| Zoom to Fit | – | **Shift+4** | **W** |
| Zoom to Selection | – | – | **Z** |

### Playing

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| Play Notes with the Computer Keyboard | **M** | **Ctrl+T** | **M** |

### Settings

| Command | Phazite | FL Studio | Ableton Live |
|---|---|---|---|
| MIDI Settings | – | **F10** | – |
| Settings | **Ctrl+,** | – | **Ctrl+,** |

## The FL Studio keymap's mouse

The FL Studio keymap also changes what the mouse does in the playlist, as FL Studio does:

- A right-click on a clip deletes it, and so does dragging on from it over others; **Shift**+right-click opens the menu.
  In an automation lane, a right-click deletes points the same way.
- A click on empty space places a copy of the clip you used last; **Ctrl**+drag selects a time range there.
- A time range selected in the playlist loops while it is selected.
