# Changelog

What was built, newest first, one line a step: its number, its title and what it gave the user. Phazite has no release
yet, so the entries are grouped by the phase or track of the plan they belong to, not by version; a date is the day
the step's last commit was made. From the first release on, entries are grouped by version (roadmap step 4.5).

"Built" here means that the step's tests pass. Whether a person has tried it is in the [hand checks](hand-checks.md).
The full text of every step, as it was written when it was built, is in the
[archived roadmap](archive/roadmap-2026-11-13.md); what comes next is in the [roadmap](roadmap.md).

## The quality audit's 16 priorities (2026-11-13, commit 3b70986)

A whole-project audit listed about 300 findings. Its 16 worst were fixed, each with a test that failed before:

- **Saving:** a file is written whole or not at all, so a full disk no longer leaves a project file cut short; a
  project restored from an autosave or a backup counts as unsaved until it is saved.
- **Undo:** adding or removing an automation lane no longer makes a later Undo take the wrong clip or send.
- **Source control:** a folder taken away by a merge or a restore leaves its tracks; no ID is used twice after a
  restore; a clip moved between tracks, or a device grouped into a container, is taken as a move.
- **Plug-ins:** a plug-in that crashes reloads with its settings; a render fails, naming the plug-in, in place of
  waiting without end; no note is left sounding.
- **Engine:** moving the playhead while recording keeps the take and starts another; loops no longer repeat or drop a
  note or a click; a device stays awake for its tail; what is not sound never reaches the outputs.
- **Devices:** two crashes fixed (the wavetable synth's unison, the dynamics curve); the compressor's drive no longer
  dulls the treble; a Mix below 100 % no longer combs the highs in the Saturator, the Filter and the Compressor.
- **Editing and the mixer:** with Snap on, a move keeps an item's offset from the grid; a drag that copies and ends
  where it began leaves nothing; the piano roll shows and changes only the notes its clip plays; a soloed return hears
  its sends; Bounce in Place takes the fader's automation along.

## After phase 4's features: rounds of fixes and audits (2026-11-10 to 2026-11-13)

- **A sidechain's bus and solo** (2026-11-13): a bus that is a sidechain's source keeps feeding it while another track
  is soloed or the bus is muted.
- **The analyzer's lows** (2026-11-13): below 150 Hz the EQ's analyzer shows a bass note as a narrow peak.
- **Typed points** (2026-11-13): an automation point's value and a tempo change's tempo can be typed over the point;
  linked clips share the piano roll's view.
- **The window, after an audit against other DAWs** (2026-11-13): each editor's tools in its tab row, the playhead's
  place and time in the transport bar, notices in cards in place of dialogs, an Undo History, a drag's readout in the
  hint bar.
- **Typing, meters, Duck** (2026-11-12): typing in a field plays no notes; a meter reads on its fader's scale; Duck
  looks ahead; a bus can be a sidechain's source; a commit can be checked out or heard against the song.
- **Scrolling without flicker** (2026-11-11): the time line scrolls on whole pixels inside its own windows, so nothing
  beside it flickers.
- **JUCE workarounds** (2026-11-11): every place Phazite works around JUCE is listed and marked in the code.
- **Source control as people use it** (2026-11-11): Undo Last Commit, Edit Commit Message, a commit opened to show what
  it changed, Merge Branch, Delete Branch and Local History.
- **Duck, and sidechains inside devices** (2026-11-11): the Compressor's Duck mode with its own history; a device's
  sidechain picked inside the device; Exodus played on presets made after its sounds.
- **Audio device notices** (2026-11-10): a card offers to switch to an audio device that was plugged in.
- **Typed values** (2026-11-10): every knob and fader takes a value typed over it; a knob double-clicked back to where
  it started is no change.
- **A bug pass** (2026-11-10): a container no longer writes past its buffer when the device's blocks grow, with the
  other bugs the pass found; grid lines and clip edges no longer wiggle as the song scrolls.

## Phase 4: toward a release

- **4.4 Documentation, begun** (2026-11-09): the guide's first pages (Getting started, Source control,
  Troubleshooting, Questions, Privacy), published by Phazite's website. Still open.
- **4.13 The sounds of dark synthwave** (2026-11-10): the Exciter and the Vinyl, a plate in the Reverb, a chorus of
  many voices, natural drum synthesis with the Dark Synthwave Kit, sounds made from their spectra, the wavetable's
  Stack and Drift, every note on a voice of its own, and the example song Exodus played on the presets made from it.
- **4.12 The drum sampler** (2026-11-09): a pad on each of 128 notes, each with an envelope, a filter, up to four
  layers, and optionally a mixer channel of its own.
- **4.11 Chains, splits and instrument containers** (2026-11-09): containers of parallel chains that layer a sound or
  split it by frequency, mid and side or left and right, and instrument containers split by keys or velocity.
- **The format audit** (2026-11-09): the project file and the versions' store made sturdier and written down for
  other tools, with test vectors and an independent reader in CI.
