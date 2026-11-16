# Changelog

What was built, newest first, one line a step: its number, its title and what it gave the user. Phazite has no release
yet, so the entries are grouped by the phase or track of the plan they belong to, not by version; a date is the day
the step's last commit was made. From the first release on, entries are grouped by version (roadmap step 4.5).

"Built" here means that the step's tests pass. Whether a person has tried it is in the [hand checks](hand-checks.md).
The full text of every step, as it was written when it was built, is in the
[archived roadmap](archive/roadmap-2026-11-13.md); what comes next is in the [roadmap](roadmap.md).

## The known bugs (from 2026-11-15)

The owner asked for every entry of [known-bugs.md](known-bugs.md) to be fixed before the hand checks of stage 3. Each
bug has a test that failed before its fix, and a reviewer read each area's changes; what the reviews found is fixed
too. A line names the bug's number in that list.

- **140 Escape in the devices' graphs:** Escape takes a drag back in every device's graph and on a knob's modulation
  ring, leaving nothing to undo or redo. Every graph takes its handles the same way now (`graph::HandleDrag`): a
  press grabs the parameter for Touch and Latch without changing it, a turn of the wheel just before a drag stays a
  step of its own, and the graphs are no stops for the keyboard's focus.
- **116 Handles that jumped to the pointer:** a handle in a device's graph moves as far as the pointer goes from
  where it was, and a click on it changes nothing; a drag along one axis leaves the other's value as it was, to the
  bit. The delay's first echo and the stereo expander's fan move from where they are set.
- **117 Drags under modulation:** a drag in a device's graph moves the value set by hand by the drag alone, whatever
  its modulation plays.
- **118 The EQ's wheel:** the wheel over an EQ band's dot turns its Q as one undo step until it rests, and elsewhere
  scrolls the view.
- **120 Analyzers and their taps:** the analyzers show each channel at its level (the louder of the two at each
  frequency), so a sound panned hard or out of phase reads as loud as one in the middle; the taps hold a quarter of
  a second at 192 kHz.
- **130 Curves worked out at every repaint:** the EQ's curves and the reverb's tail are worked out once for each
  change (a repaint of the EQ 0.92 to 0.55 ms, of the reverb 1.41 to 0.41 ms); the sampler's picture already kept
  its peaks and stays as it was.
- **22 A damaged commit restored:** a commit with an unreadable piece restores what can be read and puts no stand-in
  into the song; restoring one line takes nothing where its place in the commit can't be read, and says which piece.
- **23, 161 What a merge leaves behind:** restores, discards and merges let go of a sidechain that the result made
  into a loop, and a conflict answered for the other branch leaves out what that answer names and this branch deleted.
- **24 Moved here, deleted there:** a clip moved on one branch and deleted on the other stays where it was moved,
  with a note in the merge's list.
- **58 IDs across merges:** a branch merged again after it made an item with an ID an earlier merge gave one of its
  own keeps both items, each with its own ID; and where two computers merged the same commit with other IDs, the
  later merge's record wins by the graph, so later merges find each item once.
- **60 A merge waiting for its commit** is part of the song (a `MERGING` element, in
  [project-format.md](project-format.md) and [versions-format.md](versions-format.md)): Undo takes it back, Redo
  brings it back, it waits still in the saved project or its restored autosave, and none waits in a project closed
  without saving. No merge starts while another waits; Abort Merge keeps the song in Local History first, and takes
  out a merge record that waits for nothing; a merge the Versions folder can't write takes nothing and says why;
  Delete Branch is refused while a waiting merge took in a commit only that branch holds.
- **26 `getTracksInFolder()`** did count an empty folder inside a folder, as its callers need: only its comment read
  otherwise, and says so now.
- **38 The playhead moved while recording:** the take after a jump is made, its files open, before the jump is asked
  for, and starts where the playhead lands, to the sample; a drag of the top bar's place or time while recording
  moves the playhead once, where it is let go.
