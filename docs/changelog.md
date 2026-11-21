# Changelog

What was built, newest first, one line a step: its number, its title and what it gave the user. Phazite has no release
yet, so the entries are grouped by the phase or track of the plan they belong to, not by version; a date is the day
the step's last commit was made. From the first release on, entries are grouped by version (roadmap step 4.5).

"Built" here means that the step's tests pass. Whether a person has tried it is in the [hand checks](hand-checks.md).
The full text of every step, as it was written when it was built, is in the
[archived roadmap](archive/roadmap-2026-11-13.md); what comes next is in the [roadmap](roadmap.md).

## Stage 5. Wide: what a beat maker meets in the first week (from 2026-11-20)

- **S5.2 Wider notes: MIDI channels and program changes.** Every note and controller keeps its MIDI channel, from the
  keyboard that played it, a MIDI file, a take, Capture or a DAWproject, through the song to a plug-in, which gets each
  on its channel, so a plug-in with several parts plays each part; Phazite's own instruments take every channel as one.
  The inspector shows and sets the selected notes' channel. A program change, played on a keyboard or from a clip,
  selects a plug-in's program, and is chased where playback starts or jumps only where the program differs from the
  one set last, in a render from the block it lies in; a song that plays one stays without unsaved changes.
- **S5.3 Controller lanes in the piano roll.** The lane under the notes shows the velocities or, picked by the chooser
  beside its chevron, one controller, the pitch bend, the channel's pressure, a note's pressure or the program changes
  of one channel, as steps in the clip's colour; the chooser marks what the clips hold that the lane doesn't show, so
  that nothing plays unseen. A drag draws values on the grid (off it with Alt), a right drag a straight ramp, the
  Delete tool removes what it passes and Delete All the lane's values, each one undo step that Escape takes back; a
  clip opens on the lane it was left on. A sustain pedal recorded with a part shows there, plays and can be deleted.

## The owner's round of 2026-11-20

- **Stop leaves what rings on as it was:** a Stop that sends the playhead back keeps every automated control, and the
  song's place the devices are given, where the song stopped, until it plays again or the playhead is put somewhere by
  hand, so a reverb's tail through a filter its lane had closed no longer blips open.
- **A sidechain's key that comes late is waited for:** a device whose key comes later than its own sound (a kick through
  a linear-phase EQ keying a compressor on a bass) has its track held back to meet it, so the duck stays on its kick;
  choosing or removing a sidechain while the song plays leaves the track's sound as it was.
- **The settings as VS Code's settings modal:** a card inside the window, which dims behind it, title bar included, with
  a themed header, Maximize and Close, sized as VS Code sizes its modal and following the window, the keys and clicks
  kept in it and nothing behind repainted as it changes; the search's count, Clear and Filter (`@modified`), "Group:
  Name" titles, a sash that sizes the contents, the narrow layout with a Contents button, the settings' own control
  colours and the Modern themes' heading and modified-line colours; Alt+F4 quits over it.
- **A search field says what to type while it has the keys:** the hint of every search field (the settings', the command
  search's, the browser's) stays until something is typed.

## One history (2026-11-20)

- **The automatic saves in the History:** what was the Local History list, under the history and under another name, is
  now in the History itself: the song as it was at times while it had changes, in grey between the versions, by when
  each was taken, those between two versions folded into one row ("6 automatic saves") that a click unfolds, each with
  its Restore under the pointer. **Show Automatic Saves** (the view's menu and the settings)
  hides them. The words Local History are gone from the window ("Restore the Automatic Save of 14:02"). Undo History
  stays at the view's foot, folded.
- **Decided, not built:** the versions' store deletes nothing by itself; Free Up Space (S4.4) will free its room by
  hand ([decisions.md](design/decisions.md)).

## The owner's round after stage 6 (from 2026-11-19)

- **The window's small things:** a right-click's menu opens at the pointer, Replace is a submenu, the effects'
  switches stand in one column, the velocity lane leaves nothing behind as it is sized, the inspector's title matches
  the side panels', the transport bar is in evenly spaced groups with Low Latency in the status bar and A/B only when
  it can be used, and the areas' tabs look like tabs, with a line before the tools.
- **The dynamics curves on decibel axes:** the Compressor's and the Multiband's curves show what they play (a straight
  line a ratio, a bent one inside its points), their presets converted to play as they did, a point's menu at the
  pointer, and the Compressor's history a reading every half millisecond, drawn as a line, not stairs.
- **The browser without Phazite Sounds and Phazite Presets:** Sounds lists the downloaded sounds' folders, each
  instrument's and effect's row opens by its chevron to Phazite's and your presets as one list, containers' presets
  show under Container and Instrument Container, tracks' under Track Presets, your presets of no device listed under
  Other Presets, and a device's Swap tries that same list.
- **A version opens live:** a double-click on a version, or A/B, makes the song that version in place with no bounce, and
  Back to Now brings the song back exactly, undo and redo with it; an edit there makes a branch when it ends with
  changes, recording as it starts; nothing writes an old version as the song; bounces became an extra. Redo never offers
  a step that was thrown away (also after Escape in a drag and a swap put back).
- **The Settings window as VS Code's Settings editor:** one page with a search, the groups' contents and a row for every
  preference Phazite keeps (the audio device in Phazite's own rows, MIDI, recording, the look, editing, saving, versions,
  startup and updates), each with its modified line and Reset Setting, and Shortcuts a page of its own.
- **No new audio, video or picture files in the repository:** the pre-commit hook refuses them.

## Stage 6. Deep: every device to the standard of the best plug-ins (from 2026-11-18)

- **S6.1 A listening set and a verdict for each device:** each of the sixteen devices' pages ends with what to play
  through it, the setting and what to listen for, so that a person can judge it in ten minutes, and a verdict by what
  was measured: what it does well, with the figure, what it lacks against the plug-ins producers own, and its
  standing (one device at the bar, eleven near it, four short of it), each to be confirmed by ear; the standings and
  each device's largest gap are a table at the end of the devices' page.
- **S6.3 The Compressor:** Compress hears a sidechain, looks ahead up to 10 ms, makes its gain up by itself and holds
  its detector, so that a gate drawn on its curve opens in a tenth of a millisecond and doesn't chatter (two more gate
  shapes, three gate presets); a third mode, Transient, turns each hit's attack and what follows it up or down
  whatever the level (Punch, Sustain, Speed; two presets); the curve's points, the Multiband's too, are read, typed
  and moved by number; a change of mode turns what is on its way by the mode it was meant for; a drag of a knob
  that a device's latency follows remakes the device once, as the drag ends; however often the mode changes, each
  sample is turned by the mode it came in under and the gain never steps; and the compressor's tail keeps it awake
  until its gain has come back, so that a long release sounds live as it renders.
