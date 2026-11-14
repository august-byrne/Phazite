# Product

*Design sections 1, 2 and 3.*

Phazite is an open-source (AGPL-3.0-or-later) digital audio workstation with its own song model and playback engine,
built on JUCE. This page says what it aims to be, who it is for and the principles every other page follows.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Playlist, piano roll and mixer | Built | [playlist.md](playlist.md), [piano-roll.md](piano-roll.md), [mixer.md](mixer.md) |
| Built-in instruments and effects for electronic music | Built | [devices.md](devices.md) |
| Automation and modulation | Built | [automation-and-modulation.md](automation-and-modulation.md) |
| Song outlines and chord progressions | Built | [getting-started.md](getting-started.md), [chords.md](chords.md) |
| VST®3 hosting on Windows | Built | Each plug-in file in a helper process: [plugin-hosting.md](plugin-hosting.md) |
| Audio and MIDI recording, Capture | Partly built | Take lanes and comping are after version 1: [recording.md](recording.md) |
| Versions of a project (commits, branches, merges) | Built | [../versions.md](versions.md) |
| DAWproject export and import | Partly built | A DAWproject's plug-ins and sends are not imported; after version 1 |
| Windows | Built | Phazite is developed on Windows 11 |
| Linux | Partly built | Builds and passes its tests; the app itself has not been tried |
| macOS, with Audio Units | Planned | After version 1 |
| A beta whose projects may not open later | Planned | Release process, roadmap step 4.5 |
| Chord suggestions | Planned | After version 1: [chords.md](chords.md) |
| Sound generator | Planned | After version 1: [sound-generator.md](sound-generator.md) |
| AI and generative features that know the project | Planned | After version 1: [ai.md](ai.md) |

## What Phazite aims to be

- **Quick to start:** sound within seconds of opening it. Example songs, lessons, song outlines and chord progressions
  show what to do next.
- **Deep when needed:** routing, automation, modulation and mixing are one click away, never hidden behind a mode.
- **Fast and lean:** low CPU and memory use, in the audio engine and in the window.
- **Local and open:** no account, no cloud requirement, and projects that stay on the user's machine.
- **Smart in context (planned):** generative features that are useful because they run inside a project and know its
  key, chords, sections and sounds. None is built; all of it is after version 1.

In one line: FL Studio's speed without FL Studio's split. Phazite keeps the playlist, piano roll and mixer that
electronic producers work in, and has no channel rack and no mixer that is unlinked from its tracks.

## Scope of version 1

- the playlist, the piano roll and the mixer;
- built-in instruments and effects for electronic music;
- automation and modulation;
- song outlines and chord progressions;
- VST3 hosting on Windows.

Versions of a project and DAWproject export and import are built as well, ahead of that list. Chord suggestions and the
sound generator are after version 1. What else waits is in the [roadmap](../roadmap.md#after-10).

## Who Phazite is for

Version 1.0 is for music professionals in general, geared towards music production: people who make music in
FL Studio, Ableton Live or Logic Pro.

| Who | What they need |
| --- | --- |
| Producers who finish tracks in a DAW today | Fast clip editing by hotkey, a strong piano roll, free routing, automation on everything, their third-party plug-ins, and work that is never lost |
| People making their first tracks | Sound at once, guidance on song structure and chords, plain language, nothing hidden |
| Recording musicians and mix engineers | Audio recording is built; takes and comping, deeper metering and more interchange come later |

"Professional" here means the depth of routing, automation, mixing and plug-in hosting that music production needs, not
film post-production.

**Before 1.0 there is a beta.** The beta does not promise that the projects it saves open in later versions. Until the
project's owner says that compatibility starts, there are no format versions and no migrations
([projects-saving-export.md](projects-saving-export.md)).

## Principles

1. **One track, one channel.** Every track is a playlist row, a device chain and a mixer channel, created together with
   the same name and colour. There is no channel rack. Routing beyond that goes through sends, buses and sidechains
   ([mixer.md](mixer.md)).
2. **One window.** Panels dock around the song. The left panel, the lower panel and the inspector can each move to a
   window of their own, for a second monitor ([window.md](window.md)).
3. **No skill-level modes.** There is no beginner or pro switch. Depth appears in place: in the inspector, in
   right-click menus, in the command search.
4. **Every action is a command.** Each has a name, a description and search words, and can be searched and given another
   key ([commands-keys-help.md](commands-keys-help.md)).
5. **Opinionated defaults.** What Phazite offers sounds good and looks finished: its devices' defaults, its presets and
   its example songs. A new project itself starts empty, with the example songs and the lessons a click away
   ([getting-started.md](getting-started.md)).
6. **Help where you are.** A hint bar says what the mouse and the modifier keys do under the pointer, and hovering a
   control explains it.
7. **Nothing invisible.** Selecting a channel outlines what it is routed to, linked clips are marked, automation and
   modulation show on the controls they move, each mixer strip shows its devices' CPU use, and nothing plays while it
   is hidden.
8. **Plain names.** Track, clip, folder, section, device, send. No private vocabulary.
9. **Stable by default.** Autosave and crash recovery; a plug-in that crashes does not take the song with it
   ([plugin-hosting.md](plugin-hosting.md)).
10. **Fast by default.** Strict real-time rules on the audio thread, a window that does nothing when nothing changes,
    quick startup ([performance.md](performance.md)).
11. **Local, private, licence-clean.** No account, no personal data in project files, and only AGPL-compatible code,
    models and data ([privacy-licences.md](privacy-licences.md)).
12. **Accessible.** Everything works from the keyboard, controls have names and values for screen readers, and colour is
    never the only signal ([accessibility.md](accessibility.md)).

## Decisions

- No channel rack: the playlist, the piano roll and the mixer are the three views, because the split between channel
  rack, patterns, playlist and mixer is the complaint FL Studio's users repeat most.
- No beginner or pro mode: a mode hides features from the people who would grow into them.
- One way to do one thing: where two controls do the same, one is removed, so that nobody has to learn which to use.
- A new project starts empty rather than with a starter song: the example songs and the lessons are in the Help menu.
- Version 1.0 is for music professionals in general, geared towards music production, rather than for electronic
  producers on Windows alone or for newcomers first.
- A beta comes before 1.0 and makes no promise about opening its projects later: the formats may still change freely.
- Chord suggestions, the sound generator and every AI feature wait until after version 1, so that version 1 is the
  editing, mixing and hosting that a producer needs every day.
- Windows first, then Linux, then macOS: most of the people Phazite is for use Windows.

## Not built

- Smart, generative features in context (chord suggestions, the sound generator, an assistant): Planned, after
  version 1.
- macOS and Audio Units: Planned, after version 1.
- Layouts of the user's own: Planned without a roadmap step. Three layouts ship, and the window opens as it was left.
- Commands chained into macros by the user, scripts and an assistant that call commands: Planned (scripting and the
  assistant after version 1). The command registry can run a list of commands as one undo step, but nothing in the
  window makes one.
- A routing view that draws cables: Declined; routing is set on the mixer's strips and shows from the selection.
- A CPU readout in each playlist track header: Planned without a roadmap step; the mixer's strips have one.

## In the code

- `src/model/base/Song.h`, `src/model/base/internal/Schema.h`: the song and every name saved in a project.
- `src/model/commands/CommandRegistry.h`: every action as a command.
- `src/ui/shell/AppShell.h`: the one window and its panels.
- `src/engine/core/Engine.h`: the playback engine.
- `docs/roadmap.md`: the build plan and what waits until after version 1.

VST is a registered trademark of Steinberg Media Technologies GmbH.