- **92 A note played early:** a note played up to a tenth of a second before a take starts, such as a downbeat played
  early after the count-in, starts on the take's first beat.
- **93 Controllers recorded:** MIDI recording and Capture keep the sustain pedal, the mod wheel, the other controllers
  and the pitch bend beside the notes, as a MIDI file's import does. Only notes clear what a take records over;
  controllers moved without notes go into the clips there, or a clip of their own where there are none; with Overdub
  a controller recorded replaces that controller's earlier values across its moves; a pedal still down at a clip's end
  is let go of there, but never between a looped take's parts; a take keeps the pedal held as it started however
  long it runs. A ten-second pitch bend finishes its take in a hundredth of a second.
- **94 A write that fails while recording** (a full drive) stops recording, keeps what was written and says so, and
  Record warns when the drive has room for less than ten minutes.
- **95 The audio device restarting during a take** (another rate, or another buffer) ends the take there, kept at the
  rate it was recorded at, with a notice.
- **103 Automation written where it was heard:** points land where the song was heard, allowing for the output's
  latency and the delay compensation; a jump while writing ends the pass there; the loop's times round are counted
  by the engine where it goes round.
- **163 Escape in Write** takes back the drag alone: what the pass wrote as the loop went round, as playback stopped,
  or as a take ended by itself, is a step of its own made once the drag has ended (`Gesture::whenEnded()`); a
  control grabbed before playback starts writes from where playback starts.
- **Recording kept when a project goes:** closing a project, quitting, Save As and an untitled project's first save
  stop recording first, as Stop does, and keep the take; a take's audio and notes are placed by the take's own tempo.
- **32 The compressor's Drive:** its curve fades in from a straight line over the first tenth of Drive's travel, and
  FET's and Tube's colour over the first 3 dB of reduction (whole from there, as before), so neither the first touch
  of Drive nor a colour taking hold makes the sound jump (2 dB before; 0.02 dB now). A change of character while the
  drive is heard cross-fades from one curve to the other, and the colour follows the reduction sample by sample, so
  it plays the same however the samples are cut into blocks.
- **33 Duck switched to Compress:** a compressor made for Duck that automation switches to Compress keeps its dry copy
  in phase with its driven sound at any Mix (12 dB down at 18 kHz before), and nothing is lost or played twice at
  the switch.
- **43 The compressor's latency:** its driven sound comes exactly as late as the latency it reports, 6 samples, made
  up by an all-pass (`dsp::AlignedShaper`); it was 5.137 against 5.
- **34 The Vinyl's Mix:** the copy for the mix takes the record's own linear path (its oversampling, its delay, and
  Bass Mono's all-pass while Bass Mono is on), so a Mix below 100 % keeps its bass and its top octave: it lost 17 dB
  at Bass Mono's frequency and up to 18 dB in the top octave. At 0 % only the phase is turned.
- **74 A Multiband band's saturation** turned on or off fades over 1.5 ms against the plain delay, after the saturation
  has settled, instead of dropping samples (an error as loud as the sound before; 66 dB under it now).
- **75 The delay's echo and the reverb room's lines** are exactly as long as set (one sample long before: 2 % at
  1 ms), and the room fades over its decay time, damped or not; the reverb editor's decay map follows each line's own
  damping, within a tenth of the measured decay, and costs a fiftieth of what it did at long decays.
- **134 The effects' control steps** (the Phaser, the Flanger, the Chorus and the EQ; the Synth's are still open) go
  by the song's own grid (`dsp::ControlGrid`), so what they play no longer depends on how the samples are cut into
  blocks, and a loop's second pass, a start anywhere and a wake from sleep play what an export plays.
- **110 The Shortcuts tab is Phazite's own,** after VS Code's Keyboard Shortcuts editor: search by a command's name or
  by a key pressed (Record Keys), a row for each key with its source, and a key given to a command stays with every
  other command that has it: the card says who else has the key, which of them runs when, and what a key the
  dispatcher matches another way would take from whom.
