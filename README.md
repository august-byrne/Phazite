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

What works today, on Windows:

- **Arrange:** a playlist of tracks, folders and clips, song sections and outlines, a chord lane with 40 progressions, a tempo lane whose tempo holds, jumps or ramps, automation lanes, and Cut, Copy, Paste and Undo throughout.
- **Write:** a piano roll with a drum mode, keys and scales, chord stamps and transforms; audio clips that follow the tempo, through its changes too, change pitch or play backwards.
- **Sound:** Phazite's own instruments (Synth, Wavetable, Drum Sampler, Sampler, FM) and effects (EQ, Filter, Compressor, Multiband, Limiter, Saturator, Delay, Reverb, Phaser, Flanger, Chorus, Vocoder, Stereo Expander), and your VST3 plug-ins, each plug-in in a process of its own: one that crashes or hangs says so in its slot, with Reload, and Phazite plays on.
- **Mix:** a mixer with buses, sends, sidechains, containers of parallel chains (layers, frequency bands, mid and side, and instrument layers and key splits), modulators and macros, freeze and bounce in place, and a loudness meter.
- **Record:** MIDI from a keyboard or the computer keyboard, audio from an interface (ASIO®) with count-in and punch-in, and Capture, which turns what you just played into a clip.
- **Finish:** export the song, or its stems, to WAV or FLAC, the song normalized to a loudness if you like.
- **Share:** the song as a DAWproject, the open format that Bitwig Studio, Studio One and Cubase read, with your VST3 plug-ins and their settings, and Phazite's own devices bounced beside their notes. File > Import DAWproject opens one as a new project, so far without its plug-ins and sends.
- **Keep versions:** Source Control, in git's words: commit the song with a message, see what changed since, track by track, go back to any commit, branch to try an idea and merge it back, and hear an earlier commit in place of the song, at the same beat and loudness; work not yet committed is kept in its Local History.
- **Organize:** the Project view, with the song's notes, its audio files (a missing one to locate, unused ones to clean up) and its plug-ins (a crashed one to reload); Auto Save keeps it all saved.

The song plays, records, exports and freezes through Phazite's own engine, built on JUCE, and plug-ins run in Phazite's own helper processes.

The passes on performance and accessibility are done, and so are a Windows installer (which you [build yourself](docs/building.md#the-installer) until the first release), an update check you can turn on, every license shown in the app, and two more instruments: a sampler that plays a sound across the keys and slices loops, and an FM synth. Next come code signing, a user guide and version 1; the browser's filters, keys, collections and Swap (trying sounds and presets in place as the beat plays), planned for after it, came early. After it come rounds of testing with new users, the browser's analysis of your own sounds, chord suggestions and the sound generator. The [roadmap](docs/roadmap.md) has the plan step by step.

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
- **Source Control**, the branch icon down the left, commits the song with a message (**Ctrl+Alt+S**, or Ctrl+N with FL Studio's shortcuts), shows what changed since, track by track, and takes you back to any commit.

## Get involved

Phazite is built in the open, and every kind of help counts:

- **Try it and say what tripped you up:** [open an issue](https://github.com/august-byrne/Phazite/issues) for a bug or an idea.
- **Help write it:** [CONTRIBUTING.md](CONTRIBUTING.md) covers setting up, how the code is laid out, its rules for the audio thread, the tests and the checks a change goes through.
- **See where it's going:** [the project board](https://github.com/users/august-byrne/projects/1) (the open steps, by month), [the roadmap](docs/roadmap.md) (the plan, step by step), [the design](docs/design.md) (what Phazite should be, and why) and [the known bugs](docs/known-bugs.md).

## License

Phazite is licensed under the [GNU Affero General Public License, version 3 or later](LICENSE). It builds on JUCE, Rubber Band Library and other open-source code: [docs/third-party.md](docs/third-party.md) lists each with its license, and **Help > About Phazite** shows them all in the app.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media Technologies GmbH.
