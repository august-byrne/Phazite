# The window

*Design section 4.*

Phazite works in one window: the whole song in the centre, the selection in detail below it, a panel of views on the
left and the properties of what is selected on the right. This page says what each part shows, how the panels are
arranged, and the look they share.

```
+--------------------------------------------------------------------------------+
| logo File Edit Add View  My Song •   [<-][->][ Search commands ]  [|][_][|][#] - o x |
+--------------------------------------------------------------------------------+
| Play Rec Loop Metronome   140 BPM  4/4   17.3.2  1:23.4     master level  keyboard |
+---+----------+--------------------------------------------+-------------------+
| B | Browser  | [Playlist] [Mixer]   tools, Snap, grid      | Inspector         |
| P | Project  |  + Track   Intro    Build    Drop   (bars)  | (the properties   |
| S | Source   |  v Drums   (a folder is a bus)              |  of what is       |
|   | Control  |      Kick    [] [] [] [] [] [] [] []        |  selected)        |
|   |          |  v Synths                                   |                   |
|   |          |      Lead    [riff ] [riff ] [riff ]        |                   |
|   |          +--------------------------------------------+                   |
|   |          | [Piano roll] [Instrument] [Effects]         |                   |
+---+----------+--------------------------------------------+-------------------+
| Saved | Bass | hint: what the mouse and the keys do here | 48 kHz · 10 ms | CPU |
+--------------------------------------------------------------------------------+
```

## Status

| Feature | State | Note |
| --- | --- | --- |
| Title bar with menus, Undo and Redo, command search, panel switches | Built | The window's own title bar on Windows |
| Phazite's own title bar on Linux and beside macOS's window buttons | Planned | No roadmap step; the system's title bar stays above it |
| Transport bar | Built | |
| Playhead's place and time as fields | Built | |
| What a tempo change stretches | Built | |
| Tempo changes along the song (the tempo lane) | Built | |
| Tap tempo, tempo detection from audio | Planned | After version 1 |
| Activity bar with the Browser, Project and Versions views | Built | |
| Upper and lower panel with views that move between them | Built | |
| Each editor's tools in its tab row | Built | |
| Inspector | Built | |
| Status bar with the hint bar | Built | |
| Notices in place of dialogs | Built | |
| Panels hidden, maximized, in a window of their own | Built | The upper panel always stays in the main window |
| The window opens where it was closed | Built | Only on a screen that still exists |
| Layouts: Beat, Mix, Full | Built | |
| Layouts of the user's own | Planned | No roadmap step |
| Dark, light and high-contrast themes, Follow System | Built | |
| Frosted popups | Built | Solid in the high-contrast theme and where the system reduces transparency |
| Interface size, 75 % to 200 % | Built | |
| Inter with fixed-width digits | Built | |
| One on/off switch everywhere | Built | |
| Overlay scroll bars that fade | Built | |
| Logo and startup sound | Built | |
| The system's reduced-motion setting | Planned | No roadmap step; nothing reads it |
| Design tokens in a file a designer edits, and a gallery app | Planned | Roadmap phase A4 |

## Title bar

On the left, Phazite's logo, the menus and the project's name, with a dot after it while it has unsaved changes. In the
middle, Undo and Redo, whose tooltips name the step they take, beside the command search (Ctrl+K). On the right, a
switch for the left panel, the lower panel and the inspector, each lit while its panel shows; the Layout menu (the
layouts, each panel, Maximize the Panel in Use, Full Screen); then the window's minimize, maximize and close buttons.

- The bar's empty parts and the project's name drag the window, and a double-click on them maximizes it. Windows 11's
  snap layouts open from the maximize button. A restored window resizes from a band along its sides.
- In a narrow window the menus fold into one button, then the project's name goes, then the search narrows.
- F10 opens the menus.
- In full screen the bar stays, without the logo and the window's buttons.
- On Windows it is the window's title bar. On Linux the system's title bar stays above it. On macOS the system's title
  bar stays and the menus are in the menu bar at the top of the screen; the logo does not show there.

## Transport bar

A row under the title bar, for what belongs to the song as a whole, in groups: the controls of a group a small gap apart,
the groups a larger one, every button with an icon as wide.

