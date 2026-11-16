# src/ui

The window's components, in the `phazite_ui` static library, which the app and the tests link. There is one folder per
area, and each uses only the folders listed before it here (and `src/model/` and `src/services/`): `theme/`,
`components/`, `input/`, `devices/`, `timeline/`, `playlist/`, `pianoroll/`, `audioclip/`, `mixer/`, `browser/`,
`project/`, `versions/`, `shell/`. The UI reaches the song only through the model's functions, by IDs
([docs/architecture.md](../../docs/architecture.md)).

File names ending in `.*` stand for a header and its `.cpp`.

## theme/

| File | What it holds |
|---|---|
| `LookAndFeel.*` | `ui::LookAndFeel`, and `ui::setThemeColour()` and `ui::refreshTheme()`, which set the palette's colours again on a theme switch. |
| `DesignTokens.h` | The design tokens: colours, text sizes, spacing. Read with `ui::getPalette (component)`. |
| `Icons.*` | The icons (`ui::Icon`, shown with `ui::IconButton`) and Phazite's logo, its own drawing in its own colours (`createLogo()`). The icons are Lucide's (ISC; `resources/icons/lucide/LICENSE`). |
| `Popups.*` | What pops up over the window: menus, tooltips and the pickers. Inside the main window they show on frosted glass, blurred by Phazite itself; elsewhere they are solid. |

The dark and light themes are VS Code's 2026 Dark and Light, and the sizes those of its Modern UI, under its MIT notice;
[docs/vscode-look.md](../../docs/vscode-look.md) says where each comes from.

## components/

The base controls and what the left panel's views share.

| File | What it holds |
|---|---|
| `Controls.h` | The base controls: `ui::Knob`, with the ring of its modulation; `ui::Fader`, with the scale of its decibels beside it in the mixer's strips; `ui::FaderScale`; `ui::LevelMeter`, on that scale beside a fader. Knobs and faders are `ui::ValueSlider`s, which take a value typed in a field over them or their readout. `ui::ParameterAttachment` links a control to a `phazite::ParameterHandle`. |
| `ModulatorDrag.h` | What a modulator dragged onto a knob carries. |
| `ValueText.h` | How values read as text, and how typed text reads as values: decibels, pan, a parameter's units. |
| `ParameterText.*` | A parameter's value as text, read back as typed. |
| `TypedField.*` | The field a value is typed in, over a control or an automation point. |
| `ArmButton.*`, `TrackSwitch.*` | A track's arm switch, and its one switch for mute and solo, in its header and its mixer strip. |
| `PeakBar.*` | The strip down a track header's right edge that shows how loud the track plays now. |
| `PowerSwitch.*` | The one on/off switch, a track's look, for whatever switches on and off. |
| `ScrollBarFade.*` | Shows an overlay scroll bar while the pointer is over what it scrolls, and fades it out after. |
| `DisplayTicker.*` | The one timer that brings the displays up to date (meters, device editors), with what tells them whether to repaint (`ui::DrawnNumbers`). |
| `ToolRow.*` | A row of a view's controls laid out from the left, which puts the least needed into its More menu where it is too narrow; each editor's tools show in its area's tab row this way. |
| `ViewTitle.*` | The title along a left-panel view's top, with its actions (as VS Code's side bar). |
| `SectionHeader.*` | A section's header that folds it: a group's row or a pane's, with its count's badge. |
| `ListRowHover.h` | Which row of a list is under the pointer. |

## input/

| File | What it holds |
|---|---|
| `CommandBridge.*` | The commands as JUCE's menus and key mappings see them, and the user's changes to their keys. |
| `KeyDispatcher.*` | The keys that run commands: priorities, held keys, the Shift rule, keys over plug-in windows, keys on other layouts. |
| `KeyShares.*` | What else a key does, as the shortcuts editor says it: the other commands it runs, which runs when, the computer keyboard's notes. |
| `KeyboardLayout.*` | The keys by their places, and what the layout in use types at each (JUCE workarounds W47, W48). |
| `Keymap.*` | The keymaps. |
| `KeymapStore.*` | The user's changes to the keymaps, kept in the settings. |
| `CommandUI.*` | Menus and tooltips that show commands with their shortcuts. |
| `LibraryDrag.*` | What a dragged browser item carries, or a track dragged into the browser. |
| `ComputerKeyboard.*` | The computer keyboard as a MIDI keyboard, by the keys' places. |
| `WindowKeys.*` | The main window's keys: the computer keyboard is asked before the shortcuts. |
| `PluginKeys.*` | Lets play, stop and the computer keyboard work over plug-in windows. |
| `TextFieldRelease.*` | Gives the keys back to the shortcuts when a press lands outside the text field that has them. |

