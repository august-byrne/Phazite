---
title: Automation and modulation
description: Automating a control, lanes, drawing and shaping, write modes, and modulators and macros.
sidebar:
  order: 7
---

**Automation** changes a control along the song: a line you draw in a lane under the control's track. **Modulation**
moves a control around its value while the song plays, by a modulator (an LFO, an envelope follower...) or a macro
knob. Any of Phazite's knobs and faders, and any parameter a plug-in lets you automate, can have both.

## Automating a control

Right-click any knob or fader (in the mixer, the inspector or a device in the Devices view). The menu, headed by the
control's name, has:

| Item | Does |
|---|---|
| Type a Value... | Opens a field over the control to type its value |
| Automate | Shows the control's lane under its track |
| Delete Automation | Deletes the control's automation, with its lane |
| Modulate | The track's modulators, then a new one of each kind ([below](#modulators-and-macros)) |

A plug-in's own window belongs to the plug-in, so you automate its parameters in one of three ways:

- right-click the plug-in's slot in the mixer, **Automate**, then the parameter;
- in an empty lane, click the parameter's name and choose another parameter;
- move the knob in the plug-in's window, then run **Automate Last Touched Parameter** (**Ctrl+Shift+A**), which works
  while the plug-in's window has the keyboard. It shows the lane of the control you changed last, and its name says
  which ("Automate Cutoff").

## Lanes

A lane shows under its track in the playlist; the master's lanes show under a Master row after the tracks. Its header
shows the control's name ("Volume", or "Synth: Filter Cutoff" for a device's) and the track's write mode. In a lane without
automation the name is a button that shows another of the track's controls in the lane instead.

**Automation never hides.** Every control with automation has a lane, so nothing plays from somewhere you can't see.
To make room, a lane collapses to a thin row that still draws its line.

| To | Do |
|---|---|
| Collapse or expand a lane | Double-click its header, or drag its bottom edge up past halfway |
| Change its height | Drag the header's bottom edge; a double-click there puts it back |
| Show the selected tracks' lanes | **Show Automation** (**A**); again to collapse them. A track without lanes gets one for its volume |
| Collapse a track with its lanes | Collapse the track |

A lane's right-click menu has **Collapse Lane** (or **Expand Lane**), **Delete Automation** (**Remove Lane** for a lane
without automation), **Shape for Paint**, **Mode**, and **Delete** and **Duplicate** for the selected points.

## Drawing and shaping

The playlist's tools work in a lane:

| Tool | Gesture | Does |
|---|---|---|
| Smart | Click empty space | Adds a point, on the grid |
| Smart | Press on the line | Puts a point on the line, at the line's value, so nothing changes until you drag it |
| Smart, Draw | Drag a point | Moves it, with the other selected points |
| Smart | Drag the hollow handle in the middle of a line | Bends the line up or down |
| Smart, Draw | Double-click a point | Deletes it |
| Smart, Draw | Double-click a bent line's handle | Straightens the line |
| Smart | Drag on empty space | Selects the points in a rectangle |
| Draw | Drag | Draws a step for each grid cell it passes; with **Alt**, a free line |
| Paint | Drag | Draws a repeating shape, a cycle a grid step long, as high and low as you drag |
| Delete | Drag | Deletes the points it passes |
| Select | Drag | Selects points |

Paint's shape is **Sine**, **Triangle**, **Square**, **Saw Up**, **Saw Down** or **Line**, chosen in the lane menu's
**Shape for Paint**.

A lane runs from the bottom of its control's travel to the top, as the knob or fader turns, and a straight line is an
even turn of the knob, as in most DAWs. A frequency's knob goes through as many octaves in each part of its turn, so a
straight line on a cutoff from 100 Hz to 10 kHz sweeps evenly through the octaves, passing 1 kHz halfway.

While you drag or click, **Alt** places off the grid, **Shift** moves a value in fine steps, and **Ctrl** adds a point
to the selection or takes it out.

**Keys for the selected points:**

