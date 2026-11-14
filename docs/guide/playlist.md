---
title: The playlist
description: Tracks and clips, the tools, selecting a range of time, moving and copying, folders, sections, tempo changes and the loop.
sidebar:
  order: 3
---

The playlist shows the whole song: the tracks down the left, their clips along the time line, the song's sections over
the bar numbers, and automation lanes under their tracks. You arrange, cut and copy clips here; what is inside a clip
you edit in the lower panel, in the [piano roll](piano-roll.md) or the [audio clip editor](audio-clips.md). Press
**F5** to show it.

From the top, the playlist has an overview of the whole song, the time bar (the sections over the bar numbers), the
tempo lane while the song changes tempo, the chord lane while the song has chords, and then the tracks.

## Tracks

A track's header shows its colour, its name, its arm switch, its one switch for mute and solo, and down its right edge
a strip that shows how loud it plays now. Volume and pan are in the [mixer](mixer.md) and the inspector.

| To | Do |
|---|---|
| Add a track | Click **Track** over the headers, or **Ctrl+T**: the track goes below the selected one |
| Add a track at the end | Click **+ Add track** after the last track, or double-click under the last track. Right-click that row for **Track** or **Folder** |
| Select tracks | Click a header; **Ctrl** and **Shift** add to the selection. **Up** and **Down** select the track above or below |
| Move tracks | Drag a header, from anywhere on it. A line shows where the tracks go; on a folder, its middle puts them into it |
| Rename | **F2** or **Ctrl+R** |
| Change the colour | Click the colour strip at the header's left |
| Collapse to a thin row | Double-click the header, or **U**. A collapsed folder hides its tracks |
| Collapse everything | The **Collapse all** button beside **Track**, or **Alt+U**; again to open them all |
| Change the height | Drag the header's bottom edge, or **Alt+=** and **Alt+-** |
| Widen the headers | Drag the headers' right edge; a double-click there puts back the width they start with |
| Keep tracks in view | **Pin Track**: they stay at the top while the others scroll |
| See only some tracks | Select them and run **Focus Mode**: every other track becomes small |
| Save a track as a preset | Drag its header out past the playlist's left edge, into the browser |

A track's right-click menu has **Rename**, **Track Color**, **Mute Track**, **Solo Track**, **Arm Track**, **Pin
Track**, **Move Track Up** and **Move Track Down**, **Group into Folder**, **Ungroup Folder** and **Collapse Track**,
**Show Automation** and **Automation Mode**, **Paste**, **Duplicate** and **Delete**, **Freeze Track** and **Bounce in
Place**, **Duck Under**, **Save Track Preset...**, and **Add Track** and **Add Folder**.

## Clips

To make clips:

- Drag a sound, a MIDI file, an instrument or a preset from the [browser](browser.md) onto a track, or onto the empty
  space below the tracks for a new track. Audio and MIDI files dragged in from your file explorer work the same way.
- Double-click empty space on a track that has an instrument: an empty MIDI clip, a bar long, starts at the grid line
  before the click.
- Record ([Recording](recording.md)).

A double-click on a clip shows it in its editor in the lower panel.

**How clips look.** A clip has its track's colour. Its name shows in a tag at its top left only where it says more
than the track's name (a sample's own file name, say, or "Take 2/3"); otherwise the track's header names it. A link
mark at the top right marks an alias, and a metronome mark a sample that follows the tempo. A muted clip is faint, and
so is every clip of a track you don't hear.

**Clips that overlap all play,** mixed together, so a sample's tail rings on under the next copy of it. The playlist
stacks them one under another in the track's row.

**An alias** is a linked copy of a clip: editing the notes of one edits them all. **Make Unique** turns aliases into
separate clips again.

## The Smart Tool

The Smart Tool is the pointer you start with. On a clip, where you start a drag decides what it does:

| Where on a clip | Dragging does |
|---|---|
| The left or right edge | Trims the clip |
| The top-left or top-right corner of an audio clip | Draws a fade-in or a fade-out |
| The upper half | Selects a range of time (a clip on a short track has no upper half: all of it moves) |
| The lower half | Moves the clip. **Ctrl** copies it, **Ctrl+Shift** makes an alias |
| Where two audio clips touch | Draws a crossfade |

On empty space:

| Gesture | Does |
|---|---|
| Drag | Selects a range of time across tracks |
| Double-click on a track with an instrument | Makes an empty MIDI clip a bar long |
| Double-click under the last track | Adds a track |
| Right-click | Opens the menu |
| Middle-button drag | Moves the view, sideways and up and down |
| Wheel | Scrolls; **Shift**+wheel scrolls sideways, **Ctrl**+wheel zooms around the pointer, **Alt**+wheel makes every track taller or shorter |