## devices/

The menus that add instruments and effects, and the editors of Phazite's own devices, which the Devices view shows.

| File | What it holds |
|---|---|
| `DeviceMenus.*` | The menus that add instruments and effects. |
| `DeviceEditors.*` | The editor of each of Phazite's own devices: its own, or, for one without, a knob for each of its parameters. They work through the model's `DeviceHandle` and `ParameterHandle`s. |
| `DeviceEditorBase.*`, `DeviceLayout.*` | The frame every editor is built on: columns of parts that stack, show under tabs or narrow, and pages of them where the dock is narrower, so that every editor fits the default dock without scrolling. `DeviceEditorBase::setModes()` gives an editor modes, a page of parts for each, as tabs along its top (a choice's tabs, `ChoiceTabs`). |
| `DeviceWindowContent.*` | An editor in a window of its own, out of the Devices view (`src/app/PluginWindow.h`): as wide as the window, its parts wrapping into rows, scrolling when the window is shorter. |
| `ParameterControls.*` | A parameter's controls; `ParameterControls::showParameters()` shows a set of them. |
| `DeviceGraph.*` | Graphs' axes, spectra and histories, and how every graph takes a handle (`HandleDrag`: from its value set by hand, as far as the pointer goes, Escape taking the drag back) and turns one with the wheel (`WheelGesture`). |
| `LevelHistoryView.*` | A dynamics device's last two bars at the song's tempo, its key on the same time line, over the beats and bars. |
| `SidechainMenus.*` | What a device's sidechain listens to, a track or a bus, picked inside the device under the word every DAW prints on its picker, Sidechain. |
| `GraphDot.*` | What a graph shows under the pointer of what a drag there moves: the pointer, the handle a press takes, lit, and a dot that a drag moves (through `HandleDrag`) and a press away from it puts under the pointer. |
| `SynthParts.*` | The synths' envelope, LFO and filter views. |
| `SweepPicture.*` | The picture the phaser, the flanger and the chorus share, with their LFO. |
| `DeviceTint.*` | Each device's colour. |
| `DynamicsCurve.*` | A dynamics device's curve (`src/devices/dynamics/DynamicsCurve.h`) as the multiband's bands and the compressor show and edit it. |
| `ModulatorEditor.*` | Modulators' editors. |

The editors, one pair of files each: `ChorusEditor`, `CompressorEditor`, `DelayEditor`, `DrumSamplerEditor`, `EqEditor`,
`ExciterEditor`, `FilterEditor`, `FlangerEditor`, `FmEditor`, `LimiterEditor`, `MultibandEditor`, `PhaserEditor`,
`ReverbEditor`, `SamplerEditor`, `SaturatorEditor`, `StereoExpanderEditor`, `SynthEditor`, `VinylEditor`,
`VocoderEditor`, `WavetableEditor`. Among them:

| Editor | What it shows |
|---|---|
| `DrumSamplerEditor.*` | Sixteen of the drum sampler's 128 pads beside a map of them all, the selected pad's sound with its envelope's handles, and its controls, which follow the pad selected (with `ParameterControls::showParameters()`). |
| `SamplerEditor.*` | The sampler's sound drawn with its markers, loop and slices; a sound dropped on it loads. |
| `FmEditor.*` | The FM synth's algorithm drawn large with the harmonics its operators make, and the four operators side by side, a group of their controls at a time, with its filter's envelope beside its filter. |
| `ExciterEditor.*` | What the exciter adds, over the spectrum, with its lift and a meter of how far its band reaches into its curve. |
| `VinylEditor.*` | A record turning off its centre as far as the vinyl's wow swings it, with a speck where each crackle came, its pitch over the last seconds, and its band's ends to drag. |
| `MultibandEditor.*` | The multiband's parts on one page in the dock: the bands across the spectrum, their crossovers and levels dragged there, always with their knobs, and the band shown by its tab, its curve (`DynamicsCurve.*`) and controls, with Monitor and its Sidechain part beside it, or in its place by a switch and a tab where the dock is narrow. |
| `CompressorEditor.*` | Two tabs along its top that are its modes, each on one page in the dock: Compress, its characters and curve beside its history over its knobs; and Duck, its sidechain over its cuts and Listen beside its history, its graph (its threshold a line to drag) over its knobs. |

## timeline/

What the playlist shares with the piano roll and the audio clip editor.

| File | What it holds |
|---|---|
| `TimelineEditor.*` | The time line, grid and tools of an editor. The tools show in the tab row of the area that shows the editor (`getToolRow()`), and where a drag has what it moves, which the hint bar shows (`getDragReadout()`). Each part that draws the time line is added with `addTimelinePart()` and placed with `placeTimelinePart()`: it shows through a window on whole pixels, which clips it there while continuous scrolling moves it by parts of a pixel. What stays in view as the song scrolls (a clip's tag) is placed in `getShownTimeline()`. The view always starts on a whole pixel. `addTimelineCommands()` registers the time line's commands, which act on the editor used last. |
| `SmartTool.*` | The tools: the Smart Tool, the default, whose drag does what the place it starts on says, and the others, each doing one thing, picked with FL Studio's letters (Esc returns to the Smart Tool). |
| `SongPlaceText.*` | Places and lengths as text: "17.3.2" and "2 bars 1.5 beats". |
| `TimelineView.*` | What an editor shows of the time line: the first beat in view and the zoom, and the column a line or an edge at a beat is drawn in, rounded from the beat's own place so that it moves with the song (`beatToColumn()`); a line that changes along the song, a lane's, goes through `getLineSamples()`. |
| `TimelineGesture.*` | What the editors' tools share while the mouse is down: when a press becomes a drag, the drag's one undo step, and what it has done already. |
| `TimelineOverview.cpp` | The whole time line in miniature along the time bar's top, which is the editor's horizontal scroll bar, with the editor's marks. |
| `FrameLog.*` | What the display's frames did while the song played, written to Phazite's log as it stops. |

## playlist/

The playlist, which reaches the song by IDs through the model's `src/model/arrangement/PlaylistModel.h`.

| File | What it holds |
|---|---|
| `Playlist.*` | The playlist. |
| `PlaylistParts.h` | Its parts, declared. |
| `PlaylistRows.cpp` | The tracks' rows and their clips. |
| `PlaylistTools.cpp` | The tools. |
| `PlaylistDrops.cpp` | What is dropped on it. |
| `PlaylistAutomation.cpp` | The automation lanes under the tracks. |
| `PlaylistTracks.cpp` | The track headers' clicks, drags and menus, with the playlist's commands. |
| `PlaylistRecording.cpp` | What is being recorded: a clip growing on each track as it comes in. |
| `TrackHeader.*` | A track's header. |
| `ClipComponent.*` | A clip, with an audio clip's waveform, which the app makes (`ClipWaveform.*`). |
| `ClipWaveform.*` | An audio clip's waveform as the playlist and the audio clip editor draw it; the app makes them (`src/app/ClipWaveforms.*`). |
| `SectionsStrip.*` | The song's sections in a thin band of the time bar above the bar numbers, each dragged with everything in it. |
| `TempoLane.*` | The tempo lane under the bar numbers while the song's tempo changes: its points dragged, its lines ramped and bent, with its header. |
| `ChordLane.*` | The chord lane under the tempo lane, with its header. |
| `TrackHeights.*` | How tall tracks are shown, and the taller and shorter commands. |

## pianoroll/

The piano roll, which reaches the notes by their IDs through the model's `src/model/notes/NotesModel.h`.

| File | What it holds |
|---|---|
| `PianoRoll.*` | The piano roll. |
| `PianoRollParts.h` | Its parts, declared. |
| `PianoRollRows.cpp` | The rows. |
| `PianoRollCommands.cpp` | The transforms, the toolbar's menus and the commands. |
| `PianoRollNotes.cpp` | The note grid. |
| `PianoRollNoteTools.cpp` | The note grid's tools. |
| `PianoRollLanes.cpp` | The keys, the velocity lane and the transform bar. |
| `DrumRows.*` | Which rows a drum track shows, in what order. |

## audioclip/

| File | What it holds |
|---|---|
| `AudioClipEditor.*` | One audio clip in the song's bars, by its ID through the model's `src/model/arrangement/AudioClipModel.h`: its edges and fades as handles on its audio, its warp markers and the hits in its audio along its top, and its gain, pitch, reverse and tempo down the left. |

## mixer/

The mixer, the Devices view and the inspector.

| File | What it holds |
|---|---|
| `MixerView.*` | The mixer. |
| `ChannelStrip.*` | A channel's strip. |
| `DeviceSlots.*` | A strip's device slots. |
| `StripParts.*` | What strips share. |
| `MasterStrip.*` | The master's strip. |
| `DevicesView.*` | The Devices view, which the Instrument and Effects tabs show as two pages. Down its left, a column of the selected track's instrument (its name, Presets and window button) or of its effects one under another, as FL Studio's mixer stacks its slots, a container one line among them; beside it, the device clicked, as wide as the rest of the dock. |
| `ModulatorSlots.*` | A track's modulators and macros as chips in the Devices view, whose editors pop out over the window. |
| `ContainerEditor.*` | A container in the Devices view: its macros beside its chains side by side, each a card with its devices, or beside the device of it clicked there, under the path back to them. |
| `Inspector.*` | The properties of what is selected: a track's, clips', a section's or the song's. |

## browser/

| File | What it holds |
|---|---|
| `Browser.*` | The browser. It also saves presets and shows them, swaps a pad's sound or a device's preset in place (Swap) with its strip over the search, and names collections in a field over its tree. |
| `FilterBar.*` | The filters under its search. |
| `SoundView.*` | What a sound selected in the browser holds, under its tree: its waveform, whether it is a loop (at what tempo, how many bars) or a one-shot, how long it lasts, and its format. |
| `PluginPicker.*` | The plug-in picker (F8). |

## project/

| File | What it holds |
|---|---|
| `ProjectView.*` | What the project holds, in the left panel, through the model's `src/model/project/ProjectContents.h`: the song's notes, the audio files it plays (a missing one to locate, those nothing plays to clean up), and its plug-ins (a crashed one to reload). |

## versions/

The project's versions ([docs/design/versions.md](../../docs/design/versions.md)).

| File | What it holds |
|---|---|
| `VersionsView.*` | The Source Control view in the left panel. The branch, with its menu, or a commit checked out on its own, with Back to main; a version saved with its name and note; the changes since the last one, by track (or a merge's lines, ticked, then its clashes, each switched between its three values); the history of every branch's versions, each on its branch's line, a commit opened by a click to show what it changed, each line restorable from it. Then Local History, the song as it was while it differed from its last commit; then Undo History, the song's undo steps, a click going back or forth to one. |
| `BranchPicker.*` | Source control's quick inputs: Checkout to..., Rename Branch..., Merge Branch..., Delete Branch... and Edit Commit Message.... |

## shell/

The window's content and everything around the views.

| File | What it holds |
|---|---|
| `AppShell.*` | The window's content. |
| `ActivityBar.*` | The activity bar down the window's left edge, whose icons show the left panel's views: the browser, the project and the versions. |
| `TitleBar.*` | The title bar: Phazite's logo, the menus, Undo and Redo beside the command search, the panels' switches and, on Windows, where it is the window's own, the window's buttons and what its frame does where. |
| `TopBar.*` | The transport bar, with the playhead's place and time as fields that move it, and A/B, which hears the last commit in place of the song and back. |
| `TempoStretchPanel.*` | What a tempo change stretches, under the transport bar's tempo field while the tempo changes. |
| `StatusBar.*` | The status bar: the save state, the audio output, the CPU, and between them the hint bar. |
| `HintBar.*` | The hint bar, which shows a drag's readout while one moves something, and otherwise the guide's next step. |
| `Guide.*` | The guide whose next step the hint bar shows. |
| `Lessons.*` | The lessons of Help > Lessons. |
| `LessonCard.*` | The card that shows a lesson's step, with a ring around the control it is about. |
| `DeviceNotices.*` | The notices of audio devices plugged in, each offering to switch to its device. |
| `NoticeCard.*` | The card the lessons and the device notices show in, in the bottom right corner, where VS Code shows its notifications. |
| `Notices.*` | What Phazite tells without asking anything (a song exported, a problem saving), in cards over the others, each with a button for what to do next, in place of dialogs. |
| `CommandPalette.*` | The command palette. |
| `Panels.*` | The two areas of views, one over the other: by default the Playlist and Mixer tabs over the clip editor's tab (which shows the piano roll or the audio clip editor as the selection asks) and the Instrument and Effects tabs (the Devices view's two pages). Each view moves between the areas by its tab, and the settings keep where. |
| `PanelLayout.*` | Which panels show, which one fills the window and their sizes, with the buttons in each panel's corner. |
| `SettingsPanel.*` | The settings window's tabs. |
| `ShortcutsEditor.*` | The Shortcuts tab's list, as VS Code's Keyboard Shortcuts editor: search by name or key, the keys of every command, changed in a card that says what else a key does. |
| `Welcome.*` | The welcome window. |
| `NewProjectPanel.*` | The new-project window. |
| `ExportPanel.*` | The export window, for the song or its stems. |
| `RenderPanel.*` | The window a freeze or a bounce shows while it renders. |
| `CleanUpPanel.*` | The window of Clean Up Unused Audio. |
| `ProgressionsPanel.*` | The Chord Progressions window. |
| `AboutPanel.*` | The About window: Phazite's version, copyright and the notice its licence asks for, and every licence it ships, from the model's `src/model/engine/AppInfo.h`. |
| `UpdatePanel.*` | The Updates window, with what the menus and the settings' icon say of an update (`getUpdateCommandName()`, `isUpdateWaiting()`). |