- **4.9 Recording, warping and consolidating** (2026-11-08): recordings that grow in the playlist as they come in,
  arming on every track, Consolidate, warp markers on the hits in a clip's audio, and Quantize Audio.
- **4.8 Tempo changes along the song** (2026-11-08): a tempo lane whose tempo holds, jumps or ramps; samples follow it;
  tempo maps come in from MIDI files and go both ways in DAWprojects.
- **The preset pass and the Chorus** (2026-11-08): presets after the classic synths and drum machines, named by their
  sound and leveled alike; the Chorus effect.
- **4.7 FM synth** (2026-11-07, its editor rebuilt 2026-11-08): four operators in eight algorithms, each with
  feedback, shown side by side.
- **4.6 Sampler** (2026-11-07): one sound played across the keys in three modes (Classic, One-Shot, Slice), with its
  root key found as it loads and Slices to Clip.
- **The browser's filters, keys, Swap and collections** (2026-11-07; the first part of issue #24): filters under the
  search, keys read from names and loop data, sounds and presets tried in place as the song plays.
- **Clips and the playhead** (2026-11-07): clip tags, the chord lane as a guide shown while the song has chords,
  Normalize, and a playhead that glides with the display's frames.
- **4.2 Licences and notices** (2026-11-07): every third-party notice next to the app, in the installer and in the
  About window; no ASIO® or VST® logos.
- **4.1 Installer** (2026-11-07): a Windows installer and uninstaller with file associations, and an update check
  that is off until turned on and verifies what it downloads.
- **The DSP audit** (2026-11-07): true peaks by ITU-R BS.1770, band-limited oscillators, drive that does not alias,
  matched EQ bands, and files at another rate resampled by r8brain-free-src.

## The window's look (2026-11-05 to 2026-11-06)

- **The look:** dark and light themes after VS Code's, cards, frosted glass behind menus and pickers, Phazite's logo
  and a startup sound.
- **The left panel:** an activity bar with the Browser, the Project view and Source Control.
- **The Devices view:** effects stacked down a column, the one clicked beside them; the whole time line in miniature
  as the editors' scroll bar.
- **The piano roll:** changes from a comparison with what users praise and dislike in other DAWs' piano rolls.
- **JUCE 9.0.5,** for its hardened file readers.

## Source control (2026-11-04 to 2026-11-05)

- **Git's words** (2026-11-05): the view as Source Control, with Commit, branches switched in place, and staging.
- **D1, D2 DAWproject** (2026-11-04): the song, or a commit of it, exported as a DAWproject, and one imported.
- **Clean Up Unused Audio** (2026-11-04): removes the audio files that no version of the song plays.
- **V3, V5 Bounces** (2026-11-04): a commit rendered in the background and heard in place of the song.
- **V4 Branches and merging** (2026-11-04): a three-way merge by IDs, with its clashes shown.
- **V2 Going back** (2026-11-04): the song, or one line of what changed, restored from a commit.
- **V1 Commits** (2026-11-04): the song saved in pieces, read again and compared with the song, track by track.

## Architecture track

- **A6 Phazite's own engine** (2026-10-30 to 2026-11-04), in seven parts:
  - **T6** (2026-11-04): the build on JUCE alone; audio and MIDI files, settings and every ID Phazite's own.
  - **T4** (2026-11-04): the audio device and MIDI inputs opened and remembered by Phazite.
  - **T5** (2026-11-03): previews, export, stems, freeze and bounce rendered by Phazite's engine.
  - **T3** (2026-11-02): every plug-in in a helper process; one that crashes or hangs says so, with Reload.
  - **T2** (2026-11-02): the playback engine, its strips on several cores, and recording to the sample.
  - **T1** (2026-10-30): the song model: the project file is the song alone, and undo is the song's own history.
  - **T0** (2026-10-30): golden renders of everything that plays, and tests that build songs as the UI does.
- **A5 Parts of the engine replaced** (2026-10-30): presets in Phazite's own format, the selection by IDs, waveforms
  and sections of Phazite's own, and modulation computed inside each block.
- **A3 Phazite's types at the UI boundary** (2026-10-16): the UI reaches the model by IDs and plain values only.
- **A2 One path for every edit** (2026-10-15): the UI starts no undo step; a drag is one step.
- **A1 The model untangled** (2026-10-15): folders by area, no window code in the model, one settings list.
- **A0 Safety net** (2026-10-14): the golden tests and `tools/check-architecture.sh`.

## Phase 3: polish

- **3.6 Accessibility pass** (2026-11-04): a name and a role for every control, keys that reach every view, contrast
  tested against WCAG 2.2.
- **3.5 Performance pass** (2026-11-04): every target measured, the gaps written down.
- **Feedback rounds** (2026-10-30 to 2026-11-03): one switch for mute and solo, lessons in a card, views that move
  between the two areas, empty new projects with example songs, envelope curves, compressor characters, one
  Multiband, slide notes, continuous scrolling.
