# Projects, saving and export

*Design section 18.*

A project is a folder that holds the song and everything it uses. This page says how a project is kept and saved, how
Phazite recovers one after a crash, and how a song leaves Phazite: as an export, as stems, as a bounce, or as a
DAWproject for another DAW. A project's history of versions is in [versions.md](versions.md).

## Status

| Feature | State | Note |
| --- | --- | --- |
| A project as a folder | Built | |
| A new project untitled until its first save | Built | |
| Save, Save Project As | Built | |
| Files written whole or not at all | Built | |
| Backups of the project file | Built | Kept by age, as Local History is |
| Auto Save | Built | On by default, for a project that has been saved once |
| Autosave for a crash, and restoring it | Built | |
| A project file that can't be read: its backup or its last version offered | Built | |
| View state saved without counting as a change | Built | |
| The format written down for other tools | Built | [project-format.md](../project-format.md) |
| Files from others taken as untrusted | Partly built | A song is made consistent as it opens; a preset's sounds are not yet limited to the library's folders |
| Format versions and upgrades of older projects | Planned | When compatibility starts; no roadmap step |
| Export Song to WAV or FLAC, with Normalize | Built | |
| Export to MP3 | Planned | Before 1.0 (the owner, 2026-11-17): roadmap step S5.16 |
| Stems | Built | |
| Export of a range or of sections | Planned | A range or the loop: roadmap step S5.8; sections have no step |
| Freeze Track | Built | |
| Bounce in Place | Built | |
| Clean Up Unused Audio | Built | |
| DAWproject export | Built | Not yet tried by a person in another DAW |
| DAWproject import | Partly built | A new project gets no plug-ins or sends from the file (after v1, item 8) |
| Archives that include the media | Planned | After v1; no roadmap step |

## The project folder

