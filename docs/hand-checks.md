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
| D1 | In the Versions view, double-click an older version that has a bounce, then one without | The first plays in place of the song at once (the transport bar says Hearing); the second is bounced first, the transport bar showing its progress, and then plays; the branch and the song's changes stay | |
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
| D12 | Send a track to a reverb return and draw the send's lane from the bottom up to 0 dB over a bar, then play; in Touch, hold the send's knob while it plays; put an LFO on the send; stop | The reverb swells in with the line and the knob turns with it; in Touch the reverb follows the hand and the lane takes over on letting go; the LFO makes the reverb pulse; stopped, the knob shows the lane's value at the playhead | |
| D13 | While a send with a lane at its bottom plays into a reverb, delete the lane | The reverb comes up to the knob's level smoothly, without a click | |
| D14 | Export as DAWproject a song with a volume fade from 0 to -24 dB, a bent pan line and a send ramp, and open it in Bitwig Studio or Studio One | Halfway through, the fade reads about -8.6 dB, as Phazite's fader does; the pan's bend keeps its shape, and the send's lane is there | |
| D15 | Bounce in Place a track that has a send before the fader to a reverb; then Undo | The reverb is as loud as before the bounce, not louder; the send is on the new track; Undo puts it back on the original | |
| D16 | Delete a branch that has a version and changes left on it, then open the Versions view's menu > Recover Lost Versions...; pick its row and switch to the branch it makes | The row says the branch was deleted just now, with the changes left on it; the song on "<name> (recovered)" is as it was left | |
| D17 | Delete an audio file that a version plays, then run Verify History; run it on a big project and choose Stop Verifying History, or save a version while it runs | Progress shows, then the version is named with the file that is missing; stopped, it says so and never says the history is whole | |
| D18 | Open a project whose versions an earlier build saved | Its branches are listed and their versions can't be read (known bug 197); the song opens, Save Version works, and nothing in its `Versions` folder was deleted | |

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
