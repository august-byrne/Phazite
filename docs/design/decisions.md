# Decisions

This page lists the decisions that shape Phazite and that a contributor might otherwise reopen or get wrong: what
Phazite is and is not, who it is for, what it builds on, what it deliberately does not do, and the rules of working on
it. Each row gives the decision, the reason for it and the page that describes the area.

How Phazite works in detail is on the area pages. A key, a colour or a default that the code plainly shows is not
listed here. A decision is a rule to build by, and a few are newer than the code: the area page says what is built.
What still waits for an answer is in [open-questions.md](../open-questions.md).

## Product

| Decision | Reason | Area |
| --- | --- | --- |
| Version 1.0 is for music professionals in general, with music production first: the work FL Studio, Ableton Live and Logic Pro are used for. | Depth is judged against the DAWs professionals finish songs in, not against a beginner's tool or a post-production suite. | [product.md](product.md) |
| A beta comes before 1.0 and does not promise that its projects open in later versions. | The formats can still change freely while people try Phazite. | [product.md](product.md) |
| Where the project's owner has no strong view on a behaviour, Phazite does what Ableton Live and most professional DAWs do. | Habits carry over, and a behaviour is not argued from nothing each time. | [product.md](product.md) |
| Every track is a playlist row, a device chain and a mixer channel in one. There is no channel rack. | The split between channel rack, patterns, playlist and mixer is the complaint FL Studio's users repeat most. | [building-blocks.md](building-blocks.md) |
| There is one way to do each task: where two controls would do the same thing, one is removed. | Nobody has to learn which of two ways is the right one. | [product.md](product.md) |
| Nothing plays while it is hidden. A row or a lane collapses to a thin row and never closes, and no track plays unheard to drive something else. | What is heard can always be found in the window. | [product.md](product.md) |
| There is no beginner mode and no professional mode. | A mode hides features from the people who would grow into them. | [product.md](product.md) |
| A clip launcher is not in version 1. Planned after it. | Version 1 is the arrangement: the playlist, the piano roll and the mixer. | [product.md](product.md) |
| Chord suggestions, the sound generator and every AI feature wait until after version 1. Their first engines are algorithmic, trained models follow, and there is no stem separation. | Version 1 is the editing, mixing and hosting a producer needs every day; a trained model must first pass the licence check on its weights and data. | [ai.md](ai.md) |
| Windows and VST®3 come first. Linux follows, then macOS with Audio Units. | Most of the people Phazite is for use Windows. | [product.md](product.md) |
| There is no account and no cloud, and nothing is sent anywhere unasked: the update check runs only when the user asks, or once they turn it on. | Projects stay private and on the user's machine. | [privacy-licences.md](privacy-licences.md) |
| Updates come only from Phazite's GitHub releases, over HTTPS, and an installer is checked by its size and SHA-256 before it runs. | An update must not be a way to run someone else's code. | [privacy-licences.md](privacy-licences.md) |
| Phazite is AGPL-3.0-or-later and takes in only code, fonts, models and data under compatible licences, each listed with its notice. | It stays free to share and to build on, and everything it ships can be accounted for. | [privacy-licences.md](privacy-licences.md) |
| Phazite shows no ASIO® or VST logo. Documents write the ® at the first mention and carry Steinberg's trademark sentences. | The logos are optional under the terms Phazite uses, and they are not under a free licence. | [privacy-licences.md](privacy-licences.md) |
| Export writes WAV and FLAC, and MP3 before 1.0 (roadmap step S5.16). | The owner's decision (2026-11-17), which takes back the earlier one against MP3. The case made for it: producers send MP3 drafts daily, and its patents ended in 2017. | [projects-saving-export.md](projects-saving-export.md) |
| Plug-ins and sounds that are not Phazite's own are never built in. At most they are optional downloads, as Phazite Sounds are. | Phazite's own devices keep one design language, and the installer stays small and licence-clean. | [devices.md](devices.md) |
| No sounds or samples are in the repository. Factory sounds are synthesized, and Phazite Sounds (CC0 only) are a separate download pinned by address, size and SHA-256. | Every sound's licence is beyond doubt, and the repository stays small. | [browser.md](browser.md) |
| An example song that someone else made ships as its notes alone, under CC0 or CC BY only. | People save what they make from an example as their own, and those licences ask at most for credit. | [getting-started.md](getting-started.md) |
| A new project starts empty. Example songs and lessons are in the Help menu. | People open a DAW to start their own song; the examples are a click away for those who want them. | [getting-started.md](getting-started.md) |
| Three keymaps ship: Phazite's own, FL Studio's and Ableton Live's, chosen on the welcome window. | People keep the keys their hands know. | [commands-keys-help.md](commands-keys-help.md) |
| Everything works from the keyboard, controls have names and values for screen readers, and colour is never the only signal. | Accessibility is a rule for every control, not a later pass. | [accessibility.md](accessibility.md) |

