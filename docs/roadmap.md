# Phazite roadmap

Where Phazite is, and what comes next, in order. What is finished is listed in the [changelog](changelog.md), one line
a step; what no person has tried yet is in the [hand checks](hand-checks.md); the bugs found and not fixed are in
[known-bugs.md](known-bugs.md), and what is wanted but not planned in the [backlog](backlog.md). The plan this page
replaced, with the full text of every finished step, is kept in the
[archive](archive/roadmap-2026-11-13.md).

- **Order:** the stages come one after another. Inside a stage the steps are in no fixed order.
- **Sizes:** S under a day, M one to three days, L about a week, XL several weeks. They order the work; they are not
  dates.
- **Numbers:** a step that had a number before keeps it (4.3, 4.4, 4.5, 4.10, A4), because its GitHub issue's title
  starts with that number, in its phase's milestone and on the
  [project board](https://github.com/users/august-byrne/projects/1). The steps that are new in this plan are numbered
  `S<stage>.<n>` and have no GitHub issue yet. S4.3 is stage 4's third step; 4.3 is code signing.

## How a step is done

1. **Plan:** confirm the step's scope. If a decision changes, update the [design](design.md).
2. **Build:** follow `CLAUDE.md`: the architecture rules, the real-time rules, formatting, SPDX headers, licences.
3. **Test:** unit tests for the model, DSP tests for devices, and a test that fails before a fix and passes after it;
   `sh tools/check-architecture.sh` passes; the golden tests pass, or their files change on purpose and the commit says
   why; `windows-debug` builds without warnings (`windows-profile` too for performance work); CI is green on Windows
   and Linux.
4. **Try:** a row in the [hand checks](hand-checks.md) that says what to click and what to listen or look for. A step
   is built when its tests pass, and checked when a person has done that row.
5. **Review and commit:** read the whole diff (no secrets, no personal data), pass the git hooks, one commit for each
   step or logical part of one, with `Closes #<n>` where the step has an issue. Add its line to the
   [changelog](changelog.md) and update the table below.

## Where we are

Phazite is pre-alpha. Its first plan (phases 0 to 4, the architecture track and source control) is built except for the
steps named here. A whole-project audit then found about 300 problems; the bugs that lose work or sound are fixed, the tests that
would have found them are built, and the eight stages below are the plan for the rest.

| Phase or track | State |
|---|---|
| Phase 0. Foundations | Built: the code's layers, projects with autosave and backups, commands, keymaps and undo, the window, audio and MIDI settings. Crash recovery, Narrator and a MIDI keyboard are not checked by a person. |
| Phase 1. Make a beat | Built: the playlist, sections and outlines, the piano roll, the mixer, the browser, the first devices, MIDI recording and Capture, export, the first launch. Its usability test with a newcomer and an FL Studio user has not been held. |
| Phase 2. Finish a track | Built, and every step checked by hand by the owner: automation, modulators and macros, containers and presets, the audio clip editor, the chord lane, freeze and stems, more devices, the wavetable synth, time-stretching. |
| Phase 3. Polish | Built: every plug-in in a helper process, audio recording, the performance and accessibility passes, three keymaps, help and lessons. Recording through an audio interface and a session with a screen reader are not checked. Chord suggestions, the sound generator and the usability rounds moved after 1.0. |
| Phase 4. Release | Half built: the Windows installer, the update check, the notices, the Sampler, the FM synth, the tempo lane, warping and Consolidate, containers' chains, the 128-pad drum sampler, the Exciter and the Vinyl. Open: 4.3 (code signing), 4.4 (the guide, five pages so far), 4.5 (the release process) and 4.10 (the Multisampler), now in stages 5, 7 and 8. |
| Versions | Built: versions saved, what changed by track, branches, merge, hearing a version against the song, the automatic saves in the history, DAWproject export and import. Stage 4 cut it down to what producers use and gave it plain words, made removed versions recoverable and tied the song to the version it grew from; freeing the store's room by hand is open (S4.4). |
| Architecture track | A0 to A3, A5's decided parts and A6 are built: the layers with their check, one path for every edit, the UI on IDs and plain values, and Phazite's own song model, engine and plug-in hosting on JUCE alone ([architecture.md](architecture.md)). A4 (a token file, a gallery, a strings table) is not started. |
| The plan below | Stages 1 and 2 built (2026-11-15): on Windows, and by CI on Linux with GCC, Clang and the sanitizers, with a nightly run that passes. Then the known bugs were fixed before stage 3's hand checks (2026-11-15 and 16; in `main` since 2026-11-17): every entry but four that say why they stay ([known-bugs.md](known-bugs.md)), each fix with its test and its line in the [changelog](changelog.md), and what the fixes' reviews found fixed with them. Built with them: stage 4's untrusted files (S4.9) and mute that silences the output only (S4.8), and stage 5's plan rebuilt only for what changed (S5.7). Stage 4 is built but for one part (2026-11-17 and 18): the versions' design note approved (S4.1), the cut and the plain words (S4.2), versions that left every branch recoverable for 30 days (S4.3), Verify History, Clean Up and the packs' joining (S4.4), the song tied to the version it grew from with its changes worked out in the background (S4.5), backups kept by age (S4.6), and automation along the control's travel with logarithmic frequency knobs (S4.7). Open: the store letting go of what nothing names (S4.4's last part, known bug 196). Stage 6 is built (2026-11-19), and of stage 5 its MIDI steps, S5.2 to S5.4 (2026-11-20). Stages 3, 7 and 8, and the rest of stage 5, are not started. |

