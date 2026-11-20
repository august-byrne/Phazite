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
| H7 | Play a loud click from a speaker into the microphone while recording the metronome's beat; compare the recorded click with the grid, then set Recording Correction in the settings (Recording) | The recording lands on the beat once the correction is set | |
| H8 | Pull the interface's cable while the song plays, then plug it in again | Phazite does not crash; a card in the bottom right corner offers to switch to the device | |
| H9 | Plug in a MIDI keyboard with an instrument track selected, and play | Notes sound at once, with no setup; velocity is heard | |
| H10 | Press R, play eight bars on the keyboard with the sustain pedal, stop | The notes are in a clip, in time. (The pedal is not recorded yet: roadmap step S5.3) | |
| H11 | Without recording, play a phrase, then press Ctrl+Shift+C | Capture turns what was just played into a clip | |
| H12 | Turn on Low Latency in the status bar and play the keyboard through a track with a Limiter | The delay between key and sound gets shorter | |
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
| G-scroll | Help > Example Songs > House Groove, in a `windows-profile` or release build. Play it, and while it plays drag the thumb of the overview along the time bar's top, slowly and fast; turn the wheel with Shift held; drag the rows' scroll bar at the lanes' right edge; hold the middle button on the lanes and move | The song glides under a slow drag of the thumb rather than stepping, keeps up with a fast one, and rests sharp when the hand stops; the thumb stays under the pointer; the rows and the pan follow the pointer without stutter | |
| G116 | Drag an EQ dot, a Multiband crossover and a reverb edge, each taken at the edge of its handle; click each once | No jump at the press; the handle follows the pointer's movement; a click changes nothing and adds nothing to Edit > Undo | |
| G140 | Press Escape in the middle of a drag in a device's graph, and of a knob's modulation ring | The value goes back to where it was, and Edit > Undo has nothing new | |
| G140b | On the Filter, turn the wheel, then at once drag the dot and press Escape | The dot goes back, and Edit > Undo shows "Change Drive" alone | |
| G140c | Put a track in Touch with a ramp on a synth's resonance, play, and hold the filter's dot still | The resonance stays where it was grabbed until the dot is let go | |
| G140d | Click a pad on the drum sampler, click its waveform, press Up | The pads move a row: the waveform took no keys | |
| G118 | Turn the wheel over an EQ dot, then away from the dots | Over a dot the Q changes, as one undo step; away from them the dock scrolls | |
| G120 | Play a sine panned hard left, then one out of phase between the channels, through the EQ | The analyzer shows each as loud as when it is in the middle | |
| G60 | Merge a branch with a conflict, save, quit, reopen. Then merge again, close with Don't Save, reopen. Then press Ctrl+Z right after a merge | First the merge and its conflict are still there, and Save Version makes the merge's version; then no merge waits; then the conflict list goes and Save Version is greyed | |
| G60b | Merge, save a version, then close with Don't Save and reopen | No notice about a merge, and Cancel Merge is greyed | |
| G93 | Arm a synth track and record from a MIDI keyboard: hold the sustain pedal over a short note, move the mod wheel and the pitch bend, stop with the foot still down; play the clip, then a later clip | The note rings until the pedal comes up, the vibrato and the bend sound as played, and the later clip's notes don't ring on | |
| G94 | Record onto a nearly full USB stick, or unplug the project's drive while recording | Recording stops with "Recording stopped" and where it ends; the clip plays to there. With little space, Record warns first | |
| G95 | Record audio through an interface, and change its sample rate, then its buffer size, in its control panel during a take | Recording stops with a notice each time, and the clip plays at the right speed | |
| G103 | On a Bluetooth or other late output, in Touch, move a fader on a drum hit while playing; play it back | The automation's change lands on the hit | |
| G38 | While recording, drag the top bar's position field, then click in the ruler | One jump where the field is let go, no stray short takes; after the click the sound from the first instant is in the new take | |
| G163 | Hold a fader while recording and press Stop, then Escape. And: loop on, a track in Write, count-in on, press Record and stop after two beats | The take's clip stays, and Undo removes it alone. The lane after where playback stopped is as it was | |
| G32 | Compressor in FET, then in Tube: turn Drive up slowly from 0 on a loud drum loop; then on a drum bus turning down about 6 dB with Drive at 0 | No step as Drive leaves 0 or as the compressor starts turning down; the colour at 6 dB sounds as it did before 2026-11-15 (against an older build) | |
| G32b | Compressor on a loud loop with Drive at 50 %: switch its character between Clean, FET, VCA, Opto and Tube while it plays; automate Drive from 0 to 60 % on a beat with Tube | No jump in level and no thump at a switch; the drive is there on the beat | |
| G34 | Vinyl with Bass Mono at 100 Hz on a bass line and a bright sound: Mix at 50 %, then turned from 0 % to 30 % and back, then Bass Mono switched on and off | Neither the bass around 100 Hz nor the highs hollow out against Mix at 100 %, and nothing clicks | |
| G74 | Multiband on a full mix: switch a band's saturation on and off while it plays | No click | |
| G110 | Help > Keyboard Shortcuts: type "undo"; turn on Record Keys and press Up; select Select Track Above, Add Key..., press Ctrl+Shift+K, Assign; restart Phazite | The rows narrow as you type; Up lists Select Track Above, Transpose Up and Raise Automation Points; the card says who else has the key; Up still selects tracks; the new key is still there after the restart | |
| G112a | Windows set to French (AZERTY), computer keyboard off: press `&`, Ctrl+`&`, `-`, Shift+6; turn the computer keyboard on (M) and play Q S D ... L, then M | Smart Tool, finer grid, zoom out, Mute Tool; the notes rise in the piano's shape; M turns the computer keyboard off | |
| G112b | German layout (QWERTZ): press `+`, Shift+0, Ctrl++, Alt++; AltGr with `+`; computer keyboard on: the key labelled Z, then Y | Zoom in twice, larger interface, taller tracks; AltGr types `~` and runs nothing; Z plays G#4, Y moves the octave down | |
| G112c | Czech layout with the Ableton Live keymap: Ctrl+Alt+3, then Ctrl+Alt+Shift+3; the `+` key; Shift with it. Spanish: Ctrl+Alt+4 | Ctrl+Alt+3 does nothing (a dead key), Ctrl+Alt+Shift+3 shows the piano roll; `+` zooms in; Shift with it is the Smart Tool. On Spanish Ctrl+Alt+4 does nothing and View > Instrument works | |
| G112d | Linux, X11: `setxkbmap us,de`, German active, computer keyboard on: the key labelled Z, then Y; number pad `*`. Then `setxkbmap us,cz` (or `us,ru`), switch to it, open Settings > MIDI and press the key right of L | Z plays G#4, Y moves the octave down, number pad `*` runs nothing; the help names that layout's letters and the key plays E5 | |
| G146 | Hover the EQ's Frequency knob, the Vinyl's Low Cut, the History header in the Versions view and the Project view's Notes header | The hint bar describes each, in words that fit (the owner may reword them: open question 56) | |
| G172 | Save a version of a project with large new recordings, then quit at once; open the project again | A window "Finishing the versions of ..." shows until the version is written, and the version is there | |
| G126 | In the Versions view, click an older version in a large project; with the Project view open on a project with several branches, edit freely | The window doesn't freeze, and the version's lines appear a moment later; nothing stutters, and the count of unused files still follows | |
| G61 | With the Project view open while a snapshot is being written, look at Clean Up | It stays in place, greyed, and its tooltip says why | |
| G70 | A Synth pad with a long release: hold 16 notes, then play an 8-note chord on top | The chord comes in without clicks | |
| G71 | Drum Sampler with the Trap Kit: roll the 808 pad (note 41); turn Cut Itself off and roll again. Then fast hats over long crashes until all 32 voices are busy | Each hit cuts the one before, with no pile-up and no click; with it off the hits overlap. The oldest crash fades out instead of clicking | |
| G72 | While a Sampler note holds, drop a new sound on it | The old note fades out with no click, and a new note plays the new sound | |
| G135 | Wavetable with Basic Shapes: sweep Osc 1's Warp Amount with Sync, Bend + and Pulse Width on a held low note, a scope on the master. Then a held chord on a new FM | The wave stays centred, with no thump as notes start or end; the FM shows no slow wobble around zero | |
| G127 | In a song of 200 tracks (or 40): drag a fader and turn the wheel to scroll the mixer sideways in mid-drag; Tab past the last strip in view; drag the mixer's scroll bar quickly end to end; with three returns, turn R2's send to R1 while R1 sends to R3 | The fader keeps following and Undo puts it back; the mixer scrolls to the next strip; no stall, strips may appear over a frame or two; R3's send knob to R2 goes | |
| G144 | In the same song: scroll the playlist down and up, and drag its scroll bar end to end; Tab through the headers' mute switches past the bottom; drag a header below the bottom; add notes in the piano roll; drag a clip from a muted red track to a blue one | Headers appear with no flicker or gap; the rows scroll with the focus; the tracks scroll and the header drops at the end; only that clip redraws; the clip turns blue and stops looking faded during the drag | |
| G165 | Open a few real VST3 windows (one docked), close them and close the project. Press Space and a note key with a plug-in's window focused | Each closes at once, and no plug-in crashes or leaves a window behind; Space plays and the note sounds | |
| G166 | Turn a knob in a VST3's window and press Ctrl+D on its track, or Save Version, at once; do the same with a plug-in that is busy (a sampler loading) | The copy or the version holds the turned value; with the busy plug-in the window holds still and a progress window shows after a second, whose Cancel changes nothing | |
| G168 | With unsaved changes and a plug-in's window open, start a Windows restart; then a sign-out; then cancel a shutdown at its "apps preventing shutdown" screen and save; then run the installer's update while Phazite runs | "Save changes?" shows before Windows ends Phazite, each time; after the cancelled shutdown the save asks the plug-ins again; the installer's request asks to save as Quit does | |
| G42 | Freeze a track with a very heavy plug-in | The bar reads "Waiting for <name> (N s)", and Cancel stops it at once | |
| G78 | Play a long pad with a reverb; mute the track in mid-chord, then unmute. Mute a track with a send before its fader to a reverb return. Bounce a synth track in place | The chord and its tail come back at once; the reverb of the muted track goes on while the dry track is silent; the bounced original shows muted and frozen, and Undo brings it back live | |
| G167 | While a loop plays: add a plug-in or a limiter with latency to one track, bypass it and switch it back; delete a track, take a send off, move a track into a bus | A short dip and no click for the latency; for the other edits the other tracks play on without any dip or click | |
| G36 | With an audio clip of several minutes, click far ahead in the ruler while it plays | The clip comes in within a few milliseconds, faded, with no click | |
| G171 | Put an LFO on the EQ of a silent track and open the EQ | The band's dot and the knob's ring move | |
| G122 | In a clip of a few thousand notes (and in several clips of one alias group): drag 500 selected notes, hover over notes and empty rows, draw a selection rectangle across the clip, Select All, then Transpose, Quantize, Duplicate and Delete, each with its Undo | Each answers at once, and the right notes move, in every alias | |
| G20 | In the piano roll, with a clip right before another, select notes that run to its end and press Ctrl+D | Only the copies that fit are made, the hint bar says how many weren't for about six seconds, and Edit > Undo names the step for the copies made | |
| G174 | Set a track to Write, hold its pan knob, press Space, move the knob, press Escape while still holding | The knob goes to the lane's value and that is what plays | |
| G169 | Make a new project, save a few versions, press Save; open the Versions view while it saves | The progress window shows and Cancel works; the button reads "Saving the project...", Merge is greyed, and a bounce under way starts again once the save is done | |
| G123a | Drop a ten-minute recording on the playlist and, at once, press Ctrl+Z, click and drag; then press Ctrl+S while its window shows | Nothing happens until the clip lands, selected; after a second "Importing ..." shows with Cancel (Cancel leaves nothing); Space still plays and stops; the save's wait appears only after the import ends | |
| G123b | Normalize and Quantize Audio on that clip; delete the clip in the first second of one | Each shows its progress after a second and is one undo step; deleting the clip meanwhile gives a notice that nothing was normalized | |
| G123c | Drop a long WAV on a drum pad, on the sampler and on a wavetable; quit during an import | The window holds still, then the device gets the sound; Phazite quits without waiting for the import | |
| G128 | Add a folder of tens of thousands of sounds and type in the browser's search; type a broad word, then a folder's name | Keys never stutter and results follow a moment later; a broad word ends the list with "The first 200 of N"; a folder's name lists its sounds, with the folder itself | |
| G85a | On a big song at a 128-sample buffer: press Play, and jump around the ruler while it plays | No click and no spike of the CPU meter at the start or at a jump | |
| G85b | In a 48 kHz project, jump into the middle of 44.1 kHz audio clips while playing | The sound comes in a few milliseconds late, without a crackle | |
| G173 | Hold a chord on a plug-in synth with the mod wheel on its filter and up in the clip, and jump back into the clip while it plays; start inside a bent note; play aftertouch into a plug-in that uses it, record it and play it back | The filter is where the clip has it at once; the note starts bent; the pressure is heard live and from the take | |
| G176 | Record audio, hold a fader, press Stop, then record again further on | Both recordings show, each with its own waveform, until the mouse goes up | |
| G126b | In a big project with two branches, switch to the other and press Space at once; then again, and press Escape at once | Play and stop work, edits don't; after a second a window says "Reading branch '...'" and its Cancel leaves the song and the branch as they were; Escape at once changes nothing and no window appears | |
| G126c | Start Normalize on a long audio clip, then a branch switch; quit while a switch or an import is reading | The two progress windows show one after the other, never on top of each other; Phazite closes without waiting, and after Cancel at the save question the Versions view says "Stopped, as Phazite was closing" | |
| G126d | Import a large .dawproject (File > Import, and as a branch), once to the end and once with Cancel | The progress bar moves; Cancel leaves the open project as it was, no new branch and no unpacked audio in the project's folder; the notice comes only when the import is done | |
| G126e | Name a track "CON" and record on it; save a project as "NUL"; drop a track named "COM1" into the browser | Each saves: "CON_ Take 1.wav", a folder "NUL_", a preset "COM1_" | |
| G184 | In a project with a version saved, close Phazite, replace the .phazite file's text with garbage and open it again | "Phazite couldn't read the project" offers the backup and the last version; Open the Last Version opens the saved song, shown as changed; with a long history the window takes no input while it reads and shows "Reading the copies of ..." after a second, whose Cancel offers the backup alone or says the error | |
| G183 | Drop a drum-kit preset saved with its sounds outside the project onto a track; then swap kit presets and click a knob on another device | The window holds still, a big kit shows "Loading ..." with Cancel after a second (Cancel leaves nothing), then the kit appears and its pads play; the tried kit stays, and shortly after Undo shows "Copy Sounds into the Project" | |
| G-save | Make a new project, add a short sound, Save; then the same with the sound held open by another program that locks it | It saves as before, with no progress window, the title showing the new name within a moment; with the file locked the save fails naming that file, and nothing is left in the projects folder | |
| G-chips | In a big sound library, click the Type chip, then type in the search and click Key | Each menu opens at once with counts for what shows | |
| G177 | With a plug-in's window focused, switch the keyboard layout (to AZERTY, say), then press Space, the & key and a Shift shortcut | Play and stop and the shortcuts run, and the plug-in gets the other keys | |
| G166a | Open a big, slow plug-in's window; click its button again while it opens, then once more | After 5 s the button says it is still opening; the second click stops waiting; the third opens it | |
| G166b | Type a value into a plug-in's knob while its helper is busy; then type one while recording, and with a track in Write | After a second a window says Phazite is waiting for the plug-in, and Esc or its Cancel shows the old value; while recording or writing the typed value lands | |
| G181 | Scan a shell plug-in file (Waves' WaveShell) | All its plug-ins are listed | |
| G165L | Linux: open a plug-in's window and close it | It closes at once, the plug-in keeps running, and it opens again | |
| G-folder | Add a folder of 20,000 or more sounds as a browser folder, wait for indexing, open it and its subfolders; with it open, add a WAV to it in Explorer and switch back to Phazite within 2 s; delete a sample in Explorer and open its folder again | The folders open at once and the window never stalls; the new sound shows in place without reopening, the selected row and open subfolders stay, the search finds it and the Type filter counts it; the deleted one's row goes | |
| G-drive | With a user folder on a network drive that is asleep or unplugged, open it; open other folders, search and press Down in the search field; press Enter on the slow folder, then click another row; quit with nested folders open inside it and start again | After about 3 s its line says "This drive isn't answering..."; everything else works at once, and the folder fills in when the drive answers; the Enter is dropped; the same folders are open again once the drive answers | |

## What stage 4 built (from 2026-11-17)

| # | Check | Proves | Checked on |
|---|---|---|---|
| D1 | In the Versions view, click the play mark of a version that has a bounce; look at Hear It on one without | The first plays in place of the song at once (the transport bar says Hearing); the second is greyed, "No bounce: Open Version plays it live" | |
| D2 | Merge Branch... from a branch that changed two tracks; click a line, then Merge | The list shows both lines with their letters and no tick boxes; a click changes nothing; Merge takes both | |
| D3 | Create a branch, rename it "Half time chorus", and reopen the project | The picker, the branch button and the status bar show "Half time chorus", spaces and all | |
| D4 | Read the Versions view from top to bottom: its menu, the field, the button, the sections, a version's right-click menu, a line's tooltip | Every word is one a producer would use: Save Version, History, Switch Branch, Restore This Version; nothing says commit, checkout or staged | |
| D5 | Turn a Filter's Cutoff knob slowly from left to right | Even octave steps all the way: the middle reads about 632 Hz, a quarter about 112 Hz, and the bottom of the knob is no longer a dead stretch | |
| D6 | Automate a Filter's Cutoff from the bottom of its lane to the top over 4 bars and play | An even sweep, with no rush at the end; the line drawn is the line heard, and the lane's middle reads about 632 Hz | |
| D7 | Select a cutoff's point and press Up; then right-click it, Type a Value..., and type "2 kHz" | Up moves it a hundredth of the knob's travel, not 200 Hz; the typed point sits at 2.00 kHz, and the knob shows 2.00 kHz there while playing | |
| D8 | Put an LFO at a fifth of its depth on a cutoff set to 200 Hz, then the same on one set to 5 kHz | The same number of octaves either way | |
| D9 | Help > Example Songs: listen to House Groove's chord filter, Lo-fi Beat's Muffle macro turned fully, and the filter sweeps of Exodus' intro | Each sounds right for its song; the house chords' filter LFO was fitted again and reaches up to half an octave elsewhere at one end than before | |
| D10 | Automate an effect's Mix and a Filter's Cutoff, and play at an audio buffer of 441 or another odd size, then at 512 | The same sound at both | |
| D11 | Save a project many times over a few days of work, then look into its Backups folder | Backups from the last two hours, one an hour for the last day and one a day before that; saves that changed nothing left no copy; a file put there by hand is still there | |
| D12 | Send a track to a reverb return and draw the send's lane from the bottom up to 0 dB over a bar, then play; in Touch, hold the send's knob while it plays; put an LFO on the send; stop | The reverb swells in with the line and the knob turns with it; in Touch the reverb follows the hand and the lane takes over on letting go; the LFO makes the reverb pulse; after Stop the knob stays where the song stopped, and shows the lane's value at the playhead once the playhead is put somewhere by a click in the ruler | |
| D13 | While a send with a lane at its bottom plays into a reverb, delete the lane | The reverb comes up to the knob's level smoothly, without a click | |
| D14 | Export as DAWproject a song with a volume fade from 0 to -24 dB, a bent pan line and a send ramp, and open it in Bitwig Studio or Studio One | Halfway through, the fade reads about -8.6 dB, as Phazite's fader does; the pan's bend keeps its shape, and the send's lane is there | |
| D15 | Bounce in Place a track that has a send before the fader to a reverb; then Undo | The reverb is as loud as before the bounce, not louder; the send is on the new track; Undo puts it back on the original | |
| D16 | Delete a branch that has a version and changes left on it, then open the Versions view's menu > Recover Lost Versions...; pick its row and switch to the branch it makes | The row says the branch was deleted just now, with the changes left on it; the song on "<name> (recovered)" is as it was left | |
| D17 | Delete an audio file that a version plays, then run Verify History; run it on a big project and choose Stop Verifying History, or save a version while it runs | Progress shows, then the version is named with the file that is missing; stopped, it says so and never says the history is whole | |
| D18 | Open a project whose versions an earlier build saved | Its branches are listed and their versions can't be read (known bug 197); the song opens, Save Version works, and nothing in its `Versions` folder was deleted | |
| D19 | In a big project, save a version, then edit: move clips, drag notes in a long clip, turn knobs; watch the Versions badge, the star in the status bar and the changes list | They follow a moment after each edit, and the window never stalls; a drag shows as one change when it ends | |
| D20 | Copy the project file, save a version, close, put the copy back over the file and open it | The Versions view says the branch has 1 newer version; Save Version is greyed with its reason; Take the Newer Versions brings it in, and Ctrl+Z takes it back | |
| D21 | With the project in a synced folder, save a version on each of two computers before the sync catches up, then open it on one | The view says the other computer's version went its own way and offers to merge it; after the merge and Save Version both computers' work is in, each item once | |
| D22 | Save a version, change a volume, press Ctrl+Z, type a name and press Save Version at once | Nothing is saved; the view says "Nothing changed since the last version." and the typed name is still in the field | |

## What stage 6 built (from 2026-11-18)

| # | Check | Proves | Checked on |
|---|---|---|---|
| E1 | On a wide pad or a drum loop, add Stereo. Turn Balance fully right, then double-click it. Switch Mono on and off, then Swap, while it plays. Load its preset Checks > DC Filter after a Saturator set to Tube | Balance: the left goes silent, the right is no louder, and a double-click reads Center. Mono: the image collapses to the middle, the bar under the picture reads +1, no click. Swap: left and right trade places, no click. DC Filter: switching it clicks nothing. The editor shows whole in the dock: Width over Bass, Sides over Output | |
| E2 | Add Motion to a track playing chords. Click its Chorus, Flanger and Phaser tabs while it plays. Open Presets > Swap and try a Chorus, an Ensemble, a Flanger (Jet Sweep) and a Phaser (Classic Phase) preset in place. In Flanger or Phaser, turn Mix to 50 %, then 100 %. In Chorus, turn Feedback below 0 | Each tab switches without a click or a dip, and its picture moves with the sound. Each preset sounds as its old device did. At 50 % the flanger or phaser is whole (sound and copy half and half); at 100 % only the copy plays (a phaser without feedback is then flat, and its picture shows it flat), and one chorus voice is a vibrato. Feedback below 0 rings as loud, hollower | |
| E3 | Saturator on a bass, Drive +12 dB: turn Colour from 0 to 100 %. Choose Crush and sweep the Drive from bottom to top. Switch the Quality while it plays; on a bright synth driven into Fold compare 2x with 8x | Colour: warmer, the curve leans, no click, and turned up fast from 0 no swell in the lows. Crush: coarser the harder it is driven, the curve in steps. Quality: no click, and 8x has less grit than 2x. A click between shapes on a bass through Tube makes no thump | |
| E4 | Saturator on a vocal or a full mix: switch Band on, From 3 kHz, Drive up, Output +6 dB; drag both edges in the picture, and press Escape during a drag; switch Listen on, then show another device. Load the presets under Saturator > Exciter on a vocal, a snare and a mix; play Exodus and listen to its supersaw | Brighter highs with new harmonics, the lows untouched; the edges drag and Escape takes a drag back; Listen plays the band alone and the whole sound is back within half a second of the editor hiding. The presets sound as the Exciter's did (Vocal Air, Snare Crack, Top End Restore) | |
| E5 | In the Wavetable: slot 1 LFO 1 on Pitch at 4 % with LFO 1's Fade In at 1 s, and hold notes. Glide 150 ms with Glide Mode on Legato: play detached, then overlapping notes. Slot 1 Mod Wheel on Mod 2 Amount at 50 %, slot 2 LFO 1 on Pitch at 0 %: move the wheel. A slot from Drift to Cutoff at 2 % on a resonant low-pass: play a chord. Play Basic Shapes' saw above A6 | The vibrato grows over a second, anew with each note. Detached notes jump, overlapping ones glide. The wheel brings the vibrato in. Each note's tone wanders on its own. The high saw is clean, with no whistle under it. In the Matrix, a slot's own amount is greyed in its target list | |
| E6 | A pad with a Compressor whose Attack was changed: choose Sidechain > Kick on its Compress page. On a drum loop: the curve's menu > Shapes > Gate under -40 dB, Attack 0.1 ms, Release 0.5 ms, then Hold at 0 and at 60 ms; or the preset Gate > Drum Gate. On a snare: Attack 1 ms, Look-Ahead at 0 and at 5 ms, dragging the knob while it plays | The mode stays Compress, the pad dips with each kick by what the curve says, the history shows the kick's line, and Listen plays the kick. The gate: at 0 a tom's tail chatters, at 60 ms it opens once and closes cleanly, and each hit's start comes through whole. Look-Ahead: at 0 each hit's first click gets through, at 5 ms it is already turned down; the drag doesn't click | |
| E7 | Compressor's Transient tab on a drum loop: Punch to +100 % and -100 %, Sustain both ways, then a held pad; switch the three tabs while it plays. On the Compress tab: right-click a point > Type Values... > "-24, -20"; Tab to the curve, Right to choose a point, Ctrl+Right and Shift+Up, then Ctrl+Z; the same in a Multiband's band | Punch snaps the hits or softens them, Sustain lengthens or tightens the tails, the pad's level doesn't move, and no tab's switch clicks or jumps in level. The point goes where typed and the corner reads it with its line's ratio; the arrows move it by a tenth and by a whole decibel, and one Undo takes the whole run back | |
| E8 | Age's presets under Tapes: Clean Reel on a mix, Warm Cassette on keys, Worn Cassette, Chewed Tape, Tape Saturation on a drum bus and on a bass. On a held chord, click each part's switch off and on (Wobble, Noise, Drive, Stereo, Band) and switch the Record and Tape tabs. Under a deep Wow, turn the Rate up fast | Clean Reel: a little warmth, faint bright hiss. The cassettes: pitch drift, a rounded top, dips that lose their highs first, more of each as they wear. Tape Saturation: thicker, rounded highs, no wobble or noise, and a bass stays saturated, not louder. Each switch and the tabs: no click, no jump of the pitch; a part shows faint while off. The Rate: the pitch bends a little, never a swoop. The tape's picture (for your eye): the reels turn faster with the Rate, and the tape's line breaks at the head as a dip comes | |
| E9 | Delay's presets Clean > Wide Slap on a mono vocal, Ambient > Ducked Throw on a vocal, Creative > Dub Swell on a snare (turn its Feedback past 100 % for a few bars, then back). Feedback 50 %: play a phrase, press Freeze, play other notes, release. A held synth line: switch Note between 1/8D and 1/4 with Transition on Fade, then on Repitch. Turn Mod Rate while a Wobbly Tape echo plays | Wide Slap: two slaps, left then right, no wobble. Ducked Throw: the echoes stay low under the singing and rise in the gaps. Dub Swell: the echoes swell, hold without running away and fall again, and die away by themselves at its own setting. Freeze: the last echoes repeat as they sounded, unchanged for as long as it is on, the new notes not added; no click either way. Fade: the time changes with no bend and no click; Repitch: a tape's swoop. Mod Rate: the wobble speeds up, with no warble | |
| E10 | EQ > Dynamic > De-Ess on a vocal. Mid-Side > Wider Top on a stereo pad, then Stereo's Mono after it. A bell on Mid, +6 dB at 3 kHz, on a mix. Give a band a Dynamic (right-click its dot > Make Dynamic) and drag the hollow handle; press Escape during a drag. Set a band to Mid, another to Left, and change a placement while it plays | De-Ess: each "s" comes down, the band's mark and the curve dip with it and are back before the next vowel, no lisp and no click. Wider Top: a wider top that in mono sounds as without the EQ. The Mid bell: the middle brighter, hard-panned parts as they were, the dot reads its number and M. The handle: the Dynamic's knob follows, Escape puts it back, and a handle close to its dot can still be taken. Two curves show for Mid and Side (and two more for Left and Right), named at the top right; a placement changes without a click | |
| E11 | The two synths side by side, before the old one goes: build commit 4bcb99e5 (`git worktree add ../Phazite-two-synths 4bcb99e5`, then configure and build there), and on one track load a preset from Synth, on another its copy of the same name from Wavetable; play the same notes. Start with DCO Bass, Duo Lead, Warm Pad, Drone, Soft E-Piano with the mod wheel up, and Square Pluck. Then play House Groove and Lo-fi Beat | Each copy sounds as its original: the level, the tone, how it opens and closes, its glide and its vibrato. The pads drift a little differently from note to note, as they do on the old Synth from one playing to the next. Soft E-Piano's tremolo at full wheel and Pulse Organ's sweep at full wheel are the two known to differ. The songs sound as they did | |
| E12 | Reverb > Halls > Large Hall on a snare loop, then on a vocal. Halls > Endless: one clap, and let it die. The same clap into the Reverb's Room with Decay at 20 s. Halls > Chamber on a snare. Change Mode between Room, Plate and Hall while a tail rings | Large Hall: a smooth, wide tail, no flutter and no metal. Endless: the clap fades for 20 s without a pitch, a beat or a flutter in it. The Room at 20 s rings like a pipe: that is what the Hall is for. Chamber: is it a small bright space, or too large for its name? (its lines are longer than the Room's.) A change of Mode fades without a click | |
| E13 | Filter > Driven > Acid Squelch on a bass line of sixteenths: sweep the Cutoff, then turn the Drive up. Driven > Self-Oscillating Sine on an empty track with the song stopped: wait, then turn its Cutoff, down to 50 Hz too. Driven > Screaming Sweep on a loud saw, then Character to Clean on the same settings. Switch Character while a pad plays. A bright saw at Drive 24 dB: Quality 2x against 8x | Acid Squelch squelches at the cutoff and gets louder and thicker as it is driven, never quieter. The sine starts by itself within a few seconds at any Cutoff and stays in tune as the Cutoff moves. Screaming Sweep is louder than the same band-pass in Clean and screams at the cutoff. A change of Character fades without a click. 8x has less grit that doesn't follow the note than 2x | |
| E14 | Synth, Basic Shapes: LFO 1 at 25 Hz in a slot on Volume at full amount, and hold a note; then on Position; then on Pitch. A mono patch, legato between two keys | No buzz under the note in any of them (before stage 6 a fast LFO stepped 3,000 times a second and buzzed). The legato change of pitch is clean | |
| E17 | Synth: LFO 1 at 25 Hz on the Cutoff of a low-pass with the Resonance near 80 %, and hold a note. Osc 1 Warp on Sync with Envelope 2 on its amount, C6 held, at Quality 1x, 2x and 4x. Hold a chord and switch the Quality | No buzz under the note. The sync sweep whistles less at each Quality (the whistles that move against the sweep are aliases). The chord stops cleanly, without a click, and a key pressed just after plays | |
| E18 | Turn the wheel several notches on any knob, then Ctrl+Z once. The wheel on the Compressor's Look-Ahead while a drum loop plays. Right-click a device's switch (the Compressor's Auto Makeup, a part's title switch) and choose Automate; then let the lane flip it with the editor open | The knob goes back to where the burst started, in one step. One short gap after the burst, not one for each notch. The switch's lane shows and the switch did not flip at the right-click; as the song plays, the switch flips on screen with its lane | |
| E19 | Filter, a low-pass at 1 kHz: turn the Mix down. Compressor > Compress: put a point at -30 dB and drag it; open Shapes > Gate. A snare through the Limiter. During silence, select another device and come back. EQ: a band on Side under a wide pad. Multiband > Monitor: click to pause during a loud part, click again in a quiet one | The cut fills in on the picture as it does in the sound (about -6 dB left at 50 %). The point sits well inside the graph and is easy to take; the gate's step shows near the bottom left. Each hit's reduction shows in the history, however short. Back on the device, its history and meters are empty, not full of what played before. Two spectra, the middle's and the side's, in their curves' colours. The Monitor goes on from where it stopped, without a wide band of one level | |
| E20 | Synth > Bass > Acid Line: play a line and turn the Resonance up until it sings; hold a note and switch the filter's Character between Clean and Driven. Lead > Singing Filter played as a melody. Turn Osc 1's Warp amount with Sync, Bend, Pulse Width and Mirror chosen, then put Envelope 2 on it and hold a note. A slot from LFO 1 to the Cutoff, with one of Phazite's LFOs on the Cutoff too | The squelch turns into a scream without a click or a jump of level; the switch of Character has no dip and no ping. The filter's pitch is right on every key, its vibrato fades in, its glide is smooth. The wavetable's picture repeats, bends, narrows and mirrors with the knob, and sweeps while the note plays. The Cutoff's knob shows a dashed ring inside the solid one, and a dot that stays inside the dashed ring on high and low keys | |
| E21 | Click beside the Filter's dot, then drag from there. Shift-drag an EQ dot, pressing and letting go of Shift on the way. Drag a Delay's first echo, then double-click it. Right-click a synth envelope's point > Type a Value..., 50 ms. The wheel over an EQ dot, then over empty graph. A drum pad pressed near its bottom, then near its top | Nothing moves at the click; the dot follows from where it was. A fifth as far with Shift, no jump as Shift changes. Time and feedback back at their defaults (Source Control shows no change against a new Delay). The attack is 50 ms. The hint names the Q and it turns; elsewhere nothing turns. The pad sounds on the press, soft then full | |
| E22 | Drag the Devices view narrow on the Synth until its pages don't fit; choose a page from the More tab; click a page tab and press Space. Make the view short on the Saturator until Band is under a tab, and click its switch there. Look at the parts' titles | The tabs stay whole and a More tab lists the others, the one shown ticked and named on the tab. Space still plays the song. The Band switches from its tab, whose name dims while it is off, without changing the part shown. Titles in Title Case, faint while a part is off | |
| E23 | Synth presets: Keys > Soft E-Piano soft then hard; Keys > Pulse Organ with the mod wheel; Pad > Choir Pad with the wheel to full; Lead > Scream Lead and Bass > Ladder Bass; Lead > Sync Lead high with the wheel up. Compressor > Transient > More Punch on a drum loop. EQ > Dynamic > Boom Control on a bass. Delay > Creative > Stutter Hold: Freeze on at a phrase's end. Motion > Subtle Chorus | A bell at the start, a bark when hit hard. A steady tone; the wheel brings a slow sweep. Vowels, never a whistle. Bite and thickness at the loudness they had. A clean sync sweep. Sharper attacks, tails as they were (it peaks 4 dB over the loop: is that too much?). Only the boomy notes tamed. The last sixteenth repeats. Is Subtle Chorus subtle enough? | |
| E15 | EQ, Phase on Linear. A kick through a 48 dB low cut at 100 Hz, Linear against Minimum. A snare on two tracks (or a copy of one), one of them through a Linear EQ with a low cut and a bell, then the same in Minimum. EQ > Linear Phase > Master Air on a whole song. Switch Phase while the song plays. A bell's Gain swept by a lane in Linear | Linear: a faint swell just before each kick, none in Minimum, the low end thinned alike. The two snares add up full in Linear and thin or hollow in Minimum. Master Air: the rumble goes, a little air comes, the punch and the image stay; the editor shows the latency (85 ms at 48 kHz) and every track stays in time. The switch of Phase makes a short gap (the device is made anew) and no click after it. The swept bell moves in time with its lane, without steps or clicks | |
| E16 | A Filter on a track with no sound: Clean, Resonance at 100 %, and a lane on its Character that switches to Driven a few seconds in. Play; then export the same bars. A Delay at 2 s, Repitch: change its time while a click plays, then Freeze | The filter starts to sing at the lane's switch, live as in the export. The delay's echoes land on the new time, and the frozen loop is as long as the time shown | |