- **S6.4 The EQ, part A:** each band plays on the whole sound, its middle, its side, its left or its right, and each
  bell or shelf can be dynamic, its gain moving with the level of the sound in its band (a de-esser): it holds the
  loudest of the last 25 ms, rises in 3 ms and falls in 60. The editor gains Placement, Dynamic and Threshold, a
  handle for the Dynamic with a mark where the gain is now, and the middle's and the side's curves (or the left's
  and the right's) drawn as they play; six presets under Dynamic and Mid-Side. A dynamic band keeps the EQ awake
  until its level has fallen, so that live plays as a render.
- **S6.4 The EQ, part B:** a Phase, Minimum (as it played) or Linear, in which every band's curve plays with every
  frequency as late as every other, at a latency of 4096 samples (85 ms at 48 kHz): for a master, for one of
  several microphones of a source, and so that a Mix of 50 % is an EQ of half the depth. Its bands are symmetric
  kernels (a 2 x 2 matrix of them for the placements) played by a partitioned convolution on an FFT of Phazite's
  own, designed while it plays by a count of work and glided between on a grid of half the latency, so that
  automation stays in time; a dynamic band's moving part plays after them. From 200 Hz up it plays within 0.01 dB
  of its curve; a narrow bell or a steep cut below 100 Hz is up to 5 dB off (one kernel length). Two presets under
  Linear Phase. This ends S6.4.
- **S6.2 Two device bugs:** a Filter asleep on a silent track wakes when a lane makes it Driven, so that it sings live
  as in a render (the engine wakes a sleeping device that asks for it when its values change and it would sound for
  them; a modulated Clean filter still sleeps), and the Delay's Repitch glide ends exactly at the time set (its time
  is read in a double: a float stopped it 37 samples short at 2 s and 96 kHz). Known bugs 203 and 202.
- **A device after latency is given the place of its sound:** the song's place a device counts from (a synced LFO, a
  grid of control steps, Age's crackle) is the song's less the latency of what stands before it, as its automation
  already was, so that a latency added before a device no longer moves what it plays against its sound.
- **S6.9 The wheel as one burst, and menus on every control:** a burst of the wheel on a knob, a fader or a graph's
  handle is one undo step and one new plan, where each notch was one (known bug 201: the Compressor's Look-Ahead was
  remade at every notch), and a right-click on a switch, a list, a choice's button or tab or a part's switch offers
  Automate, Delete Automation and Modulate, as a knob's does.
- **S6.9 The editors draw what is heard:** the Filter's and the EQ's curves at their Mix (they drew the whole cut
  under 100 %), Motion's flanger teeth only as deep as the notches heard, Stereo's fan with Mono, Balance and Swap
  Sides; the dynamics curve even in decibels from -60 to +12 dB (-18 dB sat a tenth of the way up); the histories
  draw every reading across its time, and a display starts afresh when its editor is shown again; the EQ shows a
  spectrum for each of its middle and side, or left and right, curves.
- **S6.9 One grammar for every graph:** every picture in the device editors answers the mouse as a knob does: a drag
  (Shift fine), a double-click for the defaults, a right-click for each parameter's menu with Type a Value..., the
  wheel named in the hint, the values in the corner; the Filter's and Motion's dots no longer jump to a click, and
  the drum pads sound on the press, harder the higher.
- **S6.9 The editors' frame:** the pages' bar keeps its tabs whole and puts the rest in a More tab's menu; a part
  that can be off keeps its switch in its tab; section titles are written in Title Case, faint while their part is
  off; and the rules for where an editor puts things are written down and checked against every editor (Stereo's
  Bass Mono is its part's switch; four editors' outputs moved last in their parts). This ends S6.9.
- **S6.8 The presets on the finished devices:** Soft E-Piano gains its bell and its bark, Pulse Organ holds steady and
  Choir Pad stays among the vowels; five Synth presets named for a driven filter play the Driven character and four
  whose sound is a warp play at Quality 2x; Supersaw Lead, a double of Hypersaw Lead, goes; Mallets is a kind of the
  Synth's; the Delay gains Stutter Hold; every changed effect's presets were measured against their names.
- **S6.5 The Reverb's Hall:** a third mode beside the Room and the Plate: sixteen lines (2.3 s at the largest Size)
  through a Hadamard matrix, an all-pass in each loop and sways that run on over a sleep, so that a tail of 20 s
  doesn't ring (its largest peak 12.5 dB over its envelope, where decaying noise measures 10.5 and the Room 17); the
  Room and the Plate play as before; Concert Hall, Cathedral, Endless and Chamber among its presets.
- **S6.6 The Delay:** a time for each side (Link), a Clean loop beside the Tape one and a Feedback to 110 % whose
  echoes swell until the loop holds them, modulation of the times, a duck that lets the tail come back, a freeze that
  holds exactly what was heard, and a Transition that glides (Repitch) or fades at equal power without bending the pitch by more than 10 cents
  (Fade); at its defaults
  it plays as before, sample for sample, and seven presets join its six.
