# The playlist

*Design section 6.*

The playlist shows the whole song: its tracks down the left, their clips along a time line, the song's sections over
the bar numbers and the automation lanes under their tracks. Clips are arranged, cut and copied here; what is inside a
clip is edited in the lower panel ([piano-roll.md](piano-roll.md)).

## Status

| Feature | State | Note |
| --- | --- | --- |
| Smart Tool (trim, fade, select time, move, crossfade by where a drag starts) | Built | |
| Draw, Paint, Slice, Slip, Mute, Delete, Select and Zoom tools | Built | |
| Time range selected across tracks | Built | |
| Split, duplicate, alias, make unique, delete, close gap, insert and remove time | Built | |
| Nudge and quantize clips | Built | |
| Reverse, normalize, clip gain, fades with four shapes, crossfades | Built | Audio clips |
| Consolidate, Bounce in Place | Built | |
| Overlapping clips shown one under another, all playing | Built | |
| Clip tags and marks in place of name strips | Built | |
| Track headers: add, move, colour, rename, collapse, pin, resize | Built | |
| Focus Mode | Built | |
| Level strip on each track header | Built | |
| CPU readout on each track header | Planned | No roadmap step; the mixer's strips show it |
| Folders, buses and returns as rows without clips | Built | |
| Sections in the time bar | Built | |
| Loop strip in the rulers, Loop Selection, Loop Section | Built | |
| Follow Playback, Scroll Continuously | Built | |
| Adaptive grid, triplets, snap | Built | |
| Overview strip as the horizontal scroll bar | Built | |
| Zoom to Selection, Zoom Back | Built | |
| Clip picker (a searchable list of every clip) | Planned | No roadmap step |
| Linked tracks: a folder's tracks edited as one | Planned | Roadmap step S7.10 |
| Audio and MIDI clips kept on separate tracks | Built | Dropped files go to a track of their kind, or a new one below, which the lanes show and the hint bar says; a clip dragged or drawn onto a track of the other kind (or a folder, a return, the master) stays where it can go, and the hint bar says why |

## What it shows

