---
title: The piano roll
description: Drawing and editing notes, velocity, the grid, ghost notes, drum mode, the transforms, chord stamps and keys.
sidebar:
  order: 4
---

The piano roll edits the notes of MIDI clips. It is the clip editor's tab in the lower panel, called **Piano roll**
while it shows MIDI clips: select a MIDI clip in the playlist, or double-click one, and press **F7** if the tab isn't
showing.

## What it shows

- **Which notes:** the MIDI clips selected in the playlist, several at once, each in its track's colour. With a track
  selected, it shows the track's clip at the playhead or the nearest one. On a track without clips, the first note you
  draw starts a clip.
- **Time counts from the clip:** bar 1 is where the clip starts, and the grid counts from there.
- **The settings down the left,** between the keys and the notes, from the top: which clip shows and where it is in
  the song, the track's instrument (or **Choose instrument**), its key, the next note's length and velocity, the chord
  stamp, and the **Ghost notes** switch.
- **The tools,** **Snap**, the grid and **Follow Playback** are in the tab row, with the notes' menu as the More button
  (…) at its end. **Shift**+right-click opens the same menu.

Clips grow with their notes: a note drawn or moved past its clip's end makes the clip end at the end of that bar, but
never past the start of the next clip on the track.

## Drawing and editing notes

| Gesture | Does |
|---|---|
| Click on empty grid | Adds a note, with the length and velocity the next note's setting shows |
| Right-click a note | Deletes it |
| Right-click empty grid | Clears the selection |
| Drag a note | Moves it; **Ctrl** copies it |
| Drag a note's left or right edge | Changes where it starts or ends |
| **Ctrl**+click or **Shift**+click a note | Adds it to the selection, or takes it out |
| Hold **Alt** while dragging | Leaves the grid |
| **Escape** during a drag | Takes the drag back, leaving nothing to undo |
| Click a key at the left | Plays it and selects its notes; **Ctrl**+click adds them |
| Drag along the keys | Selects the notes of a range of keys |
| Wheel | Scrolls; **Shift**+wheel sideways, **Ctrl**+wheel or a trackpad's pinch zooms, **Alt**+wheel makes the rows taller or shorter |
| Middle-button drag | Moves the view |

A click and a right-click work this way in every keymap.

**The next note.** A new note takes the length and velocity of the note you touched last. The setting down the left
shows them ("1/4 beat, velocity 100"); click it to choose another length (or a step of the grid) and velocity.

With Snap on, a note moves by whole steps of the grid from where it was, so a note that is off the grid stays as far
off it; **Quantize** puts it on the grid.

## Tools

The playlist's tools work here on the same keys ([The playlist](playlist.md#the-other-tools)). The Smart Tool draws
notes, so there is no Draw tool: its key picks the Smart Tool. **Paint** lays notes along the grid as you drag (in the
key with Snap to Key, and as chords with a chord stamp). Slice, Select, Mute, Delete and Zoom work on notes as they do
on clips; Slip has nothing to do in the piano roll.

## Keys for selected notes

| Key | Does |
|---|---|
| **Up**, **Down** | Transposes them a semitone |
| **Shift+Up**, **Shift+Down** | Transposes them an octave |
| **Shift+Right**, **Shift+Left** | Lengthens or shortens them by a step of the grid |
| **Left**, **Right** | Nudges them a step of the grid |
| **Ctrl+D** | Duplicates them right after themselves |
| **0** | Mutes or unmutes them |
| **Ctrl+A** | Selects all the notes |
| **Escape** | Closes an open transform, then deselects the notes, then picks the Smart Tool |

These are the Phazite keymap's keys; the [keyboard reference](keyboard.md#notes) has the others.

## Velocity

Quieter notes are paler, with a bar along each note's foot as long as its velocity. The velocity lane under the notes
shows a stem for each note.

| In the velocity lane | Does |
|---|---|
| Drag from a selected note's stem | Changes every selected note's velocity by as much, keeping their differences |
| Drag elsewhere | Sets the velocities under the pointer |
| Right-drag | Draws a straight ramp |
| Wheel over a note's stem | Raises or lowers the velocities there, a burst of turns one undo step; elsewhere it scrolls |
| Drag the line over the lane | Makes the lane taller or shorter |
| The chevron at its top left | Folds the lane to a thin row, or opens it |

