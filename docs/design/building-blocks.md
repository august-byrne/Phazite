# Building blocks

*Design section 5.*

These are the things a song is made of, the names Phazite uses for them everywhere, and what each is in the saved
song. The other pages build on them.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Tracks that are a playlist row, a device chain and a mixer channel in one | Built | |
| Folders, which are buses or plain | Built | |
| MIDI clips and audio clips | Built | |
| Aliases (linked clips) | Built | |
| Sections | Built | |
| Chord lane | Built | |
| A key for each track | Built | |
| Devices: Phazite's own and VST3 plug-ins | Built | |
| Containers with chains and eight macros | Built | |
| Sends and return tracks | Built | |
| Modulators and macros | Built | |
| Dropping an instrument, a sound or a MIDI file makes a track | Built | |
| Audio and notes kept on separate tracks | Partly built | Dropped files go to a track of their kind; a clip dragged to a track of the other kind is not refused |
| Audio and notes on one track (hybrid tracks) | Declined | Not in version 1 |
| Effects on a single clip | Planned | No roadmap step |

## The concepts

| Concept | What it is | In the song (`Schema.h`) |
| --- | --- | --- |
| Project | A folder with the project file, the audio it plays and its versions | The song (`PHAZITE`), saved as the project file |
| Track | A playlist row, a device chain and a mixer channel in one | `TRACK`, one kind that holds audio and MIDI clips |
| Folder | A collapsible, coloured group of tracks, and a bus that mixes them (its switch can make it plain) | A folder `TRACK` with the tracks in it, a bus or plain |
| Clip | Audio or notes on a track | `CLIP`: a MIDI clip with its `NOTE`s, or an audio clip with the part of its file it plays |
| Alias | A linked copy of a clip: edit one and all change; Make Unique breaks the link | Clips that share a `link` |
| Automation | A parameter's curve along the song, in a lane under its track | `AUTOMATION` |
| Section | A named, coloured part of the song (Intro, Drop) that spans all tracks | `SECTIONS`, with a `SECTION` each |
| Chord lane | The song's chord progression | `CHORDLANE`, with its `CHORD`s and `CHORDKEY`s |
| Key | A track's key and scale | `keyRoot` and `keyScale` on its `TRACK` |
| Device | An instrument or an effect, Phazite's own or a plug-in | `DEVICE`; a plug-in plays in a helper process |
| Container | A group of devices in chains side by side, with macro knobs, saved as one preset | `CONTAINER`, with its `CHAIN`s and eight `MACRO`s |
| Send and return | A level-controlled feed to a return track | `SEND`, to a return `TRACK` |
| Modulator | An LFO, envelope follower, step sequence, random source, breakpoint envelope or note follower that moves any parameter | `MODULATOR`, with what it `MODULATES` |
| Macro | One knob that moves several parameters | `MACRO`, with what it `MODULATES` |

Each has a page: tracks, clips and sections in [playlist.md](playlist.md); notes and keys in
[piano-roll.md](piano-roll.md); channels, sends, returns and buses in [mixer.md](mixer.md); devices and containers in
[devices.md](devices.md); automation, modulators and macros in
[automation-and-modulation.md](automation-and-modulation.md); the chord lane in [chords.md](chords.md); the project
and its file in [projects-saving-export.md](projects-saving-export.md).

## Rules

- **One track, one channel.** A track is made with its mixer channel, under the same name and colour. Its instrument
  and effects are the track's, and the mixer shows them in its strip.
- **Dropping makes tracks.** An instrument, a sound or a MIDI file dropped on empty playlist space makes a new track,
  named after what was dropped. A track made from a file is coloured by what its name says it is (drums, bass, vocals,
  effects, strings or guitars, synths or keys); otherwise it takes the palette's next colour.
- **Audio and notes go on separate tracks.** A dropped file whose kind does not suit the track it is dropped on goes
  on a new track below it. The piano roll shows a track's MIDI clips and the audio clip editor its audio clips.
- **Drums without a channel rack.** There are no step buttons. Drums are made in one of two ways:
  - one-shots placed as audio clips on their own tracks (a kick track, a snare track);
  - a drum sampler track, whose clips are edited in the piano roll's drum mode, each pad a row. A pad can also play on
    a track of its own, with a mixer channel of its own ([devices.md](devices.md)).
- **Reuse without patterns.** Aliases do the work of a pattern used again: every alias of a clip plays the same notes,
  and an edit to one changes them all. Duplicate as Alias, with sections selected, repeats a whole part of the song as
  aliases.
- **Automation is not a clip.** It is a curve in a lane under its track, and Cut, Copy and Paste take the automation
  under the clips along with them.
- **IDs name everything.** Tracks, clips, notes, sections and devices each have an ID in the song, which the window,
  the commands and the versions use to name them.

## Decisions

- One kind of track, which is its own mixer channel: there is no channel rack and no separate list of mixer inserts.
- A folder is a bus by default, so that grouping tracks and mixing them together are one act.
- Aliases in place of patterns: reuse is a property of clips in the playlist, not a second place where music lives.
- No step sequencer: drums are clips of audio or of notes, edited where everything else is.
- Audio and notes do not share a track in version 1; hybrid tracks are left for later.
- Automation lives in lanes under its track, not in clips of its own.

## Not built

- Effects on a single clip (up to five devices on one clip, for one-shots without a track of their own): Planned.
- Hybrid tracks that hold audio and notes together: Declined for version 1.

## In the code

- `src/model/base/internal/Schema.h`: every name written into a project file, and where it is stored.
- `src/model/base/Song.h`, `IDs.h`, `Beats.h`: the song, the IDs, and positions and lengths in beats.
- `src/model/arrangement/internal/Tracks.h`, `Clips.h`, `AudioClips.h`, `Sections.h`: tracks, clips and sections.
- `src/model/arrangement/internal/Import.h`: files dropped or imported, and the tracks they make.
- `src/model/mixer/internal/Mixer.h`, `Buses.h`, `Containers.h`: devices, sends, buses and containers.
- `src/model/automation/internal/Automation.h`, `src/model/modulation/internal/Modulation.h`.
- `src/model/chords/ChordModel.h`, `src/model/notes/NotesModel.h`.
- `docs/project-format.md`: the project file written down for other tools.
