# Phazite

[![License: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue)](LICENSE)
[![CI](https://github.com/august-byrne/Phazite/actions/workflows/ci.yml/badge.svg)](https://github.com/august-byrne/Phazite/actions/workflows/ci.yml)

Phazite is an open-source digital audio workstation (DAW) in early development, written in C++20 on [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) and [JUCE](https://github.com/juce-framework/JUCE). What it aims for:

- **Clean UI/UX:** an uncluttered interface and straightforward workflows.
- **AI integrations:** chord-progression suggestions and drum-sound generation that work from the project you're in, plus song and chord-progression outlines for newcomers (see [the design](docs/design.md)).
- **Lean and fast:** low CPU and memory use in both the audio engine and the UI.
- **Open source:** licensed under the [AGPL-3.0-or-later](LICENSE).
- **Plug-in hosting:** VST®3 plug-ins first, Audio Units on macOS later.
- **Cross-platform:** Windows first; Linux and macOS planned.

## Status

- **Early development (pre-alpha):** there are no releases or downloads, and Phazite can't be used to make music yet. You build it from source as described below; [the roadmap](docs/roadmap.md) says what comes next.
- **What's there:** the main window with the playlist (tracks, clips, folders and song sections), the piano roll, the mixer and the browser (with free CC0 sounds to download at the first start), menus, a command search (Ctrl+K), switchable keymaps (Phazite's and FL Studio's), dark, light and high-contrast themes, and projects that save, keep backups and autosave. The first project plays a looping beat and arpeggio on Phazite's own drum sampler and synth, and a MIDI keyboard or the computer keyboard plays the synth live. A Settings window holds the audio device (including ASIO®), MIDI inputs and keyboard shortcuts. Plug-in scanning finds VST3 plug-ins, which the browser and the mixer add to tracks. [5. Run Phazite](#5-run-phazite) describes what to expect.

| Platform | Status |
|---|---|
| Windows 11 x64 | Builds (Debug and Release, zero warnings) with Visual Studio 2026 Build Tools, and runs. |
| Linux / WSL2 (Windows Subsystem for Linux), Ubuntu 24.04 | All four presets build in CI; running the app hasn't been tried yet. See [Building on Linux and WSL2](#building-on-linux-and-wsl2). |
| macOS | Planned, with Audio Units hosting. No presets yet. |

## Building on Windows

Open PowerShell (press Start, type `PowerShell` and press Enter; don't choose "Run as administrator", because the setup script asks for administrator rights itself) and type the commands one line at a time. In short, they install the build tools, fetch the *submodules* (other git repositories that Phazite pins at exact commits) and build with a CMake *preset* (a named set of build settings). The numbered steps below explain each one.

```powershell
# Windows 11 x64, about 10 GB free. Skip the winget line if git --version prints a version.
winget install --id Git.Git --exact --source winget --scope machine
# Close PowerShell and open a new window, so that it finds git:
git clone https://github.com/august-byrne/Phazite.git C:\src\Phazite
cd C:\src\Phazite
tools\setup-windows.cmd                 # click Yes when Windows asks for administrator rights; up to an hour
# If it asks you to restart your PC (or ends with exit code 3010), restart Windows, then cd C:\src\Phazite and run it again.
# Open a new PowerShell window:
cd C:\src\Phazite
git submodule update --init --recursive --depth 1
cmake --preset windows-debug
cmake --build --preset windows-debug
build\windows-debug\Phazite_artefacts\Debug\Phazite.exe   # step 5 says what to expect
```

### Requirements

| You need | Notes |
|---|---|
| Windows 11, 64-bit (x64) | The only tested system. Visual Studio 2026, which provides the compiler, doesn't support Windows 10. |
| winget | The Windows Package Manager, part of Microsoft's App Installer, which comes with Windows 11. |
| Administrator rights | For the installers. The setup script asks once, through one UAC (User Account Control) prompt. |
| Internet access | The Visual Studio Build Tools alone are about a 2 GB download. |
| About 10 GB of free disk space | About 7 GB was used on the test PC, most of it by the Build Tools and the Windows SDK; the two build folders take 1.5 GB. |
| A short folder path, not cloud-synced | Such as `C:\src\Phazite` (see below), outside folders that OneDrive or a similar service syncs: the `windows-debug` build folder alone holds more than 1 GB of files. |

**Why a short path?** Windows limits most paths to 260 characters, and the MSVC compiler and linker keep that limit even when long paths are turned on in Windows. The longest path a build creates inside the repository is 179 characters, so the repository folder's own path must stay under about 80 characters, or the build can fail with `fatal error C1081` (file name too long).

### 1. Get Git and the source

Git downloads Phazite and its dependencies. If `git --version` prints a version, skip the install. Otherwise install Git with winget (the setup script in step 2 installs it too, but you need it first; Windows may ask for administrator rights):

```powershell
winget install --id Git.Git --exact --source winget --scope machine
```

Then close PowerShell and open a new window, so that it finds `git`; `git --version` should now print a version. Cloning downloads the repository into a new folder (git creates any missing parent folders); run every later command from that folder, the repository root. Don't add `--recurse-submodules`: the submodules need a git setting from step 2.

```powershell
git clone https://github.com/august-byrne/Phazite.git C:\src\Phazite
cd C:\src\Phazite
```

### 2. Run the setup script

The setup script installs or updates every build tool and prepares your machine. Its `.cmd` wrapper runs [tools/setup-windows.ps1](tools/setup-windows.ps1) without changing PowerShell's execution policy; you can also double-click it in File Explorer.

```powershell
tools\setup-windows.cmd
```

- It installs or updates Git; CMake and Ninja, which generate and run the build; gitleaks, the secret scanner the git hooks run; and Visual Studio 2026 Build Tools (the MSVC C++ compiler and the Windows 11 SDK, without the Visual Studio IDE). It uses winget, except for updates to the Build Tools, which go through the Visual Studio Installer.
- It sets two global git options for your user account: `core.longpaths true`, which lets git handle paths over 260 characters, and a rule that fetches JUCE over HTTPS (see step 3).
- It turns on this clone's git hooks (`git config core.hooksPath .githooks`), which stop a commit or push that would publish a secret or a private email address; see [Contributing](#contributing). The setting belongs to the clone, so in another clone of Phazite, run that `git config` command once.
- It turns on crash dumps for `Phazite.exe` (full dumps; Windows keeps the last 3).
- If VS Code is installed, it installs or updates the C/C++ and CMake Tools extensions (it never installs VS Code itself).

It asks for administrator rights once (click Yes at the UAC prompt), then runs the installers in a second window that closes by itself. Don't close either window; wait until the first one prints its `Result:` line. A full run took under 6 minutes on the test PC, but can take up to an hour on a slower connection or PC. It is safe to run again: it installs what is missing and updates the rest. No versions are pinned; the build was last verified on 2026-10-11 with what it installed: Git 2.55.0, CMake 4.4.4, Ninja 1.13.2, gitleaks 8.30.1 and Visual Studio 2026 Build Tools 18.10.3 (MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0).

| Switch | Effect |
|---|---|
| `-CheckOnly` | Only reports what is installed and whether this clone's git hooks are on. Changes nothing and needs no administrator rights: safe at any time. |
| `-SkipUpdates` | Installs what is missing, but leaves installed tools as they are. |
| `-WithTestPlugin` | Also installs Surge XT, an open-source (GPL-3.0-or-later) VST3 synth, to test plug-in scanning. |
| `-WithLlvm` | Also installs LLVM (clangd, clang-tidy, clang-format). |
| `-SkipVSCodeExtensions` | Doesn't install or update the VS Code extensions. |

For example: `tools\setup-windows.cmd -WithTestPlugin`. The last line it prints states the result, such as `Result: exit code 0 (done)`:

| Exit code | Normal run | With `-CheckOnly` |
|---|---|---|
| 0 | Done | Everything is present |
| 3010 | Done, but restart Windows, then run it again | Visual Studio is waiting for a restart |
| 1 | A step failed: read the `FAILED` lines and the log named after `Full log:` | Something is missing, or not yet on this window's PATH (see below) |
| 2 | Could not start (no winget, administrator rights refused, ...) | Could not check (for example, from 32-bit PowerShell) |

**When it finishes:** if the last line says `exit code 3010`, or the output asks you to restart your PC (that can happen even with exit code 0), restart Windows, then run the script again from `C:\src\Phazite`. Then **open a new PowerShell window** and run `cd C:\src\Phazite`: programs that were already running, including this window, keep the old PATH (the list of folders Windows searches for programs) and can't find the new tools. If VS Code is running, quit it (File > Exit), start it from the Start menu and close any terminals it restores. You're ready when `tools\setup-windows.cmd -CheckOnly` ends with `Result: exit code 0 (everything present)`.

### 3. Fetch the submodules

This downloads the dependencies at the exact commits Phazite pins: Tracktion Engine (with the JUCE commit it pins) and WindowsToolchain, about 0.2 GB on disk. `--depth 1` skips their history:

```powershell
git submodule update --init --recursive --depth 1
```

The first run prints `checked out` for each of the three submodules. Run the same command after every pull or branch switch. Step 2 must come first: Tracktion lists JUCE with an SSH address (`git@github.com:juce-framework/JUCE.git`), which only works with an SSH key registered on GitHub. Otherwise ssh asks whether to trust `github.com`, and git then fails with `Permission denied (publickey)` or `Host key verification failed`. The setup script's git rule fetches JUCE over HTTPS instead.

### 4. Configure and build

Configuring finds the compiler and prepares a build folder, `build\<preset>`; building compiles and links Phazite. The presets live in [CMakePresets.json](CMakePresets.json): `windows-debug` for development and debugging, `windows-release` for an optimized app (use it in both commands); `cmake --list-presets` lists them.

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

- Configure has worked when its last line is `-- Build files have been written to: .../build/windows-debug`, and the build when its last line is a `Linking CXX executable` line (or `ninja: no work to do.`). Otherwise the first `CMake Error` or `error` line names the cause: see [Troubleshooting](#troubleshooting).
- `ctest` runs Phazite's unit tests (`phazite_tests`, written with [doctest](https://github.com/doctest/doctest)), which the build compiles alongside the app. They have passed when it prints `100% tests passed`. For doctest's own options, such as running only some tests, run `build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --help`.
- No Developer PowerShell or vcvars needed: [cmake/toolchains/windows-msvc.cmake](cmake/toolchains/windows-msvc.cmake) finds MSVC by itself.
- The first configure of a build folder also builds JUCE's `juceaide` helper, so it takes longer. On the test PC (8 cores, 32 GB RAM) the first Debug configure took 33 s and the Debug build 47 s; Release took about 2 minutes for both.
- Windows PowerShell 5.1 has no `&&`. To build only if configure succeeded, run `cmake --preset windows-debug; if ($LASTEXITCODE -eq 0) { cmake --build --preset windows-debug }`.

### 5. Run Phazite

| Preset | App |
|---|---|
| `windows-debug` | `build\windows-debug\Phazite_artefacts\Debug\Phazite.exe` (about 50 MB, plus about 0.5 GB next to it: `Phazite.pdb` with the debug symbols and `Phazite.ilk` for incremental linking) |
| `windows-release` | `build\windows-release\Phazite_artefacts\Release\Phazite.exe` (about 12 MB, no debug symbols) |

Start it from File Explorer or PowerShell, or with F5 in [VS Code](#vs-code). This is what you should see:

1. The main window opens with a menu bar; a top bar with the play, record, capture, loop and metronome buttons, the tempo and time signature, CPU use, the master level and volume, the computer-keyboard button and **Search commands**; the Browser, Playlist and Inspector panels; the editor dock with Piano roll, Audio, Automation and Mixer tabs; and a hint bar along the bottom. The Browser lists sounds, instruments and effects, the Playlist shows the tracks and the Inspector the selected track's channel; the editor dock holds the piano roll and the mixer. The editor dock's Audio and Automation tabs are placeholders that name the roadmap step that builds them. Phazite opens your default audio device, for output only.
2. The first start makes a project called "My First Song" and opens it. Like every new project, it is untitled until you first save it (Ctrl+S): then Phazite asks what to call it and where it goes (`Documents\Phazite\Projects` at first), and makes a folder of that name there for it. Later starts open the project you used last.
   - The first start also opens a **Welcome** window with three pages (**Next** and **Back** move between them; **Not Now** closes it, and **Help > Welcome** brings it back):
     - **Coming from another music app?** **FL Studio** switches to FL Studio's shortcuts and playlist mouse; the other choices keep Phazite's own.
     - **Hear Phazite:** turn your volume down a little, click **Play a Test Sound** and listen for a one-second beep. If you don't hear it, choose another output there.
     - [Phazite Sounds](https://github.com/august-byrne/phazite-sounds): 85 free drums, 808s, loops, effects and foley sounds (9.3 MB). They aren't part of Phazite's download. **Download** fetches them from GitHub into `Documents\Phazite\Packs`, and Phazite checks them before installing; **Not Now** skips them. **Help > Phazite Sounds** offers them again, and so does the browser until they're installed. Without a connection, download the zip from the [release page](https://github.com/august-byrne/phazite-sounds/releases) on another computer and install it with **Install from a File...** in the same window. On Linux, Phazite can't download them itself yet, so the button opens that page.
   - The hint bar along the bottom suggests what to try next whenever the pointer isn't on a control ("Try this: press Space to play the song..."): playing the song, playing notes, dragging in a sound, recording and exporting, each gone once you've done it. **Help > Getting Started Tips** turns these off.
3. Turn your volume down, then press Space or click the play button (▶) at the top left. You should hear a beat on the Drums track (Phazite's Drum Sampler: kick, snare and hats) and, on the Synth track, a plucked synth arpeggiating C, A minor, F and G, one chord per bar at 120 BPM, looping every 8 seconds. Space or the same button (now ■) stops it and goes back to where it started, so Space again replays the same part; click in the ruler to start somewhere else. Shift+Space (Ctrl+Space in the FL Studio keymap) pauses instead, and plays on from there. The loop is the whole song: put a clip later and the loop grows to take it in.
4. Play the synth yourself, with or without the song running (its track is selected when My First Song opens; to play the drum pads, select the Drums track and press Z twice, down to C2, where the kick is):
   - **Computer keyboard:** it starts off, so that single-key shortcuts work: press M (Ctrl+T in the FL Studio keymap) or click the keyboard button in the top bar to turn it on, and again to turn it off. Then A S D F G H J K L ; play the white keys from middle C, and W E T Y U O P the black keys. Z and X move down or up an octave, C and V lower or raise the velocity. The keyboard button shows the octave (C4 to start with). Shortcuts with Ctrl work as usual, single-key shortcuts take Shift while the keyboard plays notes (as in Ableton Live), and typing into a text field doesn't play notes.
   - **MIDI keyboard:** plug it in and play; Phazite finds it within a few seconds. **Options > MIDI Settings** lists your MIDI inputs, each with an on/off switch.
   - **Record:** press R (or click the red button in the top bar) to record what you play onto the selected track, from where playback starts; R or Space stops, and Ctrl+Z takes the whole take back. Right-click the record button for a one-bar **Count-In**, **Overdub** (on: new notes join the clips already there; off: they replace them) and **Quantize Recordings** (to sixteenths). The song's own loop doesn't loop a recording, which runs on past the song's end; a range you looped on purpose does.
   - **Capture:** played something good without recording? Press Ctrl+Shift+C (or the clock button next to record): Phazite turns the last 30 seconds of what you played into a clip on the selected track. While the song plays, the notes stay where you played them; while it is stopped, the clip starts on a bar, and in a song without clips Phazite also sets the tempo from how you played.
   - **Metronome and tempo:** O (Ctrl+M in the FL Studio keymap) or the metronome button turns the click on and off. Drag the tempo in the top bar up or down, scroll it, or click it and type a new one; click the time signature next to it for another.
5. Try the window:
   - Ctrl+K or **Search commands** finds any command by typing part of its name; the arrow keys and Enter run one.
   - F7 shows or hides the piano roll, F9 the mixer and Ctrl+Alt+B the browser; F8 opens the plug-in picker (below). **View** also switches between the Beat, Mix and Full layouts, and the dividers between panels can be dragged.
   - The small window button in the top right corner of the Browser, the Inspector and the editor dock moves that panel to a window of its own, on your second monitor if you have one. Close that window to put the panel back.
   - **Options > Theme** switches between dark, light and high contrast; **Options > Keymap** switches to FL Studio's shortcuts (there, Ctrl+Z undoes or redoes, Ctrl+Space pauses, and Alt+F8 shows or hides the browser).
   - The hint bar says what the control under the pointer does. **Help > Hover Help** turns the tooltips on or off.
   - **Options > Settings** (Ctrl+,) has three tabs: Audio, MIDI and Shortcuts, where any command's keys can be changed.
   - The Inspector shows the selected track's channel, as the mixer does (below): its level meter, volume fader, pan knob, and mute and solo. Drag a fader or knob (with Shift held for fine steps), double-click it to reset it, or Tab to it and use the arrow keys. Ctrl+Z undoes volume and pan changes, and the **Edit** menu names the change it would undo, such as "Undo Change Volume" (mute and solo, as in many DAWs, stay out of the undo history). Windows Narrator (Ctrl+Windows+Enter) reads each control's name and value.
   - Ctrl+= and Ctrl+- make the whole interface larger or smaller (75 % to 200 %), and Ctrl+0 returns to the normal size.
6. Try the playlist:
   - Each track has a header on the left: double-click its name to rename it (or press F2), click its colored strip for another color, use M and S to mute or solo it (right-click M to solo the track alone, as in FL Studio; a solo never changes the mute buttons, and a track that others' solos silence shows its M half lit), drag its bottom edge to resize it, and drag the handle (the dots at its left) up or down to move the track. The arrow before the name collapses the track to a thin row that shows its name, and expands it again; U does the same for the selected tracks, and dragging the bottom edge up past the smallest height collapses it too. Right-click a header for more.
   - **+ Track** over the track headers (Ctrl+T) adds a track below the selected one. The **+ Add track** row after the last track, or a double-click under it, adds one at the end; right-click the row for a folder or a return track. Each track's color runs through its row. Delete deletes the selected tracks and Ctrl+D duplicates them; **Edit > Undo** names each of these steps.
   - The synth's arpeggio is a MIDI clip, which shows its notes. The pointer is a Smart Tool, and the hint bar says what it does where it is: drag a clip's lower half to move it (to another track, too), with Ctrl held to copy it, or with Ctrl and Shift to make an alias, a linked copy marked with a `*`. Drag its left or right edge to trim it, and the top corners of an audio clip to fade it in or out; hold Alt to place things off the grid. Dragging a clip's upper half, or empty space, selects a time range (on a short track, all of a clip moves it). Click a clip to select it (Ctrl+click for more); Delete deletes the selected clips, and Ctrl+D duplicates them right after themselves.
   - Double-click an empty part of a track that has an instrument to add an empty MIDI clip, a bar long. Drag the bottom of the line where two audio clips meet to crossfade them.
   - The tool buttons next to **Grid** pick a tool, and so do the digits 1 to 9 in the buttons' order, and FL Studio's letters (hold Shift while the computer keyboard plays notes): P draws (places a copy of the last clip you used; of a MIDI clip, an alias), B paints copies one after another, C slices, S slips what is inside a clip, T mutes, D deletes, E selects with a rectangle and Z zooms. Esc returns to the Smart Tool. Hold a tool's key while you use the mouse to use that tool only until you let go of the key.
   - Ctrl+E splits the selected clips at the playhead, or the clips in a selected time range at its edges. With a time range selected, Delete deletes what is in it, Ctrl+D copies it right after itself (the copy replaces what it lands on, and the range moves to it, so Ctrl+D again repeats it; Ctrl+Z takes the copy back and the range with it), Ctrl+I inserts that much empty time and Ctrl+Shift+Delete removes it from every track. The arrow keys nudge the selected clips by a step of the grid, and 0 mutes them. Right-click a clip for more: duplicate as alias, make unique, quantize, fade shapes, and delete and close the gap.
   - In the FL Studio keymap, as in FL Studio, a click on empty space places the last clip you used, right-click deletes clips (Shift+right-click opens the menu), and Ctrl+drag selects a time range, which loops while it is selected (Ctrl+B repeats it after itself, and the loop follows); Shift+Left and Shift+Right nudge, and Alt+M mutes.
   - The song's sections are in the time bar, over the bar numbers, and show faintly behind the tracks. Right-click there (or use **Add > Song Outline**) to lay them out as a pop, rock, electronic (house, EDM), hip-hop and trap, lo-fi, R&B, drum and bass or techno song, with a faint energy curve; the hint bar shows each section's tip. Drag a section to move it with everything in it on every track (it goes in where the line shows), Ctrl+drag to copy it, and Ctrl+Shift+drag to copy it as aliases. Drag its right edge to make it longer or shorter: the rest of the song moves along, unless Shift is held. Double-click a section to rename it, or an empty spot to add one (or right-click > **Add Section Here**); Delete and Ctrl+D delete and duplicate the selected sections with what is in them. **File > New Project** offers the outlines too.
   - Drag audio files (WAV, AIFF, FLAC, ...) or MIDI files from File Explorer onto the playlist. Dropped on a track that suits them, the first goes there; the others, or files dropped below the tracks, get new tracks named after them and colored by their names (kicks and snares warm, bass blue, vocals purple, ...). Audio is copied into the project's `Audio` folder, so the project keeps working if the original moves.
   - **Ctrl+G** puts the selected tracks into a folder, **Ctrl+Shift+G** takes them out again, and **U** (or the arrow) collapses a folder, hiding its tracks. **View > Pin Track** keeps a track at the top while the others scroll, and **View > Focus Mode** makes every track but the selected ones small.
   - Click a track to select it, Ctrl+click to add tracks to the selection, and Shift+click to select a range; Up and Down select the track above or below. The Inspector shows the selected track, and if the track has an instrument, the MIDI keyboard and the computer keyboard play it.
   - Playback loops the whole song, from its start to the end of the bar where its last clip ends, and the ruler shows the loop as a green band in its thin lane along the top. To loop a smaller part while you work on it, drag along that lane over the bars you want (or Ctrl+drag anywhere in the ruler); drag the loop's edges to change them or its middle to move it, click the lane to turn looping off and on, and double-click it to loop the whole song again. The **Loop** button in the top bar (or L) turns looping on and off too. Select a time range and press Ctrl+L to loop just that; right-click a section for **Loop Section**. With looping off, playback stops at the end of the song and goes back to where it started.
   - Click or drag in the ruler, below its loop lane, to move the playhead. It snaps to the grid; hold Alt to place it freely; Home goes back to the start, and End goes to the end of the song (the end of the bar its last clip ends in). While playing, the playlist turns the page when the playhead reaches the right edge (**View > Follow Playback**, which the piano roll leaves off so that the notes you edit stay in view).
   - The mouse wheel scrolls, Shift+wheel scrolls sideways, Ctrl+wheel zooms around the pointer, and Alt+wheel makes every track taller or shorter. = and - zoom too. Dragging anything past an edge of the playlist scrolls it along.
   - The grid gets finer as you zoom in. The magnet button (Ctrl+4) turns snapping on or off, and the **Grid** button picks a fixed size or triplets; Ctrl+1 and Ctrl+2 make the grid finer or coarser.
   - With the FL Studio keymap, as in FL Studio: Ctrl+B duplicates, Page Up and Page Down zoom, Shift+4 shows the whole song, Shift+0 centers the view on the playhead, Backspace turns snapping on or off, and Ctrl+Up and Ctrl+Down select tracks.
7. Try the browser, on the left (Ctrl+Alt+B shows or hides it; Alt+F8 in the FL Studio keymap):
   - **Phazite Sounds** holds the 85 free sounds once you've downloaded them (until then, a click on its **Download** row fetches them): acoustic and electronic drums, 808s, loops from 90 to 174 BPM, risers, impacts and foley. They are public domain (CC0), so you can use them in your songs without credit. **Sounds by Type** lists them, and your own sounds, by what they are: Kicks, Snares, Loops and so on. Click a folder to open it, and click a sound to hear it, or move through the sounds with the arrow keys; Esc stops a sound. Loops play in time with the song: at its tempo, and on its bars while the song plays. The headphones button under the list turns previews on or off, and the fader next to it sets how loud they play.
   - Drag a sound onto a track to put it there, at the grid line before the pointer, or below the tracks for a new track named after it. Or select a track, then double-click the sound or press Enter: it goes on that track where Space plays from.
   - **Instruments** and **Effects** list the built-in devices and your plug-ins (after **Options > Scan for Plug-ins**, below). Drag one onto a track or a mixer channel, or press Enter to add it to the selected track. An instrument dropped below the tracks gets a track of its own.
   - Type in the search field (Ctrl+F) to find sounds and devices by name or by what they are, such as "kick", "loop 120" or "reverb"; Down moves into the results.
   - Click the star at the end of the selected row (or right-click the row) to add it to **Favorites**. **Recent** lists what you put into the song lately. **Add Folder...** adds a folder of your own sounds; right-click it to show it in Explorer or remove it again.
   - F8 opens the plug-in picker, as in FL Studio: type part of a device's name or maker, then press Enter to add it to the selected track.
8. Try the piano roll: double-click the synth's MIDI clip, or select it and press F7.
   - It shows the MIDI clips selected in the playlist, several at once, each in its track's color, or the selected track's clip at or nearest the playhead. Bar 1 is where the clip starts, and the header says where the clip is in the song. On a track without a clip, the first note you draw starts one. While the song plays, the playhead shows which of these notes are playing: the playhead runs through the clip while the song is in it, and runs fainter while an alias of the clip (a linked copy, which shares its notes) plays elsewhere in the song. The view stays where you are editing (**View > Follow Playback** turns the page, as the playlist does). Other tracks' notes show faintly behind (**Ghosts** hides them; double-right-click one to edit its clip). Click to add a note with the length and velocity of the note you clicked last, and keep the button down to drag it into place. Right-click deletes a note, and Shift+right-click opens the menu. Drag a note to move it and its edges to resize it; Ctrl+click adds notes to the selection, Ctrl+drag selects in a rectangle, and Shift+drag copies the selected notes. Notes play as you place or click them, and the keys on the left play while you hold them.
   - The playlist's tools and grid work here too: B paints a note on each step of the grid you drag over, C slices a note, T mutes, D deletes, E selects and Z zooms. Ctrl+wheel zooms, Ctrl+Shift+wheel makes the rows taller or shorter, and Alt+wheel changes the selected notes' velocity. Drag across the velocity lane under the notes to set their velocities.
   - With notes selected, Up and Down transpose them, Shift+Up and Shift+Down by an octave, Shift+Left and Shift+Right make them a step of the grid longer or shorter, and Left and Right move them. Ctrl+D duplicates them after themselves, Ctrl+L makes them legato, Ctrl+E chops them into steps of the grid, and Ctrl+U quantizes them. **Transform** (or Ctrl+Shift+U for quantize) shows what quantize, legato, chop or transpose would do, with their settings in a bar under the notes: Enter applies the change, and Esc closes the bar without it. **Edit > Notes** lists all of these.
   - In the FL Studio keymap, as in FL Studio: Shift+Up and Shift+Down transpose, Ctrl+Up and Ctrl+Down move notes by octaves, Shift+Left and Shift+Right move them in time, Ctrl+U chops, Alt+Q and Alt+U show quantize and chop with their settings, and Ctrl+B duplicates.
   - **Chord** picks a chord that each click places whole, from the note clicked. **Key** sets the track's key, Off until you choose one: rows outside it are shaded, **Snap to Key** places notes only on it, and **Use This Key on Every Track** shares it. **Drum Mode** (Shift+right-click; drum tracks start in it) shows the rows as drums (kick, snare, hats, ...) and the notes as hits: drag a drum's name to reorder the rows, or right-click it to hide its row.
9. Try the mixer: press F9.
   - Each track has a channel strip, in the playlist's order and colors; click one to select its track. A strip reads top to bottom in the order the sound goes through it: the track's name, its **Instrument**, its **Effects**, its **Sends**, where its sound goes (**Out: Master**, or the folder bus it is in), pan, mute (M), solo (S), fader and level meter.
   - **Instrument** is what turns the track's notes into sound: Phazite's **Synth** or **Drum Sampler**, or an instrument plug-in found by **Options > Scan for Plug-ins** (below), listed by maker. A track that has notes but no instrument shows **Choose instrument**, tinted, because its notes make no sound; the piano roll's toolbar shows the same button. Click the instrument's name to open its window, and right-click it to turn it off, replace it or remove it.
   - **+ Effect** adds an effect, which changes the sound: Phazite's **EQ**, **Compressor**, **Limiter** and **Reverb**, or an effect plug-in. On a track whose notes have no instrument, it offers the instrument first. Click an effect's name to open its window, and right-click it to turn it off, move it or remove it. Each device shows two of its controls in the strip (the EQ: its low and high shelves), and each effect a **Mix** knob before them, as in FL Studio's mixer: how much of the sound is the effect (at 100 %, the default, the effect alone). Phazite's own effects line their mix up with their sound; if a plug-in's Mix tooltip says it delays its sound, use the plug-in's own mix control instead.
   - The **EQ**'s window draws its curve over what it plays. Drag a numbered dot to move a band's frequency and gain, use the wheel over it for its width (Q), double-click it to turn the band on or off, and right-click it for its shape (cuts, shelves, bells, a notch). The band's controls are below it, and work from the keyboard.
   - The **Drum Sampler**'s window has 16 pads, the kick at the bottom left: click one, or press Space on it, to hear it and see its level, pan, tuning, attack, length and choke group. Drag a sound from the browser (or Explorer) onto a pad to play that instead; the project keeps its own copy. **Reset** brings the built-in sound back. In the piano roll, a drum sampler's track shows a row for each pad, named after its sound.
   - **+ Send** sends some of the track to a return track, which holds an effect that several tracks share. **To a New Return With an Effect...** makes the return, named after the effect you pick, and the send in one step (a reverb there gives only its echo, as it should). The knob sets how much the send takes, and **Post** switches to **Pre**, where the track's fader doesn't change it. The return tracks sit together next to the master; their strips say which tracks send to them, and soloing a track keeps them audible.
   - Click a strip to select its track: the strips it sends to, or that send to it, get a fainter outline, so you can see where the sound goes.
   - A folder's strip has a **Bus** switch. A plain folder's fader only scales its tracks' volumes; a bus mixes its tracks together through its own effects, fader and meter. A folder also becomes a bus when it gets an effect, and plain again when its last one is removed, unless you turned the switch on.
   - Right-click M to solo a track alone, as in FL Studio; right-click it again to unsolo. A solo never changes the mute buttons: a track silenced by other tracks' solos shows its M half lit.
   - In a short window the mixer scrolls up and down rather than squeezing the strips.
   - The master strip on the right has the master's effects, fader and meter, and the loudness as streaming services measure it (EBU R128): momentary (M), over the last 3 seconds (S), integrated since **Reset** (I), and the true peak (TP). It measures what the master's effects put out, before the master fader.
10. **File > Save** (Ctrl+S) saves the project and moves the previous version into the project's `Backups` folder, which keeps the newest 10. A new project's first save asks what to call it and where it goes; it goes on there with its undo history. **File > Save As** (Ctrl+Shift+S) saves a copy under a new name, in a folder of its own where you choose, and continues in the copy; **File > Open Recent** lists the projects you used last. Phazite also saves a copy every 2 minutes, of untitled projects too: if it doesn't close properly, the next start asks whether to restore that copy. Closing the window, **New Project** and **Open Project** ask first whether to save unsaved changes; the title bar shows a `*` while there are any. Changing only how the project is shown (track heights, collapsed or pinned tracks, drum mode and drum rows, the grid) doesn't count as an unsaved change, though saving keeps it.
    - **File > New Project** (Ctrl+N) opens a window for the new project's tempo, time signature, key and song outline (it lists the outline's sections). It starts with a Drums track and a Synth track, ready for the computer keyboard, unless you untick that box. Its name comes with its first save.
    - **File > Export Song** (Ctrl+Shift+R; Ctrl+R in the FL Studio keymap) renders the song to a WAV or FLAC file, from its start to the end of its last bar plus the tails of notes and effects, into the project's `Renders` folder (`Documents\Phazite\Exports` until its first save; **Change...** picks another file). Pick the sample rate and bit depth, and **Loudness > Normalize** to turn the whole song up or down to a loudness, such as -14 LUFS for streaming, without its true peak going over the ceiling. Click **Export**: a bar shows the progress, and the window then says how long and how loud the file is (when the ceiling kept it quieter than the target, a limiter on the master can make it louder). Play the file in any player to check it. The window remembers your choices.
11. If you hear nothing, choose **Options > Audio Settings** and pick an output device; audio inputs are off by default and can be turned on there. Audio and MIDI settings, the plug-in list, the theme, the keymap and your own shortcuts are saved in `%APPDATA%\Phazite\Settings.xml`.
    - With an audio interface, set **Audio device type** to **ASIO**® and pick its driver: ASIO is the low-latency driver standard on Windows. The **Control Panel** button opens the driver's own settings.
    - Without an ASIO driver, you can still try this with [FlexASIO](https://github.com/dechamps/FlexASIO) (MIT), a free ASIO driver that plays through Windows' normal audio.
12. Choose **Options > Scan for Plug-ins**. A "Plug-ins" window asks which folders to scan, with the standard VST3 folders filled in; click **Scan**. The scan runs in a child copy of Phazite (`Phazite.exe --PluginScan:...`), so a plug-in that crashes while being scanned shouldn't take Phazite down; if the child can't start, Phazite shows a warning and scans in its own process instead. Found plug-ins are listed, and the browser, the plug-in picker (F8) and the mixer's **Instrument** and **+ Effect** buttons add them to a track. While a plug-in's window is in front, Space still plays and stops, and the computer keyboard still plays notes when it is on; every other key goes to the plug-in. With no VST3 plug-ins installed the list stays empty; `tools\setup-windows.cmd -WithTestPlugin` installs Surge XT, which should then appear.

### Troubleshooting

| Problem | Fix |
|---|---|
| `fatal: not a git repository`, `tools\setup-windows.cmd` "is not recognized", or CMake `Could not read presets` | This window isn't in the repository folder (new windows start in your user folder): run `cd C:\src\Phazite` and try again. |
| `winget` "is not recognized" | Install or update "App Installer" from the Microsoft Store, then open a new PowerShell window. |
| `git`, `cmake` or `ninja` "is not recognized" | The window is older than the install. Open a new one; for VS Code, quit it completely (File > Exit) and start it from the Start menu. Then run `tools\setup-windows.cmd -CheckOnly`: if it reports `MISSING`, run the full setup again; if a new window still can't find the tools, sign out of Windows and back in. |
| Configure finds no complete Visual Studio installation | Run `tools\setup-windows.cmd`, and restart Windows first if it said a restart is required. |
| Configure says a folder under `external` is empty or missing | The submodules aren't checked out: run step 3 (after step 2 on a new machine). |
| Fetching JUCE asks whether to trust `github.com`, or fails with `Permission denied (publickey)` or `Host key verification failed` | The HTTPS rule for JUCE is missing (answer `no` if asked). Run `tools\setup-windows.cmd`, then step 3 again. |
| `fatal error C1081` (file name too long) | The repository path is too long: clone again into a short one such as `C:\src\Phazite`. |
| `LNK1104` on `Phazite.exe` | Phazite, its `--PluginScan` child or a debug session is still running. Close it and build again. |
| Errors after a Build Tools update (new MSVC) or a toolchain change | Run `cmake --preset <preset> --fresh` (the setup script prints this when MSVC changes). If it still fails, delete `build\<preset>` and configure again. |
| `pre-commit:` or `pre-push: gitleaks is not installed` (or `... is too old`) | Run `tools\setup-windows.cmd` (it installs gitleaks), then try again. On Linux, see [No secrets in git](#contributing). |
| `pre-commit: this commit would publish the ... email address ...` | Set your GitHub noreply address as described in [Your email address](#contributing), then commit again. |
| `pre-push: ... has commits or a tag with these email addresses ...` | Some commits you're pushing carry another address: fix them as the message says (`git commit --amend --reset-author`), then push again. |
| `-CheckOnly` prints `hooks ... OFF` | The git hooks are off in this clone: run `git config core.hooksPath .githooks` there. |
| `pre-commit: these staged C/C++ files don't follow .clang-format` | Save each file in VS Code (that formats it), or run `sh tools/format.sh` in Git Bash, then stage the files again and commit. |
| `pre-commit: the staged C/C++ files couldn't be checked against .clang-format` | No clang-format 22 or newer was found: install VS Code's C/C++ extension, or run `tools\setup-windows.cmd -WithLlvm`. |
| `pre-commit: this commit changes the pinned dependencies` | A submodule pointer moved by accident (for example through `git commit -a`): follow the message to unstage it. A deliberate bump is described in [CLAUDE.md](CLAUDE.md#submodules-and-third-party-code). |
| `pre-commit: these staged files are larger than 5 MiB` | Unstage them (`git restore --staged <file>`); audio files, builds and dumps don't belong in git. |
| `pre-commit: the staged changes have trailing whitespace or a leftover conflict marker` | Fix the lines it lists, stage the files again and commit. |
| `pre-commit: the staged code doesn't keep to the architecture budgets` | If a count went down, run `sh tools/check-architecture.sh --update`, stage `tools/architecture-budget.txt` and commit; if it went up, follow the advice printed for that rule. |
| `pre-commit: this commit raises architecture budgets` | Budgets only go down; for a deliberate move between layers, add the `Architecture-Budget-Raise:` line it prints to the message and use `git -c phazite.allowBudgetIncrease=true commit`. |
| `pre-push: ... has commits that raise architecture budgets without saying why` | Reword those commits (`git rebase -i`) to add the `Architecture-Budget-Raise:` line, or lower the counts instead. |
| `pre-commit: the staged code couldn't be checked against the architecture budgets` | Usually a malformed line or a missing rule in `tools/architecture-budget.txt`. |
| Anything else | The first line containing `error`, `fatal` or `FAILED` usually names the cause. If you're stuck, open an issue at https://github.com/august-byrne/Phazite/issues with that line and the command you ran. |

## Building on Linux and WSL2

> **Partly tested.** CI builds all four Linux presets on Ubuntu 24.04 with the packages below, but nobody has run the app on Linux or set up WSL2 with these steps yet, so they may need fixes.

For WSL2, install Ubuntu 24.04 from a PowerShell opened with "Run as administrator", then restart Windows:

```powershell
wsl --install -d Ubuntu-24.04
```

After the restart, open Ubuntu with `wsl -d Ubuntu-24.04`; its first start asks you to create a Linux user name and password, which `sudo` asks for. Inside Ubuntu, keep the clone in the Linux file system (such as `~/src`), not under `/mnt/c`. Install the packages before the first configure: it builds JUCE's `juceaide` helper, which already needs pkg-config and the X11, FreeType and Fontconfig headers.

```bash
sudo apt update
sudo apt install -y build-essential git cmake ninja-build pkg-config \
    libasound2-dev libfreetype-dev libfontconfig-dev \
    libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev
sudo apt install -y gdb libasound2-plugins   # optional: VS Code debugging, sound under WSL2
git config --global url.https://github.com/juce-framework/.insteadOf git@github.com:juce-framework/
git clone https://github.com/august-byrne/Phazite.git ~/src/Phazite
cd ~/src/Phazite
git config core.hooksPath .githooks   # commit and push checks; need gitleaks 8.19+ (see "No secrets in git")
git submodule update --init --recursive --depth 1
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug                    # the unit tests
build/linux-debug/Phazite_artefacts/Debug/Phazite
```

The first `git config` line is the Linux version of the setup script's HTTPS rule for JUCE; run it once per machine (WSL keeps its own git settings). The `core.hooksPath` line turns on the git hooks in this clone; they need gitleaks 8.19 or newer, installed as described in [No secrets in git](#contributing), and clang-format 22 or newer for C/C++ commits. Ubuntu 24.04's own clang-format is version 18, so install version 23 with pipx (`sudo apt install pipx`, `pipx install "clang-format==23.*"`, then `pipx ensurepath` and a new terminal) or as `clang-format-23` from LLVM's apt repository (apt.llvm.org); `tools/format.sh` finds either. The presets use the system GCC (Ubuntu 24.04: g++ 13) and need CMake 3.28 or newer (Ubuntu 24.04 has 3.28.3). The Linux build only finds Linux VST3 plug-ins (by default in `~/.vst3`, `/usr/local/lib/vst3` and `/usr/lib/vst3`, plus any folder listed in `VST3_PATH`).

| Preset | Build type | App |
|---|---|---|
| `linux-debug` | Debug | `build/linux-debug/Phazite_artefacts/Debug/Phazite` |
| `linux-release` | Release (optimized) | `build/linux-release/Phazite_artefacts/Release/Phazite` |
| `linux-asan` | Debug with AddressSanitizer and UndefinedBehaviorSanitizer (memory errors, undefined behavior) | `build/linux-asan/Phazite_artefacts/Debug/Phazite` |
| `linux-tsan` | RelWithDebInfo with ThreadSanitizer (data races) | `build/linux-tsan/Phazite_artefacts/RelWithDebInfo/Phazite` |

Run the ThreadSanitizer build under `setarch -R`, which turns off address-space randomization for that one process. Ubuntu 24.04 and WSL2 kernels use 32 bits of randomization (`vm.mmap_rnd_bits=32`), which can stop the ThreadSanitizer in GCC 13 and 14 from starting. `TSAN_OPTIONS` passes it the known races in Tracktion Engine to leave out ([tools/tsan-suppressions.txt](tools/tsan-suppressions.txt)), as the `linux-tsan` tests do:

```bash
TSAN_OPTIONS=suppressions=tools/tsan-suppressions.txt setarch $(uname -m) -R build/linux-tsan/Phazite_artefacts/RelWithDebInfo/Phazite
```

Under WSL2, the window should appear on the Windows desktop through WSLg (WSL's built-in GUI support). JUCE has no PulseAudio backend, so for sound install `libasound2-plugins` and pick the ALSA device "Pulseaudio output" in **Audio Settings**. Expect no MIDI ports, and don't measure audio performance under WSL.

## Developing

### VS Code

1. Install VS Code from https://code.visualstudio.com if you don't have it. Open the repository folder (File > Open Folder) and trust it when VS Code asks, or through the Restricted Mode banner: the C/C++ and CMake Tools extensions stay disabled until you do. Install the recommended extensions when VS Code offers them: C/C++ and CMake Tools (the setup script installs both if VS Code was already there), Claude Code, and WSL once you use WSL. Saving a C/C++ file formats it in the project's style (see [Coding guidelines](#coding-guidelines)).
2. Pick a preset with **CMake: Select Configure Preset** from the Command Palette (Ctrl+Shift+P), for example `windows-debug`. VS Code then builds exactly like the terminal, without a Developer PowerShell.
3. Run **CMake: Configure** once. VS Code doesn't configure when it opens the folder, and CMake Tools only gets IntelliSense data and launch targets from build folders it configured itself, so do this once even for a folder you built in a terminal.
4. Choose a launch configuration in the Run and Debug view and press F5:

| Configuration | What it does |
|---|---|
| Phazite (Windows) | Builds the active preset (stops if the build fails), then starts Phazite under the debugger. |
| Attach to Phazite (Windows) | Attaches the debugger to a running Phazite. |
| Open crash dump (Windows, Debug build) | Asks for a `.dmp` file and opens it with the `windows-debug` build's symbols. |
| Phazite (Linux / WSL, gdb) | Like the first, with gdb (untested). |

Once the setup script has turned crash dumps on, a crash outside a debugger leaves `Phazite.exe.<pid>.dmp` in `%LOCALAPPDATA%\CrashDumps` (in PowerShell: `$env:LOCALAPPDATA\CrashDumps`). Only `windows-debug` builds have symbols, so reproduce Release crashes in a Debug build. A full dump holds Phazite's whole memory, so never attach one to a public issue; share the call stack instead.

### Command line

- Run every command from the repository root: the presets are the only entry point. `cmake --list-presets` lists the ones for your OS; configure with `cmake --preset <preset>`, then build with `cmake --build --preset <preset>`. Don't build one preset from VS Code and a terminal at the same time: two builds in one folder break each other.
- After a Build Tools update or a toolchain change, reconfigure with `cmake --preset <preset> --fresh`; if that still fails, delete `build/<preset>` and configure again.
- Put personal presets in `CMakeUserPresets.json`, which git ignores. Each build folder gets a `compile_commands.json` for clangd and similar tools, and configure limits parallel compiles to one per 2560 MiB of RAM, at most one per logical CPU (cache variable `PHAZITE_MIB_PER_COMPILE_JOB`).

### Repository layout

| Path | Contents |
|---|---|
| `src/app/` | The app: startup, the main window with its menus and dialogs, the open project, and the services it owns and hands to what needs them; a library (`phazite_app`) the tests link, plus `Main.cpp` |
| `src/ui/` | What you see and touch, in the `phazite_ui` library, by area: the theme and design tokens, base controls, keys, keymaps and drag and drop, device menus and windows, and one folder per view (time line, playlist, piano roll, mixer, browser, and the window's shell) |
| `src/model/` | The project model on top of Tracktion Engine, by area (commands, engine, project, transport, arrangement, notes, mixer), without window code, with `base/` below them all: the one header that includes Tracktion, and the schema of what a project saves, with its format version and migrations; compiled with Tracktion Engine and JUCE into the `phazite_core` library that the app and the tests share |
| `src/services/` | What the app keeps outside a project: settings, recent projects, the browser's library index and the Phazite Sounds download (also in `phazite_core`) |
| `src/devices/` | The DSP of Phazite's own instruments and effects, without Tracktion, so that a device could run on another engine (also in `phazite_core`) |
| `resources/` | Keymaps, song outlines and icons (compiled into the app), and the Inter font (copied next to the app, into `fonts/`) |
| `tests/` | Unit tests (`phazite_tests`, doctest), including golden tests that compare saved projects, short renders and the model's commands with committed files; `ctest --preset <preset>` runs them |
| `docs/` | [The design](docs/design.md) and [the roadmap](docs/roadmap.md) |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | Build definition, presets, Windows toolchain wrapper and CMake helpers |
| `external/tracktion_engine/` | Submodule: Tracktion Engine, with JUCE as its own submodule in `modules/juce` |
| `external/WindowsToolchain/` | Submodule (MIT) that finds MSVC; used at build time on Windows only |
| `tools/` | Machine setup (`setup-windows.cmd` runs [tools/setup-windows.ps1](tools/setup-windows.ps1)), [tools/format.sh](tools/format.sh), which formats the C/C++ files, [tools/check-architecture.sh](tools/check-architecture.sh), which checks that the code keeps to its layers (budgets in `tools/architecture-budget.txt`), and `soundkit/`, the tool that makes Phazite Sounds from the recipes in the [phazite-sounds](https://github.com/august-byrne/phazite-sounds) repository |
| `.clang-format` | The C/C++ formatting rules (JUCE style) |
| `.githooks/` | Git hooks, turned on per clone with `git config core.hooksPath .githooks`: checks before every commit, merge and push |
| `.github/workflows/` | Continuous integration: GitHub Actions checks and builds every push and pull request |
| `.vscode/` | Shared VS Code settings, launch configurations and extension recommendations |
| `CLAUDE.md`, `.claude/` | Instructions and settings for Claude Code |
| `build/<preset>/` | Build output (ignored by git) |

### Dependencies and submodules

| Submodule | Pinned at |
|---|---|
| `external/tracktion_engine` | `bb386171` on Tracktion's `develop` branch (2026-10-07; module version 3.5.0, not a tagged release) |
| `external/tracktion_engine/modules/juce` | `37c894f8`, the commit Tracktion pins (2026-05-21; JUCE 8.0.13 plus 7 commits on `develop`, not a tagged release) |
| `external/WindowsToolchain` | `1373361e` (v0.14.0 plus 11 commits) |

- Never run `git submodule update --remote`: it would move JUCE away from the commit Tracktion pins.
- Everything under `external/` is read-only: don't edit, reformat or "fix" it, and never add JUCE separately. Don't commit a moved submodule pointer (`git status` shows "(new commits)") unless it is a deliberate dependency bump.
- To bump Tracktion Engine (and with it JUCE), follow [CLAUDE.md](CLAUDE.md#submodules-and-third-party-code): fetch the new commit by its full 40-character hash, review the changes and Tracktion's `BREAKING-CHANGES.md`, then reconfigure with `--fresh` and rebuild.

### Coding guidelines

- Start every C/C++ file with `// SPDX-License-Identifier: AGPL-3.0-or-later`.
- C++20. Include JUCE's module headers directly (`<juce_gui_basics/juce_gui_basics.h>`), not `JuceHeader.h`. Model and services code gets Tracktion from `src/model/base/Tracktion.h`, which includes `<tracktion_engine/tracktion_engine.h>` and declares `namespace te = tracktion;`. Qualify `juce::`.
- Create and destroy Tracktion objects (`te::Engine`, `te::Edit`) on the message thread, and destroy every `Edit` before the `Engine`.
- Keep the build warning-free under JUCE's recommended warning flags (`/W4` on MSVC). Never silence a warning without saying why, as [CMakeLists.txt](CMakeLists.txt) does for `/wd4702` and the `linux-tsan` preset in [CMakePresets.json](CMakePresets.json) does for `-Wno-tsan`.
- Formatting follows [.clang-format](.clang-format): JUCE's coding standards as a clang-format configuration (braces on their own lines, `foo (x)` but `foo()`, `if (! ready)`, `Type* p`, 4-space indents, lines up to 140 characters). VS Code formats a C/C++ file when you save it; `sh tools/format.sh` formats every file from a shell (Git Bash on Windows), and the pre-commit hook refuses unformatted files. To keep a hand-made layout, wrap that code in `// clang-format off` and `// clang-format on`.
- Keep to the layers described in [the design](docs/design.md#24-code-structure): the app uses the UI, the UI the services and the model, the services may use the model, never the other way round, and every layer may use the devices' DSP, which uses only JUCE; the model has no window code; UI code runs commands and calls model functions rather than changing the project, starting undo steps or using Tracktion itself. `sh tools/check-architecture.sh` checks this in CI and before every commit; the full rules are in [CLAUDE.md](CLAUDE.md#architecture-rules).

### Real-time rules (audio thread)

The audio thread is the audio callback and everything it calls, such as plug-in, graph and Tracktion node processing. It must deliver every block of audio on time, so code there must not:

- allocate or free memory (`new`/`delete`, growing containers, creating strings, allocating `std::function` captures): allocate up front when preparing;
- take locks or wait for other threads (`std::mutex`, `juce::CriticalSection`, `MessageManagerLock`, ...);
- post to the message thread (`triggerAsyncUpdate`, `sendChangeMessage`, `callAsync`): set an atomic flag and poll it from a `juce::Timer` instead;
- do file or network I/O, log, throw exceptions or run unbounded loops.

Threads share data only through lock-free FIFOs and `std::atomic` values, and parameters are read through `AutomatableParameter::getCurrentValue()`. Tracktion's 4OSC synth, used by the smoke test, breaks these rules: don't copy it or use it for performance measurements. The full list is in [CLAUDE.md](CLAUDE.md#real-time-rules-audio-thread).

### Working with Claude Code

- [CLAUDE.md](CLAUDE.md) holds the project instructions that Claude Code loads automatically. Its sections on submodules, architecture rules, real-time rules and code conventions apply to human contributors too; the rest is written for Claude Code.
- [.claude/settings.json](.claude/settings.json) is the shared Claude Code configuration: it pre-approves the configure and build commands, read-only git commands, `Get-Process`, `tools\setup-windows.cmd -CheckOnly` and `sh tools/format.sh`, and keeps Tracktion's own CLAUDE.md files (written for Tracktion's contributors) out of Claude's context.
- Put personal notes in `CLAUDE.local.md` and personal settings in `.claude/settings.local.json`; git ignores both.

## Contributing

**Builds.** Before you send a change, format it, run `sh tools/check-architecture.sh`, and build `windows-debug` (and `windows-release` for performance-related work) with zero warnings.

**Continuous integration.** Every push and pull request runs [.github/workflows/ci.yml](.github/workflows/ci.yml) on GitHub's machines. It scans the whole history for secrets with gitleaks, checks formatting with clang-format 23.1.3, checks whitespace, and checks the code's layers against the budgets in `tools/architecture-budget.txt` (a raise needs an `Architecture-Budget-Raise:` line in its commit message). It also builds both Windows presets with Visual Studio 2026 and all four Linux presets on Ubuntu 24.04, with warnings counting as errors. The results are on the repository's Actions tab, and compiler messages appear there as annotations.

**Licenses.** Only add code under licenses compatible with the AGPL-3.0: GPL-3.0, AGPL-3.0, LGPL-2.1-or-later, LGPL-3.0, MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC or Zlib (not the 4-clause BSD license). Never enable Tracktion's proprietary options (Elastique, ReWire, REX).

**No secrets in git.** Nothing confidential may reach the public repository: no API keys or access tokens, passwords, private keys, signing certificates, `.env` files, crash dumps, or personal data such as email addresses or local paths that contain your user name. Keep secrets in environment variables or in files git ignores (`.env`, `CMakeUserPresets.json`, `.claude/settings.local.json`), never in tracked files such as `.vscode/launch.json`, `CMakePresets.json` or `.claude/settings.json`. [.gitignore](.gitignore) ignores common secret files (`.env` files except `.env.example`, `*.pem`, `*.key`, `*.pfx`, `*.p12`, `*.p8`, SSH private keys, credential JSON files and `*.dmp` crash dumps), but it can't catch a key pasted into a tracked file.

Before every commit, check `git status` and read `git diff --cached` in full; if git shows the diff in a pager, scroll with the arrow keys and press `q` to quit. A secret that reaches a commit stays in the history even after you delete the file, so if one slips through, revoke it and create a new one.

**The git hooks.** Three hooks in [.githooks/](.githooks) check your work before it's committed or pushed. [gitleaks](https://github.com/gitleaks/gitleaks) (MIT) is the secret scanner; it shows what it finds redacted.

- `pre-commit` runs before `git commit` (also `--amend`, and finishing a merge or cherry-pick after a conflict). It checks, in this order, that:
  1. the author and committer email is a GitHub noreply address (see below);
  2. the pinned dependencies (`external/`, `.gitmodules`) don't change, except in a deliberate bump;
  3. no staged file is larger than 5 MiB;
  4. the staged changes have no trailing whitespace or leftover conflict markers;
  5. gitleaks finds nothing that looks like a secret in them;
  6. staged C/C++ files follow `.clang-format`;
  7. the staged code keeps to the architecture budgets in `tools/architecture-budget.txt`, and no budget goes up.
- `pre-merge-commit` runs the same checks for `git merge` and `git pull`.
- `pre-push` runs before `git push` and checks the emails and secrets of every commit the remote doesn't have yet, including their commit messages and any tag you push, and that every commit raising an architecture budget says why in its message. It catches what the other two can't see: commits made by `git cherry-pick`, `git revert`, `git rebase` or `git am`, and commits made with `--no-verify`.

To make an exception on purpose, for one command, add `-c phazite.allowSubmoduleBump=true` (a dependency bump), `-c phazite.allowLargeFiles=true` or `-c phazite.allowBudgetIncrease=true` (raising an architecture budget on purpose, with an `Architecture-Budget-Raise: <rule> <old> -> <new>: <reason>` line in the message) after `git`, for example `git -c phazite.allowLargeFiles=true commit`.

On Windows, `tools\setup-windows.cmd` installs gitleaks and turns the hooks on in its clone. On Linux and WSL, install gitleaks 8.19 or newer from its GitHub releases (Ubuntu's own package, 8.16, lacks the `gitleaks git` command the hooks use), then turn the hooks on once in each clone:

```bash
cd /tmp
curl -fsSLO https://github.com/gitleaks/gitleaks/releases/download/v8.30.1/gitleaks_8.30.1_linux_x64.tar.gz
curl -fsSLO https://github.com/gitleaks/gitleaks/releases/download/v8.30.1/gitleaks_8.30.1_checksums.txt
sha256sum --check --ignore-missing gitleaks_8.30.1_checksums.txt   # must print: OK
sudo tar -xzf gitleaks_8.30.1_linux_x64.tar.gz -C /usr/local/bin gitleaks
cd ~/src/Phazite
git config core.hooksPath .githooks
```

The hooks don't replace reading the diff: gitleaks can't recognize personal data or every kind of key, and it can't see inside binary files (UTF-16 text counts as binary). If gitleaks flags something you've checked and it isn't a secret, add the comment `gitleaks:allow` to that line, or, in a file that can't hold comments, add the `Fingerprint` line it printed to `.gitleaksignore`. `--no-verify` skips a hook; keep it for false positives you've checked. Git runs the hooks from your working tree, so before you check out someone else's branch, look at its changes to `.githooks/`. Gitleaks is feature-complete and now gets security fixes only.

**Your email address.** Every commit records git's `user.name` and `user.email`, and pushing to a public repository publishes them for good. Before your first commit, turn on **Keep my email addresses private** under GitHub's Settings > Emails and set the noreply address shown there: `git config --global user.email "<id>+<username>@users.noreply.github.com"` (and your name with `git config --global user.name "Your Name"`). A new address applies only to new commits, so before you push, check the addresses on your commits with `git log --format="%h %ae %ce"`. The hooks refuse any other address; to publish one on purpose, for a single command, add `-c phazite.allowPublicEmail=true` after `git` (for example `git -c phazite.allowPublicEmail=true commit`).

## License and third-party code

Phazite is licensed under the GNU Affero General Public License, version 3 or (at your option) any later version (AGPL-3.0-or-later): see [LICENSE](LICENSE). Each source file says so in its first line: `// SPDX-License-Identifier: AGPL-3.0-or-later`.

| Component | License | Role |
|---|---|---|
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) | GPL-3.0-or-later, or commercial | Audio engine, compiled in |
| [JUCE](https://github.com/juce-framework/JUCE) 8 (inside Tracktion Engine) | AGPL-3.0, or commercial | App, audio-device and plug-in-hosting framework, compiled in |
| VST 3 SDK (bundled with JUCE) | MIT | VST3 plug-in scanning (hosting later), compiled in |
| SoundTouch (bundled with Tracktion Engine) | LGPL-2.1-or-later | Time-stretching, compiled in |
| libsamplerate (bundled with Tracktion Engine) | BSD-2-Clause | Sample-rate conversion, compiled in |
| ASIO SDK 2.3.4 headers (bundled with JUCE) | GPL-3.0-only (Steinberg's GPLv3 option) | ASIO driver support on Windows, compiled in |
| [WindowsToolchain](https://github.com/MarkSchofield/WindowsToolchain) | MIT | Finds MSVC at build time on Windows; nothing compiled in |
| [doctest](https://github.com/doctest/doctest) (bundled with Tracktion Engine) | MIT | Unit-test framework; compiled into the tests only, not the app |
| [Inter](https://github.com/rsms/inter) 4.1 (in `resources/fonts/Inter`) | SIL Open Font License 1.1 | The UI font; shipped as separate files with its licence text (`OFL.txt`), not compiled in |
| [Lucide](https://lucide.dev) 1.55.0 icons (in `resources/icons/lucide`) | ISC; the parts that come from Feather: MIT | The icons Phazite uses, compiled in; the licence ships next to the app (`licenses/Lucide.txt`) |
| [Phazite Sounds](https://github.com/august-byrne/phazite-sounds): drums, 808s, loops, effects and foley, from VCSL, Virtuosity Drums, Big Rusty Drums, body_percussion, FreePats, Sonic Pi, EatMe and Kenney, and Phazite's own synthesis | CC0 1.0 (public domain) | The browser's free sounds, usable in any song without credit. Not part of Phazite: a separate download from their own repository, which Phazite fetches only when asked. Their `sources.json` says where each came from |

Tracktion Engine and JUCE are also available under commercial licenses; Phazite uses their open-source licenses (section 13 of both the AGPL-3.0 and the GPL-3.0 allows combining code under the two licenses). Both bundle further open-source code, such as choc, farbot, magic_enum, crill, rpmalloc, zlib, libpng, libjpeg, FLAC, Ogg Vorbis, HarfBuzz and SheenBidi: see `external/tracktion_engine/LICENSE.md`, `external/tracktion_engine/modules/juce/LICENSE.md`, and the license files and source-file headers of the bundled code under `external/`.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media Technologies GmbH.
