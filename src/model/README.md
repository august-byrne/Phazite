# src/model

Phazite's song model, without window code, in the `phazite_core` static library: the rules of editing a song, the
commands, saving, playback as the song asks for it, and the plug-ins' hosting. One folder per area; `base/` is below
them all. The model may include `src/engine/`, `src/sandbox/` and `src/devices/`, and nothing above it
([docs/architecture.md](../../docs/architecture.md)).

`phazite_core` also compiles JUCE, once, for the app, the helper program, the tools and the tests: only `phazite_core`
links the JUCE module targets, and every other target uses `phazite_use_core()` in `CMakeLists.txt`. `src/services/`,
`src/devices/`, `src/engine/` and `src/sandbox/` are compiled into it too.

Each area's `internal/` folder holds its insides, for the model, the app and the tests: what takes the song itself, its
elements or what plays it, which also makes what the area's other headers hand out (handles, level sources, the song's
selection). The UI includes none of them: it gets what it needs from the model's functions, by ID. Most `internal/`
headers have their `.cpp` in the area's folder.

Paths below are relative to the area's folder.

## base/

What every area builds on.

| File | What it holds |
|---|---|
| `IDs.h` | The IDs that name tracks, clips, sections and devices outside the model, and notes' IDs (`NoteID`), saved in the project, which every note of the song has from the start. |
| `Beats.h` | Positions and lengths in beats: quarter notes, whatever the time signature. |
| `ControlKinds.h` | The kinds of a controller's value a MIDI clip holds beside its notes (`ControlKind`, `hasNumber()`, `getHighestValue()`), below the notes' code, which the song's values hold. |
| `Song.h` | The song: its state, the PHAZITE element, its undo history, its count of IDs (`Song::makeID()`), the folders of its files and its cache, the values set by hand as they are set, the store of its devices' states (`Song::storeDeviceStates()`), what writes into it a while after a change and at once when it is stored or a plan of it compiled (`Song::PendingWriter`), and the scope of what writes it outside its history (`Song::OutsideHistory`). |
| `internal/Schema.h` | The names of everything Phazite writes into a project file, each defined by its description: an element with where it goes, a property with the elements it is on, its C++ type and what it holds (range, words, default, kind, whether it counts); and the song's rules as data (`describeSongRules()`: its keys, orders and IDs); which properties are view state and which aren't counted as unsaved changes, read from the descriptions. |
| `internal/SchemaDescription.h` | What a description is (`Element`, `Property<T>`, a `Family` of names made in a loop, and the facts they are made of: `within()`, `whole()`, `oneOf()`, `hexDigits()`, `byDefault()`...), the list each registers itself in (`getElements()`, `getProperties()`), the lookups by element type the loader, the check and the rules read (`Tables`, `getTables()`), and the description as data (`describeSchema()`: `tests/golden/format/song-schema.json`). |
| `internal/SongText.h` | How a song is written as text, each value in one way, the same in the project file and in the versions' pieces ([docs/project-format.md](../../docs/project-format.md)). |
| `internal/Selection.h` | What is selected, by IDs: one `phazite::Selection` for the open project; and whether the song holds a note (`hasNote()`), which it counts the notes it names by. |
| `internal/SongIndex.h` | Where a song's tracks and clips are by their IDs (a note is found in its clip's value), and its alias groups (`Song::getIndex()`): what the model's functions that take IDs find elements by, made from the song's tree as it is first asked and kept exact by the tree's notifications; the tree stays the only truth. In the tests each answer is compared with a walk through the song. |
| `internal/WholeState.h` | The song, or a part of it, whole: `copyWholeSong()` and `copyWhole()`, and a clip by its ID (`copyWholeClip()`, for what copies a track's clips without its tree), which whatever needs it with everything it holds asks (a save, a version, a comparison, a merge, the clipboard, an export), never its tree alone, and which write its clips' notes back in from their values, NOTE elements first, then CONTROL elements; what it keeps beside its tree at a moment (`keepValues()`), for a copy written or compared later, and whether two kept values hold one clip's notes alike (`holdSameNotes()`). With `Song::OutsideHistory` (`Song.h`), what writes the song without its history says so. |
| `internal/SongValues.h` | The song's values (immer, the model's alone): a MIDI clip's notes and controllers' values as one value (`ClipNotes`: `NoteValues` in the clip's order with each one's place by its ID, `ControlValues` in theirs) and the helpers that change one in batches; the song's area of them by the clip's ID (`NotesByClip`, one value for an alias group's clips), which `SongValues` holds in the song's one undo history (`Song::getValues()`, from `Song::holdValues()` in `makeSong()`) with its keeper, the listener of the song's tree that keeps the area and the tree together (a clip that comes with NOTE and CONTROL elements read in and they taken out, a clip moved given its value back, one that joins an alias group the group's value), and its listeners, told the clips whose values changed; what hears the clips change, from one place, by IDs (`ClipsListener`, which the keeper tells from the tree's notifications while the tree holds the clips: each clip with its track before and after, the tracks whose clips came in another order, a track's clips with it), and what a listener of the song's root leaves to it in its tree callbacks (`isHeardAsClips()`); the conversion between a clip's element and its value (`readClipNotes()`, of a clip's element or of a MIDI file's elements on none, `writeClipNotes()`, notes first), the one place that reads and writes NOTE and CONTROL by name; a channel kept within 1 to 16 where a value gets it (`keepChannel()`); and an alias group's clips given its keeper's events in a copy of the song (`giveClipEvents()`). And a clip as a value, which the song doesn't hold yet (S5.20, the clips' area): `ClipValue` (every property of a CLIP in Phazite's own types, with `ClipFade`, `ClipSound`, what an alias group's audio clips share, and `WarpValue`), `SongClips` (the clips by ID, each track's in order, each alias group's) with the helpers that alone write it (`withClip()`, `withoutClip()`, `withTrackOrder()`, checked by `holdsItsLists()`, and `hasGroupsOfOne()` for a group the model would have dissolved), and the conversion between a CLIP element and its value (`readClipValue()`, `writeClipElement()`, which writes only what differs, each property when the clips' code writes it, of a whole value or of the properties that differ from the value the element held, `keepWithinClip()`, what a clip holds of a value, `setClipProperty()`), the one place that reads and writes a CLIP's properties and its WARP children by name. |
| `Toggles.h` | Switches that act on several things: `toggleAll()`, `areAllOn()`. |
| `BackgroundJobs.h` | Jobs run in the background on threads of their own, never ended by force. |
| `Json.h` | JSON read from text (`parseJson()`), wherever Phazite reads it, without the numbers JUCE's parser can't count; and a number in any text with an exponent JUCE's reader of numbers would overflow, told and made one it reads (`hasExponentTooLongToRead()`, `makeExponentReadable()`, `readDouble()`, `readFloat()`). |
| `KeptByAge.h` | What a history kept without asking keeps as it grows old (`keepByAge()`): every one of the last two hours, the newest of each hour for a day, the newest of each day for 30 days; one rule for Local History and a project's backups. |
| `Numbers.h` | Whole numbers made of numbers that may be no finite number (from a plug-in, a file, a device, a division), without a cast that is undefined: `toWhole()`, `roundToWhole()`, `roundToNearestInt()`, `keepFinite()`. |
| `WholeFile.h` | A file written whole or not at all, which the project file, the autosave, the versions' files and presets are written with, and the tests' hook that makes such a write fail or slow (`WholeFileHook`); and the file of a full path that such a file holds (`makeFileOfFullPath()`). |

## commands/

| File | What it holds |
|---|---|
| `CommandRegistry.h` | The command registry, with no UI code: every user action, by its id (`category.action`), name, description and synonyms. |
| `internal/Undo.h` | Undo and redo with the undo convention: `phazite::UndoStep`, `phazite::Gesture` (a change over several events, as a drag, with `rollBack()`), `undoTargetOf()`, `withSong()`/`ifSong()`, `phazite::undo()` and `redo()`; and its Trial: what Swap tries, one step that gives way to any other change, or a version opened live, which holds the history until Back to Now (`holdHistory()`), steps begun on top of it told to it as they end; and the redo steps that are the song's own, which Redo and Undo History never reach past (`canRedo()`, `getHistorySteps()`: JUCE workaround W56). |
| `internal/UndoCommands.h` | Undo, redo and FL Studio's undo-or-redo, for the selection's song. |
| `SongGesture.h` | A drag's one step for the open project, as the UI's tools hold it. |
| `UndoHistory.h` | The undo history as the Versions view lists it, with a step gone back or forth to. |

## devices/

Phazite's own devices as songs hold them, and parameters as controls and automation reach them.

| File | What it holds |
|---|---|
| `internal/Devices.h` | Phazite's own devices, each its engine-neutral DSP in `src/devices/`, which Phazite's engine plays, in the order menus list them; and other types registered to play as Phazite's own (the tests' stand-ins). |
| `internal/DrumKit.h` | What the drum sampler's pads play (by their notes, each with up to four layers, and a pad on a track of its own, a mixer channel as FL Studio's FPC gives one): a factory sound by its name, or a sound file. |
| `internal/Wavetables.h` | What the synth's oscillators play: a factory table by its name, or a sound file. |
| `internal/SamplerSound.h` | The sampler's sound, with its root key found as it loads, and its slices moved, added and taken away by hand. |
| `internal/DeviceFiles.h` | How the files of devices (the sounds and tables above) are kept by their paths in the project. |
| `SwapModel.h`, `internal/Swap.h` | Swap: a drum pad's sound, or one of Phazite's own devices' presets, tried in place in the song, then kept as one undo step or put back. |
| `ParameterRef.h` | A parameter by its reference: one of Phazite's devices' by its ID, a plug-in's by the plug-in's own ID for it (which follows it through the plug-in's updates), a channel's volume or pan, a send's level, a modulator's parameter or a macro. |
| `internal/Parameters.h` | Parameters in the song, with their values set by hand and the values they play. |
| `ParameterHandle.h`, `internal/ParameterHandle.h` | `phazite::ParameterHandle`, a parameter as controls change it, which owns each change's named undo step; the internal header makes them. |
| `DeviceHandle.h`, `internal/DeviceHandle.h` | One of Phazite's own devices as its editor uses it: its description, a handle to each of its parameters, and its DSP for what an editor reads without waiting (an EQ's analyzer tap, a compressor's meter); the internal header makes them. |

## engine/

The engine as the app and the UI reach it, and what Phazite reads and opens around it.

| File | What it holds |
|---|---|
| `AppInfo.h` | Phazite's name and version, and the notices the About box shows. |
| `AudioFiles.h` | What Phazite reads of an audio file without playing it, a sound's key from its name or its loop data, and a file's identity on the disk (asked at most once a second), which the player and the renders' names go by. |
| `internal/LiveInput.h` | Live MIDI, which plays one track of the open project. |
| `Preview.h`, `internal/Preview.h` | Previews from the browser: each sound a plan of its own on the engine for previews, beside the song's; a loop plays in time with the project. |
| `internal/PreviewLoop.h` | A preview's loop, stretched by Rubber Band into memory as it starts. |
| `StartupSound.h` | The sound Phazite makes once it has opened, synthesized, its echoes from Phazite's delay, which the previews' engine plays. |
| `AudioDevices.h` | The audio devices as Phazite chooses and describes them: a Bluetooth output, the first run's low-latency setup. |
| `AudioDeviceWatch.h` | The audio devices plugged in while Phazite runs, watched, with the switch to one. |
| `internal/AudioIO.h` | Opens the audio devices and the MIDI inputs through JUCE's device manager, as Phazite's settings keep them, with the recordings' options. |
| `internal/MidiTap.h` | What the MIDI inputs send that MIDI learn takes before it is played live: a linked controller, or any while a parameter learns, handed to the message thread through a lock-free queue. |
| `internal/PluginScanner.h` | The plug-in scan, in the helper program, with a time limit for each file, and what a helper says a file holds read with limits (`readScannedPlugins()`). |
| `EngineContext.h` | The engine context as the UI reaches it: the audio devices, how busy the engine is, the MIDI inputs. It owns the audio device, the plug-in host and Phazite's engine on the device. |
| `internal/EngineContext.h` | The engine context as the app and the tests make it: `phazite::createEngineContext()`. |

## project/

| File | What it holds |
|---|---|
| `internal/Project.h` | A project: its folder, saving and autosave, its backups kept by age, `Project::create()` and `Project::open()`; `makeSong()`, which makes a `Song` of a project file, and `phazite::createSong()`, a song outside a project. Its unsaved mark compares only what notified with the saved copy: the clips heard from the song's values, a clip changed in place against the saved copy's clip of its ID, a track whose clips were added, taken out or moved whole (`holdSameClip()`, `holdSameTrackClips()`). |
| `internal/UnsavedProjects.h` | Projects not saved yet, each kept in a folder of its own until its first save. |
| `NewProject.h`, `internal/NewProject.h` | What a new project starts with: nothing but its tempo, key and outline. |
| `ExampleSongs.h`, `internal/ExampleSongs.h` | The example songs that Help opens as new projects. |
| `Export.h`, `internal/Export.h` | Exporting the song and its stems. |
| `internal/DawProject.h` | The song, or a version of it, written as a DAWproject for other DAWs; and one read back, as a new project's song or as a branch from the version it came from. |
| `UnusedAudio.h` | The audio files in the project's folder that no song of it plays, for Clean Up Unused Audio, and how many, found in the background for the Project view. |
| `ProjectContents.h` | What the open project holds, for the Project view: its notes, the audio files its song plays (found or missing, which `MissingAudioCheck` asks of the disk on a thread of its own), and its plug-ins, as they play. |
| `ProjectContext.h`, `internal/ProjectContext.h` | The open project as commands and panels find it: its song, selection, range, playback session, clip renders, clipboard and the parameter touched last. The internal header has `findSong()`, `getSelection()`, `getRangeSelection()` and `getSession()`, and the session's `setSong()`. |

## transport/

| File | What it holds |
|---|---|
| `internal/Playback.h` | Playback and the loop, and where playback starts, kept in the song in beats without undo steps; `getSongEnd()`. |
| `internal/Metronome.h` | The metronome and the count-in, kept in the song without undo steps. |
| `internal/Tempo.h` | The tempo and time signature, with what a tempo change stretches (samples that follow it, and patterns unless the song keeps their speed), and the tempo's changes along the song, each holding until the next or ramping to it, straight or bent. |
| `TempoValues.h` | The tempo's limits, the time signature and the song's bars, as plain values. |
| `TempoModel.h` | The tempo's changes as the tempo lane reaches them, by the context; the top bar's tempo is the one where the playhead is. |
| `internal/Recording.h` | MIDI recording onto the track live MIDI plays, and the armed tracks' audio. |
| `internal/Capture.h` | Capture: the last 30 seconds of what was played live, made into a clip. |
| `RecordingModel.h` | Audio recording, which Phazite does itself: inputs, arming (audio and MIDI), punch-in and takes by IDs, what Record records onto, a track for an input, and what is being recorded as it comes in. |
| `RecordingValues.h` | The plain values of `RecordingModel.h`. |
| `internal/AudioRecorder.h` | The arming, and the device's inputs the tracks need. |
| `internal/Take.h` | A take recorded through Phazite's engine: its inputs into files, and the notes, controllers, bends and program changes played into clips, each on its MIDI channel. |
| `TransportModel.h` | The transport as the top bar reaches it. |

## arrangement/

Tracks, clips, sections, the grid, the selection and the time line.

| File | What it holds |
|---|---|
| `internal/Tracks.h` | The song's track list: tracks and their colours. |
| `TrackColours.h` | The palette tracks get their colours from, which sections, song outlines and imported files use too. |
| `internal/Clips.h` | MIDI clips with their notes, on their tracks' elements in the song; and every clip read by its ID as a value (`getClipValue()`, a track's in order `getClipValues()`, `getAllClips()`, `hasClips()`, `isMidiClip()`, `getAliasClips()`, a value's range and content start, and a clip's range in the song worked out from its value, `getLiveClipRange()`); and the clips' places alone (`getClipPlacements()`), for what walks many clips often; and every clip written by its ID and plain values (`makeMidiClip()`, `addClip()` with its notes, `setClip()`, of a value given or, for the clips' own edits, of the value read and the next, writing only what differs, `eraseClip()`, which takes the last clip of an alias group out of it, what a clip would hold were it set to a value, `getClipValueAsSet()`, `setClipLink()`, `setClipFiles()` for Locate Audio File, `copyTrackClips()` for a duplicated track, `setTrackClipOrder()` for a track's clips put in a version's order, `moveClipStart()`, `setClipPlace()`, with `moveClip()`, `deleteClips()` and the other edits by ID, each worked out on the clips' values), each through the one undo history the clips' code chooses (`getUndoOf()`: none while the song is written outside it). `Clips.cpp`'s storage, the functions above that read and write the elements, is the one place that knows where a clip is kept; the reads of an element it still declares are for the tests and what reads a song's elements. |
| `internal/AudioClips.h` | Audio clips with the parts of their files they play, read by the clip's ID too (`isAudioClip()`, its file, played file and position, fades) and worked out from a clip's value (its file, stretch, played file and position, range, the file's range, `getPlayedClip()`), and a new one made as a value (`makeAudioClip()`, which Clips.h's `addClip()` adds); every edit of an audio clip worked out on its value, what an alias group's clips share given to each as one value (`ClipSound`); the paths of the audio files the song plays, its clips' read by their IDs and its devices' sounds (`findAudioFilePaths()`, what Clean Up and an import keep); a clip that follows the tempo, is pitched or reversed plays from a render Phazite makes itself, in the song's cache folder. |
| `internal/ClipRenders.h` | The project's `ClipRenders`, which make those renders in the background; and the renders a song needs found on the message thread and made on any other, as a render makes them. |
| `internal/Transients.h` | The hits in an audio clip's audio, found in the background. |
| `AudioClipModel.h` | An audio clip as the audio clip editor reaches it, by ID: its gain, pitch, reverse and tempo, its warp markers and Quantize Audio. |
| `internal/Sections.h` | Sections, in the song, as the chord lane is. |
| `Outlines.h`, `internal/Outlines.h` | Song outlines (`resources/outlines/`), and applying one to a song. |
| `GridSettings.h`, `internal/Grid.h` | The grid as plain values (how fine it is, whether it snaps), and the grids the editors keep in the song. |
| `internal/Selection.h` | The time range selected across tracks, and the commands that act on whatever is selected: delete, duplicate, select all and deselect all. |
| `SongSelection.h`, `internal/SongSelection.h` | What is selected in the open project, by IDs, as the UI reads and changes it. |
| `SongRange.h`, `internal/SongRange.h` | The time range selected across tracks, by beats and IDs. |
| `internal/Arrangement.h` | Time edits across every track: inserting, removing, moving and copying a range of beats. |
| `internal/Import.h` | Importing files: audio and MIDI files dropped onto the playlist or put there from the browser. |
| `internal/AudioWork.h` | The work of the commands that read much of an audio file (an import, Normalize, Quantize Audio): in the background, one job at a time, with a progress window after a second, each making its undo step once what it read is in. |
| `internal/Consolidate.h` | What consolidating clips joins on each track. |
| `internal/Clipboard.h` | Cut, Copy and Paste, whose clipboard the project context keeps: notes as plain values, clips as their values with their notes (`ClipValue`, `ClipNotes`). |
| `TimelineModel.h` | The time line as the editors reach it, in beats. |
| `PlaylistModel.h` | The playlist by IDs; `getTimelineEnd()`. |
| `PlaylistValues.h` | The playlist's edits as plain values: where dragged tracks go, audio clips' fade shapes and their warp markers. |