From the top: the overview strip, the time bar (the sections over the bar numbers, with the button that adds a track
and Collapse All beside it), the tempo lane while the song has tempo changes
([window.md](window.md#tempo-changes-along-the-song)), the chord lane while the song has chords
([chords.md](chords.md)), then the tracks, with the sections drawn faintly behind them. Its tools, Snap, the grid
and Follow Playback are in its area's tab row ([window.md](window.md#tools-in-the-tab-row)).

While the song has no tracks, the lanes say how to start: drag an instrument or a sound there from the browser,
double-click for an empty track, or open an example song from the Help menu.

## The Smart Tool

The Smart Tool is the default pointer. Over a clip, where the drag starts decides what it does:

| Where on a clip | Dragging does |
| --- | --- |
| Left or right edge | Trims the clip |
| Top-left or top-right corner of an audio clip | Draws a fade-in or a fade-out |
| Upper half | Selects a time range (a clip on a short track has no upper half: all of it moves) |
| Lower half | Moves the clip; Ctrl copies it, Ctrl+Shift makes an alias |
| The boundary between two touching audio clips | Draws a crossfade, as long as twice the distance dragged from the boundary |

On empty space:

| Gesture | Does |
| --- | --- |
| Drag | Selects a time range across tracks |
| Click | Selects (Phazite's and Ableton Live's keymaps); places the clip used last (FL Studio's keymap, where Ctrl+drag selects a time range) |
| Double-click on a track with an instrument | Makes an empty MIDI clip a bar long |
| Double-click under the last track | Adds a track |
| Double-click a clip | Shows it in its editor |
| Middle drag | Moves the view, sideways and up and down; never edits |
| Right-click | Opens the menu; in FL Studio's keymap it deletes clips, and Shift+right-click opens the menu |
| Wheel | Scrolls; Shift+wheel sideways; Ctrl+wheel and a trackpad's pinch zoom around the pointer; Alt+wheel makes every track taller or shorter |

Right-click deleting and click placing are settings of a keymap (`rightClickDeletes`, `clickPlacesClips`).

Dragging anything (clips, edges, ranges, tracks, sections, the playhead) near or past an edge of the view scrolls it
along, faster the further out the pointer is.

**A move keeps an item's offset from the grid.** With Snap on, a clip moves by the grid's steps from where it was, so
one that is off the grid stays as far off it, and a drag straight up or down changes no time. Alt moves freely, and
Quantize puts a clip on the grid. Edges (a trim) snap to the grid's lines. A drag that copies makes its copies only
once it moves them; a drag that ends where it began is a click and makes no undo step.

## The other tools

| Key | Digit | Tool | Does |
| --- | --- | --- | --- |
| Esc | 1 | Smart | As above |
| P | 2 | Draw | A click places a copy of the clip used last |
| B | 3 | Paint | A drag places copies of it, one after another |
| C | 4 | Slice | A click splits a clip |
| S | 5 | Slip | A drag moves what is inside a clip; the clip stays where it is |
| T | 6 | Mute | A click mutes or unmutes a clip |
| D | 7 | Delete | A click deletes a clip, and so does dragging over clips |
| E | 8 | Select | A drag selects the clips a rectangle touches; Ctrl adds to the selection |
| Z | 9 | Zoom | A click zooms in, Alt+click out, a drag zooms to the range dragged |

- The letters are FL Studio's. The digits never play notes, so they work while the computer keyboard does.
- A tap of a tool's key switches to the tool. Holding the key while using the mouse keeps the tool only until the key
  is let go, or until a drag still going on then ends.
- A held key repeats only the commands that take a step (nudging, zooming, moving through tracks, undo).
- The tool in use shows in the tab row and in the hint bar.

## Editing commands

| Command | Key | Does |
| --- | --- | --- |
| Split | Ctrl+E | Splits the clips in the selected time range at its edges, or the selected clips at the playhead |
| Duplicate | Ctrl+D | Copies the selection right after it; selected tracks are copied below themselves |
| Duplicate as Alias | | Copies the selected clips or sections as aliases |
| Make Unique | | Turns the selected aliases into separate clips |
| Delete | Delete | Deletes the selection |
| Delete and Close Gap | | Deletes the time range or clips and moves what comes after back |
| Insert Time | Ctrl+I | Inserts empty time as long as the selected range, on every track |
| Remove Time | Ctrl+Shift+Delete | Removes the selected range from every track |
| Cut, Copy, Paste | Ctrl+X, C, V | Notes, a time range or clips, with the automation under the clips; Paste puts them at the playhead |
| Mute | 0 | Mutes or unmutes the selected clips |
| Nudge Left, Nudge Right | Left, Right | Moves the selection a step of the grid |
| Quantize | Ctrl+U | Moves the starts of the selected clips to the nearest grid line |
| Reverse, Follow Tempo, Normalize, Quantize Audio | | For the selected audio clips ([piano-roll.md](piano-roll.md#the-audio-clip-editor)) |
| Linear, Convex, Concave, S-Curve Fades | | The fade shape of the selected audio clips |
| Consolidate | Ctrl+J | Joins the selected clips on each track, or what lies in the selected time range, into one clip |
| Bounce in Place | | Renders the selected track through its devices onto a new audio track under it, and mutes the original |

- **Duplicate with a time range selected** copies what lies in it, on its tracks, right after it, cut at the range's
  edges (the originals are not cut). Each piece replaces what it lands on, and the range moves to the copy, so
  Duplicate again repeats it. Undo puts the range back.
- **Consolidate** makes a MIDI clip of the notes as they play, or one audio file rendered from the audio clips before
  the track's devices, as one undo step.
- A clip's gain is set in the inspector and in the audio clip editor.

## Clips

**An alias** is a linked copy of a clip: editing the notes of one edits them all. Make Unique breaks the link.

**Clips that overlap all play,** mixed together, so a sample's tail rings on under the next copy of it. The playlist
shows them one under another in the track's row:

- Clips that overlap, directly or through others, share the row out into as many sub-rows as they need. A clip that
  overlaps none has the whole row.
- The clip made first takes the highest free sub-row. Dragging a newer clip over it never moves it.
- Two audio clips that overlap only where one fades out and the other fades in (a crossfade) share a sub-row, and the
  crossfade is drawn where they meet.
- It is view only: nothing is saved, there is no undo step, and playback does not change.

**How a clip looks:**

- It fills its row down to the line under the row. It is see-through, its track's colour at half strength inside an
  opaque edge of that colour, so that the grid and the sections' lines show through it. Its contents are drawn in
  black or white, whichever reads on the colour. It is drawn a pixel short of its end, so that two clips side by side
  have a line between them.
- A clip always has its track's colour.
- There is no strip for its name. A **tag** at its top left shows its name only where that says more than its track's
  header: a name of its own, a sample's file name that is unlike its track's, or the take it plays of how many were
  recorded round a loop. A name like its track's shows no tag: the same words apart from capitals and separators, and
  apart from a tempo or a key that only one of the two names gives ("Shaker 82 BPM" over a track called "Shaker").
  "Shaker 2", another tempo or another key still shows.
- The tag stays at the playlist's left edge while its clip scrolls past it.
- At the top right, a **link** marks an alias and a **metronome** a sample that follows the tempo.
- A clip without a name of its own is called after its track: renaming the track renames it, a clip moved to another
  track takes that track's name, and the project saves no name for it. The inspector's **Name** gives it one (and
  names an alias's copies with it); emptied, the clip is called after its track again.
- A **muted** clip is drawn faint, and so is every clip of a track that is not heard, muted or silenced by other
  tracks' solos. A selected clip keeps its outline.
- Tags and marks stay when the pointer leaves the playlist.

## Tracks

A track's header shows its colour strip, its name, its arm switch, its one switch for mute and solo
([mixer.md](mixer.md#mute-and-solo)), and down its right edge how loud it plays now. It has no volume or pan: the
mixer and the inspector have them.

| Gesture or command | Does |
| --- | --- |
| The Track button over the headers; Add Track (Ctrl+T) | Adds a track below the selected one |
| The row after the last track; double-click under the last track | Adds a track at the end; right-click the row for a folder |
| Click a header | Selects the track; Ctrl and Shift add |
| Drag a header, from anywhere on it | Moves the selected tracks together |
| Click the colour strip | Opens the colour menu |
| Rename (F2 or Ctrl+R) | Renames the track |
| Double-click a header; Collapse Track (U); drag the bottom edge up past the smallest height | Collapses the track to a thin row with its name, or opens it again |
| Collapse All (the button beside Track; Alt+U) | Makes every track, folder and automation lane a thin row; when all are, opens them all |
| Drag a header's bottom edge; Taller Tracks (Alt+=), Shorter Tracks (Alt+-) | Sets the track's height |
| Pin Track | Keeps the selected tracks in a band at the top while the others scroll |
| Focus Mode | Makes every track small but the selected ones and the folders they are in |
| Group into Folder (Ctrl+G), Ungroup Folder (Ctrl+Shift+G) | Puts the selected tracks into a new folder, or takes them out |
| Up, Down | Selects the track above or below |
| Drag the header column's right edge | Sets the headers' width, from 150 to 360 pixels; a double-click puts back 200 |

Rules:

- **Name and colour** come from what the track was made from and can be changed. A track made from a dropped file is
  named after the file and coloured by what its name says it is (drums, bass, vocals, effects, strings, synths);
  another new track takes the palette's next colour after its neighbour's. The row behind the clips is not tinted.
- **Moving tracks:** where they would go is decided by the half of a row the pointer is in, and shows as a line in the
  accent colour that starts at the depth in folders they would go to. On a folder, its middle puts them into it (the
  folder outlined), its top quarter before it, and its bottom quarter first in it while its tracks show. A folder
  cannot go into itself. Dragged past the playlist's left edge, a track goes into the browser as a preset.
- **A collapsed folder** hides its tracks instead of becoming thin.
- **Height:** a track starts 48 pixels tall and a folder 40; a collapsed row is 24.
- **The level strip** is 4 pixels wide down the header's right edge, in the track's colour: the louder channel's peak
  after the fader and mute, from -48 to 0 dB, as a height in a brighter shade over a faint one. It rises at once and
  falls without holding a peak; a peak over 0 dB lights its top in the danger colour for two seconds. It takes no
  clicks. A plain folder and the master's row show none. In silence nothing repaints.
- **Targets:** the colour strip takes clicks 16 pixels into the header and lightens under the pointer; the mute and
  solo switch takes clicks 24 pixels square where its row has room.

### Folders, buses and returns

- A folder's row holds no clips. It shows what its tracks play: each of their clips as a bar in its track's colour, on
  a thin lane for each track that holds clips, faint where muted, so that a collapsed folder still shows its part of
  the song.
- A folder icon before its name marks it, open while its tracks show. Its tracks are indented, and the folder's colour
  runs down their left as a rail; a folder in a folder adds a rail of its own.
- A track's own colour runs on down its automation lanes' headers.
- A return holds no clips and has no row while it has no automation lanes; the mixer shows it. With a lane, its thin
  row comes after the tracks, where the master's shows.

## Sections

Sections are a thin band in the time bar, above the bar numbers: each a block of its colour over its time, with its
name. Behind the tracks they show as a faint wash of each one's colour with a line where it starts.

| Gesture | Does |
| --- | --- |
| Drag a section | Moves it with everything in it, on every track; Ctrl copies it, Ctrl+Shift copies it as aliases |
| Drag its right edge | Makes it longer or shorter; the rest of the song moves along |
| Double-click | Adds a section there, or renames the one clicked |
| Right-click a section | Rename, Color..., Loop Section, Duplicate, Duplicate as Alias, Delete |
| Right-click the band or the bar numbers | Add Section Here, Add Chord Here, Add Tempo Change Here, Song Outline |

The Add menu has Add Section and the song outlines too ([getting-started.md](getting-started.md)). Deleting a section
deletes what is in it.

## The loop, the playhead and playback

- **The song loops** from its start to the end of the bar where its last clip ends, so the loop grows as clips go
  later. Sections do not count. That is also where Go to End (End) goes. A song without clips loops its first bar.
- **Loop Selection** (Ctrl+L) loops the time range selected in the playlist, or the whole song when none is selected.
  **Loop Section** loops the selected sections. **Loop the Song** goes back to following the song. With FL Studio's
  keymap a selected time range loops while it is selected.
- **Loop** (L) in the transport bar turns looping on or off. Playback that does not loop stops at the song's end and
  goes back to where it started.
- Where the loop is is not an undo step.

The top strip of the playlist's and the piano roll's rulers is for the loop; lower down, the ruler moves the playhead.

| In the ruler | Does |
| --- | --- |
| Drag along the loop strip | Loops that part of the song; Ctrl+drag starts a new loop anywhere |
| Drag the loop's ends or middle | Changes or moves it |
| Click the loop strip | Turns looping off and on |
| Double-click the loop strip | Loops the whole song |
| Press below the loop strip | Takes the playhead there |
| Drag below the loop strip | Takes the playhead along, on the grid (Alt places it freely), scrolling at the view's edges |

- While the song plays, a drag of the playhead silences it from the press and it plays on from where the playhead is
  let go. A recording is not paused.
- The playhead is in the focus colour: a marker at the foot of the ruler with its line over the tracks.
- The rulers show a tick and a number for the numbered bars and short, faint ticks for the other bars and the beats
  where there is room.
- **Follow Playback** (Ctrl+Shift+F; on in the playlist, off in the piano roll): the view turns the page when the
  playhead reaches its right edge. Scrolling sideways while the song plays pauses following until playback starts
  again, the playhead is moved or the view is centred on it; holding the mouse down keeps the page from turning under
  a drag.
- **View > Scroll Continuously** (off at first) holds the playhead in the middle instead and scrolls the song under
  it, in every editor that follows playback.
- The playhead shows the beat that is heard: where the engine is, less the delay compensation and the output's
  latency. It is drawn between pixels, so that it glides at any zoom.
- **Starting from silence:** when playback starts, or jumps while it plays, every device is reset, Phazite's own and
  the plug-ins, so that nothing rings on from before. Tails ring on after Stop and through the loop's end.

## Grid, zoom and scrolling

- The grid adapts to the zoom (Adaptive Grid) or keeps one size; Triplet Grid divides beats into three. Snap is on by
  default; Alt leaves the grid during a drag.
- **Scrolling by hand** (the overview's thumb, the wheel, the rows' scroll bar, the middle button's pan) moves the
  view at the display's frames, once for all the mouse reported since the last, not at each report. Sideways, the
  overview and the wheel glide the view there over a few frames, by parts of a pixel, and it rests on a whole one;
  the overview's thumb stays under the pointer meanwhile. The rows, and a pan, go straight where they were taken.
- **The overview** is a strip along the top of the time bar: the whole time line in miniature, each track that holds
  clips a thin row with its clips in its colour, faint where muted, and the part in view a see-through thumb over
  them. It is the playlist's horizontal scroll bar. Dragging the thumb scrolls, dragging either of its ends zooms, a
  press elsewhere centres that part and drags on from there, and the wheel scrolls.
- **The tracks' scroll bar** lies over the right edge of the lanes ([window.md](window.md#design-language)).
- The time line reaches to the end of everything on it, sections and chords included, with room after.
- **A big song costs what is in view.** Only the clips in view, and the headers and automation lanes of the tracks in
  view (and one more at either end, which the Tab key goes on to and scrolls into view), have components, made as they
  scroll into view; a header or a lane stays while it is in use (its name typed, the tracks dragged by it, a point
  dragged). A jump (a scroll bar's thumb dragged far) makes what part of a frame can bear, and the rest over the next
  turns of the message loop. A change of a few clips (a note
  added, a step of a drag) reads those clips' tracks again and repaints those clips alone, and the overview only if
  what it shows of them moved. A screen reader hears the headers in view, and from the headers' column how many tracks
  there are.

| Command | Key | Does |
| --- | --- | --- |
| Zoom In, Zoom Out | =, - | Zooms the time line |
| Zoom to Fit | | Shows the whole song |
| Zoom to Selection | Z in Ableton Live's keymap | Fits the selected time range or clips, with a twentieth of room each side |
| Zoom Back | X in Ableton Live's keymap | Returns to the zoom before the last Zoom to Selection or Zoom to Fit |
| Center on Playhead | | Scrolls so that the playhead is in the middle |
| Snap to Grid | Ctrl+4 | Turns snapping on or off |
| Adaptive Grid | Ctrl+5 | |
| Finer Grid, Coarser Grid | Ctrl+1, Ctrl+2 | |
| Triplet Grid | Ctrl+3 | |

Each editor keeps its own zoom steps for Zoom Back, and forgets them when another project opens. The time line's
commands act on the editor used last.

## Decisions

- While a mouse button is down, no key runs a command that changes the song, Undo and Redo included, and Escape takes
  the drag back, leaving nothing to undo or redo: a command in the middle of a drag left the undo history wrong. A tool
  chosen in the middle of a drag takes over when the mouse goes up.

- The Smart Tool is the default: one pointer does the everyday edits by where it is on a clip.
- A clip's upper half selects time, so a clip is not dragged by a name strip, and clips have none; a name shows only
  where it adds something.
- In Phazite's keymap a click on empty space selects, so that a click never adds a clip by accident; FL Studio's
  keymap keeps FL Studio's click and right-click.
- Overlapping clips all play, as in FL Studio and REAPER, rather than the later one cutting the earlier, so that a
  repeated sample's tail is not cut off. They stack inside one track rather than on rows of their own.
- A clip cannot have another colour than its track's.
- Tracks move by their whole header; there is no drag handle and no collapse arrow, and no volume or pan there.
- Folders, buses and returns hold no clips; a return has no row until it has an automation lane.
- Sections are a band in the time bar and a wash behind the tracks, not a row of their own.
- The song loops by default, to the end of its last clip's bar; the loop is not an undo step.
- Page turning is the default way to follow playback, with continuous scrolling as a setting.
- Devices are reset when playback starts, so that a song always starts from silence.
- The overview replaces the horizontal scroll bar.

## Not built

- A clip picker, a searchable list of every clip to drag from or to choose what the Draw tool places: Planned.
- A CPU readout on each track header: Planned.
- Audio and MIDI clips meant to share one track: Declined for version 1.

## In the code

- `src/ui/playlist/Playlist.h`, with its parts in `PlaylistParts.h`: rows and clips (`PlaylistRows.cpp`), tools
  (`PlaylistTools.cpp`), drops (`PlaylistDrops.cpp`), automation lanes (`PlaylistAutomation.cpp`), track headers and
  the playlist's commands (`PlaylistTracks.cpp`), recording (`PlaylistRecording.cpp`).
- `src/ui/playlist/TrackHeader.h`, `ClipComponent.h`, `ClipWaveform.h`, `SectionsStrip.h`, `TempoLane.h`,
  `ChordLane.h`, `TrackHeights.h`.
- `src/ui/timeline/TimelineEditor.h` (the time line, grid, tools, rulers and their commands), `SmartTool.h`,
  `TimelineView.h`, `TimelineGesture.h`, `TimelineOverview.cpp`, `SongPlaceText.h`.
- `src/ui/components/PeakBar.h`, `TrackSwitch.h`, `ArmButton.h`.
- `src/model/arrangement/PlaylistModel.h` (the playlist by IDs; `getClipSubRows()`, `getTimelineEnd()`),
  `PlaylistValues.h`, `SongRange.h`, `TrackColours.h`.
- `src/model/arrangement/internal/Tracks.h`, `Clips.h`, `AudioClips.h`, `Sections.h`, `Consolidate.h`, `Clipboard.h`.
- `src/model/transport/internal/Playback.h`: the loop and the song's end.
- `src/ui/input/Keymap.h`, `resources/keymaps/`: the keymaps and their settings.
- `tests/golden/commands/app-catalogue.txt`: every command with its keys.
