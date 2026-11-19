---
title: The window
description: The title bar, the transport bar, the left panel, the two areas of views, the inspector, the status bar, layouts, themes and interface size.
sidebar:
  order: 2
---

Phazite works in one window. The whole song is in the centre, the part of it you selected shows in detail under it, a
panel of views is on the left, and the properties of what is selected are on the right.

| Part | Where | What it holds |
|---|---|---|
| Title bar | Top | The menus, Undo and Redo, the command search, the panels' switches |
| Transport bar | Under the title bar | Play, Record, Loop, the metronome, the tempo, where the playhead is |
| Activity bar and left panel | Left | The Browser, Project and Versions views |
| Upper panel | Centre | The **Playlist** and the **Mixer** |
| Lower panel | Under the upper panel | The clip editor (the **Piano roll** or the **Audio clip** editor), **Instrument** and **Effects** |
| Inspector | Right | The properties of what is selected |
| Status bar | Bottom | Whether the project is saved, hints, the audio output, Low Latency, the CPU |

## The title bar

On the left are Phazite's logo, the menus (**File**, **Edit**, **Add**, **View** and **Help**) and the project's name,
with a dot after it while it has unsaved changes. In the middle are **Undo** and **Redo**, whose tooltips name the step
they take, and the command search. On the right are three switches, for the left panel, the lower panel and the
inspector, each lit while its panel shows, then the **Layout** menu and the window's buttons.

- **The command search** (**Ctrl+K**, or a click on **Search commands**) finds any command by name and runs it. Each
  row shows the command's keys. The arrow keys move through the list, **Enter** runs the command, **Escape** closes it.
- **F10** opens the menus, for the arrow keys to move through them.
- In a narrow window the menus fold into one button.
- The empty parts of the bar drag the window, and a double-click there maximizes it.

There is no Options menu: the settings are in the menu of the gear at the bottom of the activity bar (below).

## The transport bar

The transport bar holds what belongs to the song as a whole, in groups: playing and recording, the loop and the
metronome, the tempo, where the playhead is, and at the right the master and the computer keyboard.