## Slide and portamento notes

The notes' menu makes the selected notes **Slide Note** or **Portamento** notes, and plain notes again.

- A **slide note** plays nothing itself: the notes sounding as it starts glide to its pitch over its length, as an 808
  bass glides. It shows a triangle at its start.
- A **portamento** note glides in from the note played before it, over the instrument's Glide time. It shows a rising
  stroke.

Phazite's own instruments play both. Plug-ins get portamento notes as plain notes, and no slide notes.

## Keys and scales

A key belongs to a track. Click the key's setting down the left (it reads **Key: Off** until you choose one) and pick
a root and a scale.

- With a key, the rows outside it are shaded.
- **Snap to Key** places new notes, and notes you drag up or down, only on the key's notes.
- **Use This Key on Every Track** gives every track the key of the track shown.
- **Transpose...** can move notes in steps of the key rather than in semitones.

While the song has chords on its [chord lane](chords.md), the rows of the chord playing at each place are tinted, to
draw by.

## Chord stamps

Click the chord setting down the left (**Chord: Off** until you choose one) and pick a chord: Major, Minor, 7, Maj7,
m7, Sus2, Sus4, Dim, Aug, 6, m6, 9, Maj9, Power, m7b5, Dim7 or m9. From then on a click places that chord, built up from
the note clicked. Choose **Off** for single notes again.

## Ghost notes

The **Ghost notes** switch shows the notes of the other tracks' clips faintly behind the clip you are editing. A
double-right-click on a ghost note shows its clip in the piano roll.

## Drum mode

On a track with a drum sampler, the piano roll starts in drum mode: the rows show the pads' names instead of piano
keys, and notes are drawn as hits.

- Drag a row's name up or down to reorder the rows.
- Right-click a row's name to hide that row (**Hide** and its name), or **Show All Rows** to bring them all back.
- **Drum Mode**, in the notes' menu, turns it on or off.

## Transforms

The notes' menu (the More button, or **Shift**+right-click) has the transforms in two groups.

**Show what it does first:** **Quantize...**, **Legato...**, **Chop...**, **Transpose...**, **Strum...**,
**Humanize...** and **Arpeggiate...** open a bar over the notes with the transform's settings, and show what it would
do before you apply it. Change the settings until it sounds right, then click **Apply** (or press **Enter**); **Cancel**
or **Escape** leaves the notes as they were.

| Transform | Settings |
|---|---|
| Quantize... (**Ctrl+Shift+U**) | Strength, swing, and **Ends too** to move the notes' ends as well |
| Legato... | The gap left before the next note |
| Chop... | Cuts the notes into pieces a step of the grid long |
| Transpose... | Semitones, or steps with **In key** |
| Strum... | The step between a chord's notes, and **From the top note down** |
| Humanize... | How much the timing and the velocity vary |
| Arpeggiate... | The gate, and the order: up, down, or up and down |

**Right away:** **Quantize** (**Ctrl+U**), **Legato** (**Ctrl+L**) and **Chop** (**Ctrl+E**) apply at once, with the
settings Quantize..., Legato... and Chop... were given last.

The menu also has **Cut**, **Copy**, **Paste**, **Duplicate**, **Delete** and **Mute**, **Select All Notes** and
**Deselect All Notes**, and the switches **Drum Mode**, **Ghost Notes** and **Snap to Key**.

## Capture

**Capture** (**Ctrl+Shift+C**) turns what you played in the last 30 seconds, recording or not, into a clip
([Recording](recording.md#capture)).

## Not yet

- Lanes for controllers, pitch bend and per-note expression: planned. Controllers, pitch bend and the keys' pressure
  a MIDI file brought in, or a recording took in, play, but the piano roll doesn't show them.
- Ghost notes chosen track by track: planned; the switch shows every other track's notes.
- Flam, velocity scaling, reverse and flip transforms: planned.
- Editing a whole track in song time rather than clip by clip: planned.
- Pitch glides sent to plug-ins (MPE): planned.