- **S6.6 The Filter:** a Character, Clean (as it played) or Driven, whose drive sits in the filter's loop: the
  resonance saturates, the top of the Resonance sings in tune at the Cutoff, and a driven sound comes out louder and
  thicker, through a ceiling that leaves a sound under -6 dBFS alone; a Quality of 2x, 4x or 8x for the drive and the
  Driven filter, at one latency; a Driven filter never sleeps, so a lane that raises its Resonance makes it sing
  live as in a render; four presets under Driven. The instruments' filters share its code and still play Clean.
  This ends S6.6.
- **S6.6 The Saturator takes the Exciter in:** the Saturator gains a Colour (the curve's working point, for even
  harmonics from any shape), a Crush shape (fewer bits the harder it is driven), a Quality of 2x, 4x or 8x the rate,
  and a Band with Listen: only the band between From and To goes into the curve, and what the curve adds under the
  band is kept out. The Exciter goes, its eight presets converted to the Band and measured against it. Every
  Quality takes one latency, 7 samples, so that automation moves the Quality anywhere; a new shape is heard whole in
  35 ms without a thump under it, and Colour leaving 0 no longer swings the lows.
- **S6.7 The Synth's modulation glides:** pitch, levels, table position, the warps, volume and pan go in straight
  lines across each 16-sample control step instead of stepping, so a fast LFO no longer buzzes under the note (the
  buzz of a 25 Hz LFO on Volume falls from 47 to 87 dB under the note); a note with nothing moving plays bit for bit
  as before.
- **S6.7 The Synth's filter glides, and a Quality:** the cutoff and the resonance of its low-pass, high-pass,
  band-pass and notch, and its sub's pitch, go in straight lines across each control step too (a 25 Hz LFO on a
  resonant filter's cutoff: its buzz from 70 to 106 dB under the note). A Quality of 1x, 2x or 4x plays the whole
  synth at that multiple of the rate, down through half-band filters, every Quality 5 samples late: a swept sync at
  C5 is 39, 60 and 74 dB under the note, a bent wave 42, 62 and 77. A change of Quality cuts the notes sounding, with
  a 3 ms fade.
- **S6.7 The Synth's filter drives:** a Character for its filter, Clean or Driven, the Filter device's loop on every
  voice at the Quality's rate: the resonance saturates, the top of the Resonance sings in tune (within a fifth of a
  cent from C2 to C7), a change of Character hands the filter's ring over without a dip, and its ceiling works at a
  voice's level, so that Driven at no Drive leaves a patch's shape alone. Its editor draws the wavetable through its
  Warp, and shows its own matrix on the knobs it moves as a dashed ring inside Phazite's solid one. Three presets:
  Sync Sweep, Acid Line, Singing Filter. This ends S6.7.
- **S6.7 One synth, part A:** the Wavetable gains what only the Synth had, so that the two can become one: each
  LFO's Fade In, Glide Mode, a Drift source, slots that move other slots' amounts (the mod wheel brings a vibrato in),
  a Sync that modulation takes to 16 times, and tables that keep 16 samples a cycle, so that its basic waves alias
  as little as the Synth's.
- **S6.7 One synth:** the Synth and the Wavetable are one synth, the Synth, on the Wavetable's engine. The
  subtractive synth, its editor and its tests are gone, its test cases moved over where the new synth's didn't hold
  them, and its fifty presets live on as their measured copies among the Synth's 88. Phazite has four instruments:
  Synth, FM, Sampler and Drum Sampler.
- **S6.7 One synth, part B, phase 1:** the Synth's fifty presets have copies on the Wavetable's engine, converted by
  formulas and measured phrase by phrase against the Synth (35 alike in every measure, the others named with their
  causes), which the example songs now play; the Wavetable's Basic Shapes are the Synth's own waves, its 12 dB
  resonance reaches the Synth's Q, its Phase lets oscillators run free, and its Drift wanders on its own for each
  slot.
- **S6.10 Motion:** the Chorus, the Flanger and the Phaser become the three modes of one device, Motion, under three
  tabs: one Mix that means the effect alone in every mode (half and half at 50 %), a 10 ms fade between modes, their
  24 presets converted to play as they did, so that Swap tries a chorus, a flanger and a phaser on a sound in place,
  and the chorus's Feedback below 0. Its level holds through a change of mode, and the flanger's and the phaser's
  pictures draw what plays at the Mix set.
- **S6.11 Age:** the Vinyl becomes Age, one device for what a physical medium does to a sound: its record plays as
  before, and a tape sits beside it (a wow that drifts and a quicker flutter, a saturation that takes the highs first,
  a head bump, a hiss of its own that is alike at every sample rate, and dropouts that fall where the song is); each
  of its five parts has a switch; its editor has two tabs, the tape shown as two turning reels; five tape presets. The
  Rate turned up fast under a deep wow no longer swings the pitch, on the record either.
- **S6.12 Stereo:** the Stereo Expander becomes Stereo, the one device for a channel's two sides, adding Balance, a
  Mono switch, Swap Sides and a DC filter to its width, spread, bass in mono, polarity flips and gain, each switched
  without a click; its presets gain Swap Sides and DC Filter under Checks.

## Stage 4. Decided: the owner's decisions, built (from 2026-11-17)