## Window and editing

| Decision | Reason | Area |
| --- | --- | --- |
| Phazite works in one window. By default the upper panel shows the whole song (Playlist and Mixer), the lower panel shows what is selected (the clip editor, Instrument and Effects), and the inspector shows the properties of what is selected, not a copy of a mixer channel. | The whole song and the detail of one thing are both in reach with few keys to remember, and a second copy of the channel strip was a second way to do the same thing. | [window.md](window.md) |
| The dark and light themes are VS Code's 2026 Dark and 2026 Light and the sizes its Modern UI's, under VS Code's MIT notice. VS Code's name, logo and icons are not used. | A proven, familiar look that its licence allows; the name and the icons are not Phazite's to take. | [window.md](window.md) |
| Menus, tooltips and pickers open inside the main window, on frosted glass, and reach no further than its edges. | Blurring what is behind a popup needs the popup inside the window. | [window.md](window.md) |
| Every colour, size, corner radius and font in the window comes from the design tokens, never a literal. | The look changes in one place, and a theme switch reaches everything. | [window.md](window.md) |
| Every action is a command with an id, a name, a description and search words. Default keys are in the keymap files, not in code. | Menus, keys, the command search, the shortcuts editor and the tests all use one list. | [commands-keys-help.md](commands-keys-help.md) |
| While the computer keyboard plays notes, Shift and a letter run that letter's single-key shortcut, so no keymap gives Shift and a letter a different command. | Ableton Live's way: the tools stay in reach while the keyboard plays. | [commands-keys-help.md](commands-keys-help.md) |
| The computer keyboard does not play notes until it is switched on. | The single-letter tool keys work from the first start. | [commands-keys-help.md](commands-keys-help.md) |
| Basic mouse edits need no modifier keys. In an automation lane a click adds a point, a handle in the middle of a line bends it and a double-click deletes a point. | Key combinations for everyday edits have to be learned and are forgotten. | [automation-and-modulation.md](automation-and-modulation.md) |
| Lanes are the one way to automate. There are no automation clips. | Most DAWs automate in lanes tied to their tracks, and a second way would break the one-way rule. | [automation-and-modulation.md](automation-and-modulation.md) |
| Every automated parameter has a lane, and a lane goes only when its automation is deleted. | Automation that is hidden still plays. | [automation-and-modulation.md](automation-and-modulation.md) |
| Automation moves and copies with the clips above it, and with time ranges deleted or duplicated across tracks. | A part keeps its moves, as in Ableton Live, Bitwig and Studio One. | [automation-and-modulation.md](automation-and-modulation.md) |
| A straight automation line is an even move of the knob. | What is drawn is what turning the control at a steady speed would do. | [automation-and-modulation.md](automation-and-modulation.md) |
| With Snap on, a moved note or clip keeps its offset from the grid. | As Ableton Live does: an item placed off the grid on purpose stays as far off it. | [playlist.md](playlist.md) |
| Places and lengths in the song count quarter notes whatever the time signature. The time signature sets only the bar's length and the beat that the grid and the metronome count. | MIDI, DAWproject, plug-ins and every major DAW count so; counting the signature's own note played warped clips and plug-ins' playheads wrong. | [building-blocks.md](building-blocks.md) |
| The time line is in beats, so a tempo change does not move the grid. | Clips and notes keep their places in the bars. | [playlist.md](playlist.md) |
| The tempo changes in a tempo lane, and the transport bar shows the tempo at the playhead. There is no override mode. | As most DAWs do, and there is one tempo to read. | [window.md](window.md) |
| In the piano roll a left-click adds a note with the last note's length and a right-click deletes one. There is no draw-mode switch, no lasso and no right-click menu in place of delete. | It is the most praised way to enter notes in every forum the comparison of seven DAWs read, and the comparison advised against the others. | [piano-roll.md](piano-roll.md) |
| Stop goes back to where playback started. Pausing is a separate key. | The same passage plays again at one key, as in FL Studio, Ableton Live and REAPER. | [window.md](window.md) |
| Playback loops the whole song unless a range is looped on purpose. | As FL Studio's song mode: a song being built plays round without setting anything up. | [playlist.md](playlist.md) |
| The song ends at the end of the bar its last clip ends in. Sections and the chord lane do not count. | It is where an export and the song's loop end. | [playlist.md](playlist.md) |
| Clips that overlap on a track all play, and show one under another in the track's row. | REAPER's way: nothing is hidden behind another clip or silently cut. | [playlist.md](playlist.md) |
| A duplicated or pasted time range replaces what it lands on. | Nothing then plays twice, and clearing the whole stretch would delete clips on tracks where the range was empty. | [playlist.md](playlist.md) |
| Routing is set on the mixer's strips: the Out menu, a send knob for each return and a device's sidechain picker. A routing view and a wired graph, in the mixer or in containers, are Declined. | Every DAW audited routes on the strip, and a graph is a second way that its own users call complex. | [mixer.md](mixer.md) |
| One switch on a track mutes with a click and solos with a right-click, and a solo never changes the mute switches. | FL Studio's switch without its solo that rewrites the mutes, a long-standing complaint there. | [mixer.md](mixer.md) |
| Mute silences a track's output only; its instruments keep playing. | A muted track still feeds what listens to it, and comes back in time when unmuted. | [mixer.md](mixer.md) |
| An instrument has no on/off switch. The track's switch mutes it and Freeze frees its CPU. | Each job has one way, and a track whose instrument is off does nothing without showing why. | [mixer.md](mixer.md) |
| Every new folder is a bus, and a folder is the bus of the tracks in it: choosing a bus as a track's output moves the track into that folder. | The playlist and the mixer never disagree about where a sound goes. | [mixer.md](mixer.md) |
| A send gives a return a copy, and the track still reaches the master. Every strip has a send knob for each return, off until first turned. | One sentence explains sends and nothing has to be added first; a bus is the way to run several tracks through one effect. | [mixer.md](mixer.md) |
| Pan is constant power with the sound at its own level in the middle and 3 dB louder hard to one side. | Ableton Live's and Logic Pro's default: a sound keeps its loudness as it pans. | [mixer.md](mixer.md) |
| A device's sidechain is picked inside the device, not in the list of effects. | Ableton Live and Bitwig keep theirs in the device, and a button on every row cluttered the list. | [mixer.md](mixer.md) |
| View state (track heights, collapsed rows, grid settings, where a plug-in's window was) is saved with the project but is never an undo step or an unsaved change. | Looking at a song must not change it. | [projects-saving-export.md](projects-saving-export.md) |
| A change put back leaves nothing behind: a switch turned on and off again is not an unsaved change and shows nothing in the versions' changes. | The song is as it was, so nothing should say otherwise. | [projects-saving-export.md](projects-saving-export.md) |

## Sound and devices

| Decision | Reason | Area |
| --- | --- | --- |
| The built-in devices are few and of very high quality. The bar is FabFilter and Valhalla, not other DAWs' stock devices, and a device is improved before another is added. | Nobody should need a third-party plug-in for the basics, and a weak stock device is worse than none. | [devices.md](devices.md) |
| Devices have plain names: EQ, Compressor, Reverb, Synth. | Plain names, no private vocabulary. | [devices.md](devices.md) |
| Every built-in device's editor fits the lower panel at the default window size without scrolling, which a test checks. | A device is worked whole, at a glance. | [devices.md](devices.md) |
| No device gets a control for what another control already does: no balance or auto-pan in the Stereo Expander, since the track's pan and a modulator on it do both. | One way per task. | [devices.md](devices.md) |
| The Limiter has no mix. | Below 100 % the dry sound would take peaks past the ceiling. | [devices/limiter.md](devices/limiter.md) |
| One Multiband device, with the layout and abilities of FL Studio's Maximus, stands in place of a separate upward-and-downward multiband and a maximizer. | One deep device rather than two that overlap. | [devices/multiband.md](devices/multiband.md) |
| The Saturator stays a device of its own beside the Compressor's characters. | Every DAW checked keeps a saturator beside compressors that colour. | [devices/saturator.md](devices/saturator.md) |
| A preset is named by its sound, never by a trademark: "808", not a maker's model name. | People find the sound by the word they use for it, and Phazite uses nobody's mark. | [../presets.md](../presets.md) |
| A preset is accurate to its name and good on its own. Example songs use presets as they come and only mix them. | A preset tuned for one demo song fails everyone else. | [../presets.md](../presets.md) |
| Presets of a kind are leveled alike, which the tests check. | Switching presets does not jump in loudness. | [../presets.md](../presets.md) |
| Audio clips and browser previews are stretched and pitched by one stretcher, Rubber Band's R3 engine. | Measured, it did best on drums and on held chords, and one stretcher makes a preview sound as the clip will. | [engine.md](engine.md) |
| A stretched clip is to be heard at once, as in other DAWs, not silent until it has rendered. | A tempo change must not silence parts of the song. | [engine.md](engine.md) |
| What plays does not depend on the audio device's block size: automation and modulation move on fixed control points along the song. | A song sounds the same on every machine and in every export. | [engine.md](engine.md) |
| Nothing that plays is random without a fixed seed. | A song renders the same every time, so golden renders can guard it. | [engine.md](engine.md) |
| A recording is placed where it was played, to the sample. Sound lost while the drive stalls becomes silence in its place, and recording goes on. | The rest of the take keeps its place, where stopping would lose the performance. | [recording.md](recording.md) |
| A take round a looped range keeps everything played, including what came before the range and a first, partial time round. | Nothing played is thrown away. | [recording.md](recording.md) |
| Nothing normalizes by itself. The playlist draws clips at their own level, and Normalize is a command that sets a clip's gain. | Every DAW checked draws and plays clips at the level they have. | [piano-roll.md](piano-roll.md) |
| A project saves a plug-in's parameter by the plug-in's own ID for it. | Automation and modulation follow the parameter through the plug-in's updates. | [plugin-hosting.md](plugin-hosting.md) |

## Projects and versions

| Decision | Reason | Area |
| --- | --- | --- |
| There is no backward compatibility before the beta: no format versions, no migrations and no code kept for what earlier builds saved. Saved names may be renamed freely. | The formats can be made right while nobody depends on them. | [projects-saving-export.md](projects-saving-export.md) |
| Projects and presets from other people must load, so every file Phazite reads is untrusted input. Built: a song is made consistent as it opens, a preset's volumes and sound paths are checked, a DAWproject's unpacking is capped, and every reader is fuzzed in every test run. A preset takes sounds from its own folder only, and only the channels handed to a helper are read back. | A shared file must not be able to crash Phazite or make it do anything but open a song. | [projects-saving-export.md](projects-saving-export.md) |
| Auto Save stays on. | People think about versions, not about saving. | [projects-saving-export.md](projects-saving-export.md) |
| Audio that is dropped in or recorded is copied into the project's Audio folder and named by a path relative to the project. | A project's folder can be moved or shared whole. | [projects-saving-export.md](projects-saving-export.md) |
| Every name written into a project file is defined once, in `src/model/base/internal/Schema.h`, and Phazite saves its own values and names, never a library's enum number. | The format does not change when a dependency does. | [projects-saving-export.md](projects-saving-export.md) |
| The versions feature is what producers use: commit, history, restoring the song or one track, branches, A/B and Local History. Staging, a commit checked out on its own and a merge's ticked lines are not part of it. | Non-programmers stumble on the staging area and on merges, and the creative tools people praise hide that machinery. | [../versions.md](versions.md) |
| The view is called Versions and says what it does in plain words: Save Version, History, Switch Branch, Restore This Version. Branch and merge stay; git's other words stay only as search words for the commands. | Every DAW that keeps versions says "version", and "commit", "checkout" and "working tree" mean nothing in a studio (the owner, 2026-11-17, which takes back git's words of 2026-11-05). | [versions-cut.md](versions-cut.md) |
| Each computer keeps its own records of a project's branches, as built, and nothing more is built on them until collaboration is designed. | They are what makes a project in a synced folder safe on a studio PC and a laptop: no file has two writers (the owner, 2026-11-17). | [versions-cut.md](versions-cut.md) |
| A version that leaves every branch (undone, or its branch deleted) is kept for 30 days and can be recovered; a merge takes every line; a double-click on a version hears it. | A history must not lose work, and one track from another branch is a restore, not a merge. | [versions-cut.md](versions-cut.md) |
| Collaboration, live or by merging each person's branch, comes later. | Versions first serve one person's song. | [../versions.md](versions.md) |
| There is no stash. Each branch keeps its own work, and A/B hears the last commit in place of the song. | A stash was wanted for comparing two states by ear, which A/B does. | [../versions.md](versions.md) |
| Versions are Phazite's own store in the project's folder, the song in pieces named by their SHA-256. They are not a git repository. | Git merges lines of text, which a song is not, and rewrites its own files, which a folder synced between computers can damage; pieces written once cannot be. | [../versions.md](versions.md) |
| DAWproject is for exchange with other DAWs, not Phazite's own format. | It has no place for modulators, the chord lane, containers' macros or Phazite's devices. | [../versions.md](versions.md) |