## What the owner's round after stage 6 built (from 2026-11-19)

| # | Check | Proves | Checked on |
|---|---|---|---|
| X1 | Right-click the master volume, a mixer fader, an EQ dot, a knob and a view's tab | Each menu opens at the pointer | |
| X2 | Right-click an effect's name in a mixer strip and point at Replace; pick one. The same on an instrument | The list opens without a click, and the pick replaces the device | |
| X3 | A mixer strip with a Compressor, an EQ and an effect with a long name | The green switches are in one column | |
| X4 | Drag the piano roll's velocity lane's edge up and down | "Velocity" is drawn once, never left behind | |
| X5 | Look at the inspector's title beside the browser's | Same inset, height and size | |
| X6 | Look along the transport bar; click Low Latency beside the CPU in the status bar, and press its key | Even gaps in groups; Low Latency lights while on; A/B shows once there is a version to compare with | |
| X7 | Look at the playlist's area: its tabs and its tools | The tabs read as tabs, the shown one joined to its view; a thin line stands before the tools | |
| X8 | Compressor, and a Multiband's band: right-click a hollow handle; drag it up and down; look at the default curve's corner readout | The line goes straight with the handle on its middle; it bows toward the pointer, never past its points or the edges; the corner reads 4.0:1 | |
| X9 | Right-click a point of the curve | Its menu opens at the pointer | |
| X10 | Compressor in Duck under a kick at 120 and at 174 BPM, in the dock and in a wide window | Smooth dips with their attack, hold and release, no stairs, no notches to silence between kicks | |
| X11 | Load Compressor > Noise Gate, Drum Bus Glue and Gentle Master, and Multiband > Upward Squash, on what they are for | They sound as they did; the gates open and close where they did | |
| X12 | Browser: Instruments, click Synth's chevron; click its name; press Enter on it | Its kinds of sound and your own presets in one list, no "Phazite" heading; the name only selects; Enter adds a default Synth | |
| X13 | Save a preset from an EQ's Presets menu; drag a track into the browser; save a container of effects as a preset | The browser opens Effects > EQ at the new preset; Track Presets opens at the track; Container appears at the top of Effects | |
| X14 | A Synth's Presets > Swap, then Down several times | One list, each preset tried in turn | |
| X15 | Without Phazite Sounds installed, open Sounds; install them | A row that downloads them; then Drums, Loops and the rest, nothing named "Phazite" | |
| X16 | Put a preset file at the top of Documents/Phazite/Presets; with presets saved for a plug-in, run the app with that plug-in not scanned, then scan | Other Presets shows after Track Presets and holds them; after the scan the plug-in's folder is under its row, with no restart | |
| X17 | Save a preset from a plug-in's own window while its row is open under Effects | It appears in the row at once; what was open and selected stays | |
| X18 | Play the song with unsaved changes and double-click an older version; press Back to Now; press N several times while playing | It changes at once on the beat with no bounce, "Looking at …" in the view, the transport bar and the status bar, the title's star as it was; Back to Now gives the song, Undo and Redo as before; N switches with no gap | |
| X19 | With a version open: click a knob, scroll one at its end, press Escape in the middle of a drag; then drag a knob | Nothing happens for the first three ("Looking at …" stays); the drag shows "this is now the branch idea-N", Undo once returns to the version and is then greyed, Back to main gives the song | |
| X20 | With a version open: press Ctrl+S; close and open the project | The bar goes; the project is your song, never the old version | |
| X21 | With a version open: arm a track and record; drop a sound file on the playlist | The branch notice shows as recording starts and the take survives Ctrl+S; the clip lands on a new branch and the window still takes clicks | |
| X22 | A track in Latch, play, move a knob, press N; select a time range by hand, open a version, Back to Now; start a Swap on an open version | A/B is greyed, "Stop first", and after Stop the lane holds the move; the same range is selected; Back to Now reads "Finish swapping first" | |
| X23 | Settings: type "save" in the search; click Recording in the contents; scroll the list with the wheel across Recording Correction | Only the matching settings show, with counts in the contents; the list scrolls there and the marked group follows; the wheel scrolls past the field without changing it | |
| X24 | Settings, Audio, on real hardware: change the output device, the sample rate and the buffer while a song plays; Test Sound; with an ASIO interface, Driver Control Panel; plug a USB interface in and out with the window open; pick a device another program holds | Sound carries on at each change; a tone; the driver's window opens and the buffer shows after; the lists follow; the row says why and what played plays on, and Phazite starts with sound next time | |
| X25 | Settings: switch Theme to Light in its row, then back from the gear menu; change a setting and Reset Setting from its row's menu; Tab through the page | The window and the menu's tick follow each other; the thin line shows and goes; search, contents, then each row's control | |
| X26 | With any plug-in (best: one named as one of Phazite's devices, such as a VST3 called "Reverb"): save a preset from its window; look under the plug-in's row and under Phazite's device of that name in the browser, and in Swap on Phazite's device | The preset is in `Documents/Phazite/Presets/Plug-ins/<name>`, shows under the plug-in alone, and Phazite's device lists its own presets alone | |
| X27 | A song plays a sound file from its `Audio` folder, once plainly and once in a clip that is reversed or has a tempo of its own: write over that file with another sound from another program, wait a second, add a note anywhere | Both clips play the new sound (the reversed or stretched one after it shows "preparing" for a moment), and their waveforms show it | |

## What the owner's round of 2026-11-20 built

| # | Check | Proves | Checked on |
|---|---|---|---|
| Y1 | A sustained sound into a Reverb, then a Filter whose cutoff's lane goes from open at bar 1 to nearly closed at bar 3: play from bar 1 and press Space at bar 3; then click the ruler at bar 1; then play | The tail dies away dark, with no bright blip; the click moves the cutoff's knob to open; Play starts open | |
| Y2 | A kick on one track and a bass on another, Duck Under (bass under kick); set the kick's EQ Phase to Linear and play; switch Phase back and forth while it plays | The duck lands on each kick, with no late or clicky pumping; each switch is one short dip, no click, and the duck stays on the kick | |
| Y3 | While a song plays, choose a sidechain on a Compressor (its key's track with no latency), and take it away; delete the key's track | The Compressor's track plays on without a gap or a click each time | |
| Y4 | Press Ctrl+, : look at the whole window; resize the window; double-click the card's header; click Maximize | The window dims, its title bar and window buttons too; a centred card with the gear, "Settings", Maximize and Close, the keys in the search, which reads "Search settings"; the card follows the window; the header and the button make it as large as the window less a margin, and back | |
| Y5 | With the settings open: click the dimmed area; open them again and press Escape in the list, in a search with text, in the empty search | The click closes them and nothing behind reacts (no track selected, nothing muted); Escape closes from the list, clears the text and stays, and does nothing in the empty search | |
| Y6 | With the settings open: press Space and letters; Ctrl+S; F11; Ctrl+=; Alt+F4 | Nothing plays and no note sounds; the song saves; full screen; the interface grows; Phazite quits, asking about unsaved changes over the card | |
| Y7 | Open the Driver or Theme drop-down in the card; drag the line beside the contents, double-click it, close and open the settings; click the funnel and Modified; Tab from the search | The list opens over the card and picks; the width is kept, the double-click puts it back; only changed settings show, with their count; Tab lands on Clear or the funnel, the arrows move between them | |
| Y8 | Make the window narrower than about 880 px with the settings open | The contents hide and a button at the search's left lists the groups and Shortcuts | |
| Y9 | Put the settings beside VS Code's (Ctrl+, there) in the dark and the light theme; play a song and open the settings over it; with Narrator, open them | Same header, search, contents, titles, rows, tick boxes and drop-downs, but for the row of tabs (User, Workspace), which Phazite leaves out, and the icons; the playhead moves on behind, dimmed, and scrolling the settings stays smooth; Narrator says a dialog named Settings | |

## What One history built (2026-11-20)

| # | Check | Proves | Checked on |
|---|---|---|---|
| Z1 | Open a project with a few versions, work for half an hour with changes, and open the Versions view | No list called Local History; grey rows with a clock between the versions, each above the versions saved before it; the versions still read first | |
| Z2 | Click a row that says "N automatic saves"; click it again; press Return on it | It unfolds to a row for each, newest first, and folds again; the lines of the branches run through them unbroken | |
| Z3 | Point at an automatic save and click its arrow; press Ctrl+Z | The song is as it was then; Undo ("Restore the Automatic Save of ...") brings it back | |
| Z4 | The view's ... menu: untick Show Automatic Saves; look in Settings under Versions; tick it again there | The grey rows go and come back, and the menu and the setting follow each other | |
| Z5 | Switch to another branch, change something, discard it, and switch back | That automatic save's ring is in the other branch's column, and its row says "on" that branch | |

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