- **112 Other keyboard layouts:** the computer keyboard plays by the keys' places on any layout (the key that turns it
  off stays that command's); a key means what is printed on it first, then its digit where it sits at a digit's
  place (AZERTY's `&` is 1), then the character it types (keys that type = or + zoom; Ctrl++ and Alt++ added); a
  character typed with AltGr runs no Alt shortcut. On six layouts every command of the three keymaps has a key (one
  of Live's on Spanish excepted, and said in the guide), and what every key press of the US layout runs is a golden
  file (`tests/golden/keys/`).
- **146 Every control explains itself in the hint bar:** the EQ's band controls, the Vinyl's and the Delay's band
  ends, and the Project and Source Control panes' headers have their tooltips, and the walk of every control now
  fails on any that has none.
- **25 Another tool's numbers:** a song file another tool wrote, equal to its last commit, opens with no changes and
  no "As opened" snapshot: ranged numbers and devices' values are kept as Phazite writes them.
- **59 A branch whose last commit can't be read** says so and offers the newest commit that reads, from any
  computer's log of the branch or from the packs (Reset Branch to Last Readable Commit); nothing is committed on top
  without a word, and Commit is greyed meanwhile.
- **61 Clean Up Unused Audio** keeps the files that Local History, every computer's kept songs, commits that can be
  recovered, the staged song and a waiting merge's earlier song still play; it offers nothing while any of them
  can't be read whole (a version still syncing), and looks in the background.
- **62 A damaged piece** is read from another pack and written again by the next commit that holds it, and a branch
  isn't renamed while a commit writes its record.
- **172 A commit as the project closes:** a commit or a snapshot still being written as the project closes is
  finished in the background, the close waiting for neither; quitting says what it is finishing.
- **126, 129 The versions off the message thread:** the Source Control badge compares nothing while nothing changed
  and only the track that changed after an edit; opening a commit reads it in the background; the Project view
  finds the unused audio in the background, reading each kept song once. (Switching branches and starting a merge
  still read there: what is left of 126.)
- **35 Sounds with what isn't a number:** a sampler's, a drum pad's or a wavetable's sound file whose float samples
  hold what isn't a number plays those samples as silence, rather than silencing the whole mix while it plays.
- **70 A chord that takes several busy voices at once** in the Synth, the Wavetable, the FM or the Sampler fades out
  every voice it takes over 3 ms (16 places to fade in), where all but one were cut dead.
- **71 The Drum Sampler's voices:** a voice taken for a new hit fades out over 5 ms, the quietest dying one first,
  and a pad can cut its own earlier hits (a new switch for each pad, Cut Itself: on for the 808 Kit's kick and the
  Trap Kit's 808); a pad whose sound is swapped while it plays fades out.
- **72 Sounds and tables swapped while they play** are freed only once nothing can still read them, without waiting
  for the next one given, and never pile up on a track that isn't played; the sampler's old notes fade out over 3 ms
  however short the blocks.
- **73 Alike at every sample rate and in any blocks:** the synths' sustain glide, drift, noise and drive fade take the
  same time and level at any rate (noise is as loud per hertz, with 48 kHz, where the presets are leveled, as it
  was).
- **134 The synths' control steps** go by the song's grid too (the Synth, the Wavetable, the FM and the Sampler): the
  Synth plays the same samples however blocks and automation cut them (35 dB under its sound before), and a count-in,
  a loop's second pass and a wake from sleep play what an export plays.
- **135 Offsets under held notes:** Osc 2 Sync on the Synth and Sync, Bend and Pulse Width on the Wavetable leave no
  constant offset (up to 0.65 and 0.78 of the notes' level before; 0.0003 and 0.003 now), taken out where they
  arise; where a drive, Ring Mod or an FM warp makes one, a blocker takes it out, and only there, so clean patches
  play bit for bit as they did.
- **136 The FM's blocker** is 12 dB at 5 Hz: a new FM's electric piano leaves 0.0085 of its level under held notes,
  where it left 0.08. What a modulator a few hertz off a whole ratio leaves stays (it is what FM does), as the entry
  now says.
- **178 The live notes' timing tests** measure each note by the engine's clock and the blocks played, so a busy
  machine no longer fails them, and a note taken late or played off its place still does.
- **153 An assertion on the audio thread** fails its test by its place without the tests' net locking or allocating
  there, and the real-time audit says that an assertion fired instead of counting JUCE's message as playback's.
- **149 A damaged zip's dates** stay an assertion the DAWproject fuzz case expects, around its reading alone (JUCE
  workaround T8): a Release build makes nothing undefined of them.
- **150 Full Screen** is switched in a test, in a real window on Windows, and the main window's tests are compiled
  and run on Linux as well.

## Stage 2, the last step: the first push (2026-11-15)

- **S2.15 CI and the nightly workflow run for the first time:** the Linux builds (GCC, Clang, AddressSanitizer with
  UBSan, ThreadSanitizer), AddressSanitizer on Windows, the hour-long run and the fuzzer's 20,000 rounds each ran for
  the first time, and what they found is fixed. CI is green on every preset, and a whole nightly run passed on the
  same commit (dab254bd). In Phazite itself they found: copied automation that was empty in a GCC build, so Paste put
  none back; a stray "id" in a damaged project that was counted as the song's largest ID; a path with no drive from
  another computer's project that made JUCE assert on Windows (W37); a pack's empty piece that left every piece after
  it unread; a DAWproject's notes and audio a hair past a song's ranges; a number too long for JUCE's JSON parser to
  count, in a preset, a keymap or a download (every JSON text is now read through `parseJson()`, W38); and the status
  bar going through every device of the song four times a second, in silence too. The rest were the tests' own:
  what GCC and Clang refuse that MSVC takes, stand-in plug-ins the sanitizers reported, and tests that raced on a busy
  machine. For the next such finding: a test case that takes too long says where it is
  (`PHAZITE_TEST_STALL_SECONDS`), and the nightly workflow started by hand can run one job alone. Left: known bugs
  169 (a first save that, seldom, can't copy a file of the Versions folder) and 170 (an exponent of ten digits in a
  project file's number).

## Stage 1. Safe: the bugs that lose work or sound (2026-11-14)

Built on Windows. Each bug has a test that failed before its fix; a reviewer then read each part's changes, and what
the reviews found is fixed too. The numbers in brackets are those the bugs had in [known-bugs.md](known-bugs.md).

- **S1.1 Playback:** the click and the count-in come as late as the song's sound does; an audio clip moved or slipped
  under the playhead plays from its new place; the transport is state the engine reads, not a queue that could fill
  and lose a Stop; notes and the transport asked for during a render are kept; the engine's workers flush denormals;
  one output channel plays both sides; plans are freed only once a block that stalled has ended; notes held live end
  when live MIDI moves to another track or its input goes, and the song's own notes play on; an instrument holding a
  note never sleeps (37, 76, 77, 79 to 84).
- **S1.2 Fades:** 3 ms at Stop (the audio clips that were playing), at the loop's end (the clips it cuts) and at a
  jump (the whole mix, out and in). A render without a loop is bit for bit as it was.
- **S1.3 Plug-ins:** a block's deadline is a millisecond at least; a late, busy or crashed effect fades to silence
  in time with the song and fades back in, where it passed its input through dry; plug-ins are told that a render
  plays them offline, and that they play live again after it; a render waits for a late live block, fails at once
  naming a plug-in that had crashed, and never waits on a cancel; a crashed plug-in reloads at the engine's rate;
  only the channels handed over are read back; a VST3's switch is described as a switch (27 to 31, 64, 66, 141, 152).
- **S1.4 Plug-ins off the message thread:** as a project opens its plug-ins load in the background, beside each
  other, and the song plays and can be edited meanwhile, each plug-in silent in its place until it arrives and then
  fading in; a save asks every
  plug-in for its settings at once and never keeps the window waiting, and names a plug-in that didn't answer; Auto
  Save asks only the plug-ins that changed; closing waits for no helper (69, 89). Closing a plug-in's window, and a
  commit or a preset saved after a plug-in changed, can still wait (165, 166).
- **S1.5 Plug-in editors:** as large as the plug-in at any Interface Size (68).
- **S1.6 No helper left behind:** the helpers end with Phazite however it ends (a job on Windows, which leaves out
  what a plug-in starts; a watch of the parent on Linux and macOS), and POSIX shared memory is unlinked once the
  helper has opened it.
- **S1.7 Saving:** an autosave that can't be read gives way to the project's own file; a save that fails while
  closing offers Save As, Don't Save and Cancel; a backup that can't be made is told; the library's facts and the run
  marker are written whole (40, 41, 87, 88).
- **S1.8 Selection and undo:** selected automation points are held by their elements in the song, so points added
  before them, Undo and Redo leave the same points selected; the undo history keeps 1000 steps, set by
  `phaziteUndoSteps`, where a size budget left about 30 (53).
- **S1.9 The first save:** a new project's folders are copied beside the destination and renamed when whole, in the
  background with progress and Cancel when they are large; a failed copy says why and leaves nothing; a copy left by
  a crash is removed at the next save beside it (90).
- **S1.10 Renders and drags:** a clip render that fails stops, the clip says so, and it is tried again when its
  source or the Cache folder changes; a note dragged past its clip's end and back leaves the clip as it was (44, 86).
- **S1.11 The small bugs:** only tracks hold clips, and a paste that runs out of tracks adds them; a clip can't land
  on a track of the other kind; keys are spelled as their chords are; Spread uses the chord's own notes; Chop and
  Arpeggiate keep their remainder; Quantize takes the nearest swung line; typed lengths can be as short as dragged
  ones; Bounce in Place no longer collapses the original track; a Shift or Ctrl click on a distant track no longer
  scrolls back; the small ones in the lanes' gestures; the first lesson's second step; a preview stops when the left
  panel hides; a panel's own window plays the computer keyboard; a branch that can't be made renames nothing; Escape
  asks before it stops an export; the Stereo Expander's scope, the compressor's history and the random LFO are drawn
  as they sound (46 to 52, 54, 56, 57, 63, 108, 109, 111, 115, 119, 121, 138). The shortcuts editor that takes away
  shared keys (110) is left: it needs an editor of Phazite's own.
- **S1.12 References follow their targets:** what leaves the song takes what names it along in the same undo step
  (a removed device's modulations, the sends to a deleted return, a removed drum sampler's pad tracks); a folder
  switched between a bus and a plain folder keeps its volume's automation and modulation; bent automation keeps its
  shape through clip moves, Paste and the time edits; the automation of plug-ins that aren't loaded moves with the
  rest; a preset loaded while a track writes automation writes no lanes; a device on a drum sampler's track can't
  listen to its own pads (96 to 102, 104, 143).
- **S1.13 Escape:** takes back a drag of the ruler's loop and playhead, a section, a chord's edge and every control
  linked to a parameter, with nothing left to undo. The devices' graphs and a knob's modulation ring are left (140).
- **S1.14 The wheel and pinch:** the wheel acts by its distance everywhere; a burst over the velocity lane is one
  undo step, and where no note is it scrolls; pinch zooms the playlist and the piano roll about the pointer (55).
- **S1.15 The window:** opens where it was closed, maximized if it was, on a screen that still exists.
- **S1.16 Files and the library:** a project double-clicked while a dialog is open opens after it; the library's
  indexer skips folders that lead back into themselves; a preset keeps its sounds in a folder beside it, holds no
  path of the computer it was saved on, and says which sounds it couldn't take; Clean Up Unused Audio counts what the
  saved file, the autosave and the backups play; a DAWproject's files are unpacked once, Bitwig's clips in clips are
  read, and the import says what it left out (91, 107, 113, 114, 151).
- **S1.17 Build:** the folder beside the app is made the same as its sources at every build; line endings are LF by
  the repository's rules; a pushed tag must be the project's version (132, 133).
- **Guide notes aren't recorded:** a click in the piano roll, or a preset tried in the browser, no longer lands in
  Capture and in takes (147).

## Stage 2, the rest: the build (2026-11-14)

- **S2.13 Precompiled headers, AddressSanitizer on Windows, Clang:** JUCE's module headers are precompiled
  (`PHAZITE_PRECOMPILED_HEADERS`), which took a full Debug build on the owner's PC from about 28 minutes to about 6;
  CI builds without them, so every push also shows that each file includes what it uses. A `windows-asan` preset for
  the nightly run, and a `linux-clang` preset and CI job with clang-tidy; neither could be run where they were
  written, and both ran first with S2.15.
- **S2.5 The allocation guard:** counts what the engine's worker threads allocate too, and has a Linux half.
- **The nets' gaps closed:** the event queue, the handover, the playhead's stamps and the transport raced on real
  threads; saves that fail and first saves that fail part way; Cut, Copy and Paste of linked clips, audio, ranges and
  what Undo makes of them; the drum machines' pitches, decays and bands; the effect graphs' axes; the command palette
  and files dropped on a track; waits in the tests wait for a condition, with a limit only a hang reaches. Two tests
  that failed when every test file ran in one process pass (154).

## Stage 2. Caught: the tests that would have found the audit's bugs, and the docs (2026-11-14)

Built on Windows; the Linux builds, CI's new jobs and the nightly workflow first ran with S2.15, above. Each net was shown to hold
by breaking the code it guards and seeing its test fail.

- **S2.14 The docs as pages that say what is** (commit f997722): the design as a page for each area and each device,
  with a table of decisions and the open questions; the roadmap cut to its status and stages; this changelog; the hand
  checks; the audit's open findings in the known bugs and the backlog; one map of the layers; the performance numbers
  on one page; the old documents kept in `archive/`.
- **S2.2, S2.3, S2.4, S2.12 Playback, undo and the devices** (commit f997722): every command's Undo and Redo checked
  to the property; the golden project holds every saved name; playback checked at five pairs of rate, block size and
  tempo; every golden song also played live against its render; fingerprints that see spectrum and pitch; every device
  through one conformance suite. A note in a stretch's last half sample no longer plays a sample early, and an export's
  first block has the effects' mix right.
- **S2.1, S2.5, S2.7, S2.8, S2.12 A real VST3, assertions that fail, editors driven by the mouse** (commit 3e7def0): two
  VST3s built for the tests and loaded as a user's are; a JUCE assertion fails its test; the real-time audit counts
  every block while the song is edited; every tool of the playlist and the piano roll is driven by the mouse, with the
  song changed in the middle of a drag; the app is started by ctest. A mono plug-in plays to both sides, a plug-in's
  program no longer overrides its state, no key changes the song while a mouse button is down, and Escape takes a drag
  back in the playlist, its lanes and the audio clip editor.
- **S2.6, S2.9, S2.10, S2.11, and most of S4.9: files from others, speed, the whole window** (commit 255297d): every file reader is
  fuzzed in every test run; songs made by seeded random edits read back, join from their pieces and merge into songs
  that hold together; counts of work that don't depend on the machine fail a change that does more per edit; a nightly
  workflow runs an hour of music under the sanitizers and the benchmark gates; the real main window is built in a test,
  every view is painted in every theme, and the walks of every control run in three themes at three sizes; the
  architecture check counts literal colours and sizes in the UI. A project from anyone opens as a song that holds
  together, its numbers within their ranges and no sound looked for on a network path; a preset's sound on a network path, or
  one that isn't audio, is refused; a DAWproject unpacks no more than an archive of its size can hold; moving the
  playhead compiles no plan; a second theme switch no longer leaves text in the first's colour.

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