- **3.3 Stability** (2026-11-02, as T3): plug-ins in helper processes, a scan with a time limit, a plug-in that
  crashed as it loaded kept off.
- **3.7 Keymaps and help** (2026-10-29): Cut, Copy and Paste, FL Studio's keymap completed, an Ableton Live keymap,
  help for every command and control, five lessons.
- **3.4 Audio recording** (2026-10-29): arming, monitoring, punch-in, takes round a loop, a recording correction.

## Phase 2: finish a track

- **2.17 Devices and the window** (2026-10-29): a picture for every device, Instrument and Effects tabs, the Phaser,
  the Flanger, the Vocoder and the Stereo Expander, sidechains chosen in one place, the title bar and the status bar.
- **2.18 Time-stretching** (2026-10-28): clips stretched and pitched by Rubber Band's R3 engine, every drum hit once
  and on its beat.
- **2.16 Performance** (2026-10-21): benchmarks, a Low Latency switch, the latency shown, devices that sleep on
  silence, a playlist that makes components only for the clips in view.
- **2.15 More filter types** (2026-10-18): notch, a 24 dB slope, vowel, phaser and sample-and-hold in every filter.
- **2.14 Comb filters** (2026-10-18): a comb in the synths and the Filter.
- **2.13 Wavetable synth** (2026-10-18): two wavetable oscillators with warps, unison and the user's own tables.
- **2.12 Plug-in windows and presets** (2026-10-18): windows that remember their place, plug-in presets, Send All
  Keys to the Plug-in.
- **2.11 Freeze, bounce in place and stems** (2026-10-18).
- **2.10 More devices** (2026-10-18): Filter, Saturator, Delay, multiband dynamics, and unison for the Synth.
- **2.9 Mixer depth** (2026-10-18): a send knob for every return, an Out menu, a sidechain picker, CPU for each track.
- **2.7 Chord lane and progressions** (2026-10-18): chords as Roman numerals in a key, and 40 progressions.
- **2.6 Audio clip editor** (2026-10-17): a clip's edges, fades, gain, pitch, reverse and tempo.
- **2.5 The window** (2026-10-17): the playlist or the mixer in the centre, the Devices view, an inspector of what is
  selected.
- **2.2 Automation that goes with the music** (2026-10-17): automation moves and copies with its clips.
- **2.4 Containers and presets** (2026-10-16): devices grouped with macros; device, container and track presets.
- **2.3 Modulators and macros** (2026-10-16): six modulators dragged onto any knob, with a ring that shows the amount.
- **2.1 Automation lanes** (2026-10-16): any parameter automated in two clicks, with touch, latch and write.

## Phase 1: make a beat

- **1.9 First launch** (2026-10-15): the welcome window, the guide in the hint bar, the new-project window.
- **1.8 Export** (2026-10-15): WAV and FLAC, normalized to a loudness under a true-peak ceiling.
- **1.7 Transport, MIDI recording and Capture** (2026-10-15): metronome, count-in, tempo and time signature,
  recording with overdub, and Capture of what was just played.
- **1.6 First devices** (2026-10-15): drum sampler, Synth, EQ with an analyzer, Compressor with a sidechain, Reverb
  and Limiter.
- **Feedback rounds** (2026-10-13): the loop as FL Studio's song mode, the mixer redesigned, mute and solo as one
  control, and tests that drive the window with the mouse.
- **1.5 Browser:** places, search, tags, favorites, previews in time, the plug-in picker, Phazite Sounds.
- **1.4 Mixer:** strips, sends and returns, folder buses, a master with a loudness meter.
- **1.3 Piano roll:** FL Studio's mouse, the velocity lane, ghost notes, scales, chord stamps, drum mode, transforms.
- **1.2 Sections and song outlines:** sections that move everything in them, and outlines to start a song from.
- **1.1 Playlist:** the time line, tracks and folders, clips, the Smart Tool and the single-key tools.

## Phase 0: foundations

- **0.7 Audio, MIDI and the computer keyboard:** the audio and MIDI settings, and the computer keyboard as a piano.
- **0.6 The window and the design system:** panels, themes, design tokens, Inter, base controls, the hint bar.
- **0.5 Commands, keymaps and undo:** every action a command, the command palette, named undo steps.
- **0.4 Projects, saving and autosave:** the project folder, autosave, backups, recent projects.
- **0.3 Real-time audit:** a test that counts what the audio thread allocates.
- **0.2 Engine setup:** no personal data in a saved project.
- **0.1 Code structure and tests:** `app`, `model` and `ui`, a test runner in CI on Windows and Linux.
- **Before the plan:** the build, the Windows setup script, CI, the git hooks, and a smoke-test app.

ASIO is a registered trademark of Steinberg Media Technologies GmbH. VST is a registered trademark of Steinberg Media
Technologies GmbH.