## notes/

| File | What it holds |
|---|---|
| `NoteValues.h` | Notes as plain values, with the transforms, keys and scales, chord stamps and drums' names; the piano roll's controller lanes (`ControlLane`, `ControlPoint`, a ramp's values). |
| `NotesModel.h` | The notes and the controllers' values as the piano roll reaches them, by IDs. |
| `internal/Notes.h` | Notes in MIDI clips, as the song holds them (a clip's value, `getClipNotesValue()` and `setClipNotesValue()`, each edit a function from one value to the next, set once for each alias group), by their IDs (`NoteID`, in `src/model/base/IDs.h`), and their controllers' values, each on a MIDI channel, with what the controller lane reads and changes of them; and what changes them from outside the notes' edits: a tempo change keeping the patterns' speed, a version or a merge taken in, a DAWproject read back. |
| `internal/NoteElements.h` | Each note and each controller's value of a clip's value (`forEachNote()`, `forEachControl()`, `forEachEvent()`), as the notes' code, what plays them and the playlist's read of their shapes walk them. |

## chords/

The chord lane, a guide the playlist shows while the song has chords.

| File | What it holds |
|---|---|
| `ChordValues.h` | Chords as Roman numerals in a key, and chords written as notes for the progressions' preview and the example songs, as plain values. |
| `Progressions.h` | The progressions that ship in `resources/progressions/`. |
| `ChordModel.h` | The lane as the UI reaches it: its chords, keys and the progressions laid on it. |
| `internal/Chords.h` | The lane's chords and keys in the song. |

## mixer/

| File | What it holds |
|---|---|
| `MixerModel.h` | The mixer by IDs, as the UI reaches it; it hands out parameter handles, level sources and mix handles. |
| `internal/Mixer.h` | The devices of each track and the master, in their order, with the values set by hand, the files of Phazite's own and which plug-in each plug-in is, with its mix and what it keeps of its own; each channel's fader by its ID, its volume, pan and sends; and the track each device listens to through its sidechain. All in the song, on the tracks' elements. `getDeviceTypes()`. |
| `internal/Buses.h` | Folders as buses. |
| `internal/Sidechain.h` | Sidechains. |
| `SidechainChoice.h` | A track a device's sidechain can listen to, as the Devices view and the mixer show it. |
| `DeviceType.h` | A device that can be added: one of Phazite's own or a plug-in found by scanning. |
| `ContainerModel.h`, `ContainerValues.h` | Containers, of effects or a track's instrument, with their chains side by side, their split and their macros; plain values in `ContainerValues.h`. |
| `internal/Containers.h` | Each container in the song, with its chains and their devices, which Phazite's engine plays as one device. |
| `FreezeModel.h` | Freezing and bouncing a track in place, and consolidating clips. |
| `internal/Freeze.h` | Which tracks are frozen, kept on their elements; the frozen files in the project's `Cache` folder; the keeper that unfreezes a track when what it froze changes (a change of its clips, or one an undo or a redo makes, at the next turn of the message loop, or before the song is stored or played: `Song::PendingWriter`). |
| `LevelSource.h`, `internal/LevelSource.h` | `phazite::LevelSource`, what a meter shows: one of Phazite's engine's meters. |
| `MixHandle.h`, `internal/MixHandle.h` | An effect's mix as a knob changes it, a drag one undo step. |

## presets/

| File | What it holds |
|---|---|
| `PresetModel.h` | Presets of devices, containers and tracks, in files of their own, saved and loaded by IDs. |
| `internal/Presets.h` | A preset as its file holds it, read and written, and made into the song's elements. |

## automation/

| File | What it holds |
|---|---|
| `AutomationModel.h` | Automation by IDs, as the playlist's lanes and the controls' menus reach it. |
| `AutomationValues.h` | The lanes, curves, points (each a position along its control's travel, and the conversions to and from the parameter's units), shapes and write modes, as plain values. |
| `internal/Automation.h` | The song's curves, in beats, each a parameter's by its reference, and each track's write mode on its element; its `AutomationWatcher` remembers the parameter touched last. |

## modulation/

| File | What it holds |
|---|---|
| `ModulationModel.h` | Each track's modulators and macros and what they modulate, by IDs. |
| `ModulationValues.h` | Their plain values. |
| `internal/Modulation.h` | Modulators and macros kept on the tracks' and containers' elements in the song, which Phazite's engine plays. |

## midi/

| File | What it holds |
|---|---|
| `MidiLinkModel.h` | MIDI learn: a MIDI controller's knob or fader linked to a parameter, which it turns as its control does; the links, learning them and removing them, by handles and plain values, and MIDI Learn Last Touched Parameter. |
| `MidiLinkValues.h` | A MIDI controller as a plain value, and how menus and sentences name it. |
| `internal/MidiLinks.h` | The song's links, and what learns and turns them for the open song (`MidiLinker`, which the project context holds): a burst of a knob's moves one undo step. |

## versions/

The project's versions ([docs/design/versions.md](../../docs/design/versions.md)).

| File | What it holds |
|---|---|
| `internal/SongPieces.h` | The song in pieces, each named by the SHA-256 of what it holds. |
| `internal/PieceStore.h`, `internal/VersionStore.h` | The pieces, kept in packs in the project's Versions folder, with each computer's record of its branches and their logs, as [docs/versions-format.md](../../docs/versions-format.md) writes them down for other tools; and the versions' clock (`getVersionsTime()`), which tests set. |
| `internal/StoreSweep.h` | The store swept as a history opens: this computer's own packs joined by size, keeping every piece; another computer's packs are never touched. Its letting go of what nothing names (older than 30 days, with the bounces of the versions that go) and each computer's copy of what its history names (`adoptVersions()`) are built but off in the product (`sweepLetsGo`; known bug 196). |
| `internal/SongDiff.h` | What differs between two songs, by track, in Phazite's own words. |
| `internal/ChangesWorker.h` | The open song's changes against the version it grew from (or one compared with it), worked out on the history's thread from copies of the parts the song's notifications said changed, a round at a time. |
| `internal/ApplySong.h` | The song made like a version, or a line of it, with the fewest edits: the tracks and the rest element by element, then the tracks' clips in a pass of their own through the clips' functions by their IDs and values (`ApplySong.cpp`: `ClipPass`), a clip on another track moved, keeping its ID, whichever track comes first, each track's clips in the version's order, and each alias group given its notes and sound as one. |
| `internal/SongMerge.h` | The three-way merge of two lines by IDs, with its clashes. |
| `internal/VersionHistory.h` | The open song's history, which the project context makes for each song: the branch the song is on and the version it grew from, which the song says (`versionsBranch`, `versionsBase`) and the history keeps right; its branches and the merge being made, with Verify History and Recover Lost Versions, Undo Last Version, Delete Branch, the versions' names edited beside them, and Local History. |
| `VersionsModel.h` | What the UI reaches, by plain values and IDs. |

## playback/

The song as Phazite's engine plays it.

| File | What it holds |
|---|---|
| `internal/PlaybackHost.h` | The app's engine on the audio device Phazite opens, with the previews' engine beside it, which `EngineContext` owns; the device's inputs, the Low Latency switch, and the notes played live in the last 30 seconds. |
| `internal/SongPlayer.h` | The song compiled into what the engine plays (plans), with what sounds on kept by the song's IDs and the values set by hand taken in at once; each plan compiles again only what the song's changes touched (a track's notes and audio, a channel's devices' values), which the tests compare with a read of the song (`setChecking()`). |
| `internal/PlaybackSession.h` | Each open song's session, which the project context makes for it: it gives the engine the song again as it changes, owns the song's plug-in instances and its recorder, plays the transport, takes live MIDI and guide notes into it, and keeps the take it records. With `renderWithOwnEngine()`, the song rendered offline, which the session lends its plug-ins to while it plays them. |
| `internal/OfflineRender.h` | The song or a part of it played offline on a thread of its own, for an export, a freeze or a bounce, each job (an export's stem) compiled on the message thread as its turn comes, once the clips' renders it plays are made on its own thread; the sound files it plays let go of what it has played. |
| `internal/AutomationWriter.h` | The automation written by hand while the engine plays, in touch, latch and write. |

## plugins/

Plug-ins hosted in helper processes, the only way Phazite hosts them.

| File | What it holds |
|---|---|
| `internal/PluginHost.h` | The host that starts one helper for each plug-in file and makes its instances, which the engine context owns, opening them in the background beside each other as a project opens and ending each helper that goes on a thread of its own: the plug-ins scanning found and what it left out of a file (`takeScanProblems()`), the reload of one that crashed, the plug-ins kept off that crashed as they loaded, and none loaded after a crash of Phazite's until the user says (`setLoadsPlugins()`). |
| `internal/PluginInstances.h` | A song's plug-ins, each an instance by its device's ID, made from the song and kept in step with it, which its playback session owns and which stores their states into the song: loaded in the background as the song opens, as one is added, brought back or reloaded (a render of a song no session plays waits for them with `whenLoaded()`), and asked for their states at once, without the message thread waiting, as it saves. |
| `internal/SandboxedInstance.h` | Each instance as a plug-in instance of JUCE's. |
| `internal/SandboxedProcessor.h` | What Phazite's engine plays of it: a block whole through its channel, with a deadline, its events as MIDI on their channels. |