| Key | Does |
|---|---|
| **Up**, **Down** | Raises or lowers them by a hundredth of the control's travel |
| **Shift+Up**, **Shift+Down** | By a tenth |
| **Left**, **Right** | Nudges them a step of the grid |
| **Delete** | Deletes them |
| **Ctrl+D** | Copies them right after themselves |

**Typing a value.** Right-click a point and choose **Type a Value...**: a field opens over it with its value in the
control's units ("-6.0 dB", "2.50 kHz"). **Enter** sets every selected point in the lane to it; **Escape** leaves them.
The inspector shows the selected points' value and bend as fields too.

**Automation goes with the music.** Moving or copying clips along their track takes the automation under them along,
and so do Delete and Close Gap, a range of time deleted or duplicated, and Cut, Copy and Paste of clips back onto
their own track ([The playlist](playlist.md#selecting-moving-and-copying)). A clip moved to another track leaves its
automation behind.

## Write modes

Each track has a write mode, Read at first, shown in its lanes' headers and the inspector. **Read Automation**, **Touch
Automation**, **Latch Automation** and **Write Automation** (in the track's **Automation Mode** menu and the command
search) set it for the selected tracks. A mode that writes shows in red.

| Mode | While the song plays |
|---|---|
| Read | The lanes play. A control you move by hand goes back to its lane at the lane's next change |
| Touch | A control writes while you hold it; the lane takes over again when you let go |
| Latch | A control writes from your first move until playback stops |
| Write | Every automated control of the track is overwritten from when playback starts until you let go of it; then the track is in Latch |

Moving a parameter in a plug-in's own window writes the same way. Changing the mode is not an undo step. What you write
lands where you heard it, allowing for the audio output's delay. **Escape** during a drag takes back only that drag.

## Modulators and macros

Each track, folder and the master has modulators of its own, and tracks and returns have macros. They move the
controls of that track's own devices, and its volume, pan and sends.

| Kind | Follows |
|---|---|
| LFO | A wave, repeating |
| Envelope Follower | How loud the track is |
| Step Sequencer | A sequence of steps |
| Random | Random values |
| Breakpoint Envelope | An envelope drawn as points, repeating |
| Note Follower | The pitch or velocity of the notes the track plays |
| Macro | A knob that moves several controls at once |

They show as chips under the devices in the Devices view: a grip, the name, and a meter of what it puts out (a macro's
chip is its knob). **+ Modulator** adds one.

**Assigning one:**

- **By menu:** right-click a knob, **Modulate**, then one of the track's modulators (ticked while it moves that knob;
  choose it again to stop), or **New LFO**, **New Envelope Follower**, **New Step Sequencer**, **New Random**, **New
  Breakpoint Envelope**, **New Note Follower** or **New Macro**, which adds one already moving the knob.
- **By dragging:** drag a modulator by its grip onto a knob. While you drag, every knob it can move shows a faint ring.

**The ring.** A modulated knob shows a ring: the range its modulators move it over, with a dot where it is now. Drag
the ring (the knob's outer edge) up or down to change how far it moves, with **Shift** for fine steps; the rest of the
knob turns it as before.

**The editor.** Click a chip to open its editor over the window: the modulator's name and its **On** switch, what it
put out over the last two seconds, and its settings. A step sequencer's steps and a breakpoint envelope's points are
drawn with the mouse. A click elsewhere or **Escape** closes it.

A chip's right-click menu has **Open Editor**, **Modulates** (with **Stop Modulating** for each control it moves),
**On**, and **Remove**. Switched off, a modulator moves nothing but keeps what it moves and by how much, so you can
hear the sound with and without it.

Automation sets a control's value along the song, and modulators move it around that value.

## Not yet

- MIDI learn, a hardware knob assigned to a control: planned.
- Automating or modulating a modulator's own settings: planned.
- Modulation for each note an instrument plays: planned.
- Copying automation points with Cut, Copy and Paste: planned (Duplicate copies them).
- Automation clips, placed freely as in FL Studio: not planned. Lanes under the track are the one way to automate.
