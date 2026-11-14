# Getting started and song outlines

*Design section 13.*

What a new user meets: a welcome window that sets Phazite up, an empty project, a guide in the hint bar, example
songs and lessons. Song outlines lay out a song's sections so that newcomers see how songs are usually shaped.
Nothing here needs an account or a sign-in.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Welcome window: keymap, sound, Phazite Sounds, where to start | Built | |
| New Project window: tempo, time signature, key, outline | Built | |
| A new project starts empty and untitled | Built | |
| The guide: next steps in the hint bar | Built | |
| Example songs: Exodus, Lo-fi Beat, House Groove | Built | |
| Lessons | Built | [commands-keys-help.md](commands-keys-help.md) |
| Song outlines: eight styles | Built | |
| More outline styles (dubstep, trance, future bass, DJ-friendly variants) | Planned | No roadmap step |
| Outlines saved from a song's own sections | Planned | No roadmap step |
| Outlines that suggest a chord progression for each section | Planned | With chord suggestions, after v1 |

## First launch

The first time Phazite starts, the welcome window opens once, over the project. Every page is optional; **Back** and
**Next** move between pages, and **Not Now** (**Done** on the last page) closes it. **Help > Welcome** opens it
again.

| Page | What it asks or offers |
| --- | --- |
| Coming from another music app? | **FL Studio** (FL Studio's keymap and its playlist's mouse), **Ableton Live** (Live's keymap), **Logic Pro or another app**, or **None: I'm new to making music** (both Phazite's keymap). A choice takes effect at once |
| Hear Phazite | The audio output, with the sample rate and buffer size behind a button, and **Play a Test Sound**, a beep for a second through the output chosen. It warns while the output is Bluetooth |
| Phazite Sounds | The free sounds ([browser.md](browser.md)): a button that downloads them and shows its progress, or **Install from a File...** |
| Where to start | The example songs, each with what it shows, and the lessons; each is a button that opens it and closes the window |

On Windows the sound page explains that with an audio interface, choosing ASIO® as the device type and then its
driver gives less delay, and that some such drivers play one app at a time. **Help > Phazite Sounds** opens the
window at its third page. The keymap can be changed later in the gear's **Keymap** menu
([commands-keys-help.md](commands-keys-help.md)).

Starting Phazite opens the project that was open last, or the file it was started with.

## A new project

**File > New** (Ctrl+N in Phazite's and Ableton Live's keymaps) opens a window with:

- the tempo;
- the time signature;
- the key: none, or a root with a scale;
- an optional song outline.

**Create** makes the project. It starts with no tracks; the key goes on the tracks added later. It is untitled until
its first save asks what to call it and where it goes ([projects-saving-export.md](projects-saving-export.md)). What
a new project starts with (its tempo, time signature, key and outline) is part of its first save: it is neither an
undo step nor an unsaved change.

## The guide

While the pointer rests on nothing with a tip of its own, the hint bar suggests the next step, with the keys of the
keymap in use:

1. Put an instrument or a sound into the song (from the browser or the plug-in picker), or open an example song.
2. Play notes on the computer keyboard.
3. Play the song.
4. Record, or capture, what is played.
5. Export the song.

A step goes once it is done, in any way (a key, a button, a menu or the command search), and stays done in later
sessions. Recording counts as playing notes. **Help > Getting Started Tips** turns the guide off and on. While a
lesson runs, the guide waits.

## Example songs

**Help > Example Songs**, and the welcome window's last page, open one as a new untitled project, so its first save
asks where it goes and nothing can overwrite the example. As it opens, its lead track is selected, so the computer
keyboard plays it.

| Song | What it shows |
| --- | --- |
| Exodus | Dark synthwave at 114 BPM, by XCALIBUR (CC BY 4.0), played on Phazite's presets: its tempo speeding up and slowing down in the tempo lane, an arpeggio in layers over a bass and a sub, two buses that pump under the kick through sidechains, each drum on a track of its own, plate reverbs, and the master's filters sweeping in automation lanes |
| Lo-fi Beat | 82 BPM: a lazy, swung kit with fills; shaker, vinyl and rain samples; ninth chords on a wobbling e-piano through a container with macros; an LFO that pans them; a walking bass; automation and a dub delay |
| House Groove | 124 BPM, with a breakdown and two drops: drums in a bus, samples in the playlist, a bass and chords that pump under the kick through sidechains, a pad, automation, an LFO, risers, and sends to a reverb and a delay |

How they are made:

- Each is built in code through the model's functions, by IDs, as the window builds a song, so that it keeps working
  however the model holds the song. None is a saved project file.
- Each uses Phazite's own devices and presets, each preset as it comes (a device stays at its defaults where a
  preset is missing), and samples that Phazite synthesizes as the song is made. Phazite ships no sound files.
- A song someone else made ships as its notes alone, a MIDI file (`resources/songs/Exodus.mid`, written from the
  author's LMMS project by `tools/lmms-to-midi.ps1`), under CC0 or CC BY only: people take an example song apart and
  save it as their own, and those licences ask nothing more of what they make than credit. The credit is in the
  song's description, its project's notes (so that it goes with what is made from it) and the About box.
- Presets are never tuned for a song: a song uses them as they come and only mixes them
  ([presets.md](../presets.md)).

## Song outlines

An outline is a template that lays out a song's sections. Applying one (**Add > Song Outline**, the New Project
window, or the command search: "Apply Outline: Pop"):

- replaces the sections there are with the outline's, from the song's start, in bars of the song's time signature;
  what is on the tracks stays where it is;
- gives each section a name, a colour by its role, and a typical length;
- gives each section an **energy**, how intense it is, which the inspector shows as a percentage;
- gives each section a one-line **tip**, which the inspector shows: for a pre-chorus, "Build toward the chorus:
  rising chords or melody, and fewer drums just before it hits."

It is one undo step, and the outline's command is ticked while the song has that outline's sections. Sections stay
fully editable afterwards ([playlist.md](playlist.md)).

The outlines, with their sections' lengths in bars:

| Style | Sections (bars) |
| --- | --- |
| Pop | Intro 4, Verse 8, Pre-chorus 4, Chorus 8, Verse 8, Pre-chorus 4, Chorus 8, Bridge 8, Chorus 16, Outro 4 |
| Rock | Intro 8, Verse 16, Chorus 8, Verse 16, Chorus 8, Solo 8, Bridge 8, Chorus 16, Outro 8 |
| Electronic (house, EDM) | Intro 16, Build-up 8, Drop 16, Breakdown 16, Build-up 8, Drop 16, Outro 16 |
| Hip-hop and trap | Intro 4, Hook 8, Verse 16, Hook 8, Verse 16, Hook 8, Outro 4 |
| Lo-fi beat | Intro 4, Loop A 8, Loop B 8, Loop A 8, Break 4, Loop B 8, Outro 8 |
| R&B | Intro 4, Verse 16, Pre-chorus 4, Chorus 8, Verse 16, Pre-chorus 4, Chorus 8, Bridge 8, Chorus 8, Outro 4 |
| Drum and bass | Intro 32, Build-up 8, Drop 32, Breakdown 24, Build-up 8, Drop 32, Outro 24 |
| Techno | Intro 32, Groove 32, Build-up 16, Breakdown 16, Peak 32, Outro 32 |

Outlines are data files (`resources/outlines/*.outline.json`): an id, a name, a description, an order, and for each
section its name, role, bars, energy and tip. Adding a style needs its file and its line in `CMakeLists.txt`, and no
other code.

## Decisions

- **No account and no sign-in,** ever, to start making music.
- **A new project starts empty,** with no tracks; the example songs and the lessons show what Phazite does instead
  of a starter project.
- **The project is named at its first save,** not when it is made.
- **Example songs open as new untitled projects,** so an example can't be overwritten.
- **Example songs are made in code,** not saved as project files, so they don't have to follow every change of the
  project format.
- **Someone else's song ships as notes only, under CC0 or CC BY only,** so that what people make from it is theirs.
- **Presets stand on their own;** songs use them as they come.
- **Outlines replace the sections, never what is on the tracks.**
- **Phazite Sounds are offered, not bundled.**

## Not built

- More outline styles: dubstep, trance and future bass, and DJ-friendly variants with 32-bar intros and outros:
  Planned.
- Outlines saved from a song's own sections: Planned.
- Outlines that suggest a chord progression for each section ([chords.md](chords.md), [ai.md](ai.md)): Planned.

## In the code

- `src/ui/shell/Welcome.h`: the welcome window and its pages.
- `src/ui/shell/NewProjectPanel.h`, `src/model/project/NewProject.h`: the New Project window and what a new project
  starts with.
- `src/ui/shell/Guide.h`: the guide's steps; `src/ui/shell/Lessons.h`, `LessonCard.h`: the lessons.
- `src/model/project/ExampleSongs.h`: the example songs; `resources/songs/`: the notes of songs others made, each
  with its notice.
- `src/model/arrangement/Outlines.h`, `internal/Outlines.h`: outlines and applying one;
  `resources/outlines/*.outline.json`.
- Tests: `tests/WelcomeTests.cpp`, `NewProjectTests.cpp` (outlines too), `GuideTests.cpp`, `LessonsTests.cpp`,
  `ExampleSongTests.cpp`.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