- **S4.1 The design note of the cut to versions:** [design/versions-cut.md](design/versions-cut.md) says what the
  versions keep, what goes, the plain words the view takes and how a history cannot lose work; the owner approved
  it with both of its recommendations (plain words; each computer's own records stay).
- **S4.2 Versions cut down to what producers use:** staging, a version checked out on its own and a merge's ticked
  lines are gone (a version records the whole song, a double-click hears a version, bouncing it first if it has no
  bounce, and a merge takes every line), and the view is called Versions and speaks in plain words: Save Version,
  History, Switch Branch, Restore This Version. A branch is named as typed.
- **S4.3 A history that cannot lose work:** a version that leaves every branch (undone, its branch deleted, reset) is
  listed by Recover Lost Versions... for 30 days with when and why it left, and comes back as a branch, a deleted
  branch with the changes left on it; the logs say why each branch moved, on every computer.
- **S4.4 Housekeeping, the part that is sound:** Verify History checks every version's audio files (missing, another
  size, changed) and every kept song, with progress, and stops for a save; Clean Up Unused Audio keeps what any
  version the store holds plays and offers nothing while a pack can't be read; a computer's packs are joined by size,
  each piece copied from a copy that reads, so their count stays small; every time the store writes is later than
  the one before, whatever the clock does. The store lets go of nothing yet (known bug 196), and a history written
  by an earlier build is not read (known bug 197).
- **S4.5 The song tied to its version:** the project file says which branch the song is on and which version it grew
  from, and the changes are worked out against that version, on the history's thread, for the tracks that changed
  alone (an edit costs the window a fraction of a millisecond on a 100-track song, and a drag is compared once, as it
  ends). Newer versions on the branch, from another computer or after a project file was put back from a backup, are
  offered by Take the Newer Versions or Merge the Newer Versions rather than shown as removals and taken out by the
  next save; a version saved beside another computer's goes its own way and is offered to merge; a song whose branch
  hasn't reached the folder waits with its saves. Save Version writes nothing where nothing changed, and keeps what
  was typed. A merge of another computer's merge of the same version brings no item twice.
- **A number's text read by the bytes it holds:** a project file whose number ended in half a character (bytes that
  aren't UTF-8) made the loader read a byte past the text's end; found by the fuzzer under AddressSanitizer in CI. The
  loader, the comparison of saved values and the writer of a plug-in's class ID take a text's bytes as they are.
- **S4.7 Automation along the control's travel:** automation is kept and played as the control's position, and
  every knob in hertz travels logarithmically, so a straight line in a lane is an even sweep through the octaves
  (632 Hz halfway from 20 Hz to 20 kHz, 1 kHz halfway from 100 Hz to 10 kHz); keys nudge by a part of the travel,
  typed values stay in the parameter's own units, and the example songs' sweeps and modulation depths were fitted
  again. With it, the Filter and every effect's automated mix play the same at every block size.
- **S4.6 Backups kept by age:** a save backs the project file up only when it differs from the newest backup, and
  Backups/ keeps what Local History keeps (every one of the last two hours, the newest of each hour for a day, the
  newest of each day for 30 days), each backup dated by when its version was saved, and the newest older than that
  too, so a day's work leaves backups from an hour, a day and a week before. Only files named as the project's
  backups are thinned or offered; anything else in the folder stays.
- **Known bug 192, sends' automation and modulation** (found in S4.7's review): a send's level follows its lane and its
  track's modulators and macros along the fader's travel, on the song's control points as a fader does. It plays live at
  any rate and block size and in renders, freezes and bounces, stays in time with the sound it sends through delay
  compensation, is written by Touch, Latch and Write, and its knob shows it. A send that leaves its lane eases to the
  level set by hand over 15 ms.
- **Known bug 193, DAWproject automation:** an exported lane plays in another DAW as in Phazite: points are added along
  each line, bent ones too, until DAWproject's straight lines stay within 0.1 dB (a thousandth of the range on a pan or
  a plug-in's parameter), and sends' lanes are exported as well.
- **Known bug 194, Bounce in Place and sends:** a track's sends move to the bounce's new track with their automation and
  modulation, so a send before the fader no longer plays the bounced sound into its return twice from the muted
  original; Undo gives them back.
- **Known bug 195, instruments rest as a project opens:** a stopped song's instruments rest as a project opens after one
  closed while it played: the stop that reaches them, an all-notes-off, no longer counts as something heard, so the FM
  synth, the drum sampler and other instruments with long tails no longer work through those tails. It showed as a
  performance gate that failed after other test files in one process.

## After the known bugs' first push (2026-11-17)

CI built the round's 36 commits for the first time and failed on every preset. What it found, and two tests that
failed in some runs here, fixed on a branch that CI built before `main` moved; the rules that came of it are in
CLAUDE.md ("Checks by stage", "Agents").

- **A new project's folder deleted from under it:** a project not saved yet keeps its folder in `Unsaved Projects`,
  named after the second it was made in, and a folder's deletion is put off while its project's history still writes
  there. A deletion put off deleted whatever was at its path when its turn came: a project made at the same path within
  that second lost its folder. No new project is given a path whose deletion is still to come, and the names for one
  second are told apart by a number looked for among those too.
- **A history that went as its first read began left that read waiting for itself** (known bug 190): the read waits
  for what earlier histories of its folder still write, on the threads kept for them, and its own thread was among
  them once its history had gone. It never ended, and neither did whatever waited for the histories that went: a
  test's folder as it goes (`AppTests` ran into ctest's ten minutes in some runs on CI's Linux builds, where a stalled
  test now writes its stack, which named the wait), and Phazite as it quits, if a project closed in the instant its
  history began to read. A history's jobs wait only for the histories that had gone when the
  job was given, so that none waits for its own thread, nor two histories of one folder for each other (which the
  fix's review found); and the jobs assert that none of them waits for all of them (`BackgroundJobs::isOnOwnThread()`).
- **A plug-in that joins the song is taken in when the changes are asked for,** not only at the history's next tick:
  the badge's count just after a project's plug-ins loaded is right, and no tick later compares every track again
  (known bug 187, which a performance gate showed in two runs of three).
- **Tests that failed on CI and not here:** the rows' scrolling frames may differ by 16 pixels each with the platform's
  renderer, which without a graphics card rounds a header's switch a little otherwise as it passes one line of the
  window (each frame is still compared with a playlist made afresh, and the software renderer to the pixel); a held
  unsaved folder is held inside one Phazite too, where Linux gives a process its own lock again; the golden project is
  saved as the app saves, once its plug-ins have given their states (known bug 188); a take is written as far as its
  first click before its drive fills; and the lines that GCC refuses and MSVC and Clang don't (an enumerator
  shadowing a member, an int added to a size_t through `?:`, `linux` as a name, a constant used in a lambda inside a
  lambda, a `long` given to a `juce::var`).