## Engineering

| Decision | Reason | Area |
| --- | --- | --- |
| Phazite builds on JUCE alone, with its own song model, playback engine and plug-in hosting. No other DAW engine is linked. | The engine Phazite began on broke the real-time rules and the timing of clips in places that could not be fixed from outside it. | [engine.md](engine.md) |
| There is one copy of JUCE, compiled once in `phazite_core`. Dependencies are submodules pinned to a commit and are never edited. | Builds are reproducible and a dependency bump is a reviewable change. | [code-structure.md](../architecture.md) |
| Dependencies point down the layers (app, UI, services, model, engine and sandbox, devices), and there is no window code below the UI. | The UI can change without touching the rules of editing, and every edit can be tested without a window. | [code-structure.md](../architecture.md) |
| The devices, the engine, the sandbox and the helper program know nothing of the song model. | A device could run on another engine, and a part of the engine can be replaced. | [code-structure.md](../architecture.md) |
| The UI decides nothing about the project: it runs a command or calls a model function, by IDs and plain values. | The rules of editing are in one place and are tested without a window. | [code-structure.md](../architecture.md) |
| Every change to a project is one named undo step with one owner, and a drag is one step. | Undo reads as what the user did. | [code-structure.md](../architecture.md) |
| `tools/check-architecture.sh` counts what the architecture rules forbid against budgets of 0, in CI and before each commit. | A rule that is not checked erodes. | [code-structure.md](../architecture.md) |
| The audio thread never allocates, locks, waits, logs or reads a file, and the real-time audit's guarded scenarios fail if steady playback allocates. | Playback must not glitch at low latency. | [engine.md](engine.md) |
| Plug-ins are hosted only in helper processes, one for each plug-in file. | A plug-in that crashes or hangs must not take Phazite or the song with it, and the measured cost of a round trip is small. | [plugin-hosting.md](plugin-hosting.md) |
| The plug-in sandbox exists to survive crashes and hangs, not to contain a hostile plug-in. | A plug-in is code the user chose to install; the helper is there for stability, not as a security boundary. | [plugin-hosting.md](plugin-hosting.md) |
| No thread is ever ended by force. | On Windows a thread ended while it holds a lock leaves the lock held for ever, which once hung every thread started after it. | [code-structure.md](../architecture.md) |
| A race or a leak in Phazite's own code is fixed, never suppressed. | A suppression hides the next one too. | [code-structure.md](../architecture.md) |
| Displays poll small read-only sources and repaint only when what they draw moved. Nothing that changes many times a second is stored in the project or sent as a message. | In silence, with nothing turned, nothing repaints. | [performance.md](performance.md) |
| Golden files (saved projects, render fingerprints, the commands' catalogue, the formats' test vectors) change only on purpose, with the reason in the commit message. | If a refactoring changes one, the behaviour changed. | [code-structure.md](../architecture.md) |
| A reported UI bug is reproduced with a test before it is fixed. | The same kind of fault is then found by a test, not by a user. | [code-structure.md](../architecture.md) |
| Every place the code works around JUCE is listed in one document and marked in the code. | A JUCE upgrade can remove what it fixed and check what leans on JUCE's insides. | [../juce-workarounds.md](../juce-workarounds.md) |
| A bug that cannot be fixed in the change that finds it is written into the list of known bugs, and the commit that fixes it removes it. | Nothing known is left only in someone's memory. | [../known-bugs.md](../known-bugs.md) |
| Nothing confidential reaches the repository: the git hooks check every commit and push and are never skipped. | The repository is public, and a commit stays in its history. | [../../CONTRIBUTING.md](../../CONTRIBUTING.md) |
| The build is free of warnings, with warnings as errors in CI on MSVC and GCC, and the code is formatted by clang-format. | A warning left alone hides the next one, and each compiler finds what the other does not. | [../building.md](../building.md) |
| The user guide is written in this repository, and the website that publishes it is a repository of its own. | A change updates the guide's page in the same commit, and the site's packages stay out of Phazite's build. | [../guide/getting-started.md](../guide/getting-started.md) |
| The website's pictures of Phazite are painted by a test in this repository (`tests/SitePictures.cpp`) and kept in the website's. | The home page's captions point at places in the pictures, so both change in one commit there; this repository's history doesn't grow by megabytes at each repaint; and the site is published without waiting for a change here to reach `main` (the owner, 2026-11-17). | [../../tests/README.md](../../tests/README.md) |

ASIO is a registered trademark of Steinberg Media Technologies GmbH. VST is a registered trademark of Steinberg Media
Technologies GmbH.
