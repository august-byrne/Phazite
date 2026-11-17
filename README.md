# <img src="resources/logo/phazite.svg" alt="" width="32" height="32"> Phazite

[![CI](https://github.com/august-byrne/Phazite/actions/workflows/ci.yml/badge.svg)](https://github.com/august-byrne/Phazite/actions/workflows/ci.yml)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue)](LICENSE)
[![Status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)](#status)

Phazite is a free, open-source digital audio workstation (DAW) for making beats and finishing songs. It is written in C++20 on [JUCE](https://github.com/juce-framework/JUCE), with its own playback engine, Windows first.

**[Website](https://august-byrne.github.io/phazite-site/)** · [User guide](https://august-byrne.github.io/phazite-site/guide/getting-started/) · [Download](https://august-byrne.github.io/phazite-site/download/)

![Phazite with the House Groove example song: the browser on the left with Phazite's instruments, the whole song in the playlist with its sections, chord lane, a filter's automation and the playhead in the second drop, and the synth of the Chords track under it](docs/images/phazite.png)

- **Clean and quick:** an uncluttered window in a dark, light or high-contrast theme, one clear way to do each thing, and the shortcuts of Phazite, FL Studio or Ableton Live.
- **Help for newcomers:** song outlines, a chord lane with chord progressions, example songs and lessons in the app, with chord suggestions and drum-sound generation planned after version 1 ([the design](docs/design.md)).
- **Lean and fast:** low CPU and memory use, in the audio engine and the UI.
- **Plug-ins:** VST®3 now; CLAP, and Audio Units on macOS, later.
- **Open source:** [AGPL-3.0-or-later](LICENSE).

## Status

**Pre-alpha.** There are no downloads yet, so you [build it yourself](#try-it). Projects saved now may not open in later builds.

Everything listed here is built and passes its automated tests. Much of it has not yet been tried by a person: the
"Tried by hand" list after it says which is which, and [the hand checks](docs/hand-checks.md) have every check that
is still open, as steps you can follow.

What is built, on Windows:

- **Arrange:** a playlist of tracks, folders and clips, song sections and outlines, a chord lane with 40 progressions, a tempo lane whose tempo holds, jumps or ramps, automation lanes, and Cut, Copy, Paste and Undo throughout.
- **Write:** a piano roll with a drum mode, keys and scales, chord stamps and transforms; audio clips that follow the tempo, through its changes too, change pitch or play backwards.
- **Sound:** Phazite's own instruments (Synth, Wavetable, Drum Sampler, Sampler, FM) and effects (EQ, Filter, Compressor, Multiband, Limiter, Saturator, Exciter, Vinyl, Delay, Reverb, Phaser, Flanger, Chorus, Vocoder, Stereo Expander), and your VST3 plug-ins, each plug-in in a process of its own: one that crashes or hangs says so in its slot, with Reload, and Phazite plays on.
- **Mix:** a mixer with buses, sends, sidechains, containers of parallel chains (layers, frequency bands, mid and side, and instrument layers and key splits), modulators and macros, freeze and bounce in place, and a loudness meter.
- **Record:** MIDI from a keyboard or the computer keyboard, audio from an interface (ASIO®) with count-in and punch-in, and Capture, which turns what you just played into a clip.
- **Finish:** export the song, or its stems, to WAV or FLAC, the song normalized to a loudness if you like.
- **Share:** the song as a DAWproject, the open format that Bitwig Studio, Studio One and Cubase read, with your VST3 plug-ins and their settings, and Phazite's own devices bounced beside their notes. File > Import DAWproject opens one as a new project, so far without its plug-ins and sends.
- **Keep versions:** save a version of the song with a name, see what changed since, track by track, go back to any version, branch to try an idea and merge it back, and hear an earlier version in place of the song, at the same beat and loudness; work not yet in a version is kept in its Local History, and its Undo History goes back or forth to any change since the project opened.
- **Organize:** the Project view, with the song's notes, its audio files (a missing one to locate, unused ones to clean up) and its plug-ins (a crashed one to reload); Auto Save keeps it all saved.

The song plays, records, exports and freezes through Phazite's own engine, built on JUCE, and plug-ins run in Phazite's own helper processes.

There is also a Windows installer (which you [build yourself](docs/building.md#the-installer) until the first release), an update check you can turn on, and every license shown in the app.

Tried by hand:

- **Tried by the owner:** automation, modulators and macros, containers and presets, the audio clip editor, the chord lane, the mixer's sends and sidechains, freeze, bounce and stems, the devices' sound against reference plug-ins, time-stretching, the presets by ear, plug-in windows, reopening with plug-ins off after a crash, and the installer.
- **Passed its tests only:** recording through an audio interface, a MIDI keyboard, a screen reader (Narrator) reading the window, a DAWproject opened in Bitwig Studio or Studio One, a wide set of third-party VST3 plug-ins, the update from a published release, and a first session by someone new to Phazite.

What is not there yet, among the things you may expect: MIDI controllers (the sustain pedal, pitch bend and the mod wheel are not recorded or edited), MIDI learn and MIDI output, plug-ins with more than one stereo output, take lanes and comping, time-signature changes along the song, and sampled acoustic instruments. A stretched or warped clip is silent until its render finishes.

A whole-project audit found about 300 problems. The bugs that could lose work or sound are fixed, and the tests that would have caught them are built (a fuzzer of every file Phazite reads, random songs, checks of every command's undo, of playback at several rates and block sizes, of every device and of the whole window); the rest are the plan. Next come the hand checks above, the decisions built (source control cut down to what producers use, automation along a control's travel), and the features a beat maker meets first, then a beta; a signed installer, the full user guide and projects that open in every later version come with 1.0. The [roadmap](docs/roadmap.md) has the plan stage by stage, and the [changelog](docs/changelog.md) what was built so far.

| Platform | Status |
|---|---|
| Windows 11 x64 | Builds and runs: Phazite is developed here. |
| Linux, WSL2 (Ubuntu 24.04) | Builds and passes its tests in CI; the app itself hasn't been tried yet. |
| macOS | After version 1, with Audio Units. |

## Try it

You need Windows 11 x64 with about 15 GB free. Open PowerShell (not as administrator) and run:

```powershell
winget install --id Git.Git --exact --source winget --scope machine   # skip if "git --version" prints a version
# Close PowerShell and open a new window, then:
git clone https://github.com/august-byrne/Phazite.git C:\src\Phazite
cd C:\src\Phazite
tools\setup-windows.cmd   # installs the build tools: click Yes when Windows asks; if it says to restart, restart Windows and run it again
```

Then, in a new PowerShell window:

```powershell
cd C:\src\Phazite
git submodule update --init --recursive --depth 1
cmake --preset windows-profile
cmake --build --preset windows-profile
build\windows-profile\Phazite_artefacts\RelWithDebInfo\Phazite.exe
```

The setup takes a few minutes to an hour, and the build a few minutes. [Building Phazite](docs/building.md) explains each step, has the instructions for Linux and WSL2, and a table of fixes for when something goes wrong.

## First steps

- The **Welcome** window that opens at the first start sets you up: your old DAW's shortcuts, your audio output with a test sound, the free [Phazite Sounds](https://github.com/august-byrne/phazite-sounds) (85 public-domain drums, loops and effects) and a song or a lesson to start from. **Help > Welcome** brings it back.
- **Help > Example Songs** opens a finished song to play and take apart; **Help > Lessons** walks you through making a beat, recording, arranging, effects and chords.
- **Space** plays and stops. **M** (Ctrl+T with FL Studio's shortcuts) turns the computer keyboard into a piano (A S D F G H J K L ; for the white keys, W E T Y U O P for the black ones), and a MIDI keyboard plays as soon as it is plugged in.
- **Ctrl+K** finds any command by name, and the bar along the bottom says what the control under the pointer does.
- **Scan for Plug-ins**, in the menu of the gear at the bottom left, finds your VST3 plug-ins. If you hear nothing, choose your output in **Audio Settings**, in the same menu.
- **Ctrl+S** saves: the first time, Phazite asks for a name and suggests `Documents\Phazite\Projects`, and from then on Auto Save keeps the project saved. **File > Export Song** renders it to a file.
- **Versions**, the branch icon down the left, saves a version of the song with a name (**Ctrl+Alt+S**, or Ctrl+N with FL Studio's shortcuts), shows what changed since, track by track, and takes you back to any version.

## Get involved

Phazite is built in the open, and every kind of help counts:

- **Try it and say what tripped you up:** [open an issue](https://github.com/august-byrne/Phazite/issues) for a bug or an idea.
- **Help write it:** [CONTRIBUTING.md](CONTRIBUTING.md) covers setting up, how the code is laid out, its rules for the audio thread, the tests and the checks a change goes through.
- **See where it's going:** [the project board](https://github.com/users/august-byrne/projects/1) (the open steps, by month), [the roadmap](docs/roadmap.md) (the plan, step by step), [the design](docs/design.md) (what Phazite should be, and why) and [the known bugs](docs/known-bugs.md).

## License

Phazite is licensed under the [GNU Affero General Public License, version 3 or later](LICENSE). It builds on JUCE, Rubber Band Library and other open-source code: [docs/third-party.md](docs/third-party.md) lists each with its license, and **Help > About Phazite** shows them all in the app.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media Technologies GmbH.