| Item | Contents |
| --- | --- |
| `<name>.phazite` | The project file: the song alone |
| `Audio/` | Recordings, and imported audio copied in on import |
| `Generated/` | Sounds made by Phazite's generators |
| `Renders/` | Exports: the song, its stems and DAWproject files |
| `Backups/` | Earlier saves of the project file, kept by age ([Backups](#backups)), and the autosave |
| `Cache/` | What Phazite can make again: frozen tracks' sound and stretched audio clips' renders |
| `Versions/` | The project's history ([versions.md](versions.md)) |

- The song names its files by paths relative to the project's folder, so a project moves as its folder.
- Closing a project deletes what nothing in it uses any more from `Cache/`.
- The project file holds no personal data ([privacy-licences.md](privacy-licences.md)).

## The project file

- The project file is the song alone, Phazite's own document, as XML: its tracks and clips with their notes, its
  devices, automation and modulation, its sections, the tempo map, the chord lane, and its view state.
- Every name in it is defined once, in `src/model/base/internal/Schema.h`. Phazite saves its own values, names or its
  own enums, never a library's enum numbers.
- Each value is written in one way, the same in the project file and in the versions' pieces. A choice is saved by
  its name, not its number. Each property is on a line of its own, in the order of their names, so that a tool that
  compares text shows a change as the lines it changed.
- A VST3's state is kept as the format's own preset file, a `.vstpreset`.
- The format is written down for other tools in [project-format.md](../project-format.md), and the history's in
  [versions-format.md](../versions-format.md), with test vectors in `tests/golden/format/` that a reader written from
  the text alone checks in CI.
- `.phazite` is Phazite's own format. DAWproject is for going to and from other DAWs.

## Compatibility

- Phazite is pre-alpha and keeps no backward compatibility. A project, preset or settings file saved by an earlier
  build need not open, or keep what it held.
- So nothing in Phazite reads an old format: a project carries no format version, nothing converts it as it opens,
  and there are no migrations. Names and IDs that projects save may change when that makes the code clearer.
- A beta comes before 1.0. The beta does not promise that its projects open in later versions.
- 1.0 is for music professionals in general, with production first. Format versions, upgrades of older projects and
  the handling of a newer Phazite's projects start when compatibility starts.

## Files from others

Projects, presets and DAWprojects come from other people, so every file Phazite reads is untrusted input: whatever it
says, it can't crash Phazite or make it do anything but open a song.

- **A project is made consistent as it opens** (`makeConsistent()`), whoever wrote it, and the same for its autosave,
  a backup and a version's song. An element Phazite knows, where it doesn't belong, is left out, and one of a type it doesn't know stays untouched; every
  number is brought within its range, and one that is no number at all takes its default; IDs held twice or missing
  are given anew; what names something the song hasn't (a lane, a curve, a modulation, a sidechain, a pad's track) is
  dropped or cleared; and a sound file named on a network path, or by a path that climbs out of the project's folder, is looked for in its
  `Audio` folder by its name, listed as missing in the Project view until it is located; a full path on this computer stays. A file Phazite wrote comes through unchanged.
  [project-format.md](../project-format.md), section 7, lists the rules.
- **A preset** keeps its volumes within a fader's range (-100 to +6 dB, as everywhere a volume is set), and a sound it
  names on a network path, or one that isn't an audio file, is refused without being opened.
- **A DAWproject's** `project.xml` and `metadata.xml` are read only up to 256 MB each, and what it unpacks only up to
  what an archive of its size can hold, so a small archive can't fill the memory or the disk. Its audio is unpacked off the
  message thread, before the file is taken in (a new project's import in the progress window, an import back as a branch
  on the history's thread), with progress and Cancel; what a cancelled import unpacked, and what nothing plays, is
  deleted again.
- **The helper of a plug-in** is read with its lists capped by its message's length, and a plug-in's latency, tail and
  channel counts clamped.
- **The settings file** gives its default for a number that isn't a finite one.
- **The readers are fuzzed** in every test run: project files, a version's pieces and records, presets, DAWprojects, a
  VST3's state, the helper's messages, the settings, keymaps and audio files' headers (`tests/FuzzTests.cpp`).

A preset takes its sounds from its own folder only (a full path in one is not followed), and only the channels handed
to a helper are read back from shared memory. A DAWproject that would hold 4 GB or more, or more than 65,534 files, is
refused before its bounces are rendered, its bounces counted as large as they can render: JUCE's zip writer keeps sizes
and places in 32 bits and has no ZIP64, and readers take 0xFFFF files for ZIP64's marker (`checkDawProjectFits()`,
`src/model/project/internal/DawProject.h`; JUCE workaround W41). The writer also holds each file in memory while it writes
it, packed or stored, so that a large recording in a DAWproject takes as much memory as it is long.

## Saving

### A new project

- A new project is untitled until its first save. The first save (Save, Save Project As, or Save in the question
  when closing) asks, with the system's save dialog, what to call it and where it goes, starting in
  `Documents/Phazite/Projects`. Phazite makes the project's folder of that name there. Later saves don't ask.
- Until then the project lives in a folder of its own in the user's application data (`Unsaved Projects` in
  Phazite's folder there), so that recordings, imported audio and the autosave have a place from the start.
- The first save copies its folders across and goes on there with its undo history. The untitled folder is deleted
  when the project closes, saved or not. The folders are copied into "*name* (saving)" beside, which takes the
  project's name only once it is whole; until then the project stays where it was. The copy always runs in the
  background, however small, the window taking no input meanwhile: after a second a progress window shows it, with
  Cancel, which leaves nothing behind (Escape cancels it before then), so a small project's save looks done at once.
  The copy takes no link inside the folders (a symbolic link or a junction), nor what it leads to, and copies what
  OneDrive or Dropbox keep. A file another program holds a moment (an antivirus, the indexer, a backup tool) is tried
  again for a second or so. It leaves behind a clip's render in `Cache` that goes or changes while it copies, as
  Phazite renders it again; any other file it can't copy, a frozen track's among them, fails the save, which says which
  file. A "(saving)" folder holds a marker of Phazite's; one that
  a crash left goes to the system's trash, in the background, at the next first save into that folder. A folder of that
  name without the marker, or with a project file in it, is never touched.
- The `Versions` folder is copied last, by the history's own thread after what it was writing: meanwhile nothing is
  written into it (a version saved, a merge taken or a branch's change waits, and the Versions view says the project
  is being saved), and a bounce of a version under way starts again once the copy is over, in the project's new folder.
  A project that closes during the copy starts nothing more in the folder it leaves.
- An untitled project is not in Open Recent, and Export Song offers `Documents/Phazite/Exports` for it.
- A lock keeps a second Phazite running at the same time away from the folders the first one uses.
- What a new project starts with (its tempo, key and outline) is part of its first save. It is not an undo step and
  not an unsaved change.

### Save

- **Save Project** (`project.save`, Ctrl+S) writes the project file. A copy of the previous file goes into
  `Backups/` first ([Backups](#backups)). A backup that can't be made is reported, once until one is made again, and
  the save goes on without it.
- **Save Project As** (`project.saveAs`, Ctrl+Shift+S) saves the project, unsaved changes included, as a new folder
  with copies of its `Audio`, `Generated`, `Renders` and `Cache` folders and its `Versions`, and goes on in the copy.
  The project it came from keeps its last saved version.
- Before a save, the song takes in what its plug-ins hold of their own (`Song::storeDeviceStates()`).

### Backups

`Backups/` keeps the project file as it was saved at earlier times, for a save made over good work and for a file
that can't be read.

- **What goes in:** the project file as it was, before a save replaces it, unless the newest backup holds the same:
  saves that change nothing make no backup. Auto Save backs up at most every 10 minutes.
- **By when it was saved:** each backup is named and dated by when its version was saved ("*project*
  2026-11-17 14-02-00", with "_2" and on for more of one second). Only files so named are the project's backups:
  anything else in `Backups/` is left alone, and never offered in the project's place.
- **How long:** as Local History ([versions.md](versions.md#local-history)): every backup of the last two hours, then
  the newest of each hour for a day, then the newest of each day for 30 days. So a day's work leaves backups from an
  hour, a day and a week before.
- **And the one before that:** the newest backup older than 30 days is kept too. A backup's age is its version's, so
  a project opened after a month away backs up a file already older than that, and keeps it: the project it opened
  as stays one to go back to. The file each session opened is otherwise the last save before it, the newest of its
  hour and its day, which the schedule keeps.
- The autosave is not a backup, and stays until a save or closing deletes it.
- Phazite offers the newest backup that reads when the project file can't be read ([After a crash](#after-a-crash)),
  and Clean Up Unused Audio keeps what any backup plays.

### Written whole or not at all

Every file that holds the user's work is written whole or not at all: the project file, the autosave and the
history's files (`writeWholeFile()` in `src/model/base/WholeFile.h`).

- The new contents go into a temporary file beside the old one and are flushed to the disk.
- The temporary file takes the old file's place only once every write worked.
- A write that fails, on a full disk or a drive that went away, leaves the old file as it was, leaves no temporary
  file behind, and is reported as a failure with the operating system's reason.

### Auto Save

**Auto Save** (File > Auto Save, `project.autoSave`) is on by default, and the settings keep it.

- A project with a file of its own saves itself one second after its last change. The song is copied on the message
  thread and written in the background, so the window and what plays don't wait for it.
- While the song plays, it saves at most every 10 seconds.
- It never saves while the song records, halfway through a drag, or while something is being tried in place (Swap).
  It saves at once as a recording ends.
- The previous file goes into `Backups/` at most every 10 minutes (the first time at once), so the backups keep the
  project as it was then, not every second of it.
- Closing the project saves it without asking. If that save fails (a drive gone, a folder that can't be written),
  closing asks instead: **Save As...** (a copy where the user chooses), **Don't Save**, or **Cancel**.
- An untitled project is not auto-saved into a file the user never chose. It is autosaved for a crash instead.

### Autosave for a crash

An untitled project, and any project while Auto Save is off, is autosaved into a separate file in `Backups/`.

- It is written in the background 15 seconds after the first change, then once changes have waited two minutes, and
  at once when a recording ends. It is never written during a recording.
- While the song plays, it waits for the song to stop, for 10 minutes at most.
- The status bar says how long ago the changes were autosaved: "Unsaved changes · autosaved 2 min ago". A save
  deletes the autosave, and the status bar says "Saved". An untitled project reads "Not saved yet".
- Three autosaves or auto saves that fail in a row are reported once, and the status bar says so until one works.

### After a crash

- Phazite keeps a marker while it runs. If the marker is still there at the next start, Phazite did not close
  properly, and it asks before it opens the project that was open then: **Open**, **Open with Plug-ins Off**, or
  **Start a New Project** ([plugin-hosting.md](plugin-hosting.md)).
- A project with an autosave newer than its file offers the autosaved changes: **Restore**, or **Open the last
  saved version**. An autosave that can't be read gives way to the project file, with a message that says so.
- An untitled project with changes saved automatically is offered first, before anything else: **Restore** or
  **Discard them**. Recordings made after the last autosave come back on tracks of their own. Untitled folders with
  nothing to restore are deleted.
- A project file that can't be read offers the newest backup that reads (**Open the Backup**) or the song of its
  branch's last version (**Open the Last Version**), opened in its place. Both are read in the background before the
  question shows, the window held as for any work (a progress window after a second; its Cancel, or Escape, offers
  the backup if it was read by then; a quit stops it). Without either, it leaves Open Recent with an error that says
  so.
- **A restored project is unsaved until it is saved.** A project opened from an autosave, a backup or a version does
  not match its project file, so it shows as changed whatever else happens, and closing it saves it (with Auto Save)
  or asks (without it, or for an untitled project).

### Unsaved changes

- Unsaved changes are what the project holds that it did not hold when it was opened or last saved. They show as
  the `*` in the title bar, in the status bar and in the question when closing, and are what the autosave writes.
- Notifications that change nothing don't count: opening or making a project never shows a `*`. Phazite keeps a
  copy of the song as opened or last saved and compares against it.
- A change a plug-in makes in its own window counts as an unsaved change.
- Before it asks to save, closing asks the plug-ins for their settings and compares the whole song, because many
  plug-ins say that their settings changed when nothing did.

### What is saved without being a change

- **View state** is saved with the project, so it reopens as it was left: track heights, collapsed and pinned
  tracks, drum mode and drum rows, the editors' grid settings, and where a device's window was. Changing it is not
  an undo step and not an unsaved change.
- **How the song plays** is saved too: whether the loop is on, whether it loops the song or a range, where the loop
  is, where playback starts, the metronome's switch and Punch In. Changing these is not an unsaved change either:
  they are about playing the song, not the song.

## Export

**Export Song** (File > Export Song, `project.export`, Ctrl+Shift+R) opens the Export window, whose first choice is
what it exports: the song, or its stems.

### The song

- It renders the song through the master's devices, from its start to its end, which is the end of the bar its last
  clip ends in. Muted tracks stay silent.
- **Tails** play until they are over: until the sound has been under -70 dB for as long as the longest tail the
  song's devices report, so that a delay's echoes are not cut at the silence between them. They last 10 seconds at
  most.
- **Format:** WAV or FLAC, at 44.1, 48, 88.2 or 96 kHz, in 16 bits (dithered) or 24 bits, or 32-bit float for WAV.
- **Normalize** turns the whole song up or down to an integrated loudness (EBU R 128), from -30 to -5 LUFS, never
  taking its true peak over a ceiling, from -6 to 0 dBTP. It is one gain and does not limit, so a mix with high
  peaks can end up under the target; the report then says by how much.
- **The file** goes into the project's `Renders` folder, named after the project. It is written next to that name
  and replaces a file there only once it is complete, so a cancelled or failed export leaves the old one.
- **It runs in the background,** with a progress bar and Cancel. Playback stops for it. Escape asks before it stops
  the export ("Stop the export?"), and the window closed while it exports stops it and says so in a notice, so a
  stray key never throws a long export away without a word.
- **A plug-in that fails, fails the export.** A plug-in that crashes while the song renders, or that holds a block
  for 30 seconds, fails the render, which names the plug-in. Nothing is written without it. Cancelling does not
  wait for a plug-in either.
- The settings keep the format, sample rate, bit depth and loudness choices.

### Stems

Stems are a file for each channel that reaches the master: each track outside a bus, each bus with the tracks in it,
and each return. Together they add up to the mix without the master's devices.

- Each stem goes through its channel's devices, fader and pan. Its sends are left to the returns' own stems.
- The tracks that feed a channel through a sidechain or a send play silently while it renders, so it sounds as it
  does in the mix.
- A channel that is muted, or that a solo leaves silent, has no stem.
- All stems start at the song's start, so they line up wherever they are imported, and run to its end with their own
  tails.
- Their format, sample rate and bit depth are the song's. Normalize does not apply, so they keep their levels.
- They go into a folder named "*project* Stems" in the `Renders` folder, each file named after its channel and
  numbered in the mixer's order ("01 Kick.wav"). The folder is written beside the old one and swapped in only when
  every stem is complete. Exporting again replaces the folder, after asking, and the old one goes to the recycle bin.

## Freeze

**Freeze Track** (`track.freeze`, in a track's menu) renders the track's notes or audio through its instrument and
devices, up to its fader, and plays that file instead, so that its devices take no CPU.

- The fader, pan, mute, solo, the sends and the devices after the frozen part keep working. A send, before the fader
  or after it, takes the sound after the devices, the frozen file's included.
- The frozen part stops before a device that listens to a sidechain, so that it keeps hearing the other track live.
- **A frozen track shows it:** a mark in its header, which unfreezes it at a click, and its frozen devices greyed,
  their controls off and their plug-in windows closed.
- **What plays is what is shown:** changing what a frozen track froze (its clips or notes, or a frozen device, also
  by undo or redo) unfreezes it, so its sound follows the change.
- **Freezing and unfreezing are not undo steps.** A track keeps one frozen file, so undo could otherwise bring back
  a frozen state whose file a later freeze had replaced.
- It renders in the background, with a progress window and Cancel, and its tail ends as an export's does.
- The frozen sound is kept in the project's `Cache` folder, so a frozen track opens frozen. Without its file, the
  track opens unfrozen.
- Returns, buses and folders are not frozen.

## Bounce in Place

**Bounce in Place** (`track.bounceInPlace`, in a track's menu) renders a track through its instrument and devices,
before its fader, from the song's start to the end of its last clip and its tail, into a file in the project's
`Audio` folder.

- The file plays on a new audio track right under the original, named after it with "(Bounce)", in its colour, with
  its fader, pan, sends and output, and the automation and modulation of its fader and sends.
- The original is muted, and shown as it was (its rows are view state, which no undo step holds, so the bounce leaves
  them alone and Undo gives back exactly what was): nothing is lost, and nothing hidden plays. A muted track plays on
  underneath, its instrument and effects too ([mixer.md](mixer.md#mute-and-solo)), so the original is also frozen,
  the bounce its frozen file, where all its devices can be (none listens to a sidechain): it then costs a file's read
  at most.
- It is one undo step, "Bounce in Place", which takes back the mute and the freeze with the new track.
- A device that listens to a sidechain hears its source while bouncing.
- Folders, buses and returns are not bounced: their sound is their tracks', and stems export it.

Consolidate, which joins clips into one, is in [playlist.md](playlist.md).

## Clean Up Unused Audio

**Clean Up Unused Audio...** (File menu, `project.cleanUpAudio`) lists the files in the project's `Audio` and
`Generated` folders that neither the song, nor its file as saved, its autosave or a backup, nor any version or branch
of its history plays, and deletes the ticked ones.

## DAWproject

DAWproject is an open exchange format that other DAWs open. Phazite writes version 1.0 of it: a zip of
`project.xml`, `metadata.xml` and the files they name.

### Export

**Export as DAWproject...** (File menu and the Versions view, `version.exportDawProject`) writes the song as it
is, or the version selected in the history, into the project's `Renders` folder ("*project* - *version*.dawproject").

- **What goes in:** the tracks, folders (a bus as a submix), returns with the sends to them, and the master; each
  channel's volume, pan, mute, solo and output; MIDI clips' notes; audio clips' files with their fades; the tempo
  and time signature with their changes; sections, as markers; the faders' volume and pan automation; and VST3
  plug-ins, each with its state as its `.vstpreset` and its automation.
- **What another DAW can't play is bounced:** Phazite's own devices and containers, a plug-in of another format,
  modulation, an audio clip's gain, stretch, pitch, reverse or takes, and a frozen track. The track's or return's
  sound, before its fader, goes onto an audio track of its own beside it, and the track itself is muted with its
  notes and clips kept.
- **What is left out:** a bus's or the master's devices that can't be played, and what DAWproject has no place for
  (a note's mute, slide and glide, a clip's controller events, a fade's shape). The file's comment says what was
  left out.

### Import

**Import DAWproject...** (File menu, `project.importDawProject`) opens a DAWproject file as a new project.

- A file that was exported from a version of the open project comes back as a branch from that version, showing only
  what the other DAW changed. It takes notes, faders, mutes and tracks added or taken away. Its devices, automation
  and audio clips stay the version's. What DAWproject can't hold stays as the version had it, instead of counting as
  removed.
- A new project made from a file gets no plug-ins or sends from it. It takes notes, faders, mutes, audio clips (an
  audio file once, however many clips play it; Bitwig Studio's clips stretched by their warps follow the tempo at the
  tempo the warps give them), the tempo, the time signature and the markers. What it couldn't bring in is told by
  track when it opens: devices and plug-ins by name, sends, automation, groups, and audio it couldn't take.
- A `.dawproject` file offers Phazite in its Open With menu, without taking the file type from another DAW.

## Decisions

- A project is a folder, not a single file, so that recordings and renders have a place and the song stays small.
- A new project is untitled until its first save asks where it goes, so that starting a song asks nothing.
- Auto Save is on by default, and a project keeps versions from its start, so that work is neither lost nor needs
  remembering. Backups are made at most every 10 minutes under Auto Save, and kept by age on Local History's schedule,
  so that they reach back a month and there is one rule to learn; a count of ten turned over in under two hours.
- Files are written whole or not at all, with a flush to the disk, because a failed write must never leave a cut
  file where a whole one was, nor pass for one that worked.
- View state and how the song plays are saved but are not changes, so that scrolling or looping never asks to save.
- MP3 joins WAV and FLAC before 1.0 (the owner, 2026-11-17; it was declined before), for the drafts producers send.
- Normalize is one gain and never limits; a mix that can't reach its target is reported, not squashed.
- Tails end by silence measured against the devices' own tail lengths, with a cap, rather than a fixed length.
- A plug-in that crashes or hangs fails the render rather than leaving a silent part in the file.
- Freeze is before the fader and is not an undo step; Bounce in Place is an undo step and keeps the original, muted
  and frozen.
- A render leaves out what it can't hear: a strip that is silenced and feeds nothing heard (no send that sends, no
  sidechain that is heard, no pad's track) costs an export, a stem, a freeze or a bounce nothing, as nothing can
  unmute during it; a stem plays only what reaches its channel.
- `.phazite` stays Phazite's own format. DAWproject can't hold modulators, the chord lane, containers' macros or
  Phazite's devices, so it is for exchange, with a bounce beside what another DAW can't play.
- No backward compatibility before the owner says it starts, so the format can still be made clearer.

## Not built

- Format versions, upgrades of older projects and the handling of a newer Phazite's projects: Planned, when
  compatibility starts.
- Export of a chosen range or of sections: Planned.
- MP3 export: Planned, before 1.0 (roadmap step S5.16), with an encoder under a compatible licence.
- A DAWproject's plug-ins and sends imported into a new project: Planned, after v1.
- Warp markers in DAWproject files: Planned.
- Archives of a project that include its media: Planned, after v1.
- The window layout saved with the project: Planned; the settings keep it for the app.

## In the code

- `src/model/project/internal/Project.h`: the project folder, `Project::create()`, `open()`, `save()`, the autosave,
  Auto Save (`AutosaveTimer`), backups, `openWithSong()`, `readNewestBackup()`, and `makeConsistent()`.
- `src/model/base/WholeFile.h`: `writeWholeFile()`; `src/model/base/KeptByAge.h`: what the backups and Local History
  keep as they grow old.
- `src/model/project/internal/UnsavedProjects.h`: untitled projects' folders and what a crash left in them.
- `src/model/project/ProjectContext.h`: the open project as commands and views find it.
- `src/app/ProjectSession.h`: opening, saving, closing and their questions; `src/app/RunMarker.h`: whether the last
  run closed properly.
- `src/model/base/Song.h`, `src/model/base/internal/Schema.h`, `src/model/base/internal/SongText.h`: the song, its
  names, and how it is written as text.
- `src/model/project/Export.h`, `internal/Export.h`: Export Song and stems; `src/ui/shell/ExportPanel.h`.
- `src/model/playback/internal/OfflineRender.h`: the song played offline, its tails and its plug-ins' failures.
- `src/model/mixer/FreezeModel.h`, `internal/Freeze.h`: Freeze Track and Bounce in Place; `src/ui/shell/RenderPanel.h`.
- `src/model/project/UnusedAudio.h`, `src/ui/shell/CleanUpPanel.h`: Clean Up Unused Audio.
- `src/model/project/internal/DawProject.h`: DAWproject export and import.
- Tests: `tests/ProjectTests.cpp`, `tests/WholeFileTests.cpp`, `tests/ExportTests.cpp`, `tests/FreezeTests.cpp`,
  `tests/CleanUpTests.cpp`, `tests/DawProjectTests.cpp`, `tests/GoldenFormatTests.cpp`; for files from others,
  `tests/InconsistentProjectTests.cpp`, `tests/FuzzTests.cpp` and `tests/SongCheck.h`.
