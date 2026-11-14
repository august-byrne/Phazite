# Commands, keys and help

*Design section 12.*

Every action in Phazite is a command with a name and a description. Menus, keys, right-click menus and the command
search all run the same commands, each as one named undo step, and the same descriptions are the help the hint bar
and the tooltips show.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Every action a command with an id, name, category, description and search words | Built | |
| Command search (Ctrl+K) | Built | |
| Menu bar: File, Edit, Add, View, Help | Built | |
| Settings in the menu of the gear at the activity bar's foot | Built | |
| Keymaps: Phazite, FL Studio, Ableton Live | Built | |
| Keymaps after other apps (Logic Pro, Pro Tools) | Planned | No roadmap step; their users get Phazite's keymap |
| Any command's keys changed in the shortcuts editor | Built | |
| Ctrl+Alt+click on a menu item to give it a key | Planned | No roadmap step |
| Commands that share a key, by what is selected | Built | |
| Text fields give the keys back on a press elsewhere | Built | |
| The computer keyboard plays notes | Built | |
| Cut, Copy and Paste | Partly built | Notes, clips and time ranges; not between projects, and not automation points, sections or tracks |
| Command macros (several commands as one) | Partly built | The registry runs one as one undo step; there is no way to make one in the app |
| Named undo steps, and the Undo History list | Built | |
| Hint bar and hover help | Built | |
| Lessons | Built | |
| A manual page for the control under the pointer | Planned | No roadmap step |
| Commands with parameters and results, for scripts and an assistant | Planned | After v1 ([roadmap](../roadmap.md#after-10)) |

## Commands

A command has:

| Part | Use |
| --- | --- |
| id | Stable, `category.action` (`edit.split`); keymaps and saved key changes name commands by it |
| name | Shown in menus and the command search, and the undo step's name |
| category | "Transport", "Edit", "View"... |
| description | One sentence, shown by the hint bar, tooltips, the command search and the shortcuts editor |
| synonyms | Other words the search matches: searching "cut" finds Split |

A command can also say whether it is enabled or ticked now, what its name is now ("Undo Change Volume"), whether its
key repeats while held, and whether its key works while a plug-in's window has the keys (play and stop do).

The registry itself has no window code: it lists the commands, runs each inside its named undo step, says whether
one is enabled or ticked, and searches them. The window connects it to menus and keys
([architecture.md](../architecture.md)).

## The command search

Ctrl+K (**Search Commands**), or the search field in the title bar, opens a search over every command. Typing
narrows the list, best matches first; each row shows the command's shortcut. The arrow keys move through the list,
Enter runs the selected command, and Esc or a click elsewhere closes it.

## Menus

The menu bar has five menus. There is no Track, Song or Transport menu: their commands are in Add, Edit and View,
on the transport bar, on the keys, in right-click menus and in the command search. Play and stop are on the
transport bar and the keys.

| Menu | Holds |
| --- | --- |
| File | New, Open, Import DAWproject, Open Recent; Save, Save As, Auto Save, Commit; Export Song, Export as DAWproject; Clean Up Unused Audio; Quit |
| Edit | Undo and Redo; Cut, Copy, Paste; Delete, Duplicate, Duplicate as Alias, Rename; Select All, Deselect All; Quantize, Consolidate; **Notes** (the piano roll's edits of the selected notes); **Tracks** (colour, mute, solo, folders, order); Insert Time, Remove Time |
| Add | A track, a folder, a return track; a section; **Song Outline**; Chord Progressions; a tempo change |
| View | Showing and hiding the left panel, the song, the editor and the inspector; the browser, the project and the versions; **Panels** (maximize, detach, the layouts, focus mode); full screen; the piano roll and the mixer; **Zoom**; **Playback** (the start and end, centring on and following the playhead, scrolling continuously, the loop); **Grid**; **Tracks** (heights, collapsing, pinning); **Interface Size** |
| Help | Search Commands, Keyboard Shortcuts, Hover Help, Getting Started Tips; **Lessons**; **Example Songs**; Phazite Sounds, Welcome; Check for Updates, About |

There is no Options menu. The settings' windows (Settings, the audio and MIDI settings, Keyboard Shortcuts, Scan for
Plug-ins), **Theme**, **Keymap**, the startup sound and the update check are in the menu of the gear at the activity
bar's foot ([window.md](window.md)), and in the command search. The computer keyboard's switch is on the transport
bar.

## Keymaps

A keymap is a data file that gives keys to commands by their ids, and says how the playlist's mouse works. Three ship
with Phazite; the welcome window's first page chooses one ([getting-started.md](getting-started.md)), and the gear's
**Keymap** menu changes it later.

| Keymap | For | Mouse in the playlist |
| --- | --- | --- |
| Phazite (the default) | Conventions most music apps and the operating system share, with single-key tools | Right-click opens menus |
| FL Studio | FL Studio's keys, from its manual, for the commands Phazite has | Right-click deletes; a click on empty space places a clip; a selected time range loops while it is selected |
| Ableton Live | Live 12's keys, from its manual, for the commands Phazite has | Right-click opens menus |

Where the three differ most ("Ctrl" is Cmd on macOS):

| Command | Phazite | FL Studio | Ableton Live |
| --- | --- | --- | --- |
| Play or stop | Space | Space | Space |
| Play on from where it stopped | Shift+Space | Ctrl+Space | Shift+Space |
| Record | R | R | F9 |
| Metronome | O | Ctrl+M | O |
| Undo | Ctrl+Z | Ctrl+Alt+Z; Ctrl+Z undoes or redoes the last change | Ctrl+Z |
| Redo | Ctrl+Shift+Z, Ctrl+Y | Ctrl+Z | Ctrl+Y, Ctrl+Shift+Z |
| Cut, Copy, Paste | Ctrl+X, C, V | Ctrl+X, C, V | Ctrl+X, C, V |
| Duplicate | Ctrl+D | Ctrl+B | Ctrl+D |
| Split | Ctrl+E | Insert | Ctrl+E |
| Quantize | Ctrl+U | Ctrl+Alt+Q, Shift+Q | Ctrl+U |
| Rename | Ctrl+R, F2 | F2 | Ctrl+R |
| Mute the selected clips | 0 | Alt+M | 0 |
| Export | Ctrl+Shift+R | Ctrl+R | Ctrl+Shift+R |
| Search Commands | Ctrl+K | Ctrl+K | Ctrl+K |
| Mixer | F9 | F9 | Ctrl+Alt+M |
| Piano roll | F7 | F7 | Ctrl+Alt+3 |
| Add Instrument or Effect | F8 | F8 | F8 |
| Computer keyboard on and off | M | Ctrl+T | M |

More of each:

- **Phazite's** has FL Studio's single-key tools (Esc, P, B, C, S, T, D, E, Z) and the digits 1 to 9 for the same
  tools ([playlist.md](playlist.md)); Ctrl+1 to 5 set the grid; F10 opens the menus.
- **FL Studio's** also has: Ctrl+Insert and Ctrl+Delete insert and remove time; 9 reverses audio clips; Alt+Up and
  Alt+Down move tracks; Ctrl+Alt+C bounces in place; Backspace turns snap off and on; F10 opens the MIDI settings.
- **Ableton Live's** also has: Ctrl+L loops the selected time; R reverses the selected clips; S solos and C arms the
  selected tracks; U collapses them; A shows automation; Ctrl+B bounces in place; Ctrl+Alt+Shift+F freezes;
  Ctrl+Alt+4 shows the instrument; W zooms to fit; Shift+/ turns hover help on and off; Ctrl+Up and Ctrl+Down step
  through takes. Phazite's tools have no keys in Live, so B is the Draw tool and E the Slice tool, with Esc and the
  digits 1 to 9 as in Phazite's keymap. Capture has no key. Tab stays for moving the keyboard focus, so the mixer is
  Ctrl+Alt+M.

Rules for every keymap:

- Every keymap gives a key to the core actions: play and stop, record, the metronome, undo and redo, cut, copy,
  paste, delete, duplicate, select all, split, quantize, rename, open, save, save as, export, the command search,
  zoom, the mixer and the piano roll, the Smart and Draw tools, moving notes, and the computer keyboard. A test checks
  it, and that every command a keymap names exists.
- A keymap must not give Shift and a letter a different command than the letter alone (see the computer keyboard,
  below).
- Default keys belong in the keymap files, not in code.

## Changing keys

The settings' **Shortcuts** tab (Help > Keyboard Shortcuts) lists every command with its keys and changes them. Each
keymap keeps its own changes, saved in the settings by the command's id.

## Keys that depend on what is selected

Several commands can share a key; the enabled one with the highest priority runs. Commands for selected notes,
selected automation points and selected devices have the higher priority:

- with notes selected, the arrow keys move the notes; with automation points selected, they move the points;
  otherwise Up and Down select tracks and Left and Right nudge clips;
- Ctrl+G groups the selected devices into a container, or else the selected tracks into a folder;
- Ctrl+E chops the selected notes, or else splits clips.

A held key runs its command once. Commands that take a step (nudging, zooming, transposing, moving through tracks,
undo) repeat while the key is held. The time line's commands (zoom, grid, tools, nudge, quantize) act on the editor
used last.

## Text fields let the keys go

- A press anywhere outside a text field gives the keys back to the shortcuts, so that Space plays again, and the
  field shows no selection once the keys have gone.
- A knob or a fader that has the keys lets them go the same way.
- A press in a menu over the field (its own Cut, Copy and Paste) is the field's, and a press outside a dialog does
  nothing to the dialog's fields.
- Enter and Escape in the inspector's names let the keys go too, after the name changes.
- Showing a view never puts the cursor in a field: the browser's icon gives the browser the keys, not its search
  (Ctrl+F or a click does that), and the Source Control icon gives the view the keys, not the commit's message.

## The computer keyboard plays notes

**Play Notes with the Computer Keyboard** turns the typing keyboard into a MIDI keyboard, which plays the selected
track's instrument as a MIDI keyboard does. It starts off, so that the tools' letters work; a button on the transport
bar and M (Ctrl+T in FL Studio's keymap) turn it on, and Phazite remembers the choice.

| Keys | What they do |
| --- | --- |
| A S D F G H J K L ; | The white keys, from middle C |
| W E T Y U O P | The black keys |
| Z, X | An octave down, up |
| C, V | Velocity down, up, in steps of 20 (from 100) |

- It takes only keys pressed without Shift, Ctrl, Alt or Cmd, and never keys typed into a text field.
- **Shift and a letter run that letter's single-key shortcut** while the computer keyboard plays notes (the tools'
  letters, for example). With the computer keyboard off, the letters alone run them. The digits 1 to 9 pick the tools
  either way, in the keymaps that have them.
- Play, stop and the computer keyboard also work while a plug-in's window has the keys.

## Cut, Copy and Paste

Ctrl+X, Ctrl+C and Ctrl+V in every keymap, in the Edit menu, and in the clips', notes' and tracks' right-click menus.

- **Copy** takes what Delete would delete, in Delete's order: the selected notes; or the selected time range, its
  clips cut at its edges; or the selected clips; with the automation under the clips or in the range.
- **Cut** copies and then deletes as Delete does.
- **Paste** puts clips at the playhead, from the selected track down (else the first selected clip's track, else
  where they came from), each in place of what lies under it, so that nothing plays twice. They are copies, not
  aliases, never on a return or a folder; clips from more tracks than there are from the selected one down get empty
  tracks added after the last, as REAPER, Studio One and Logic Pro add them. The automation comes with them when they
  go back onto their own tracks
  ([automation-and-modulation.md](automation-and-modulation.md)).
- **Notes** go into the clip the piano roll shows at the playhead, or, with none of them there, into its first clip
  as far in as they were in theirs, so that a part copies from one pattern to another.
- What Paste puts in is selected (a range's becomes the range), as one undo step ("Paste 2 Clips").
- While stopped, the playhead is the play start, set by a click in a ruler.
- The clipboard belongs to the open project and empties when another opens, since clips name their files by paths
  relative to their project.

## Named undo

- Every command that changes the project is one named undo step ("Split", "Apply Outline: Pop"), and so is every
  edit the window asks the model for. A drag is one step, opened when it starts and kept when it ends.
- The window never starts an undo step itself ([architecture.md](../architecture.md)).
- **Undo History,** at the foot of the Source Control view ([versions.md](versions.md)), lists the song's undo
  steps; a click goes back or forth to one.
- In FL Studio's keymap Ctrl+Z is **Undo or Redo**, which undoes the last change or redoes it, and Ctrl+Alt+Z undoes
  step by step.

## Help

- **The hint bar,** in the status bar, is always there: it says what the control under the pointer does, and how to
  work it.
- **Hover Help** (on at first; Help > Hover Help turns it off and on) shows the same in a tooltip when the pointer
  rests on a control.
- **Every command has help text:** a name, a category and a sentence that says what it does, which the menus' hints,
  the command search and the shortcuts editor show. A test checks every command the window registers.
- **Every control explains itself,** with a tooltip or a description. A test walks every visible control of the
  window, the mixer, the piano roll and the Instrument and Effects pages. A device's knob, choice or switch says what
  it ranges over and where it starts, and how to work it: drag it, click it and type a value, double-click it for
  where it started, or right-click it to automate or modulate it.
- **The guide** in the hint bar suggests a newcomer's next step ([getting-started.md](getting-started.md)).

### Lessons

Help > Lessons, and the welcome window's last page, start one of five short lessons for a project that starts empty:

| Lesson | Steps | What it teaches |
| --- | --- | --- |
| Make a Beat | 5 | A drum sampler, hits clicked into the piano roll, played and repeated |
| Record Audio | 4 | An input, the metronome, a take, and punching in |
| Arrange a Song | 4 | An outline's sections, repeating one, exporting the song |
| Add Effects | 4 | An effect in the mixer, and automating one of its knobs |
| Write Chords | 4 | A progression on the chord lane, and a part written over it |

- A lesson shows in a card over the bottom right corner of the window's centre: the lesson's name, a dot for each
  step, the step in large text, **Back**, **Next** (**Skip** while the step isn't done, **Done** at the end), and a
  button that stops it.
- A ring in the accent colour surrounds the control the step is about. It pulses a few times as the step starts and
  then stays still.
- Steps name the keys of the keymap in use, or say to search for a command that has no key.
- A step goes on when it is done, in any way: its command runs, however it is run, or the song shows it (an
  instrument on a track, a track armed). Only a step that nothing can see done waits for Next.
- The card covers nothing but its corner, and everything goes on working while it shows.
- While a lesson runs, the guide's tips wait. A finished lesson is marked "(done)" in Help > Lessons from then on.
  **Stop Lesson**, or the card's close button, ends one.

## Decisions

- **No Track, Song, Transport or Options menu.** Five menus; settings live at the gear.
- **Space stops back at the play start,** where playback last started, so a sound can be replayed while it is
  tweaked; Shift+Space (Ctrl+Space in FL Studio's keymap) plays on from where it stopped
  ([window.md](window.md)).
- **The computer keyboard starts off,** so that the tools' single letters work in a new install.
- **Shift and a letter run the letter's shortcut** while the computer keyboard plays notes; so no keymap may give
  Shift and a letter a command of its own that differs from the letter's.
- **No keymap uses Tab:** it moves the keyboard focus.
- **Key changes are saved by command id,** not by number, so they survive commands being added.
- **Paste goes to the playhead** and replaces what lies under it, as a duplicated range does.
- **The clipboard is the project's,** not the app's.
- **Lessons show beside the work,** in a card that takes over nothing, with a mark on the real control.
- **Three keymaps for v1:** Phazite's, FL Studio's and Ableton Live's.

## Not built

- Ctrl+Alt+click on a menu item to give its command a key: Planned. The shortcuts editor changes keys meanwhile.
- Making command macros in the app, and giving one a key: Planned. The registry can already run a list of commands
  as one undo step named after the macro.
- Commands that take parameters and return results, for scripts and an assistant: Planned, after v1.
- A manual page for the control under the pointer: Planned.
- Copying between projects, and Cut, Copy and Paste for automation points, sections and tracks (Duplicate copies
  those): Planned.
- Keymaps after other apps than FL Studio and Ableton Live: Planned, not for v1.
- Settling the default keys by watching new users do set tasks and counting where they hesitate: Planned, after v1
  (the roadmap's usability rounds).

## In the code

- `src/model/commands/CommandRegistry.h`: the commands, their search, and `addMacro()`;
  `src/model/commands/internal/Undo.h`: undo steps, gestures and `undo()` / `redo()`;
  `src/model/commands/SongGesture.h`: a drag's one step, as the window's tools hold it;
  `src/model/commands/UndoHistory.h`: the history as the Source Control view lists it.
- `src/ui/input/CommandBridge.h`, `KeyDispatcher.h`, `CommandUI.h`: the commands as menus and keys reach them, the
  keys' priorities, and menu items that show shortcuts.
- `src/ui/input/Keymap.h`, `KeymapStore.h`; `resources/keymaps/phazite.json`, `fl_studio.json`, `ableton_live.json`.
- `src/ui/input/ComputerKeyboard.h`, `WindowKeys.h`, `PluginKeys.h`, `TextFieldRelease.h`.
- `src/model/arrangement/internal/Clipboard.h`: Cut, Copy and Paste.
- `src/app/MainWindow.cpp`: the menu bar (`getMenuForIndex()`); `src/ui/shell/ActivityBar.h`: the gear's menu.
- `src/ui/shell/CommandPalette.h`, `SettingsPanel.h` (the Shortcuts tab), `HintBar.h`, `Lessons.h`, `LessonCard.h`.
- `tests/golden/commands/app-catalogue.txt`: every command the app registers, in order, with its keys.
- Tests: `tests/CommandTests.cpp`, `CommandGoldenTests.cpp`, `ShortcutTests.cpp`, `WindowKeysTests.cpp`,
  `ComputerKeyboardTests.cpp`, `TextFieldReleaseTests.cpp`, `ClipboardTests.cpp`, `LessonsTests.cpp`.