- Play, Record and Capture. Record has an arrow joined to its right, one control with it (lit together under the
  pointer), which opens its options (the recording's commands), as a right-click on the button does.
- Loop and the metronome, whose arrow opens its count-in in the same way.
- The tempo and the time signature. The tempo field is dragged, scrolled or typed in; the time signature is a menu.
- The playhead's place in bars, beats and sixteenths ("17.3.2", each counted from 1) and its time ("1:23.4"). Each is a
  field that moves the playhead when dragged, scrolled or typed in ("17.3", "1:05"). While the song plays they follow
  the playhead at the display's frames. A narrow window hides the time first, then the place.
- A/B, which plays the last version's bounce in place of the song and the song again, with what is heard beside it
  ([../versions.md](versions.md)). It shows only while there is a version to compare with: the bar holds nothing that
  can't be used.
- At the right, the master level and volume; then the computer keyboard's switch.

A key belongs to a track, so the piano roll shows it and the transport bar does not. Low Latency is a state of the
audio engine that is set once and left, so it is in the status bar, beside how busy the engine is, as Logic keeps it
out of its default control bar.

### What a tempo change stretches

As soon as the tempo field is dragged, scrolled or typed in, a panel under it says how many of the song's samples
follow the tempo and whether its patterns do, with a switch for each (the commands Samples Follow Tempo and Patterns
Follow Tempo). It goes a few seconds after the tempo stops changing, or at a press anywhere else.

- A sample that follows the tempo plays faster or slower with it, from a render Phazite makes
  ([piano-roll.md](piano-roll.md#the-audio-clip-editor)), and shows a metronome mark on its clip.
- Patterns (MIDI clips) follow by default, their notes staying on their beats.
- What is not stretched keeps its speed. Every clip still starts on the same beat.
- Samples Follow Tempo switches every sample's own Follow Tempo at once.

### Tempo changes along the song

The tempo field shows the tempo where the playhead is, and changing it changes the tempo in effect there. A song with
one tempo therefore works as if there were no tempo changes at all, and there is no override mode.

- **Add > Add Tempo Change** adds a change at the playhead; a right-click on the time bar offers Add Tempo Change Here.
- From then on a tempo lane shows under the time bar, above the chord lane, until **Remove Tempo Changes** leaves the
  first tempo all the way through.
- The lane draws the tempo as the engine plays it, with a point where each tempo starts and its value beside it.

| In the tempo lane | Does |
| --- | --- |
| Click | Adds a tempo change at the tempo clicked |
| Press on the line | Pulls a new point from it |
| Drag a point | Moves it on the grid; Alt places it freely, Shift moves it in fine steps; the first point moves up and down only |
| Right-click a line | Makes it ramp to the next tempo, or hold and jump |
| Drag the handle in a ramp's middle | Bends the ramp |
| Double-click | Deletes a point, or straightens a ramp |
| Right-click a point, Type a Value... | Opens a field over the point that takes "128" or "92.5 bpm"; one undo step |

Rules:

- Each tempo holds until the next and jumps to it, or ramps to it, straight or bent as an automation lane's line bends.
  The engine plays a straight ramp exactly and a bent one in short straight pieces.
- Samples that follow the tempo stretch along its changes, ramps included, each beat of their audio on a beat of the
  song.
- Patterns keep their beats along the changes. Patterns Follow Tempo, off, keeps their speed only while the song has
  one tempo.
- A MIDI file whose tempo changes brings them into a song that has neither clips nor tempo changes yet. A DAWproject
  carries them both ways, a bent ramp as straight pieces.
- With nothing selected, the inspector shows how far the tempo goes ("50 to 260 BPM").

## Left panel

The activity bar down the window's left edge picks the left panel's view; a click on the icon of the view that shows
hides the panel.

- **Browser:** instruments, effects, plug-ins, presets, sounds and projects ([browser.md](browser.md)).
- **Project:** what the song holds, in three panes that fold.
  - *Notes*, kept in the song and saved with it. They are typed in place; each burst of typing is one undo step,
    "Change Notes". Showing the view does not give the notes the keys, so shortcuts go on working until the notes are
    clicked.
  - *Audio Files:* each audio file the song plays (its audio clips' and its devices' sound files) with the tracks that
    play it; the header counts them and the missing ones. A missing file shows in the warning colour with Locate...
    (also a double-click): the file chosen is copied into the project and takes the missing one's place everywhere, as
    one undo step, "Locate Audio File". Under the list, while the project's folder holds audio files that nothing
    plays, their count with **Clean Up...**, which runs Clean Up Unused Audio.
  - *Plug-ins:* each plug-in the song holds, on its track or the master; the header says how many crashed, are not
    installed or are off. A crashed one has Reload. A click selects the plug-in and shows it in the Devices view.
- **Versions:** the project's versions and branches ([versions.md](versions.md)).

## Centre: the song above, the selection below

The centre has two rows of tabbed areas, the **Upper Panel** and the **Lower Panel**. By default:

- the upper panel shows the whole song, as the **Playlist** ([playlist.md](playlist.md)) or the **Mixer**
  ([mixer.md](mixer.md));
- the lower panel shows the selection in detail: the **selected clip's editor**, whose tab is named for what it shows,
  the piano roll for MIDI clips and the audio clip editor for audio clips ([piano-roll.md](piano-roll.md)); and
  **Instrument** and **Effects**, two pages of the Devices view for the selected track ([devices.md](devices.md)).

The clip editor follows the selection: MIDI clips selected show in the piano roll, audio clips in the audio clip
editor, a selected track in the editor for what it holds, and a double-click on a clip shows it there. Automation has
no tab: it is edited in lanes under its tracks in the playlist
([automation-and-modulation.md](automation-and-modulation.md)).

### Views move between the areas

There are four views: the playlist, the mixer, the clip editor and the devices (whose Instrument and Effects tabs move
together).

| Gesture | Does |
| --- | --- |
| Drag a tab onto another area's tabs or middle | Moves the view there; the area tints and a line shows where the tab goes |
| Drag a tab onto an area's left or right quarter | Puts the view in an area of its own beside it, while that row has one area |
| Drag a tab onto an area's edge toward an empty row | Moves the view into that row |
| Drag a tab along its own area | Changes its place among the tabs |
| Right-click a tab | Offers the same moves, and Reset View Places |
| Drag the line between two areas side by side | Shares the row out; each area keeps at least 240 pixels |

- Each row holds one area or two side by side, so the centre holds at most four areas, two by two.
- An area left without views hides, and so does a row; the other row then fills the centre. The row's switch (Show the
  Lower Panel, say) brings its own views back to it.
- The settings keep where each view is and where the line between two areas is. **View > Reset View Places** puts the
  views back.
- A tab drags only within the main window. The lower panel in a window of its own moves its views from their menus.

### Tools in the tab row

The playlist, the piano roll and the audio clip editor have no toolbar of their own. Their tools, Snap, the grid (a
button that names the grid's size, such as "1/4 auto", and opens its menu) and Follow Playback sit in the tab row of
the area that shows the editor, right of its tabs, after a gap with a thin line in its middle that sets the tools apart
from the tabs. Where the row is too narrow, the least needed go into a More menu (…) at its end.

The tabs are drawn as VS Code's editor tabs, so that they don't look like the tools: the tab that shows is on its
view's own background, joined to the view under it, with a line in the accent colour along its top; the others are
names on the row's background, quieter. The tab of the editor used last, which the time line's commands act on, also
has a line in the accent colour along its foot.

## Inspector

The right panel shows the properties of what is selected, a name and a value on each row. It is not a copy of a mixer
channel. Its title, which names what it shows ("Bass", "3 clips", "Song") with the colour of a track or a section
beside it, is laid out as the left panel's views' titles are: as tall, its name as far in and in the same text, its X
where their actions are.

| Selected | Shows |
| --- | --- |
| A track | Name, colour, volume, pan, mute and solo, where its sound goes, its sends, its instrument, its effects and its automation mode |
| Clips | Name, start, length, mute, fades, whether it is an alias; for audio clips also gain and pitch |
| Notes | Start, length, pitch, velocity, mute |
| Automation points | Parameter, position, value, bend |
| A section | Name, colour, place, length, energy and tip |
| Nothing | The song's tempo (or its range), time signature and length |

- Values are fields to drag, scroll, step or type in. A drag is one undo step.
- Several notes, clips or points show together. A value shows the range of theirs while they differ ("64 to 100").
  Dragged, scrolled or stepped, each changes by as much, and notes and clips move together only as far as all can go.
  Typed, each length, velocity, gain and value becomes what was typed; the earliest start and the lowest pitch go
  there, the others with them.
- No key combinations change how a value applies to several items.
- Each page shows only what the selection has.
- A sound in the browser is not in the song, so the inspector does not show it; the browser does.

## Status bar

| Place | Shows | Click |
| --- | --- | --- |
| Left | Whether the project is saved: "Saved", "Unsaved changes", "Not saved yet", or "Autosave failed" in the warning colour | Saves |
| Left | What is selected: "Bass", "3 clips", "16 notes", "Bars 5 to 8" | |
| Left | Where played notes go: "MIDI → Bass" | Opens the MIDI settings |
| Middle | The hint bar | |
| Right | "Stretching 2 clips" with a progress bar, while clips are rendered in the background | |
| Right | The song's delay compensation, while it has some | Opens the audio settings |
| Right | The audio output's sample rate and delay ("48 kHz · 10 ms"; a Bluetooth output's in the warning colour) | Opens the audio settings |
| Right | Low Latency, lit in the accent colour while it is on | Turns it on or off |
| Right | How busy the engine is (CPU), in the warning colour near its limit | |

Each item says more under the pointer. In a narrow window the items leave the hints room, what is selected going
first.

**The hint bar** says what clicking, dragging and the modifier keys do under the pointer, shows the guide's next step,
and shows alerts such as no audio output. For six seconds after a command made less than its name says (Duplicate or
Paste of notes, some of which would land past a clip that the next clip keeps from growing), it says so ("2 notes of 3
weren't made..."), the undo step named for what was made ("Duplicate Note"). While a drag moves something it says where that is now: a clip's start
("Starts at 17.3.1", and its track once it moves to another), a trimmed edge with the clip's length ("Ends at 19.1.1,
2 bars long"), a note's pitch and place ("E4 at 3.2.1", counted from its clip), an automation point's value and place
("-6.0 dB at 9.1.1").

## Notices

What Phazite tells without asking anything (a song exported, a freeze or a bounce done, a problem saving, opening or
updating) shows in a card over the centre's bottom right corner, with a button for what to do next (Show in Folder),
and nothing stops meanwhile.

- News goes by itself once it has shown for 15 seconds while Phazite is in front and the pointer is not over it.
- A problem stays until it is closed.
- At most three show, the oldest news going first. The audio devices' notices show under them.
- A question that cannot wait, such as "Save changes?", is still a dialog.

## Panels and layouts

The panels are the left panel, the upper panel, the lower panel and the inspector.

- **Hide and show.** Each panel has a command whose name says what it does now ("Hide the Lower Panel", "Show the Lower
  Panel"), in the View menu, the Layout menu and the command search, and a switch in the title bar (the left panel's is
  also its activity bar icon). The lower panel's top right corner has Maximize, its own window and an X that hides it;
  the inspector has the same X at the right end of its title.
- **The centre always shows something.** Hiding the row that shows shows the other, and the upper panel stays while the
  lower one is in a window of its own.
- **Dividers.** A divider resizes the panels beside it. Dragged past half the least its panel shrinks to, it closes the
  panel; dragged back before the drag ends, the panel opens again. A closed panel opens later at the size it had. A
  double-click on a divider puts the panel beside it back at the size it starts at.
- **Maximized,** a panel fills the window between the transport bar and the status bar, one at a time. Its button, its
  command again or **Maximize the Panel in Use** (the panel with the keyboard focus) brings back the others as they
  were. Asking to see another panel (F5, F6, F7, F9, a double-click on a clip, Ctrl+F) restores them first.
- **A window of its own.** The left panel, the lower panel and the inspector can each move to their own window, on
  another monitor if there is one. Closing that window docks the panel again. A panel in its own window cannot be
  maximized, and comes back docked when Phazite opens.
- **Sizes are kept.** A panel hidden and shown again comes back at its size, and the window opens with its panels as
  they were left.
- **The window opens where it was closed,** at its size, maximized if it was, as long as a screen there still exists.
  One that would open off every screen opens at its first size (1280 by 800) on the main screen, and one larger than
  its screen, or partly off it, is fitted to it.
- **Nothing plays out of sight.** A sound the browser previews stops as the browser hides.
- **Full Screen** fills the screen with the window.
- **Layouts** arrange the panels in one step. Three ship, and they are layouts, not modes: *Beat* (the playlist, the
  piano roll and the browser), *Mix* (the mixer, the selected track's devices and the inspector) and *Full* (every panel).
- **Interface size.** Larger Interface and Smaller Interface scale everything from three quarters to twice the normal
  size.

| Key (Phazite's keymap) | Does |
| --- | --- |
| F5 | Shows the playlist |
| F9 | Shows the mixer |
| F7 | Shows the clip editor |
| F6, Shift+F6 | Shows the Instrument page, the Effects page |
| F8 | Opens the plug-in picker |
| Ctrl+K | Command search |
| Ctrl+F | Searches the browser |
| F10 | Opens the menus |
| Ctrl+Alt+B | Shows or hides the left panel |
| Ctrl+Alt+L | Shows or hides the lower panel |
| Ctrl+Alt+I | Shows or hides the inspector |
| Shift+F11 | Maximize the Panel in Use |
| F11 | Full Screen |
| Ctrl+=, Ctrl+-, Ctrl+0 | Larger, smaller, normal interface |

F5, F6, F7 and F9 show a view wherever it is. In the lower panel, F6, F7 and F9 hide the panel when it already shows
what they show. FL Studio's keymap shows and hides the left panel with Alt+F8 and maximizes the upper panel with F12.

## Design language

- **Typeface: Inter,** shipped as font files next to the app with its licence (SIL Open Font License 1.1), so that text
  takes the same room on every platform. Tempo, time, levels and meter readouts use its fixed-width digits, so values
  do not jitter as they change. Three weights (regular, medium, semibold); body text is 13 pixels, labels 12. If the
  font files are missing, Phazite uses the system font.
- **Colour:** every colour is a named token read from the theme, never a literal in a component. There are three
  themes, dark, light and high-contrast, and **Theme: Follow System** takes the dark or the light one as the operating
  system is set and changes with it. Tracks take their colours from an eight-colour palette that colour-blind users
  can tell apart. A track's header and every clip on it are in its colour.
- **The dark and light themes' look** is taken from VS Code's 2026 Dark and Light themes and its Modern UI layout: each
  panel a card with rounded corners, the activity bar joined to the left panel, and its title bar, status bar, tabs,
  buttons, fields, lists, menus, command search and tooltips. VS Code's name, logo and icons are not used. What was
  taken and where Phazite differs: [../vscode-look.md](../vscode-look.md). The high-contrast theme is Phazite's own.
- **Popups on frosted glass:** menus, tooltips and the lists that drop down (the command search, the pickers) open
  inside the main window and blur what is behind them under their own colour at 80 %. They are solid in the
  high-contrast theme, where the system is set to reduce transparency, and in Phazite's other windows. An editor in a
  call-out (a modulator's) stays opaque.
- **Menus where they are asked for:** a right-click's menu opens at the pointer (JUCE moves it where it would not fit),
  on a knob, a fader, a graph's handle, a tab or a device's name alike; a menu a button drops (a menu button, More, a
  picker, Record's arrow) hangs from its button. A choice that leads to more choices is a submenu that opens under the
  pointer, as Automate and Replace are in a device's menu, never an item that opens a second menu.
- **Switches in a column:** in a list of devices each switch is in the same place, whatever the name's length and
  whatever else the row shows (a mixer slot's reduction meter goes between its name and its switch); in a title, the
  switch comes first, before the name (a modulator's editor).
- **Spacing:** a 4-pixel grid, standard control heights and consistent padding.
- **One on/off switch:** whatever switches on and off has the track's switch, a round light filled green when on and a
  hollow ring when off: an effect, a modulator, an EQ band, a part of an instrument (which dims while it is off). A
  choice between modes stays a row of buttons.
- **Scroll bars over what they scroll:** the playlist's and the piano roll's rows have a thin, see-through scroll bar
  over their right edge, shown only while there is more than fits and while the pointer is over the editor; it fades
  out 1.5 seconds after the pointer leaves. An editor that scrolls itself does not show it. Faded out, it still takes
  clicks. Time lines scroll by their overviews ([playlist.md](playlist.md#grid-zoom-and-scrolling)).
- **Icons:** Lucide's line icons (ISC licence), drawn in the theme's colours, with Phazite's own drawn to match. Only
  the icons in use are copied in.
- **Logo:** Phazite's own drawing, a hexagonal frame shaded as three faces of a cube around a smaller cube. It keeps
  its colours in every theme. It is the app's icon, the helper program's icon, and sits at the title bar's left end.
- **Startup sound:** a short synthesized blip with echoes, about a second and a half, made as it plays (no sound files
  ship). It plays once the project has opened, at the browser's preview volume, and only through an audio output that
  opened. **Startup Sound** in the settings menu and the command search turns it off; it is on at first.
- **Motion:** none in places that redraw constantly.

## Decisions

- The whole song in the centre and the selection below: the lower part of the window is for the selection in every DAW
  compared, and a view of the selected track's devices in line is what users of the newer ones praise.
- The mixer is a view of the upper panel by default, beside the playlist: both show every track.
- Views move between the two rows by their tabs, so that nobody is held to the default places.
- Editors' tools sit in their area's tab row, not in the transport bar: the transport bar is the song's, and the tools
  act on one editor of two that can show at once.
- Automation has no tab: it is edited where it plays, under its track.
- The inspector shows properties, not a copy of a mixer channel.
- What follows the tempo is a setting shown where the tempo changes, not a dialog at each change; a tempo lane shows
  only while the song has tempo changes.
- Notices replace dialogs for anything that asks nothing.
- The dark and light themes copy VS Code's values under its MIT notice; its name, icons and Segoe UI are not taken.
- Popups open inside the main window, so that they can blur what is behind them.

## Not built

- Phazite's own title bar on Linux, and the bar beside macOS's window buttons: Planned.
- Tap tempo and tempo detection from audio: Planned, after version 1.
- Layouts saved by the user: Planned.
- Reading the system's reduced-motion setting: Planned.
- Design tokens in a file in the Design Tokens Community Group's format, generated into C++, and a gallery app that
  shows every control in every theme: Planned, roadmap phase A4.
- A declarative layout tool for dialogs: Declined unless dialogs become a bottleneck.

## In the code

- `src/ui/shell/AppShell.h`: the window's content and its panels.
- `src/ui/shell/TitleBar.h`, `TopBar.h`, `StatusBar.h`, `HintBar.h`, `ActivityBar.h`.
- `src/ui/shell/Panels.h`: the two rows of areas and the views that move between them.
- `src/ui/shell/PanelLayout.h`: which panels show, their sizes, the dividers and the lower panel's buttons.
- `src/ui/shell/TempoStretchPanel.h`, `src/ui/playlist/TempoLane.h`, `src/model/transport/TempoModel.h`,
  `src/engine/core/TempoMap.h`: the tempo.
- `src/ui/shell/Notices.h`, `NoticeCard.h`, `DeviceNotices.h`.
- `src/ui/project/ProjectView.h`, `src/ui/versions/VersionsView.h`, `src/ui/browser/Browser.h`: the left panel's views.
- `src/ui/mixer/Inspector.h`.
- `src/ui/components/ToolRow.h`, `PowerSwitch.h`, `ScrollBarFade.h`.
- `src/ui/theme/DesignTokens.h`, `LookAndFeel.h`, `Popups.h`, `Icons.h`; `resources/logo/phazite.svg`;
  `src/model/engine/StartupSound.h`.
- `src/services/Settings.h`: the keys that keep the theme, the panels' layout and the views' places.