| Control | Does |
|---|---|
| Play | Plays, or stops (**Space**). Stop goes back to where playback started, so you can hear a part again while you change it. **Shift+Space** stops where the playhead is, and plays on from there next time |
| Record, and its arrow | Records onto the armed tracks (**R**). The arrow, or a right-click, opens Record's options ([Recording](recording.md)) |
| Capture | Turns what you played in the last 30 seconds into a clip (**Ctrl+Shift+C**) |
| Loop | Turns looping on or off (**L**) ([The playlist](playlist.md#the-loop-and-the-playhead)) |
| Metronome, and its arrow | Clicks on every beat (**O**). The arrow, or a right-click, has the count-in |
| Tempo | Drag it, scroll it or type in it |
| Time signature | A menu |
| Place and time | Where the playhead is, in bars, beats and sixteenths ("17.3.2") and in minutes and seconds ("1:23.4"). Drag, scroll or type in either to move the playhead ("17.3", "1:05") |
| A/B | Opens the last version in place of the song, live, and goes back to Now at the next press (**N**) ([Versions](versions.md)). It shows once the song has a version to compare with; beside it, the bar says which version is open |
| Master volume and level | The master's volume and how loud it plays |
| Computer keyboard | Turns the typing keyboard into a piano (**M**) ([Keyboard shortcuts](keyboard.md#the-computer-keyboard-plays-notes)) |

The tempo field shows the tempo where the playhead is. As soon as you change it, a panel under it says how many of the
song's samples follow the tempo and whether its patterns do, with a switch for each: **Samples Follow Tempo** and
**Patterns Follow Tempo**. A sample that follows the tempo plays faster or slower with it, keeping its pitch; patterns
(MIDI clips) follow by default, their notes staying on their beats. To change the tempo along the song, use the tempo
lane ([The playlist](playlist.md#tempo-changes)).

## The left panel

The icons down the window's left edge (the activity bar) pick the left panel's view. A click on the icon of the view
that shows hides the panel.

- **Browser:** instruments, effects, plug-ins, presets and sounds ([The browser](browser.md)).
- **Project:** what the song holds, in three panes that fold.
  - **Notes:** your notes about the song, typed in place and saved with it.
  - **Audio Files:** each audio file the song plays, with the tracks that play it. A missing file shows in the
    warning colour with **Locate...** (or double-click it): the file you choose takes its place everywhere it played.
    While the project's folder holds audio files that nothing plays, **Clean Up...** offers to delete them. Files a
    version or Local History still plays are kept, so a file a version once held stays (Recover Lost Versions and Local
    History can still bring it back). While a version can't be read whole (another computer's files still syncing),
    Clean Up waits and says why.
  - **Plug-ins:** each plug-in in the song. A crashed one has **Reload**. A click shows the plug-in in the Devices view.
- **Versions:** the song's versions ([Versions](versions.md)).

**The gear** at the bottom of the activity bar opens a menu with **Search Commands**, **Settings**, **Audio Settings**,
**MIDI Settings**, **Keyboard Shortcuts**, **Scan for Plug-ins**, **Theme**, **Keymap**, **Startup Sound** and **Check
for Updates...**.

## The upper and lower panels

The centre has two rows of tabs. By default the upper panel shows the song as the **Playlist** or the **Mixer**, and
the lower panel shows what is selected: the clip editor, and the selected track's devices on two tabs, **Instrument**
and **Effects**.

The clip editor follows the selection. Its tab is called **Piano roll** while it shows MIDI clips and **Audio clip**
while it shows an audio clip; a double-click on a clip in the playlist shows it there. Automation has no tab: you edit
it in lanes under its track in the playlist ([Automation](automation.md)).

| Key | Shows |
|---|---|
| **F5** | The playlist |
| **F9** | The mixer, or the playlist again |
| **F7** | The clip editor |
| **F6**, **Shift+F6** | The Instrument tab, the Effects tab |

In the lower panel, **F6**, **F7** and **F9** hide the panel when it already shows what they show.

**Moving views.** Drag a tab onto the other panel's tabs to move the view there, onto an area's left or right quarter
to put it beside that area, or along its own row to change its place. A right-click on a tab offers **Move to the
Lower Panel** (or **Move to the Upper Panel**) and **Reset View Places**, which puts every view back where it started.
**View > Reset View Places** does the same. Phazite remembers where each view is.

**Each editor's tools** are in its tab row, right of the tabs after a thin line: the tools, **Snap**, the grid and
**Follow Playback**. Where the row is too narrow, the ones needed least go into a More menu (…) at its end. The tab that
shows is joined to its view, with a line along its top; the tab of the editor you used last also has a line along its
foot: the zoom, grid and tool commands act on that editor.

## The inspector

The inspector on the right shows the properties of what is selected, a name and a value on each row.

| Selected | Shows |
|---|---|
| Nothing | The song's tempo, time signature, key and length |
| A track | Its name, colour, volume, pan, mute and solo, output, instrument, effects, automation mode and sends |
| Clips | Name, start, length, mute and fades; for audio clips also gain and pitch |
| Notes | Start, length, pitch, velocity and mute |
| Automation points | The parameter, place, value and bend |
| A section | Name, colour, place and length |

Values are fields: drag, scroll or type in them. With several notes, clips or points selected, a value that differs
among them shows its range ("64 to 100"). Dragged or scrolled, each changes by as much; typed, each takes the value you
typed.

## The status bar

| Item | Shows | A click |
|---|---|---|
| Save state | "Saved", "Unsaved changes", "Not saved yet", or "Autosave failed" in the warning colour | Saves |
| Selection | What is selected: "Bass", "3 clips", "16 notes" | |
| MIDI | Which track live MIDI plays: "MIDI → Bass" | Opens the MIDI settings |
| Hint bar | What clicking, dragging and the modifier keys do under the pointer | |
| Audio output | Its sample rate and delay ("48 kHz · 10 ms") | Opens the audio settings |
| Low latency | Lit while it is on. While on, what you play has no delay: devices that look ahead are bypassed, delay compensation is off and the buffer shrinks. Turn it on to play or record, off to mix | Turns it on or off |
| CPU | How busy the engine is, in the warning colour near its limit | |

**The hint bar,** in the middle, always says what the control under the pointer does and how to work it. While you
drag something it says where it is now, such as "Starts at 17.3.1". **Hover Help**, in the Help menu, shows the same in
a tooltip when the pointer rests on a control.

**Notices.** What Phazite tells you without asking anything (a song exported, a problem saving) shows in a card at the
bottom right of the centre, with a button for what to do next. Nothing stops meanwhile. News goes by itself after 15
seconds while Phazite is in front; a problem stays until you close it.

## Panels and layouts

- **Show and hide.** The title bar's switches, the View menu and the Layout menu show and hide the left panel, the
  upper panel, the lower panel and the inspector (**Ctrl+Alt+B**, **Ctrl+Alt+L** and **Ctrl+Alt+I** for the left
  panel, the lower panel and the inspector). The lower panel's top right corner has **Maximize**, **Its own window**
  and **Hide**; the inspector has the same **Hide**.
- **Resize.** Drag the line between two panels. Dragged far enough, a panel closes; a closed panel opens later at the
  size it had. A double-click on the line puts the panel back at its first size.
- **Maximize.** A panel can fill the window between the transport bar and the status bar. **Maximize the Panel in
  Use** (**Shift+F11**) maximizes the panel you are working in, and again brings the others back.
- **Its own window.** The left panel, the lower panel and the inspector can each move to a window of their own, on a
  second monitor for example (**View > Panels**). Closing that window docks the panel again.
- **Layouts.** **Beat Layout** (the playlist, the piano roll and the browser), **Mix Layout** (the mixer, the selected
  track's devices and the inspector) and **Full Layout** (every panel) arrange the panels in one step. They are in the
  Layout menu and **View > Panels**.
- **Full Screen** (**F11**) fills the screen with the window.

Phazite keeps the panels' sizes and opens with them as you left them. The window opens where you closed it, at its
size, maximized if it was, unless the screen it was on is gone: then it opens on the main screen.

## Themes and interface size

**Theme**, in the gear's menu, has **Dark**, **Light**, **High Contrast** and **Follow System**, which takes the dark or
the light theme as your operating system is set and changes with it.

**View > Interface Size** makes everything larger or smaller, from three quarters of the normal size to twice it:
**Larger Interface** (**Ctrl+=**), **Smaller Interface** (**Ctrl+-**) and **Normal Interface Size** (**Ctrl+0**).

**Startup Sound,** in the gear's menu, turns off the short sound Phazite plays once it has opened.

## Not yet

- Layouts of your own: planned.
- Tap tempo, and finding a tempo from audio: planned.
