# Automation and modulation

*Design section 10.*

Automation makes a parameter change along the song: a line drawn in a lane under the parameter's track. Modulation
moves a parameter around its value while the song plays, by a modulator (an LFO, an envelope follower and so on) or a
macro knob. Any knob or fader of Phazite's, and any parameter a plug-in lets be automated, can have both.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Automate from a control's right-click menu | Built | |
| Automation lanes under each track, and under a Master row | Built | |
| Lanes never hide: they collapse to a thin row | Built | |
| Lane tools: add, move and delete points, bend lines, draw steps, paint shapes, select | Built | |
| Keys for the selected points | Built | |
| A point's value typed | Built | |
| Automate Last Touched Parameter | Built | |
| Plug-in parameters automated | Built | From the plug-in's slot, a lane's menu or Automate Last Touched |
| Write modes for each track: Read, Touch, Latch, Write | Built | |
| Automation moves with the clips above it | Built | |
| A straight line as an even move of the control | Planned | Today a line is straight in the parameter's own units |
| Automation clips, placed freely as FL Studio's | Declined | Lanes are the one way to automate |
| Linked automation (aliases that share a curve) | Declined | Went with automation clips |
| Six modulators for each track, folder and the master | Built | |
| Macros on tracks and returns | Built | Containers' macros: [devices.md](devices.md) |
| Assigning a modulator by menu or by dragging it onto a knob | Built | |
| The modulation ring on a knob, dragged to set the amount | Built | |
| A modulator's On switch | Built | |
| Automating or modulating a modulator's own parameters | Planned | No roadmap step |
| Modulation for each note (per voice) | Planned | No roadmap step |
| MIDI learn | Planned | No roadmap step |

## Automating a control

Right-click any of Phazite's knobs or faders (the mixer's, the inspector's, a device's in the Devices view). The menu
is headed by the parameter's name:

| Item | What it does |
| --- | --- |
| Type a Value... | Opens a field over the control to type its value |
| Automate | Shows the parameter's lane under its track, expanded |
| Delete Automation | Deletes the parameter's automation with its lane, as one undo step |
| Modulate | A submenu: the track's modulators, then a new one of each kind (see [Modulation](#modulation)) |

A plug-in's own window belongs to the plug-in, so its parameters are automated in one of three ways:

- right-click the plug-in's slot in the mixer, **Automate**, then the parameter;
- choose the parameter in a lane's parameter button (below);
- move the knob in the plug-in's window, then run **Automate Last Touched Parameter** (Ctrl+Shift+A), which works
  while a plug-in's window has the keys. The command shows the lane of the parameter last changed by hand, in a
  Phazite control or a plug-in's window, and is named after it ("Automate Cutoff").

## Lanes

A lane shows under its track in the playlist ([playlist.md](playlist.md)). The master's lanes show under a Master row
below the tracks while it has lanes.