- **189 An import cancelled once its branch is made:** two tests of it failed in some runs of a release build, the
  import done before the cancel; they hold the history's read of the switch where it starts (a seam that lets a count
  of reads start first).
- **CI's clang-tidy runs over what a push changed:** over every file it took 36 minutes of each run, more than any
  other job, with nothing kept from run to run. CI's Clang job checks the files changed since `main`
  (`tools/clang-tidy.sh --since`), and the nightly workflow every file.

## The known bugs (from 2026-11-15)

The owner asked for every entry of [known-bugs.md](known-bugs.md) to be fixed before the hand checks of stage 3. Each
bug has a test that failed before its fix, and a reviewer read each area's changes; what the reviews found is fixed
too. A line names the bug's number in that list.

- **186 Versions from before a song's IDs were given anew:** a song whose IDs the loader gave anew (a file from
  another program, with IDs too large to count on from) is stamped with its numbering, and its versions and Local
  History snapshots hold the stamp their song had. A line's Restore, a line's Discard Changes and Merge refuse a
  version numbered otherwise, saying why and what to do, where they matched its tracks wrongly before; a merge's
  list refuses a song that became another numbering's since the list was made. Opening such a version, switching
  to it and restoring it whole take it as it was numbered.
- **205 The versions' changes, asked for too early:** the changes since the last version count as worked out only
  once the history has read the project's versions; before, a test that asked just after a project opened again got
  an empty list, on any machine fast or slow enough. The plug-in update's test holds the read to make that order.
- **191 A load just after a helper crashed:** a plug-in asked for in the fifth of a second before Phazite noticed
  that its helper had crashed went to the dead helper, failed, and was kept off as one that crashed as it loaded; it
  opens in a fresh helper now. A plug-in whose load ends without it says why, to the tests for now. The failures on
  CI themselves have another cause, not proved yet (the entry says which is likely).
- **209 A plug-in's presets apart from Phazite's devices':** a plug-in's presets are kept in a folder of its own in
  `Plug-ins`, so a plug-in named as one of Phazite's devices ("Reverb") no longer shares that device's presets in the
  browser, Swap or the presets menus, and Other Presets shows the folders of plug-ins not scanned by their names.
  Presets a plug-in saved in the old place aren't moved (no backward compatibility yet).
- **198 A sound file written again in its place:** it plays as it is now from the first plan a second on. The player
  asks the disk about each file it plays at most once a second, and at once after a DAWproject import unpacked
  files; a stretched, pitched or reversed clip gets a render of the new file (a render is named for its source's
  size and time, not its path alone); every track that plays the file is made again in the same plan; an export
  makes the renders each of its jobs plays as the job's turn comes; and the clip's waveform shows the new file,
  without a paint ever asking the disk. A name made safe for a file that was only dots and spaces names no folder
  above.
- **199 A merge that made two items one:** a line merged back into the line that merged it, two lines that each merged
  the same version with other IDs, and a note in a clip a merge gave a new ID, keep every item once with its own
  content, and what names an item names it still. A merge takes the other line's song to the IDs the version both
  lines have gives them, then to this line's, one step each, the other line's own items given IDs of their own first;
  it records what it took by the IDs the merged song holds; and it refuses a merge that would still put two items
  under one ID, changing nothing. Left, as known bug 211: the other line's merge of a later version than the parting.
- **204 Consolidate over part of a clip, in CI's Windows release build:** the joined clip had no notes there, and
  nowhere else. A test that takes the join's steps one at a time named the step on CI: the part of the clip in the
  range, made as `BeatRange (std::max (...), std::min (...))` of what the two ranges' getters return, came out empty
  (its end the range's start) with that compiler. The join asks the range for its intersection now, and the five
  other places that made a range so ask it for its union.
- **208 The switch read before a tick:** the right-click menu's test read a part's switch before the displays' ticker
  had shown it a click made on another control of its parameter; a release build runs the whole case within one
  frame, so CI's linux-release job failed it in every run. The test ticks the displays first. Nothing was wrong with
  the value as it plays, which the entry suspected.
- **207 The speed gate's count without its plan:** the gate that compares what a note added allocates in a small and
  a big song makes the note's plan inside the count, without the message loop, and requires one: a wait for the
  plan there took in whatever else the loop delivered meanwhile, so the same song counted differently from run to run.
- **206 The menus compared at rest:** the test that compares every menu item with its command builds the menus once
  the versions' history is at rest after the last message delivered, with nothing delivered in between.
- **200, 210 A folder two threads made:** the first save's test made `Versions/local` while the history's thread
  made its own folder inside it; JUCE's `createDirectory()` tells the thread that came second that it failed. The
  test asks whether the folder is there.
- **The Versions view's button, read too early:** the test of Take the Newer Versions waits until the song was
  compared with the version it grew from; until then the button says Merge, as CI's Clang job caught once.

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
- **127 The mixer in large songs:** strips are made only for the channels in view, and a device's change brings only
  its channel's strip up to date (a change of where sound can go, every strip's send knobs): a mixer of 200 tracks
  opens in 17 ms instead of 546, and its ticks take 1.5 ms instead of 43. A send that would make a loop is never
  made, whatever offers it.
