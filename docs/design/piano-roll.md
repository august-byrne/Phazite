# The piano roll

*Design section 7.*

The clip editor is the first tab of the lower panel. For MIDI clips it is the piano roll, where notes are drawn, moved
and transformed; for audio clips it is the audio clip editor, which shows one clip's audio with its edges, fades and
warp markers. The tab is named for what it shows and follows the selection ([window.md](window.md)).

## Status

| Feature | State | Note |
| --- | --- | --- |
| Notes drawn, moved, resized, copied, deleted by the mouse | Built | |
| Escape takes a drag back | Built | |
| Paint, Slice, Select, Mute, Delete and Zoom tools | Built | |
| Several clips edited at once | Built | |
| Clips that grow with their notes | Built | |
| Velocity lane, velocity shown on the notes | Built | |
| Controller, pitch-bend and per-note expression lanes | Planned | No roadmap step; imported controllers play but cannot be seen or edited |
| Slide and portamento notes | Built | Phazite's own instruments play them |
| Pitch glides sent to plug-ins as MPE | Planned | After version 1 |
| Ghost notes | Built | All other tracks at once; no choice per track |
| A track's key, rows outside it shaded, Snap to Key | Built | |
| Tones of the chord lane's chord tinted | Built | |
| Chord stamps | Built | |
| Stamp the chord lane's current chord | Declined | A chord made once is copied as easily |
| Transforms with preview: quantize, legato, chop, transpose, strum, humanize, arpeggiate | Built | |
| Transforms: flam, velocity scaling, reverse, flip | Planned | No roadmap step |
| Drum mode | Built | |
| Overview strip as the horizontal scroll bar | Built | |
| Song time in place of clip time | Planned | No roadmap step |
| Articulations in a lane | Planned | After version 1 |
| Audio clip editor: trim, fades, slip, gain, pitch, reverse, follow tempo | Built | |
| Normalize | Built | |
| Warp markers and Quantize Audio | Built | |
| A choice of stretch algorithms, formants | Planned | No roadmap step |
| Zoom and scroll in the audio clip editor | Declined | It shows the whole file |

## What it shows

- **Which notes:** the MIDI clips selected in the playlist, several at once, each in its track's colour; or, when a
  track is selected, its clip at the playhead or the nearest one. A track without clips shows from the bar the
  playhead is in, and the first note drawn starts a clip there.
- **Time counts from the clip:** bar 1 is where the clip's content starts, and a clip opens there. The grid and
  snapping count from it too. With several clips it counts from the first.