Hold **Alt** during a drag to leave the grid. With Snap on, a clip moves by whole steps of the grid from where it was,
so a clip that is off the grid stays as far off it; **Quantize** (**Ctrl+U**) puts it on the grid. Dragging near the
edge of the view scrolls it along.

The [FL Studio keymap](keyboard.md#the-fl-studio-keymaps-mouse) changes the mouse: a right-click deletes clips and a
click on empty space places one.

## The other tools

The tools are in the playlist's tab row, and on keys. Tap a tool's key to switch to it, or hold the key while you use
the mouse to keep the tool only until you let go.

| Key | Tool | Does |
|---|---|---|
| **Escape** or **1** | Smart | As above |
| **P** or **2** | Draw | A click places a copy of the clip you used last; on a track with an instrument and no clip used yet, a new MIDI clip |
| **B** or **3** | Paint | A drag places copies of the clip you used last, one after another |
| **C** or **4** | Slice | A click splits a clip there; **Alt** splits off the grid |
| **S** or **5** | Slip | A drag moves what is inside a clip, leaving the clip where it is |
| **T** or **6** | Mute | A click mutes or unmutes a clip; a drag does it to several |
| **D** or **7** | Delete | A click deletes a clip; a drag over clips deletes them all |
| **E** or **8** | Select | A drag selects the clips in a rectangle; **Ctrl** adds to the selection |
| **Z** or **9** | Zoom | A click zooms in, **Alt**+click out, a drag zooms to the time dragged across |

These are the Phazite keymap's keys; the [keyboard reference](keyboard.md#tools) has the others. The digits never
play notes, so they work while the computer keyboard plays.

## Selecting, moving and copying

A range of time selected across tracks (drag on empty space, or on a clip's upper half) works with the same commands as
selected clips.

| Command | Key | Does |
|---|---|---|
| Split | **Ctrl+E** | Splits the clips in the selected range at its edges, or the selected clips at the playhead |
| Duplicate | **Ctrl+D** | Copies the selection right after itself. A copied range replaces what it lands on, and the range moves to the copy, so pressing it again repeats it |
| Duplicate as Alias | | Copies the selected clips or sections as aliases |
| Cut, Copy, Paste | **Ctrl+X**, **Ctrl+C**, **Ctrl+V** | Paste puts clips at the playhead, from the selected track down, in place of what lies under them |
| Delete | **Delete** | Deletes the selection |
| Delete and Close Gap | | Deletes the range or clips and moves what comes after them back |
| Insert Time | **Ctrl+I** | Inserts empty time, as long as the selected range, on every track |
| Remove Time | **Ctrl+Shift+Delete** | Removes the selected range from every track and moves the rest back |
| Nudge Left, Nudge Right | **Left**, **Right** | Moves the selection a step of the grid |
| Quantize | **Ctrl+U** | Moves the starts of the selected clips to the nearest grid line |
| Mute | **0** | Mutes or unmutes the selected clips |
| Consolidate | **Ctrl+J** | Joins the selected clips on each track, or what lies in the range, into one clip |

**Automation goes with the clips.** Moving or copying clips along their track (dragging, nudging, Duplicate, Cut and
Paste back onto the same track) takes the automation under them along, in the same undo step.

A clip's right-click menu has **Cut**, **Copy** and **Paste**; **Split**, **Duplicate**, **Duplicate as Alias**, **Make
Unique** and **Consolidate**; **Mute** and **Quantize**; for audio clips **Fade Shape**, **Reverse**, **Follow Tempo**,
**Quantize Audio** and **Normalize** ([Audio clips](audio-clips.md)); **Take**, for a clip recorded round a loop; and
**Delete** and **Delete and Close Gap**.

## Folders, buses and returns

- **Group into Folder** (**Ctrl+G**) puts the selected tracks into a new folder; **Ungroup Folder** (**Ctrl+Shift+G**)
  takes them out again. **Add Folder**, in the Add menu, adds an empty one.
- A folder holds no clips. Its row shows its tracks' clips in miniature, so that a collapsed folder still shows its
  part of the song.
- A folder is a bus: it mixes its tracks through its own effects and fader ([The mixer](mixer.md#folders-and-buses)).
- A return track has no row in the playlist until it has an automation lane; the mixer shows it.

## Sections

Sections are the coloured blocks over the bar numbers: the song's intro, verse, chorus and so on. They show faintly
behind the tracks too.

- **Add > Add Section** adds an 8-bar section after the last one. **Add > Song Outline** lays out a whole song's
  sections at once: pop, rock, electronic (house, EDM), hip-hop and trap, lo-fi beat, R&B, drum and bass, or techno.
  An outline replaces the sections there are and leaves the tracks as they are.

| Gesture | Does |
|---|---|
| Drag a section | Moves it with everything in it, on every track. **Ctrl** copies it, **Ctrl+Shift** copies it as aliases |
| Drag its right edge | Makes it longer or shorter, and the rest of the song moves along. With **Shift**, only the section changes |
| Double-click | Adds a section there, or renames the one clicked |
| Right-click a section | **Rename**, **Color...**, **Loop Section**, **Duplicate Section**, **Duplicate Section as Aliases**, **Delete Section** |
| Right-click the section band or the bar numbers | **Add Section Here**, **Add Chord Here**, **Add Tempo Change Here**, **Song Outline**, **Loop**, **Loop the Song**, **Loop Selection** |

Deleting a section deletes what is in it.

## Tempo changes

A song can change tempo along the way. **Add > Add Tempo Change**, or **Add Tempo Change Here** in the time bar's
right-click menu, adds a change, and the tempo lane shows under the bar numbers from then on.

| In the tempo lane | Does |
|---|---|
| Click | Adds a tempo change, at the tempo clicked |
| Drag a point | Moves it on the grid (**Alt** places it freely, **Shift** changes the tempo in fine steps) |
| Right-click a line | **Ramp to the Next Tempo**, or **Hold Until the Next Tempo** |
| Drag the handle in a ramp's middle | Bends the ramp |
| Double-click | Deletes a point, or straightens a bent ramp |
| Right-click a point | **Type a Value...**, the ramp choices and **Delete Tempo Change** |

**Remove Tempo Changes** leaves the song's first tempo all the way through, and the lane goes. Samples that follow
the tempo stretch along its changes; patterns keep their notes on their beats.

## The loop and the playhead

The song loops by default, from its start to the end of the bar its last clip ends in, so the loop grows as you add
clips later. **Go to End** (**End**) goes to that same place.

- **Loop** (**L**) turns looping on or off.
- **Loop Selection** (**Ctrl+L**) loops the selected range of time, or the whole song when none is selected.
- **Loop Section** loops the selected sections. **Loop the Song** goes back to looping the whole song.

The top strip of the ruler is for the loop; below it, the ruler moves the playhead.

| In the ruler | Does |
|---|---|
| Drag along the loop strip | Loops that part of the song; **Ctrl**+drag starts a new loop anywhere |
| Drag the loop's ends or middle | Changes or moves it |
| Click the loop strip | Turns looping off and on |
| Double-click the loop strip | Loops the whole song |
| Click below the loop strip | Moves the playhead there |
| Drag below the loop strip | Takes the playhead along, on the grid (**Alt** places it freely) |

**Follow Playback** (**Ctrl+Shift+F**, on in the playlist) turns the page when the playhead reaches the view's right
edge. **View > Playback > Scroll Continuously** holds the playhead in the middle and scrolls the song under it
instead. **Center on Playhead** scrolls the playhead into the middle.

## Grid, zoom and scrolling

The grid's button, in the tab row, names its size and opens its menu.

| Command | Key | Does |
|---|---|---|
| Snap to Grid | **Ctrl+4** | Turns snapping on or off. **Alt** leaves the grid during a drag either way |
| Adaptive Grid | **Ctrl+5** | The grid follows the zoom |
| Finer Grid, Coarser Grid | **Ctrl+1**, **Ctrl+2** | |
| Triplet Grid | **Ctrl+3** | Divides beats into three |
| Zoom In, Zoom Out | **=**, **-** | |
| Zoom to Fit | | Shows the whole song |
| Zoom to Selection, Zoom Back | | Fits the selection; goes back to the zoom before |

**The overview** along the top of the time bar is the whole song in miniature, with the part in view as a see-through
thumb: it is the playlist's horizontal scroll bar. Drag the thumb to scroll, drag either of its ends to zoom, or press
elsewhere to bring that part into view.

## Not yet

- A searchable list of every clip, to drag from: planned.
- A clip dragged onto a track of the other kind (audio onto an instrument's track) is not refused yet: planned.
- Escape taking back a drag in the playlist: planned (the piano roll has it).