- **144 The playlist in large songs:** track headers and automation lanes have components only for the rows in view,
  and a note added or a clip dragged reads and repaints only the clips it changed: 3 ms instead of 300 in a song of
  4,800 clips. The song's changes name the clips, tracks and channels they touched (`SongChanges`). A jump of the
  mixer or the playlist makes what comes into view over the next turns of the message loop, never stalling a frame.
- **165 Plug-in windows close at once:** closing a plug-in's window no longer keeps Phazite waiting two seconds, nor
  as long as a hung helper lives (2.0 s and 4.9 s before; about 20 ms now): the helper's window sits in a window of
  Phazite's own, which leaves Phazite's window first; keys a plug-in's window doesn't use still reach Phazite.
- **166 Plug-ins' states without waiting, and never stale:** copies, commits, snapshots, presets and closed plug-in
  windows no longer wait for the plug-ins; what reads their states asks first, in one place: a command says whether
  it reads them (always, or by what is selected), and commit, Source Control's restores and merges, presets saved
  and loaded, Save As, DAWproject export, Duplicate and Delete of tracks with plug-ins, a chain's Duplicate and the
  Stage "+" run once the plug-ins that changed have answered, the window holding its input meanwhile and a progress
  window after a second (Go On Without Them, or Cancel). A plug-in that keeps telling changes is asked a second
  after its first; a commit records the count of a plug-in's changes as of the state it holds, so an answer that
  comes later shows as a change. A sample-rate or block change prepares plug-ins in the background, silent
  meanwhile. Left: plug-ins added later, Reload, a window opening and parameter texts still wait (known bug 166).
- **166 Reload:** a plug-in that crashes again within five seconds of a reload opens from what the project holds,
  says so, and its own last settings are kept for the next try.
- **168 The save question at shutdown:** Windows shutting down or signing out is heard by a window of Phazite's own
  (JUCE workaround W49); a close that waits for a plug-in's settings then goes on without them at once, so the save
  question shows; a shutdown cancelled, or an installer asking Phazite to close, is an ordinary close.
- **42 Slow plug-ins in renders:** an export, a freeze or a bounce waits for a slow plug-in as long as it takes, its
  progress bar naming the plug-in after 5 seconds, and Cancel stops it at once; a plug-in that can't be prepared
  for the render's rate fails the render instead of leaving silence in it.
- **78, S4.8 Mute and solo silence the output only:** a muted track's or chain's instrument and effects play on
  underneath, so a note held, a reverb's tail and a delay's echoes are heard at once when it is heard again; its
  sends before the fader go on, those after it are silent with it; a track silenced by another's solo sends
  nothing but to a soloed return. A muted track costs what it costs heard: Freeze is what saves CPU, Bounce in
  Place freezes the original it mutes, and renders leave out what they can't hear (a stems export of the benchmark
  song: 28 s to 14 s).
- **167 Delay compensation that changes while the song plays:** every path into a sum keeps a ring of what came
  into it (one for all the paths from one point of a strip), so a plug-in with latency that joins, leaves or is
  bypassed never silences a track: the mix dips for two 3 ms fades and plays on. An everyday edit (a track deleted
  or moved into a bus, a send added, removed or switched before or after its fader) fades only the paths it ends or
  starts; every other track plays through it sample for sample.
- **162 A Stop during a jump's fade** waits for the fade to end and stops where the jump lands, without a step.
- **36, 39 A jump into audio not read yet:** the landing is asked for first and the reading thread woken, so the
  silence lasts only as long as the read (0.1 ms after the jump, where it was up to 20 ms); A/B's bounce is read
  ahead at the loop's start and where playback starts.
- **171 A device asleep on silence** still shows its modulation on its knobs and its graph's handles.
- **Audio files with what isn't a number:** a clip of a float file plays silence where the file holds NaN or
  infinity, stretched or not, and the rest of the song plays on (the whole mix went silent before).
- **122 Notes, clips and tracks found by their IDs:** the song keeps an index of them (`phazite::SongIndex`), kept
  exact by the song's own changes and checked against the song at every answer, and the piano roll finds what is
  under the mouse among the notes it paints: a step of a drag of 500 notes in a clip of 5,000 takes 4.5 ms where it
  took 22; Select All 3 ms where it took 341; Transpose, Quantize, Duplicate and Delete of 5,000 notes take tens of
  milliseconds where they took seconds; a view's read of the clips of 200 tracks 6 ms where it took 253.
- **20 Duplicate and Paste of notes** name their undo step for the notes made, and the hint bar says how many didn't
  fit before the next clip.
- **160 A new project whose first save fails** takes its folder away, so the name can be tried again.
- **169 A first save and the project's versions:** the cause of the rare "couldn't copy a file of the Versions
  folder" was a commit written into the old folder while the rest was being copied. The versions are now copied by
  their history's own thread, after its writes, with progress and Cancel; commits, merges and branch changes wait
  meanwhile (Source Control says "Saving the project..."), and every write into the versions asserts in a Debug
  build if the folder is held.
- **164 A sidechain that would hear its own track's sound round a loop** is cleared as a project opens.
- **174 Escape on a control held as Play was pressed in Write** takes back the drag alone, the value going back to
  what the pass writes; an Undo or Redo while a pass writes keeps what plays and what is written alike, and one
  during loop recording no longer drops a round of automation.
- **106 A DAWproject that would reach 4 GB or 65,535 files** is refused before anything is rendered, saying what to do
  (JUCE workaround W41).
- **148 Numbers that are not finite:** numbers from plug-ins, files, devices and typed text become whole numbers and
  floats without undefined conversions (`src/model/base/Numbers.h`); a value typed that is no finite number changes
  nothing; a MIDI file's far-off events import within a song's beats; a step sequencer's steps and a sound's slices
  are kept within their ranges as a project opens.