- **The settings down the left,** a row each in a column between the keys and the notes, which scrolls where the piano
  roll is short: which clips show and where they are in the song, the track's instrument, its key, what a click adds
  (the next note's length and velocity), the chord stamp, and ghost notes.
- **The tools, Snap, the grid and Follow Playback** are in the area's tab row, with the notes' menu (the transforms and
  the piano roll's switches) as the More button (…) at its end. Shift+right-click opens the same menu, and on a note
  selects it first.
- **The playhead** shows when the clip plays. While an alias of a clip shown plays elsewhere in the song, the playhead
  shows, fainter, which of the notes are playing. The view does not turn pages while the song plays unless Follow
  Playback is turned on.
- **Zoom on opening:** a clip opens zoomed to show it with a quarter of its length after it, at least two bars of 4/4.
  A clip shown again opens as it was left, and an alias opens as the last of its links shown was left.
- **The overview** runs along the top of the ruler as the horizontal scroll bar: the notes in miniature, in their
  clips' colours, under the thumb of the part in view, which scrolls and zooms as the playlist's does
  ([playlist.md](playlist.md#grid-zoom-and-scrolling)). The rows' scroll bar lies over the notes' right edge. The
  ruler's loop strip works as the playlist's.

## The mouse

| Gesture | Does |
| --- | --- |
| Click on empty grid | Adds a note with the length and velocity the Next note button shows |
| Right-click a note | Deletes it |
| Right-click empty grid | Clears the selection |
| Drag a note's body | Moves it; Ctrl copies |
| Drag a note's edge | Resizes it |
| Ctrl+click, Shift+click a note | Adds it to the selection or takes it out |
| Alt during a drag | Leaves the grid |
| Escape during a drag | Takes the drag back, leaving nothing to undo or redo |
| Click a key | Plays it and selects its notes; Ctrl+click adds them |
| Drag along the keys | Selects the notes of a range of keys |
| Double-right-click a ghost note | Shows its clip in the piano roll |
| Wheel | Scrolls by as far as it turns; Shift+wheel sideways; Ctrl+wheel and a trackpad's pinch zoom around the pointer; Alt+wheel makes the rows taller or shorter |
| Wheel over a note's stem in the velocity lane | Raises or lowers the velocities there, a burst of the wheel one undo step; elsewhere in the lane it scrolls |
| Middle drag | Moves the view; never edits |

Rules:

- **The next note** takes the length and velocity of the note pressed last with a tool that places or edits notes
  (deleting or muting one does not choose it). Its button shows them, and its menu chooses others, or a step of the
  grid.
- **Edges:** both edges of a note have a 6-pixel zone inside it. A note too short for that is moved from inside and
  resized from 4 pixels outside its edges.
- **Escape,** when no drag is going on, closes a transform, then deselects the notes, then picks the Smart Tool.
- **A move keeps a note's offset from the grid:** with Snap on, a note moves by the grid's steps from where it was, so
  one off the grid stays as far off it, and a drag straight up or down changes no time. Alt moves freely and Quantize
  puts it on the grid. A resized edge snaps to the grid's lines.
- A drag that copies makes its copies only once it moves them; a drag that ends where it began is a click and makes no
  undo step. A drag of selected notes that moves nothing keeps the selection.
- **Clips grow with their notes, into empty space only:** a note drawn, moved, lengthened or duplicated past its
  clip's end makes the clip end at the end of that bar, but never past where the next clip on the track starts. A
  looped clip does not grow.
- **Only the notes the clip plays** are shown, selected and changed. What a split or a trim left outside the clip
  stays as it is, and no note is made where it would not play. A note that plays cannot be moved, nudged or quantized
  to where its clip does not play it.
- Notes and the velocity lane's stems are drawn on whole pixels, their outlines inside their edges.

## Tools

The playlist's tools on the same keys ([playlist.md](playlist.md#the-other-tools)): Paint, Slice, Select, Mute,
Delete and Zoom.

- **Paint:** a drag either way lays notes along the grid, in the key with Snap to Key and as chords with a chord
  stamp. The first painted replace the selection.
- The Smart Tool draws, so the Draw tool's key picks the Smart Tool here and its button is hidden. Slip does nothing:
  a note has nothing inside it.

## Velocity

- Quieter notes are paler, with a bar along each note's foot as long as its velocity's share of the most.
- The velocity lane under the notes shows a stem for each note.

| In the velocity lane | Does |
| --- | --- |
| Drag from a selected note's bar | Moves every selected note's velocity by as much, keeping their differences |
| Drag elsewhere | Sets the velocities under the pointer |
| Right drag | Draws a straight ramp |
| Drag the line between the lane and the notes | Makes the lane taller or shorter (32 pixels at least) |
| Click the chevron at its top left | Folds the lane to a thin row, or opens it |

The settings keep the lane's height and whether it is folded.

## Slide and portamento notes

The notes' menu makes the selected notes slide notes (Slide Note) or portamento notes (Portamento), and plain again.

- A slide note plays nothing itself: the notes sounding as it starts glide to its pitch, evenly, over its length, then
  stay there. It shows a triangle at its start.
- A portamento note glides in from the note played before it, over the instrument's Glide time, or a tenth of a second
  while Glide is off. It shows a rising stroke.
- Phazite's own instruments play both. Plug-ins get portamento notes as plain notes and no slide notes.

## Key, chords and ghost notes

- **A key is a track's,** chosen among the piano roll's settings and off until chosen. Without one the rows are a
  piano's, black keys darker. With one, the rows outside it are shaded and Snap to Key places new notes, and notes
  dragged up or down, only on it. A new track gets the key chosen last in the song. **Use This Key on Every Track**
  gives every track the key of the track shown.
- Transposing in steps of the key needs a key; without one it is in semitones.
- **Chord tones:** the rows of the chord on the chord lane at each place are tinted ([chords.md](chords.md)).
- **Chord stamps:** pick a chord type among the settings and a click places the chord.
- **Ghost notes:** the notes of other tracks' clips show faintly behind the clip being edited. One switch (Ghost
  Notes) shows or hides them all.

## Transforms

Each transform opens a bar with its settings and shows what it would do before it is applied. Its settings stay
editable until Apply (Enter); Escape, or closing the bar, leaves the notes and the settings as they were.

| Transform | Settings |
| --- | --- |
| Quantize... (Ctrl+Shift+U) | Strength, swing, the ends too; counted from the clip's bar 1 |
| Legato... | A gap |
| Chop... | Pieces a step of the grid long |
| Transpose... | In the key's steps or in semitones |
| Strum... | A step between a chord's notes, up or down |
| Humanize... | Timing and velocity; the same settings give the same result, so the preview is what Apply makes |
| Arpeggiate... | A step of the grid, a gate, and up, down or up and down |

Quantize (Ctrl+U) and Legato (Ctrl+L), without the bar, use what Quantize... and Legato... were given last, and their
names in menus say so ("Quantize (50%)"). FL Studio's keymap has Alt+S for Strum... and Alt+A for Arpeggiate....

| Key | Does |
| --- | --- |
| Up, Down | Transposes the selected notes a semitone |
| Shift+Up, Shift+Down | An octave |
| Shift+Right, Shift+Left | Lengthens or shortens them by a step of the grid |
| Left, Right | Nudges them a step of the grid |
| Ctrl+E | Chop |
| Ctrl+A | Select All Notes |
| 0 | Mutes or unmutes them |

## Drum mode

For a track with a drum sampler, the rows show the pads' names instead of piano keys, and notes are drawn as hits
without length. A drag on a row's name reorders the rows and a right-click hides a row or shows them all again; the
track keeps the order and the hidden rows. Drum mode comes on by itself with such tracks. The notes' menu and the
command search turn it on or off; it has no button.

## Capture

Capture (Ctrl+Shift+C) turns what was played in the last 30 seconds, recording or not, into a clip
([recording.md](recording.md)).

## The audio clip editor

While the selection is an audio clip, or a track that holds audio, the tab shows the audio clip editor and is called
"Audio clip". It edits one clip: the first audio clip selected.

- **The whole file, as wide as the editor.** It never scrolls or zooms. The clip's part is in its track's colour and
  the audio its edges trim off is shaded, so the clip can be trimmed back out to it. Gain shows in the waveform's
  height. The waveform is drawn backwards while the clip plays backwards.
- **The ruler counts the song's bars** where the audio plays, so trims and fades line up with the song's grid. Its
  grid is the lower panel's, which the piano roll shares.
- **There are no tools to choose.** Alt places freely, as everywhere.

| On the audio | Does |
| --- | --- |
| Drag an edge (a bracket with a grip) | Trims the clip |
| Drag a round handle at a top corner | Fades the clip in or out |
| Double-click a fade's corner handle | Takes the fade away |
| Drag the hollow handle half way along a fade | Bends its curve |
| Double-click that handle | Straightens the fade |
| Right-click that handle | Switches the fade between a curve and an S-curve |
| Drag the clip's part | Moves it along the audio: the clip stays where it is in the song and plays another part of its audio |
| Right-click elsewhere on the clip | The four fade shapes |
| Double-click along the top | Pins the audio there with a warp marker |
| Drag a warp marker or a hit | Moves it, on the grid unless Alt is held; a hit becomes a marker |
| Double-click a warp marker | Removes it |
| Right-click along the top | Remove Warp Marker, Quantize Audio, Remove Warp Markers |

**A fade** is a curve or an S-curve bent from -1 to 1. A curve bent to 1 is convex (quick at first), to -1 concave; an
S-curve bent to 1 is slow at both ends, to -1 quick at both. The four fade shapes of the menus and the inspector set
the four ends of that range.

**How the clip plays its audio,** down the left, as fields to drag, scroll or type in, each change one undo step:

- **Gain,** from -60 to +24 dB, with **Normalize** beside it, which sets the gain so that the loudest peak of the part
  of the file the clip plays is at -1 dB. Normalize is one undo step and writes no file. It reads the part as
  recorded, before pitch, stretch and fades. A silent clip keeps its gain. Nothing normalizes by itself.
- **Pitch,** up to 48 semitones up or down, and **Fine,** 50 cents either way. The clip keeps its length.
- **Reverse:** the same part of the audio plays, backwards, where the clip is.
- **Follow Tempo,** with the clip's **Tempo:** following, the clip stretches with the song's tempo, keeping its pitch,
  from the tempo its file gives, or else the song's where the clip starts, so nothing changes until either tempo does.
  Setting the tempo the audio was played at puts it in time: the clip keeps its start and the audio in it, and gets
  shorter or longer.

Reverse, Follow Tempo and Normalize are commands too, for the selected audio clips, in a clip's right-click menu in
the playlist and in the command search.

**Warp markers:**

- A marker pins a time of the clip's audio to a beat. Between two markers the audio stretches to fit; before the first
  and after the last it plays straight; the clip's start keeps its audio.
- A clip with markers follows the tempo, and turning Follow Tempo off takes them away. Markers keep their beats when
  the clip's tempo changes. An alias shares its clip's markers.
- Along the top of the audio, the hits Phazite finds in it (once for each file, in the background) show as small
  ticks, and the markers as flags in the warning colour, each with a line down the audio.
- **Quantize Audio** moves each hit the clip plays to the nearest grid line, through a marker of its own; the clip's
  start holds, and so do markers no hit replaces. In the audio clip editor it is Quantize (Ctrl+U), to its grid; in
  the playlist, the clip menu's Quantize Audio, to the playlist's. One undo step.

**Commands that read much of a file work in the background.** An import's copy of a file into the project (and its
look at a file of the same name there), the copy of a sound put on a drum pad, the sampler or a wavetable, kept by
Swap or found for a missing file, Normalize's peaks and Quantize Audio's hits are read on a thread of their own, one
command after another, so that a long recording never stops the window's drawing and the audio. To the user they
behave as if they read at once: from the moment one starts, the window takes no clicks or keys (but those of the
few commands that change nothing, as while a mouse button is down: play and stop, the tools, the zoom), so nothing
done meanwhile lands before their step or is lost under it; a short one ends before anyone notices, and one that takes
longer than a second shows its progress with Cancel in the window a freeze shows (a render asked for meanwhile waits
its turn). The command's undo step is the next after the step before it, never inside a drag or a Swap's trial in
progress (a trial open as it starts is kept first, as an edit made at once keeps it). If something other than the user
changed the song meanwhile so that the result no longer fits (the clip or the track gone, the clip trimmed, at another
gain or playing another file, the project moved), nothing changes and a notice says why; opening another project
stops it at once. Cancel leaves the project as it was, and lets go of the window at once, even when the read is stuck
in the system (a network share gone), which is then let finish unseen, what it wrote deleted. A quit doesn't wait for
it, nor does another project; an unsaved project's folder is deleted once no such read may write there.

**Stretched, pitched and reversed audio is prepared in the background.** Phazite renders the whole file with Rubber
Band's R3 engine into the project's cache folder, and the clip plays the render. The clip is silent until the render
is ready, and the editor and the status bar say so meanwhile. Clips that play a file alike share a render, and undo
plays the earlier render at once.

## Decisions

- Time counts from the clip, not from the song: a pattern's bar 1 is where it starts, as in FL Studio and Ableton Live.
- FL Studio's mouse model: a click adds a note and a right-click deletes it, in every keymap.
- The piano roll does not turn pages while the song plays, so that the notes being edited stay in view.
- The Smart Tool draws notes, so the piano roll has no Draw tool.
- A key belongs to a track and is off until chosen.
- No "stamp the current chord": the chord lane's tones are tinted in the rows to draw by.
- Transforms stay editable until applied, so that a preview is never made permanent by selecting something else.
- The audio clip editor shows the whole file and never scrolls or zooms, so that the clip cannot be lost in it; it has
  handles and no tools.
- Normalize sets the clip's gain to -1 dB and writes no file, with no level to choose.
- One stretch algorithm, rendered in the background before it plays, rather than several played live.

## Not built

- Controller, pitch-bend and per-note expression lanes: Planned. Controllers that a MIDI file brought in play, but the
  piano roll does not show them.
- Pitch glides between notes sent to plug-ins as MPE note expression: Planned, after version 1.
- A choice of ghost notes per track, and a single click on a ghost note to go to its track: Planned; today one switch
  shows every other track's notes, and a double-right-click on one shows its clip.
- Flam, velocity scaling, reverse and flip transforms: Planned.
- A key of its own for every transform: Planned; Quantize..., and in FL Studio's keymap Strum... and Arpeggiate...,
  have one.
- A switch from clip time to song time, to edit a whole track: Planned.
- Articulations in a lane under the notes: Planned, after version 1.
- "Stamp the current chord": Declined.
- In the audio clip editor: a choice of stretch algorithms (a mode for drum loops), formants, warping from a detected
  tempo, a sensitivity for the hits, and gain changes inside a clip: Planned.
- A waveform zoom for the whole playlist: Planned.
- Zoom and scroll in the audio clip editor: Declined.

## In the code

- `src/ui/pianoroll/PianoRoll.h`, with its parts in `PianoRollParts.h`: rows (`PianoRollRows.cpp`), the note grid
  (`PianoRollNotes.cpp`) and its tools (`PianoRollNoteTools.cpp`), the keys, the velocity lane and the transform bar
  (`PianoRollLanes.cpp`), and the transforms, menus and commands (`PianoRollCommands.cpp`).
- `src/ui/pianoroll/DrumRows.h`: which rows a drum track shows, and in what order.
- `src/model/notes/NotesModel.h` (the notes by IDs), `NoteValues.h` (notes as plain values, the transforms, keys and
  scales, chord stamps).
- `src/ui/audioclip/AudioClipEditor.h`.
- `src/model/arrangement/AudioClipModel.h` (gain, pitch, reverse, tempo, warp markers, Quantize Audio),
  `PlaylistValues.h` (`FadeCurve`), `internal/Transients.h` (the hits), `internal/ClipRenders.h` (the renders).
- `src/devices/stretch/Stretch.h`: stretching, pitching and reversing with Rubber Band.
- `src/ui/timeline/TimelineEditor.h`: the time line, grid and tools both editors share with the playlist.
