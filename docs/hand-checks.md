# Hand checks

Every check that only a person at the PC can do: something to hear, to see on a real screen, or to try with hardware
or another program. Phazite's tests cover what each row describes, but a test cannot listen, plug in a keyboard or
read the screen aloud.

**How to use this page.** Do a row's steps in a build of the current `main` (`windows-profile`, as the
[README](../README.md#try-it) builds it). If it does what "Proves" says, write your name and the date in "Checked on".
If it does not, leave the cell empty and add the bug to [known-bugs.md](known-bugs.md) or open an issue. "Checked by
the owner" marks what the owner tried before this page existed, with no date recorded. A row is checked again when the
code under it changes much.

Stage 3 of the [roadmap](roadmap.md) is this page: the beta waits for every row here, and the Linux rows for stage 8.

In the steps, the **gear** is the icon at the bottom of the bar down the window's left edge; **Ctrl+K** opens the
command search, which runs any command by its name.

## Hardware (roadmap steps S3.1 and S3.2)

Needs an audio interface with an ASIO® driver, a microphone or a guitar, and a MIDI keyboard.

| # | Check | Proves | Checked on |
|---|---|---|---|
| H1 | Gear > Audio Settings: choose ASIO and the interface, 128 samples. Close and reopen Phazite | The interface opens, plays, and is still chosen after a restart | |
| H2 | Help > Example Songs > House Groove. Play it for its whole length at 128 samples | No crackle or dropout on a real interface; the status bar's CPU stays low | |
| H3 | New project. Add an audio track, click its arm switch and choose the input the microphone is on. Speak | The input is heard while the track is armed, and the track's meter moves | |
| H4 | With a drum loop on another track, turn on Count-In, press R, play or sing four bars, press Space | A bar of clicks, then a clip that grows with its waveform as it records; played back, it is in time with the loop | |
| H5 | Select a range of two bars in the playlist and press Ctrl+L to loop it, turn on Punch In (Record's menu), play from a bar before the loop and record | Only what was played inside the loop is kept, without a click at its edges | |
| H6 | Keep recording round the loop three times, then stop | One clip with three takes; its menu, or Previous Take and Next Take, choose between them | |
| H7 | Play a loud click from a speaker into the microphone while recording the metronome's beat; compare the recorded click with the grid, then set Recording correction in the Audio settings | The recording lands on the beat once the correction is set | |
| H8 | Pull the interface's cable while the song plays, then plug it in again | Phazite does not crash; a card in the bottom right corner offers to switch to the device | |
| H9 | Plug in a MIDI keyboard with an instrument track selected, and play | Notes sound at once, with no setup; velocity is heard | |
| H10 | Press R, play eight bars on the keyboard with the sustain pedal, stop | The notes are in a clip, in time. (The pedal is not recorded yet: roadmap step S5.3) | |
| H11 | Without recording, play a phrase, then press Ctrl+Shift+C | Capture turns what was just played into a clip | |
| H12 | Turn on Low Latency in the transport bar and play the keyboard through a track with a Limiter | The delay between key and sound gets shorter | |
| H13 | Press M and play A S D F on the computer keyboard; then click into a text field and type | The letters play notes, and typing in a field plays none | |

## Plug-ins (roadmap step S3.4)

Needs about twenty third-party VST®3 plug-ins: instruments and effects, free and commercial, among them the ones you
use most. Write the list tried, and what failed, under the table.

| # | Check | Proves | Checked on |
|---|---|---|---|
| P1 | Gear > Scan for Plug-ins | Every installed VST3 is found; one that hangs or crashes the scan is left out and the scan finishes | |
| P2 | For each plug-in: add it to a track, open its window, play notes or audio through it | It sounds right, and its window draws and resizes at Interface Size 100 % and 150 % | |
| P3 | For each: turn a knob in its window, save, close and reopen the project | The setting came back | |
| P4 | For each: move a knob in its window, press Ctrl+Shift+A, and draw a ramp in the lane that appears | The parameter touched last gets a lane, and the knob in the plug-in's window follows it | |
| P5 | For three of them: type a name into a text field of the plug-in's own, with and without Plug-in > Send All Keys to the Plug-in | Typing reaches the plug-in; Space still plays and stops without the switch | |
| P6 | For three effects with a sidechain input: pick a track under Sidechain in the plug-in's window | The plug-in reacts to the other track | |
| P7 | Save a preset from the Plug-in menu, change the sound, load the preset | The sound is back | |
| P8 | Freeze Track on a track with a heavy plug-in, then unfreeze it; export the song (Ctrl+Shift+R) | The frozen track and the exported file sound like live playback | |
| P9 | End a plug-in's `PhaziteHost` process in Task Manager while the song plays | The song plays on; the plug-in's slot says it crashed and offers Reload; Reload brings it back with its settings | |
| P10 | A plug-in's window over the main window: press Space, and play the computer keyboard | Play, Stop and the notes work while the plug-in's window has the keyboard | Checked by the owner |
| P11 | After a crash of Phazite itself, start it again and choose to open the project with its plug-ins off | The project opens, its plug-ins off until turned on | Checked by the owner |

## Screen reader and keyboard (roadmap step S3.2)

Needs Windows Narrator (Ctrl+Win+Enter). Put the mouse aside.

| # | Check | Proves | Checked on |
|---|---|---|---|
| A1 | Start Phazite with Narrator on. Tab through the title bar, the transport bar and the status bar | Every control is read with a name and a role; none is "button" alone | |
| A2 | Press F5, F9, F7, F6 and Shift+F6 | Each view takes the keyboard, and Narrator says its name | |
| A3 | Ctrl+K, type "new project", Enter; fill in the window that opens and confirm it | A project can be made by ear | |
| A4 | With the keyboard alone: add an instrument track, record four bars from the computer keyboard, play them back | The first lesson's task can be done without the mouse | |
| A5 | In the mixer, Tab to a fader, change it with the arrow keys, type a value | Narrator reads the fader's name and its value in decibels as it changes | |
| A6 | Export the song (Ctrl+Shift+R) with the keyboard alone | The export window's fields are read and reachable | |
| A7 | Gear > Theme: the high-contrast theme. Look at every view | Nothing is unreadable or invisible | |
| A8 | Make the interface smaller with Ctrl+- down to 75 %, then larger with Ctrl+= up to 200 %, on a 1920 x 1080 screen | Nothing overlaps or is cut off at either size | |

## Other programs (roadmap step S3.3)

Needs Bitwig Studio or Studio One (a demo will do).

| # | Check | Proves | Checked on |
|---|---|---|---|
| D1 | Open Exodus (Help > Example Songs), add one third-party VST3 with a changed setting, save, then run Export as DAWproject... | A `.dawproject` file is written | |
| D2 | Open that file in Bitwig Studio or Studio One | The tracks, clips, notes, tempo changes and the audio of Phazite's own devices are there, in time; the VST3 loads with its setting | |
| D3 | In that DAW, change the VST3's setting and a few notes, export a DAWproject, and open it with File > Import DAWproject... | The notes and tempo come in as a new project. (Plug-ins and sends are not imported yet) | |

## Installing and updating

| # | Check | Proves | Checked on |
|---|---|---|---|
| I1 | Build the installer and run it; start Phazite from the Start menu; double-click a `.phazite` file; uninstall it | It installs without administrator rights, runs, opens its projects from Explorer, and leaves projects, presets and settings behind | Checked by the owner |
| I2 | Download the installer in a browser from a release and run it | Waits for roadmap step 4.3: without signing, Windows SmartScreen warns | |
| I3 | With an older release installed, Help > Check for Updates..., Download and Install, Install and Restart | Phazite closes, updates silently and opens again. Waits for a first release (step 4.5) | |

## The first hour (phases 0 and 1)

| # | Check | Proves | Checked on |
|---|---|---|---|
| F1 | On a PC that never ran Phazite, start it and follow the Welcome window | Sound within a minute, without help. Best done by someone new to Phazite | |
| F2 | The usability test of phase 1: a newcomer and an FL Studio user each make an 8-bar beat from a song outline, with drums, bass and chords, mix it with Phazite's devices and export it. Watch, and write down every hesitation | Where newcomers and FL Studio users trip | |
| F3 | Work in a project for a few minutes, then end `Phazite.exe` in Task Manager and start it again | Phazite offers the work as it was just before; accepting it loses nothing | |
| F4 | Leave Phazite open and stopped with a song loaded; watch its CPU in Task Manager for a minute | Near zero at rest | |
| F5 | From the browser, lay out a beat of one-shots and MIDI clips using the Smart Tool and the keys only: move, copy with Ctrl, trim, split, duplicate | The playlist's editing feels right at speed; each drag is one Undo | |
| F6 | New project with the Electronic outline; put clips in the first Drop; drag the Drop's section elsewhere | Seven sections; everything in the Drop moves with it | |
| F7 | In the piano roll write a bass line and chords; use a chord stamp, Quantize and Transpose; open a drum sampler's clip | Writing is quick; drum mode shows the pads by name | |
| F8 | Build a mix with a bus (a folder with an effect) and a reverb return with sends from two tracks | The mixer shows what the playlist has; the return is heard; the loudness meter reads | |
| F9 | Search the browser for "kick", move through results with the arrow keys, press Enter; preview a loop while the song plays | A kick is placed in seconds; the loop previews in time | |
| F10 | Export a song to WAV with Normalize at -14 LUFS; measure the file in another program | The file plays and reads -14 LUFS, under its true-peak ceiling | |
| F11 | Make a whole track from first idea to master, with automation, modulation and your own plug-ins (the check of phase 2 as a whole) | Nothing stops a finished song | |

## Editing and devices built in phase 4

| # | Check | Proves | Checked on |
|---|---|---|---|
| E1 | Sampler: drop a drum loop on it, choose Slice, play the keys from C2; run Slices to Clip | Each hit on its own key; the clip plays the loop in time | |
| E2 | Sampler: drop an 808, play it across the keys, hold one note and play another in mono | In tune on every key; the pitch glides; a held note loops without a click | |
| E3 | FM synth: play the sound a new one starts with; step through the eight algorithms; play a high note loudly | An electric piano; every algorithm about as loud; no harsh aliasing | |
| E4 | Tempo lane: Add Tempo Change twice, drag a ramp from 90 to 120 BPM over a bar, with a loop that follows the tempo under it | Every hit of the loop stays on its beat through the ramp; each edit is one Undo | |
| E5 | Audio clip editor: drag a warp marker on a drum hit; then Quantize Audio on a loosely played loop | The hit plays where it was dropped; the loop's hits land on the grid. (Heard only once the clip's render finishes: step S5.1) | |
| E6 | Select two clips on a track and press Ctrl+J; then a range across two tracks | One clip on each track, as one Undo; audio without the track's effects | |
| E7 | Group two effects (Ctrl+G), add a chain, split by frequency bands; mute and solo a chain | The bands add up to the sound; each chain's volume, mute and solo are heard | |
| E8 | Instrument container with two synths split by keys | Low keys play one, high keys the other; a held note ends on the instrument it began on | |
| E9 | Drum sampler: drop a General MIDI drum file on its track; give a snare two velocity layers; press Own Track on the kick and choose it as the Sidechain of a Compressor on the bass | Every drum on its note; soft and hard hits differ; the kick ducks the bass | |
| E10 | Browser: filter to kicks in one key; start Swap on a pad while the beat plays, move through sounds, Enter, then again and Esc | The list narrows; each sound is heard in place; Enter keeps, Esc puts back | |
| E11 | Switch between Phazite's, FL Studio's and Ableton Live's keymaps (gear) and try each one's keys for play, tools, split and duplicate; run each of Help > Lessons | The keys match what those DAWs' users expect; every lesson can be finished | |
| E12 | The presets added since phase 2 (the Sampler's, the FM synth's, the classic drum machines' kits, the Chorus's, the dark synthwave set, the Exciter's and the Vinyl's), each played alone | Each sounds like its name, at an even level | Checked by the owner |
| E13 | Phase 2's steps: automation lanes and write modes, modulators and macros, containers and presets, the Devices view and the inspector, the audio clip editor, the chord lane, the mixer's sends, Out menu and sidechains, freeze, bounce and stems, plug-in windows and presets, the wavetable synth, the filter types, Low Latency, time-stretching | Each step's "Done when" as it was written ([archive](archive/roadmap-2026-11-13.md)) | Checked by the owner |

## What stage 1 fixed (2026-11-14)

Each has its test; these rows are what a test can't hear, see on a real screen or try with another program.

| # | Check | Proves | Checked on |
|---|---|---|---|
| F1 | Play a held pad or a sine clip; press Stop in mid-note, click elsewhere in the ruler while it plays, and loop a part that cuts it | No click at the stop, at the jump or at the loop's end | |
| F2 | Put a plug-in with a long look-ahead on the master, turn the click on with a count-in, and record MIDI to it | The click is in time with the song, and the recorded notes land on their beats | |
| F3 | Hold a key on a MIDI keyboard and unplug it; hold a key and select another track while the song plays a long note on the first | The held note ends; the song's own note plays on | |
| F4 | With a real effect plug-in on a return (a limiter, a wet reverb), end its PhaziteHost.exe in Task Manager while the song plays, then reload it | It fades out without a click and comes back without one; nothing passes through dry | |
| F5 | Export a song that plays a sampler plug-in which streams from disk | No note is missing: the plug-in was told that it renders offline | |
| F6 | End Phazite.exe in Task Manager with plug-ins loaded | No PhaziteHost.exe is left | |
| F7 | Record or import a few gigabytes into a new project, then Save; press Cancel half-way, then Save again | A progress window shows; Cancel leaves no folder; the second save finishes | |
| F8 | With Auto Save on, open a project from a USB drive, change it, pull the drive out, then quit | The question offers Save As; saving elsewhere then quits | |
| F9 | Keep a project in a OneDrive or Dropbox folder and Save As to another drive | Every file of Audio and Versions is in the copy | |
| F10 | Save a Drum Sampler preset with samples of your own; copy the preset and its "Sounds" folder to another PC and load it | The pads play; the preset file holds no user name | |
| F11 | Double-click a `.phazite` file in Explorer while Phazite's Settings are open | It opens once Settings closes | |
| F12 | Make a reversed clip, then make the project's Cache folder read-only and change the clip; make it writable again | The clip shows a warning bar and the audio clip editor says the audio couldn't be prepared; then it renders by itself | |
| F13 | On a precision touchpad: pinch in the playlist and the piano roll; scroll with two fingers over a note's stem in the velocity lane, then press Ctrl+Z once | The view zooms about the pointer; the whole burst is undone by one Undo | |
| F14 | Drag a fader while the song plays with its track in Latch, and press Escape before letting go; the same with the loop's edge in the ruler and with a section | Each is back where it was, nothing is written and nothing is left to undo | |
| F15 | Drag an audio file over an instrument track, a folder and a return | The line shows where a new track will be made, and the hint bar says why | |
| F16 | Close Phazite maximized on a second monitor and open it again; unplug that monitor and open it again | It opens where it was; then on the main screen, whole | |
| F17 | Press Escape during a long stems export; then close the export's window during one | Escape asks first; closing stops it and a notice says so | |
| F18 | With the computer keyboard on, give the lower panel a window of its own and play A S D F in it; close that window with a key held | The notes play, and the held note ends | |
| F19 | Remove a drum sampler that has two pads on tracks of their own, one of them with a clip; press Ctrl+Z | The empty pad's track goes, the other stays as a plain track; Undo brings both back as pads | |
| F20 | Switch a bus with volume automation to a plain folder and back, while it plays | The automation plays throughout | |
| F22 | Open a project with several heavy VST3 plug-ins | The window answers at once; the Project view lists each plug-in as loading; each sounds as it arrives | |
| F23 | Press Ctrl+S while a plug-in is busy (a sampler loading a large library) | The window keeps answering, and the save finishes when the plug-in answers | |
| F24 | Open plug-in windows at 75 % and at 200 % Interface Size, a resizable one several times | Each window is its plug-in's own size, with nothing cropped and no black border | |
| F21 | After the first push: read CI's `linux-clang` job, the nightly `windows-asan` job and the real-time audit on Linux | Each ran, and what it found is fixed or in known-bugs.md | Claude, 2026-11-15 (CI run 43709371891 and nightly run 43709371062, on dab254bd) |

## What the known-bugs round fixed (from 2026-11-15)

Each has its test; these rows are what a test can't hear or see on a real screen. The number after G is the bug's in
the [changelog](changelog.md).

| # | Check | Proves | Checked on |
|---|---|---|---|
| G116 | Drag an EQ dot, a Multiband crossover and a reverb edge, each taken at the edge of its handle; click each once | No jump at the press; the handle follows the pointer's movement; a click changes nothing and adds nothing to Edit > Undo | |
| G140 | Press Escape in the middle of a drag in a device's graph, and of a knob's modulation ring | The value goes back to where it was, and Edit > Undo has nothing new | |
| G140b | On the Filter, turn the wheel, then at once drag the dot and press Escape | The dot goes back, and Edit > Undo shows "Change Drive" alone | |
| G140c | Put a track in Touch with a ramp on a synth's resonance, play, and hold the filter's dot still | The resonance stays where it was grabbed until the dot is let go | |
| G140d | Click a pad on the drum sampler, click its waveform, press Up | The pads move a row: the waveform took no keys | |
| G118 | Turn the wheel over an EQ dot, then away from the dots | Over a dot the Q changes, as one undo step; away from them the dock scrolls | |
| G120 | Play a sine panned hard left, then one out of phase between the channels, through the EQ | The analyzer shows each as loud as when it is in the middle | |

## Known bugs that need an ear or a real plug-in

Numbers are those of [known-bugs.md](known-bugs.md).

| # | Check | Proves | Checked on |
|---|---|---|---|
| K34 | Vinyl on a bright sound, Mix at 50 % | Whether the mix hollows out the highs (bug 34 is not measured) | |
| K32 | Compressor in FET, turn Drive up slowly from 0 on a loud drum loop | Whether the step at the start of Drive's range is heard | |
| K36 | In a song with an audio file of several minutes, click far ahead in the ruler while it plays | How long the silence before the sound is | |
| K42 | Export a song with the slowest plug-in you own | That no real plug-in keeps one block for 30 seconds, which would end its helper | |

## Performance on other machines

Numbers and how to run the benchmarks are in [performance.md](performance.md).

| # | Check | Proves | Checked on |
|---|---|---|---|
| M1 | Run the benchmarks on a mid-range laptop, on battery and on power | The targets hold away from the development PC | |
| M2 | Time a cold start with a stopwatch: from the double-click to the window answering | The whole start, which the benchmark times only from inside | |

## Linux (roadmap step S8.3)

Needs a Linux desktop (Ubuntu 24.04 is what CI builds on). The app has never been run on Linux; only its tests have.

| # | Check | Proves | Checked on |
|---|---|---|---|
| L1 | Build `linux-release` and start the app | A window opens, in Inter, with menus and popups in the right place | |
| L2 | Choose an audio output and play an example song | Sound, without dropouts | |
| L3 | Scan for plug-ins and load a Linux VST3 | It plays in its helper process, and its window shows inside Phazite's | |
| L4 | Help > Phazite Sounds: download the pack | The download works over HTTPS and the sounds show in the browser | |

ASIO is a registered trademark of Steinberg Media Technologies GmbH. VST is a registered trademark of Steinberg Media
Technologies GmbH.