- **170 A number whose exponent JUCE's reader would overflow** is read as the number it is, in projects, the
  versions' pieces, presets, settings and DAWprojects (JUCE workaround W38).
- **179 Switches and IDs another tool wrote in other forms** ("true", "12.0") are written as Phazite writes them as a
  project opens, so an unchanged file shows no changes; a project Phazite saved is the same, byte for byte, once
  opened and saved again, which the fuzzer now checks at every round.
- **S4.9 Files from others are untrusted** is built with 106 and 148.
- **123 Audio is read and copied in the background.** An import, Normalize, Quantize Audio, a sound put on a pad, the
  sampler or a wavetable, Swap's Keep and Locate Audio File read and copy their audio on a thread of the song's work
  while the window holds its input (play and stop still work), show a progress window with Cancel after a second,
  and make their undo step once the audio is read, changing nothing and saying why if the clip, the track or the
  project changed meanwhile; a render asked for meanwhile waits its turn. The window's hold is one for these and for
  the wait for plug-ins' states. Quantize Audio's warped clips no longer cost a tenth of a second at each plan to
  name their render. Left: a preset's sounds are still copied at once (known bug 183).
- **128 The browser's search runs in the background** through search keys made with the index, each key cancelling
  the search before it; it finds folders, and sounds by their folders' names, says where its list is cut ("The first
  200 of N"), keeps equal matches in the index's order, and Enter or Down acts on the newest text's results.
- **An unsaved project discarded while its folder couldn't be deleted** is marked and deleted at the next start, and
  the look for unused audio skips empty and temporary files.
- **85 The loop's wrap and a jump without a burst.** Each track's notes and controllers have an index made with the
  plan, each resampled clip keeps a spare resampler made ready ahead for the loop's start, a jump among resampled
  clips that aren't ready waits up to 20 ms of whole silent blocks for them, and a new plan never waits for a file
  being read. The wrap's own work on 200 tracks and 40 resampled clips went from about 80 % of a 128-sample block to
  3.6 %.
- **Play and a jump reset the delay rings in place**, without writing to them (400 rings: 0.1 % of a block, where
  clearing them took 32.5 %): Play on a big song no longer starts with a burst.
- **Controllers, bend and pressure are chased.** Where playback starts, jumps or goes round the loop, each track's
  controllers, bend and pressure take their values at that place before its notes start, as Cubase and Ableton Live
  chase them; a track without controllers gets none, and a controller that changes exactly there is sent once.
- **173 Aftertouch and channel pressure** are heard live and played back by plug-ins, kept by takes, Capture and MIDI
  imports, thinned to one value a millisecond for each controller and each note, and taken without harm by Phazite's
  own instruments.
- **175 A pedal across a jump's seam.** A take after a jump that lands where the take before ended carries the pedal,
  the bend and the pressure on; a jump during a drag places the take before once the drag ends, in a step of its
  own.
- **176 A take stopped during a drag** stays in the playlist with its waveform until its clip replaces it; a take
  waiting for a drag's end and the take recording after it on the same track each show their own waveform.
- **126 The versions read off the message thread.** Checking out a branch or a commit, Back to Branch, a merge's
  list and Merge, the restores, Compare, Create Branch from a commit, Undo Last Commit, Reset Branch and Import
  DAWproject as a branch read and write whole songs on the versions' own thread. The window holds its input
  meanwhile with the one hold it has (Space still plays and stops), shows what it reads after a second with Cancel,
  Escape cancels from the start, and a quit stops the read and says so. A switch, merge or restore that can't finish
  says why in Source Control; Delete Branch asks when what was left on a branch can't be read. Left: known bug 184.
- **Songs read from the versions pass the loader's checks.** A history another computer or tool wrote can't put
  impossible values, an ID twice or a reference to nothing into the song; a merge or a restore of a commit that held
  an ID twice is refused with a word; a restore from a damaged commit leaves the song as its file would open.
- **Import DAWproject unpacks its audio in the background**, into a new project or as a branch, with progress and
  Cancel; a cancelled or failed import leaves the open project as it was and deletes what it unpacked.
- **Names Windows keeps for devices** (CON, NUL, COM1...) are no longer given to files named from what the user
  typed: projects, recordings, presets, stems, freezes, bounces, exports and the versions' files get "_" added.
- **An unsaved project closed right after a commit** no longer has its folder deleted under that commit's last
  write.
- **182 The test program can't crash as it exits.** On Windows it ends as soon as its static objects have gone, with
  its result and every leak still checked, so that a thread the graphics driver starts during the runtime's teardown
  can no longer crash it after all tests passed (reproduced in 2 of 34 loaded runs before, none after).
- **180 Tests no longer measure against the wall clock**, but for the plug-in tests: Phazite's timeouts, the CPU
  meters and the engine's timing read a clock that tests hold still (`devices::Clock`) and that never goes back once
  let go; the engine is handed the song by message delivery rather than a settle; and every wait for a condition
  keeps only the hang limit.
- **145 A plan compiles again only what an edit changed.** A device's values are read again once something of its
  devices changes (not for its channel's height, a mute or a plug-in's stored state), and each parameter's name is
  made once: an edit in a song of 100 tracks takes about 2 to 3 ms of the message thread where it took 45 to 90.
- **124 A plan keeps what it knows.** A track's audio clips are placed again only once they or the tempo change, with
  one tempo map; the disk is asked about a file only for a track whose clips changed; each automation curve is kept
  until it changes; and view state makes no plan. A device's sounds that came back whole with a song applied (a
  branch switch, a merge) played the old ones: they are loaded again.
- **125 An export of stems compiles each stem as its turn comes** and lets go of the one before: 17 ms before the
  first starts where it was 127, and a quarter of the memory.