## The plan from here

Eight stages. A **beta** follows stage 5: it makes no promise that its projects open in later versions. **1.0** follows
stage 8: from it on, a project opens in every later version.

Added on 2026-11-17, after a comparison with another free DAW that people can already download: MP3 export
(S5.16, the owner's decision), resampling (S5.17), presets heard in the browser (S5.18), linked tracks (S7.10), and
what S5.5 and S7.6 gained. The release process (4.5) moved from stage 8 to stage 5, so that the beta is a release
anyone can download. What the comparison left for later is under [After 1.0](#after-10) and in the
[backlog](backlog.md).

Added on 2026-11-20 and 2026-11-21 (the owner's decisions, so that the model under Phazite is robust before the
beta): the song's schema as data (S5.19) and the song as an immutable value (S5.20), which was weighed and left at
first and taken once it was measured ([decisions.md](design/decisions.md), "Engineering"). S5.19 comes before S5.20,
and both before the stage's other steps that add to the model, which then write through them. Of S5.20, every
area is built (2026-11-21 to 24): a clip's notes and its controllers' values, the clips themselves (with each
track's order of clips and the alias groups), the tempo map, the chord lane (its element kept as the sign that the
song has one), the MIDI links (their element kept as the place the first link made), automation's curves (their
AUTOMATION kept as their place, each point with a key the selection of points holds), the sections (theirs kept as
the place the first section made), modulation (each track's, the master's and each container's modulators and macros,
kept as their tracks and containers come and go), the sends (each track's, kept as its track comes and goes), and what
the devices store in the history and outside it alike (a plug-in's values set by hand, state, program, format ID and
version, a chain's mute and solo: the first keyed area, undone by entry, with the windows' places outside the history)
and the devices, containers and chains themselves (each by its ID, each owner's list in its order, kept as their tracks
come and go), and the tracks' switches (mute, solo, frozen and the automation mode: keyed, undone by entry) with their
view (each row's height, collapsed and pinned switches, drum rows and automation lanes, outside the history), and the
tracks, folders, returns and master themselves (each by its ID, each folder's list and the top list in their order, the
master a member: the tree's TRACKS and MASTER kept as empty places a whole copy writes them into), and the song's own (its
root's properties and its LASTKEY, PIANOROLL and MERGING: what a step alone writes, plain; the key set last and the
versions' names, keyed; the loop, the play start, the grids and the rest of how it plays and shows, outside the history;
its count of IDs on the song, told to no one; what Phazite doesn't know at its root dropped as it opens) are values
beside the song's tree, in the song's one undo history, with immer in `external/`, and the song's tree holds only empty
placeholders. The end step is still to come: the history as a list of roots, and the tree goes.

### Stage 1. Safe: the bugs that lose work or sound

Built on Windows (2026-11-14): every step, S1.1 to S1.17, each in the [changelog](changelog.md), each bug with a test
that failed before its fix, and each part's changes read by a reviewer whose findings are fixed too. What the steps
left, and the numbered bugs of the audit and of the stages' own tests, were fixed in the round that followed
(2026-11-15 and 16); the entries still in [known-bugs.md](known-bugs.md) say why they stay.

### Stage 2. Caught: the tests that would have found those bugs, and the docs

Built (2026-11-15), each in the [changelog](changelog.md): S2.1 to S2.15, with the gaps the nets had left. The last
step, S2.15, was the first push: the Linux builds with their sanitizers, the Clang build, AddressSanitizer on
Windows, what the steps added to CI (UBSan's reports, the helper's sanitizer reports, the real VST3s' targets,
`windows-profile`, the app started by ctest) and the nightly workflow (`.github/workflows/nightly.yml`) ran for the
first time, and what they found is fixed: CI is green on every preset, and a whole nightly run passed on the same
commit. The owner's PC still has no Linux, no Clang and no x64 ASan component (the setup script installs the last),
so what those find is reproduced in CI: the nightly workflow started by hand runs one job alone
([performance.md](performance.md#the-long-run)). The nets these steps built are listed in
[tests/README.md](../tests/README.md), "The nets"; the smaller test gaps left are in the [backlog](backlog.md), "Tests".

### Stage 3. Checked: a person tries what only tests have tried

The steps are the tables of the [hand checks](hand-checks.md). Each is done when its rows have a name and a date, and
what they found is fixed or in known-bugs.md.

| Step | Size | What |
|---|---|---|
| S3.1 | M | Recording through an ASIO® interface, and playing and recording from a MIDI keyboard |
| S3.2 | M | A session with Narrator and the keyboard alone, and its fixes |
| S3.3 | S | A DAWproject from Phazite opened in Bitwig Studio or Studio One, and one of theirs opened in Phazite |
| S3.4 | M | About twenty third-party VST3 plug-ins loaded, played, saved, reopened and automated |
| S3.5 | M | The other open rows: phases 0, 1, 3 and 4 |

### Stage 4. Decided: the owner's decisions, built

| Step | Size | What | Done when |
|---|---|---|---|
| S4.4 | M | Housekeeping, what is left of it: Free Up Space, a command that lists what the store holds that nothing on a branch names (deleted branches by name and date, automatic saves older than 30 days, bounces), each with its size, and removes what is ticked, in a history one computer alone has written ([known bug 196](known-bugs.md)). Nothing is deleted by itself (the owner, 2026-11-20). With it: what the store still holds stays offered (Recover Lost Versions past 30 days, one automatic save a month past 30 days). Built: packs joined by size, Verify History checking audio and every kept song, Clean Up following | Free Up Space shrinks a history's folder by what it listed, and a review finds no way for it to lose work that a branch, the song or a second computer still names |

### Stage 5. Wide: what a beat maker meets in the first week

Built, each in the [changelog](changelog.md): S5.7 (the plan rebuilt only for what changed, with the known bugs), S5.2
(MIDI channels and program changes), S5.3 (controller lanes in the piano roll) and S5.4 (MIDI learn), the last three
on 2026-11-20, and S5.19 (the song's schema as data) on 2026-11-21. What they left is in the [backlog](backlog.md): a track's own channel, which waits for S7.3, several
lanes at once, and pick-up, ranges and relative encoders for a link. Their rows of the [hand checks](hand-checks.md)
(W1 to W13) need a MIDI keyboard with knobs and a plug-in with several parts.

| Step | Size | What | Done when |
|---|---|---|---|
| S5.1 | L | Stretched, pitched and reversed clips heard at once; then stretch modes and a formant switch | A warp marker dragged is heard on the next play, before any render finishes |
| S5.5 | M | Recording keeps a note played just before the take starts; an input meter; pre-roll; a key that drops the take being recorded and records it again from where it began; takes that survive a full disk and a crash | A take cut off by ending Phazite is offered when the project reopens |
| S5.6 | M | Arming never reopens the audio device; automation written where it is heard (held live notes end when the track changes: built in S1.1) | Arming a track during playback makes no gap in the sound |
| S5.8 | M | MIDI import of every track of a file; MIDI export; range and loop export; project templates | A clip exported as a MIDI file and imported again holds the same notes |
| S5.9 | M | Devices moved, copied, duplicated and deleted between tracks, by commands and by dragging | A device dragged to another strip keeps its automation and modulation |
| S5.10 | M | The strip: trim, polarity, mono, pan law, track delay | Each is a parameter with a control, automation and a test |
| S5.11 | S | Presets carry their own samples: built in S1.16 (a folder of sounds beside the preset); left: that folder swept of what the preset no longer names, and a preset and its sounds moved as one file | A drum kit preset loads on another PC |
| S5.12 | L | The audio clip editor zooms and scrolls; snapping to edges and markers | A hit in a four-minute file can be placed to the millisecond |
| S5.13 | L | The mixer: undo for the wheel, a decimal comma, several strips edited at once, menus from the keyboard, keyboards other than US | Each has a test through `TestWindow` |
| S5.14 | L | Secondary windows that do not block; one Settings window; browser search on a large library | Search answers within a frame's time on 100,000 sounds |
| S5.15 | L | Each layer a library of its own; file sizes counted; the solo rule in one place | `check-architecture.sh` counts files over the limit |
| S5.16 | M | MP3 export, beside WAV and FLAC (the owner, 2026-11-17), with an encoder under a compatible licence in the notices | An exported MP3 plays in another player, and imported again its first note is where the song's is |
| S5.17 | M | Resampling: an audio track records what another track, a bus or the master plays | A track armed with the master as its input records what is heard, placed to the sample |
| S5.18 | M | Presets heard in the browser before they go into the song: an instrument's plays a phrase, an effect's a loop | A preset selected in the browser is heard, and the song is unchanged |
| S5.20 | XL | The song as an immutable value: each area's elements as plain values in Phazite's own types (a clip's start in beats, a choice as its enum), made from S5.19's description and kept with the immer library, which share what a change leaves alone. One area at a time behind the model's functions, the notes first (their code reaches them through typed views alone already: `src/model/notes/internal/NoteElements.h`). While both are there, an area's value changes through the song's one undo history, as an action that puts a root in another's place, so that steps, gestures and trials hold the tree's changes and the values' alike; the song's index, what plays it and its views learn what changed by comparing roots; and the project file stays as it is, an area read into its value as the song opens and written from it as it saves. The code that walks any element (the song's text, the versions' pieces, differences and merge, the loader) goes by the description. immer joins `external/` with its notice (Boost Software License 1.0, which joins the licences Phazite takes). A `check-architecture.sh` rule counts `juce::ValueTree` and raw property access in the model outside the song's own files (about 1,340 and 1,080 today), whose budget only falls | That budget is 0 and the song holds no tree; a value read as another type does not build; the golden files are as they were; Undo, the copy a save takes and a merge no longer grow with the song ([performance.md](performance.md)); and the speed gates pass |
| 4.5 | S | The release process: the version from the tag, release builds and the installer from CI, the changelog by version | A tag produces a release with the installer, its SHA-256 and `phazite-update.json` |

**Beta.** The steps above are done, stage 3's rows are checked, and the beta is a release made by 4.5 that anyone can
download. Its installer is not signed yet (4.3 is in stage 8), which the release has to say.

### Stage 6. Deep: every device and editor to the standard of the best plug-ins

An audit of the device list came first (2026-11-17): twenty devices become sixteen, each generic and whole, by the
merges in S6.6, S6.7 and S6.10, and S6.11 and S6.12 widen two more ([decisions.md](design/decisions.md), "Sound and
devices"). The steps that change the list come first, since the listening sets, the presets and the editors follow it.
Built, each in the [changelog](changelog.md): S6.3 (the Compressor), S6.4 (the EQ), S6.5 (the Reverb's Hall), S6.2 (the devices' bugs), S6.6 (the Saturator, the Delay and the Filter), S6.9 (the editors), S6.7 (the Synth),
S6.10 (Motion), S6.11 (Age), S6.12 (Stereo), S6.1 (a listening set and a verdict in each device's page, their
standings in [design/devices.md](design/devices.md)) and S6.8 (the presets on the finished devices).

Every step of the stage is built (2026-11-19). What waits for the owner: the hand checks E1 to E23
([hand-checks.md](hand-checks.md), "What stage 6 built") and each device's listening set, by ear; and the benchmarks of
the release build on a quiet machine, before `main` moves ([performance.md](performance.md)).

### Stage 7. Professional: what bands, vocalists and keyboard players need

| Step | Size | What | Done when |
|---|---|---|---|
| S7.1 | XL | Mono tracks, more than one output pair, a cue mix | A performer hears a mix of their own on a second output pair |
| S7.2 | XL | Scale: a scheduler that does not spin, the benchmarks on a laptop (read-ahead before a jump is built). The 100-track benchmark is built, its numbers in [performance.md](performance.md) | 100 busy tracks play at 128 samples on four threads with room to spare |
| S7.3 | L | Multi-out plug-ins and MIDI channels | A multi-out drum plug-in plays each output on a strip of its own |
| S7.4 | L | Routing: buses send to returns, sidechains inside containers, macros automated, any track or bus as an output | Each has a model test and plays in a golden render |
| S7.5 | L | Note effects, and MIDI from plug-ins | An arpeggiator before an instrument plays in time, live and rendered |
| S7.6 | L | MIDI output, clock out, a MIDI input for each track, which can be another track's notes | A hardware synth plays a track's notes in time with the song, and a second instrument track plays the notes of the first |
| S7.7 | L | Take lanes and comping | A vocal is put together from three takes, each edit one undo step |
| S7.8 | M | Time-signature changes along the song | A change added at a bar moves the bar lines, the metronome and the count-in |
| S7.9 | L | Clips, notes and automation points reachable by a screen reader | Narrator reads the note under the keyboard's cursor |
| S7.10 | L | Linked tracks: the tracks of a folder edited as one, so that a move, a trim, a split or a deletion of one track's clip is made to the others' at the same place | A split on one of four linked drum tracks splits all four, as one undo step |
| 4.10 | XL | Many recordings across the keys in the Sampler (what was planned as a Multisampler of its own), as a plain SFZ player with one piano | The Grand Piano, picked in the browser, downloads its pack and plays from disk without a dropout; an SFZ file of the user's plays, listing what Phazite does not read |

### Stage 8. Release

| Step | Size | What | Done when |
|---|---|---|---|
| 4.3 | M | Code signing: the installer and its updates signed, and an update checked for the same signature | Windows shows the publisher and no SmartScreen warning |
| 4.4 | L | The guide: a page for each view and each device, with pictures painted by tests | Every view and device has a page in [guide](guide/getting-started.md), and a keyboard reference |
| A4 (part) | L | The text the user reads in a strings table | No user-facing string is left in C++ outside the table |
| S8.1 | M | Release builds with symbols, and crash dumps the user can send | A crash in a release build gives a readable stack |
| S8.2 | M | The compatibility promise: a format version in the file, a golden project for each release, newer files refused with a reason | The 1.0 golden project opens in every later build's tests |
| S8.3 | L | Linux as a beta: PipeWire or JACK, HTTPS for the downloads, install rules, and the app tried by a person | The Linux rows of the hand checks are checked |
| S8.4 | S | The installer's silent update tested | CI updates an installed older build and starts it |

**1.0.** Every stage is done, and the hand checks have no open row.

### After 1.0

Not in 1.0, in no fixed order. More, with reasons, is in the [backlog](backlog.md).

- **From the first plan:** usability rounds with new people (was 3.8); the browser's analysis of the user's own
  sounds, with Show Similar (the second part of issue #24); chord suggestions (was 3.1); the sound generator and a
  drum synth device (was 3.2); the AI assistant; articulations in the piano roll; guitars, and a synthesized organ,
  electric piano and plucked strings; a DAWproject's plug-ins and sends imported; neural sound generation;
  audio-to-MIDI; MPE; Linux screen readers.
- **A4, the rest:** the design tokens in one file a designer edits, a gallery app and a guide for the designer. It
  starts when a designer joins.
- **Out of 1.0 by the plan:** video, notation, a clip launcher, scripting, CLAP, ARA, macOS with Audio Units,
  translations, Ableton Link and control surfaces.
- **From the comparison with another DAW (2026-11-17):** a catalogue of free plug-ins and sound packs installed from
  inside Phazite, each pinned by its size and SHA-256, as Phazite Sounds are; vocal pitch correction; tools that write
  notes in the piano roll (Euclidean rhythms and the like).

## Decisions still needed from the owner

One line each; the rows, with what is built today, are in [open-questions.md](open-questions.md).

| Decision | Needed before |
|---|---|
| What the activity bar holds beyond its three views | S5.14 |
| Whether Phazite gets a Pitch device | Stage 7 |
| Whether the Sampler's many recordings (4.10) stay in 1.0 | 4.10 |
| A certificate, or an open-source signing program | 4.3 |
| The limit on a source file's size | S5.15 |
| Whether the usability test with new users is held before the beta | The beta |
| The choices made on the owner's behalf and never reviewed | 1.0 |

## Risks

| Risk | What is done about it |
|---|---|
| The scope grows. It is not fixed: phase 4 took eight feature steps while its release steps waited, and the audit added most of the stages above | 1.0 is the eight stages and nothing else. A new idea goes to the backlog unless it replaces a step here |
| Built is taken for working: most of phases 0, 1, 3 and 4 has passed its tests and been tried by nobody | Stage 3, before any more features; the README says which is which |
| One PC, one user: no audio interface, MIDI keyboard, screen reader, other DAW or Linux desktop has met Phazite, and every measurement is from one machine | Stage 3, the Linux rows in stage 8, and usability rounds with new people |
| Projects saved now may not open later, so nobody can keep real work in Phazite | Said in the README; compatibility starts at 1.0 (S8.2) |
| Built-in devices fall short of the plug-ins people own | Stage 6: a listening set and a verdict for each device, with DSP tests |
| The audio thread breaks the real-time rules | The real-time audit after every change to playback; its guarded scenarios fail if steady playback allocates (S2.5 widens them) |
| A fix or a restructuring changes behaviour by accident | The golden tests of saved projects, renders and commands, and stage 2's nets |
| The layers erode, and files grow | `tools/check-architecture.sh` in CI and before every commit, every budget at 0 but that of the literals the design tokens should hold, which only falls; file size is not counted yet (S5.15) |
| The UI is the largest part of the work | One view at a time, on the model's functions, each with tests through `TestWindow` |
| An unsigned installer is warned about by Windows | 4.3 |

ASIO is a registered trademark of Steinberg Media Technologies GmbH. VST is a registered trademark of Steinberg Media
Technologies GmbH.