- **Header:** the track's colour down its left, the parameter's name ("Volume", or "Synth: Cutoff" for a device's),
  and the track's write mode. In a lane without automation the name is a button, as wide as its text, that shows
  another of the track's parameters in the lane instead: the track's volume, pan and sends, then each device's
  parameters in a submenu (a drum sampler's by pad).
- **Curve:** drawn as the engine plays it, in the track's colour.
- **Height:** 48 pixels at first, from 32 to 320, changed by dragging the header's bottom edge; a double-click on
  that edge resets it.
- **View state:** which lanes show, their heights and whether they are collapsed are saved with the track, but
  showing, collapsing or resizing a lane is not an undo step and not an unsaved change.

**Automation never hides.** Every parameter with automation has a lane, even automation recorded while no lane
showed, so nothing plays from a place that isn't seen. To make room, a lane collapses to a thin row that still draws
its line:

| Gesture | What it does |
| --- | --- |
| Double-click a lane's header | Collapses or expands the lane |
| Drag the header's bottom edge up past halfway to a thin row | Collapses the lane |
| Collapse the track | Its lanes become thin rows too |
| Double-click a thin lane | Expands it |
| A (Show Automation) | Expands the selected tracks' lanes, or collapses them all while one shows expanded; a track without lanes gets one for its volume |

A lane goes only with its automation. Its right-click menu has **Collapse Lane** (or **Expand Lane**), **Delete
Automation** (**Remove Lane**, for a lane without automation), **Shape for Paint**, **Mode**, Delete and Duplicate.
Delete Automation is one undo step, and an undo brings the lane back with the automation. Focus mode is the one view
that leaves lanes out: those of the tracks out of focus.

## Editing a lane

The time line's tools ([playlist.md](playlist.md)) work in a lane as follows.

| Tool | Gesture | What it does |
| --- | --- | --- |
| Smart | Click empty space | Adds a point, on the grid |
| Smart | Press on the line | Puts a point on the line, at the line's value there, so nothing sounds different until it is dragged |
| Smart, Draw | Drag a point | Moves it, with the other selected points |
| Smart | Drag the hollow handle in the middle of the line under the pointer | Bends the line, up or down, no further than where it still runs from point to point |
| Smart, Draw | Double-click a point | Deletes it |
| Smart, Draw | Double-click a bent line's handle | Straightens the line |
| Smart | Drag on empty space | Selects the points in a rectangle |
| Draw | Drag | Draws a step for each grid cell it passes; with Alt, a free line |
| Paint | Drag | Draws a shape between where the drag starts and ends, a cycle a grid step long, as high and low as it drags |
| Delete | Drag | Deletes the points it passes |
| Select | Drag | Selects points |

Paint's shape is Sine, Triangle, Square, Saw Up, Saw Down or Line, chosen in the lane menu's **Shape for Paint** for
every lane.

Keys only refine a gesture, with the meanings they have everywhere in Phazite:

| Key | While dragging or clicking |
| --- | --- |
| Alt | Places off the grid |
| Shift | Moves a value in fine steps, a fifth as far |
| Ctrl | Adds a point to the selection, or takes it out |

Selected points show larger, in the text colour with an accent ring. Each edit is one undo step, a drag one step from
its start to its end.

**Right-click.** A right-click on a point selects it and shows the lane's menu with **Type a Value...** first; on
empty space it shows the lane's menu. With FL Studio's keymap a right-click on a point deletes it, and the points the
drag passes, as it deletes clips; Shift+right-click shows the menu there.

**Keys for the selected points** (Phazite's keymap; the commands take the arrow keys only while points are selected,
as the notes' commands do):

| Key | Command | What it does |
| --- | --- | --- |
| Up, Down | Raise, Lower Automation Points | Moves them by a hundredth of the parameter's range |
| Shift+Up, Shift+Down | Raise, Lower Automation Points More | Moves them by a tenth of the range |
| Left, Right | Nudge | Moves them a grid step |
| Delete | Delete | Deletes them |
| Ctrl+D | Duplicate | Copies them right after themselves |

Automate Last Touched Parameter is Ctrl+Shift+A, not Shift+A, because Shift and a letter run the letter's own
shortcut while the computer keyboard plays notes ([commands-keys-help.md](commands-keys-help.md)).

**A point's value typed.** Type a Value... (`automation.typeValue`; no key of its own, so the command search and a
key the user gives it reach it) opens a field over the point with its value in the parameter's units, all of it
selected ("-6.0 dB", "L20", "2.50 kHz"). Enter sets every selected point in the lane to what the field says, as one
undo step ("Set Automation Value"); Esc leaves them. The field reads text as a knob's field does. The inspector
shows the selected points' value and bend as fields too.

## Points, lines and time

- A point has a place in beats, a value in the parameter's own range, and the bend of the line to the next point.
  Before the first point and after the last, the value stays at theirs. Two points at one beat are a jump.
- The song keeps automation in beats, as it keeps clips, so it stays on its beats when the tempo or the time
  signature changes.
- What automation plays changes nothing in the song: playing a lane is not an edit.
- **A line is straight in the parameter's own units** (hertz, milliseconds, decibels as the fader holds them):
  points are stored, and the values between them worked out, in those units. So a straight line on a filter's cutoff
  is an even change in hertz, not an even turn of the knob, and sounds as if it moves slowly and then quickly. This is
  to change (see [Decisions](#decisions)).

## Write modes

Each track has a write mode, shown in its lanes' headers and the inspector, Read by default. Changing it is not an
undo step, as mute and solo aren't. A mode that writes shows in red. The commands Read, Touch, Latch and Write
Automation set the selected tracks' mode.

| Mode | While the song plays |
| --- | --- |
| Read | The lanes play. A control moved by hand goes back to its lane at the lane's next change |
| Touch | A control writes while it is held; the lane takes over again when it is let go |
| Latch | A control writes from its first move until playback stops |
| Write | Every automated parameter of the track is overwritten from the moment playback starts until its control is let go; then the track is in Latch |

A pass writes the value where it started, each move after, and where it ended, in place of the points in between,
thinned to the moves that change the line. Around a loop, a pass ends at the loop's end and another starts at its
start. What a control's pass writes joins its drag's undo step; what is still being written when playback stops is
one step, "Record Automation". A parameter moved in a plug-in's own window writes the same way.

## Automation goes with the music

Moving or copying clips along their track (dragging, Ctrl+drag, nudging, Duplicate) takes the automation of the
track's parameters under them along, in the same undo step, in place of what is where it lands. So do:

- deleting clips with Delete and Close Gap, which takes their time out of the track;
- deleting or duplicating a time range selected across tracks (and their folders);
- Cut, Copy and Paste of clips or a range, when they go back onto their own tracks.

Rules:

- Each stretch keeps its shape at its edges wherever it goes; where a moved stretch was, the line runs straight
  across.
- A clip moved to another track leaves its automation: that track's devices are others.
- A clip deleted without closing its gap leaves its automation.
- A drag moves the automation once, when the drag ends.
- Automation on a folder or the master doesn't follow the clips of the tracks inside; a time range selected across
  the folder carries it.

## Modulation

### Modulators and macros

Each track, each folder and the master has modulators of its own, and tracks and returns have macros. They modulate
the parameters of that track's own devices and its volume, pan and sends, never another track's, a modulator's own
parameters or a macro.

| Modulator | What it follows |
| --- | --- |
| LFO | A wave, repeating |
| Envelope Follower | How loud the track is |
| Step Sequencer | A sequence of steps |
| Random | Random values |
| Breakpoint Envelope | An envelope drawn as points, repeating |
| Note Follower | The pitch or velocity of the notes the track plays |
| Macro | A knob that moves several parameters at once |

They show as chips in the Devices view's column ([devices.md](devices.md)): a grip, the name, and a meter of the
output (a macro's chip has its knob instead). A button adds one.

### Assigning one

- **By menu:** right-click a knob, **Modulate**, then one of the track's modulators (ticked while it modulates the
  knob; choosing it again stops that), or **New LFO** to **New Macro**, which adds one already modulating the knob,
  as one undo step.
- **By dragging:** drag a modulator by its grip onto a knob of one of its track's devices. The grip is in the
  modulation colour and lights under the pointer. Once dragged, a chip with the modulator's name follows the pointer,
  the chip it comes from is outlined, and every knob it can modulate shows a faint ring, in every window; the knob
  under the pointer shows a full ring over a wash.

A modulation starts at a quarter of the parameter's range. It moves the parameter around where its knob is set: both
ways for a bipolar modulator (LFOs, random sources and breakpoint envelopes as their Polarity says, a step sequencer
always; new LFOs and random sources start bipolar), and the other way for a negative amount. An amount runs from -1
to 1, at most the whole of the range.

### The ring

A modulated knob shows a ring in the modulation colour: the range its modulations can move it over, and a dot where
it is now. The knob itself shows its own value, or its automation's. Dragging the ring (the knob's outer edge) up or
down changes its first modulation's amount, with Shift in fine steps, as one undo step ("Change Modulation"); the
rest of the knob turns it as before.

### Editors

A click on a modulator's chip pops its editor out over the window, beside it, as it opens when the modulator is
added; a click elsewhere, Escape or a click on the chip closes it. It holds:

- the modulator's name and its On switch;
- its output over the last two seconds;
- a knob for each of its parameters (rate, wave, depth, polarity, sync and so on);
- a step sequencer's steps, drawn with the mouse, one undo step per drag;
- a breakpoint envelope's points, dragged and bent with the mouse.

A macro's chip is its knob. A modulator's right-click menu has **Open Editor**, **Modulates** (with **Stop
Modulating** for each parameter), **On**, and **Remove**, which removes it with what it modulates.

### On and off

Each modulator has an On switch, in its editor and its right-click menu, to hear a sound with and without it without
taking it apart. Off, it moves nothing and keeps what it modulates and by how much; its meter goes faint, and so do
the rings of knobs that only switched-off modulators modulate (showing where they would move them, with no dot).
Switching is one undo step ("Turn Modulator Off"), saved with the project. Presets load modulators on. A macro has
no switch: it is a knob, turned down to do nothing.

## How automation and modulation combine

- The lane sets the base value, and modulators move the parameter around it.
- Each control shows both: its automated value, and the modulation range as the ring.
- What modulation plays is not an unsaved change.

## How the engine plays them

- Automation and modulation move what they move on the song's control points, one every 32 samples from the song's
  start, not once an audio block. So fast modulation keeps its edges on time and a song plays the same at any buffer
  size.
- A parameter moves from the value its automation leaves; modulation is added in the parameter's range as its control
  spans it, one sum for the whole device.
- Plug-ins' parameters are moved on the same points, each change handed to the plug-in at its sample, and so are the
  faders' volume and pan: an LFO moves a plug-in, a device and a fader in step.
- Steady playback of automation and modulation allocates nothing on the audio thread; a guarded scenario of the
  [real-time audit](../realtime-audit.md) checks it.

## Decisions

- **Lanes, not automation clips.** Automation stays with its parameter, in lanes under the parameter's track. Free
  clips fit a playlist whose rows aren't tied to an instrument; Phazite's rows are tracks, and a clip moved anywhere
  that still set another track's parameter hid the track's own clips and broke the rule that everything in a row
  belongs to its track.
- **Automation never hides.** A lane with automation can only collapse, so nothing plays from an unseen place.
- **Automation moves with clips by default,** which gives what automation clips do well (going along when parts are
  rearranged) without them.
- **Write modes are each track's,** Read by default, rather than one global arm button.
- **Modulators belong to a track** and modulate only that track's devices and mixer controls.
- **A straight line is to be an even move of the knob.** Automation is to be stored and played along the control's
  travel, as other DAWs do, so that a straight line on a cutoff is an even sweep. Today it is stored and
  interpolated in the parameter's own units.
- **Ctrl+Shift+A, not Shift+A,** for Automate Last Touched Parameter, so that Shift and a letter stay free for the
  letter's shortcut while the computer keyboard plays notes.

## Not built

- Automation stored and played along the control's travel (above): Planned.
- MIDI learn, a hardware control assigned to a parameter: Planned.
- Automating or modulating a modulator's own parameters: Planned.
- Modulation for each note, an instrument moving a parameter for each voice (the note follower's each note's pitch
  and velocity, envelopes started by each note): Planned.
- Copying automation points with Cut, Copy and Paste (Duplicate copies them): Planned.
- Automation clips and linked automation: Declined.

## In the code

- `src/model/automation/AutomationModel.h`, `AutomationValues.h`: lanes, points, shapes, write modes and the
  automation commands, by IDs; `internal/Automation.h`: the song's curves, the lanes a track keeps, and automation
  that goes with clips.
- `src/model/playback/internal/AutomationWriter.h`: what Touch, Latch and Write record while the engine plays.
- `src/engine/automation/Curve.h`: a curve as the engine plays it, and the control points' spacing.
- `src/ui/playlist/PlaylistAutomation.cpp`: the lanes, their headers, tools and menus.
- `src/ui/components/Controls.h`: `ui::ParameterAttachment`, whose menu has Automate and Modulate, and the knob's
  ring; `ModulatorDrag.h`: what a dragged modulator carries.
- `src/model/modulation/ModulationModel.h`, `ModulationValues.h`: modulators, macros and what they modulate, by IDs;
  `internal/Modulation.h`: the song's side.
- `src/engine/modulation/ModulatorNode.h`, `src/devices/modulation/Modulators.h`, `Modulation.h`: modulators as the
  engine plays them, and how modulation moves a parameter.
- `src/ui/mixer/ModulatorSlots.h`, `src/ui/devices/ModulatorEditor.h`: the chips and the editors.
- Tests: `tests/AutomationModelTests.cpp`, `AutomationLaneTests.cpp`, `AutomationFollowsClipsTests.cpp`,
  `ModulationModelTests.cpp`, `ModulationUiTests.cpp`, `GoldenModulationTests.cpp`.