- **S5.7 The plan rebuilt only for what changed, notes and clips found by an index** is built with 122, 124 and 145.
- **184 A damaged project's copies are read in the background.** A project whose file can't be read no longer reads
  its backups and its branch's last commit on the message thread before it offers them: the window holds, a progress
  window shows after a second, its Cancel offers what was read by then, and a quit stops the read.
- **183 A preset's sounds are copied in the background.** A preset whose sounds are outside the project (a kit's, a
  sampler's, a wavetable's) has them copied as an import's are and goes in once they are, with Cancel; Swap's trial
  copies nothing, and a trial kept by another change has its sounds copied in afterwards, as a step of its own
  ("Copy Sounds into the Project"). A copy made for a change whose device or track went meanwhile is deleted.
- **A drag or a paste of many notes** finds where its clip can grow once for a command instead of walking the
  track's clips once for each note (2,701 elements looked at instead of 103,501 for a step of 500 notes on a track of
  200 clips).
- **The browser's filter menus count what they offer on the library's thread**, as the search does, the newest
  words' counts alone, and a pad's Swap picks its type from counts the index keeps.
- **A new project's first save always copies in the background**, however small: the window no longer waits for a
  commit being written, a file another program holds for a moment is tried again, and the progress window shows only
  after a second.
- **166 Nothing waits for a plug-in.** Plug-ins added, reloaded or brought back by an undo load in the background;
  their windows stay open while a plug-in comes back, wait for an editor as long as its helper lives, and stop
  waiting at a second click; their states, their values' texts and the values typed into them are asked without
  waiting. A typed value is read by the plug-in alone, never guessed: the window is held with its one hold for work,
  says so after a second, Escape gives it up, and it is dropped after an undo but not because the song records. A
  reload sends the plug-in what the song changed meanwhile.
- **177 Shortcuts over a plug-in's window work on any keyboard layout**, matched by the key's place.
- **181 What a helper tells is checked as it comes in**: numbers, texts and sizes; a scan's list (a shell file's
  thousands of plug-ins are all found, and what is left out is said); the helper's process ID; floods of values and
  gestures, each gesture paired; and parameter IDs, made unique by their order among twins.
- **180 The plug-in tests no longer measure against the wall clock** either: the plug-ins' time limits read the clock
  tests hold, a promise that something doesn't wait is checked by what a wait would have changed, and only the tests
  of hangs set short hang limits.
- **A plug-in's window closed opens again** for a plug-in that makes its own editor (the helper told it that its
  editor went).
- **With a filter set, the browser lists the places it leaves on the library's thread**, the newest filters' alone:
  the tree keeps the places it showed until they are in, and a key pressed meanwhile acts on them once they arrive
  (0.02 ms of the message thread instead of 18 ms in a library of 50,000 sounds).
- **Opening a folder in the browser no longer waits for the disk, and shows what the folder holds now.** A folder of
  an indexed place opens at once from the index and is read again on a listing thread, its rows brought up to date
  in place: a file added, removed or changed outside Phazite shows, and the search and the filters find it. Open
  folders in view are read again whenever Phazite comes to the front. A drive that stops answering, on a folder or a
  file, holds up only its own folder: its root moves on after three seconds, spare threads take reads while every
  thread is stuck, and the folder says "This drive isn't answering..."; a key pressed on a folder being read waits
  only a moment and gives way to whatever the user does next (10 to 17 ms of the message thread instead of 420 to
  440 ms for a folder of 20,000 sounds).
- A stopped song's devices that have heard nothing since they were reset rest after a quarter of a second rather than
  ring out a tail from silence: 20 tracks of a synth, an EQ, a compressor and a reverb cost 1.1 % of a core stopped,
  not 7.5 % (as in `main`: each reverb played two seconds of tail as the song opened). While the song plays nothing
  changes. The Vinyl rests while the song is stopped and silent, and wakes with its noise as the song plays. A
  performance gate counts that no device works a block once its sound has ended, and the conformance suite checks that
  a device given nothing plays nothing while stopped and after a reset, which the rule leans on.
- The live-note benchmark holds the engine's clock and checks its delays: 101 samples for each limiter the note goes
  through or is lined up with, none for one switched off (the 45 samples the quiet run showed were the wall clock's).
- The check for unsaved changes after a notification compares only what notified, and values without writing them as
  text: 0.01 ms instead of 22 ms on a song of 16,384 notes (the cost was older than the round: every track compared,
  every value written as text, at each notification). Found on the way and fixed: a plug-in's state that Save a Copy
  As, or an autosave after a failed one, stored into the song made the project unsaved only by chance, at the next
  edit; it does now as it is stored.
- A project opens with its checks done without the copies and lookups they made for every element and number: half
  the time for the checks of a song of 100,000 notes. The loader counts IDs as the song does, one function for both.
- Each icon is parsed from its SVG once and copied for every button that shows it.
- The playlist-and-mouse benchmark reports the window's own paint apart from the image read back from the graphics
  card, which the mouse never changed.
- A Save whose write fails compares the plug-ins' states it stored, as Save a Copy As and an autosave do: with a
  plug-in that changed its state without saying so, a later edit undone said "saved" and closing lost the state.
- A file from another program with an ID just below 2^62 left no IDs to count on from, and each opening dropped the
  sends, lanes and modulations of what was made since. A song whose IDs reach 3 * 2^60 now gets new IDs once, from
  2^61 up, names following; every other song keeps its IDs as they are. A track given a new ID as its song loads is
  no longer frozen, as its frozen file is found by its ID. The versions refuse a commit such a song is in for Merge
  and Restore. An ID past 2^61 inside an element Phazite doesn't know, which stays as it is, counts nowhere. One edge
  is accepted and written at `renumberMark` (`Schema.h`): a file with an ID within a session's count below the mark
  is given anew at its next opening, with what that session made; what the versions do wrong after it is entered as
  known bug 186.

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
