# Versions

*Status: draft plan for review, 2026-11-04, written at the owner's request, and revised the same day after a survey of [what others tried](#what-others-tried).* This draft covers three things:
- versions of a project that work as git's commits and branches do, each with a bounce or without;
- a column of icons down the window's left side, as VS Code's activity bar, whose second icon opens the versions;
- how DAWproject import and export fit with the versions.

Each choice below is Claude's, for the owner to review. Once the owner agrees, the decisions go into [design.md](design.md), the steps into the [roadmap](roadmap.md), and an issue for each step onto GitHub. Sizes are relative effort, S < M < L < XL, as in the roadmap; they are not dates.

## Why

**Producers ask for it.** People ask for version control of their projects in GitHub's style, on Gearspace ([thread](https://gearspace.com/threads/github-type-version-control-repository-for-music-production.1396184/)) and on Ardour's forum ([thread](https://discourse.ardour.org/t/how-is-version-control-not-a-thing-in-daws/89124)). The best-loved part of Splice Studio ([CDM](https://cdm.link/splice-studio-is-free-backup-version-control-and-collaboration-for-your-daw/)) was its history of versions, each with a bounce. Splice closed it in 2023.

**What DAWs offer today is less:**
- Logic Pro's project alternatives are branches that never merge.
- Studio One's, Cubase's and FL Studio's Save New Version keep whole copies of the project file.
- Ableton Live and Phazite itself (`Backups/`) keep the last saves.

**Phazite is in a good position to do more.** Only a DAW knows a project's structure. Phazite's song already names every track, clip, device and section by an ID that is never given twice, and every note by an ID within its clip. So versions can be:
- stored as differences rather than copies;
- compared thing by thing;
- merged track by track, even note by note.

**The tools that survived kept bounces, not projects.** A project that versions the recipe gives an example ([Wit](https://github.com/sep-lab/Wit)): 29 versions of one Ableton set took 270 MB as copies, 0.8 MB in git and 0.3 MB as differences. It also says that every funded tool in this space versions bounces, not projects, and that almost all of the others have closed. Phazite can do both: keep the recipe, and attach the render when the user wants one. That is the "bounce or not".

**It needs nothing else.** Versions are useful to one person with no account and no network. They are also the base the cloud plan builds on: backup, share links with comments, and collaboration ([later](#later-the-cloud-on-top)).

## What others tried

A survey on 2026-11-04 looked in three places:
- DAWs and the tools people built around them;
- music's cloud services;
- other fields that keep versions of their files: game audio, CAD, design tools, and research on writing together.

It found seven ways of keeping versions. Two findings stand out:
- **Nobody has shipped merges of songs.** In every DAW and service, merging means importing tracks from one project into another by hand.
- **Every history kept only on a music company's servers closed with it:** Splice Studio (2023), VST Transit (2023), Gobbler (2023), Blend (2024) and Endlesss (2024).

| | A version costs | Shows what changed | Branches and merges | Audio | Outlives its maker | Used by |
|---|---|---|---|---|---|---|
| 1. Whole copies | the whole file | no | copies, merged by hand | shared | yes | every DAW |
| 2. Git on the project file | little, once git packs it | lines of text, mostly noise | as text, which breaks songs | in Git LFS | yes | hobby tools |
| 3. Many text files, with locks | little | file by file | file by file; binary files locked | in the repository | needs a server | game studios |
| 4. Pieces by content, compared by meaning | bytes to kilobytes | tracks, notes and values | three-way, by ID | stored once | yes | Dolt, for data; no DAW yet |
| 5. A company's server | on its servers | which tracks changed | forks; merges all or nothing | uploaded | no | many closed |
| 6. Every edit, merged live | every edit | needs work for each kind of document | live | uploaded | depends | Ohm Studio, closed |
| 7. Bounces alone | 6 to 8 MB a minute | by ear | no | it is audio | depends | growing |

**1. Whole copies, in the DAW.**
- **What they do:**
  - Studio One (now Fender Studio Pro) keeps each Save New Version in the song's History folder, with a description ([PreSonus](https://support.presonus.com/hc/en-us/articles/8798575899917-Studio-One-6-Exploring-the-Save-Options)).
  - Cubase numbers its copies, and Ableton Live keeps the last 10 saves.
  - Logic Pro's alternatives and Ardour's snapshots are copies that share the audio.
  - None shows what changed, and merging is importing tracks.
- **What users say:**
  - Cubase's users ask for a note on each version, and for the Hub not to list every one ([forum](https://forums.steinberg.net/t/save-as-new-version-add-note-for-version-information/1031505)).
  - Ardour's users find snapshots confusing, because saving changes the snapshot that is open ([thread](https://discourse.ardour.org/t/what-exactly-is-a-snapshot/79857)).
  - In one producer's 30 Logic projects, no alternative was ever used. The producer branched by Save As instead, into copies of 500 to 700 MB ([Wit](https://github.com/sep-lab/Wit)).
- **What Phazite takes:** a note on each version, and versions listed inside the project, not beside it among the projects.

**2. Git on the project file.**
- **What they do:** these tools put the project file in git and its audio in Git LFS:
  - [Ableton-Live-tools](https://github.com/danielbayley/Ableton-Live-tools), with 381 stars;
  - [ableton-git](https://github.com/clintburgos/ableton-git);
  - [GitDaw](https://github.com/raphaelDkhn/GitDaw);
  - [SnapTrack](https://github.com/jakeyjakeyy/SnapTrack), which runs git from a plug-in.

  The file is Ableton's `.als`, unzipped, or Reaper's plain-text `.rpp`.
- **How it went:** none grew past a hobby.
  - Switching versions means closing the set and opening it again.
  - The differences in the text are noise: across 29 saves of one set, the median save "changes 271 raw lines and 2 musically meaningful ones" ([Wit](https://github.com/sep-lab/Wit)).
  - A merge that is clean as text can break a song:
    - a Reaper developer warns that one can damage a plug-in's state ([forum](https://forum.cockos.com/showthread.php?t=258218));
    - the test under [Decided against](#decided-against) merged two branches cleanly into duplicate IDs.
  - Outside music, the git tools for Sketch's design files (Abstract, Kactus, Plant) are gone or dormant. The history Figma built into the tool itself won.
- **What Phazite takes:** git's ideas without its files, and differences in Phazite's own words.

**3. Many text files, with locks (game audio and game engines).**
- **What they do:**
  - Wwise splits a project into XML work units, and locks the files that can't be merged ([Audiokinetic](https://www.audiokinetic.com/qa/4343/which-wwise-file-types-require-exclusive-locks)).
  - FMOD Studio keeps one XML file per object, and offers Accept Yours, Accept Theirs or Merge for each conflict ([FMOD](https://d1s9dnlmdewoh1.cloudfront.net/2.03/studio/using-source-control.html)).
  - Unity's Smart Merge merges a scene object by object, by ID ([Unity](https://docs.unity.com/en-us/engine/current/manual/get-started/project-configuration/version-control/smart-merge)).
  - Unreal Engine keeps one file per actor, and locks binary files in Perforce ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/one-file-per-actor-in-unreal-engine)).
- **How it went:** it is the norm in game studios, which have a server and people trained to use it. What they learned:
  - Splitting the project by stable IDs did more than clever merging.
  - Files made from the project and kept in the history (caches, builds) conflicted all the time. A game-jam team gave up FMOD over it.
  - A change taken only in part can leave references to things that aren't there.
- **What Phazite takes:**
  - pieces split by ID;
  - renders kept out of the song's history: bounces are attached, never merged;
  - references repaired after every merge, take and bring back.

**4. Pieces named by their content, compared by meaning.**
- **What they do:**
  - [Wit](https://github.com/sep-lab/Wit) is a pre-release tool from 2026.
    - It keeps an Ableton set's history at about 11 KB a save, and stores audio once.
    - It keeps renders as a cache that can be pruned.
    - It describes a change as `volume: 0.794 -> 0.525`.
    - Its merges are untested.
  - [alsdiff](https://github.com/mattypie/alsdiff) compares two Ableton sets by their internal IDs.
  - [DAWLab](https://github.com/omrynskyi/DAWLab) adds branches and previews, under a non-commercial licence.
- **Outside music, Dolt** versions database tables as git versions files, and has shipped it for years:
  - **Prolly trees.** It splits a table into chunks where their content says to. So versions share every chunk that didn't change, and a row inserted changes only its own chunk ([Dolt](https://www.dolthub.com/blog/2020-04-01-how-dolt-stores-table-data/)).
  - **Merges by key, then by cell.** Two sides that changed different cells of one row merge. A cell both changed is a conflict, kept with its three values until someone resolves it: the common ancestor's, ours and theirs ([Dolt](https://www.dolthub.com/blog/2020-07-15-three-way-merge/)).
- **What Phazite takes:** this is the plan's approach. Dolt adds two refinements:
  - long lists split into runs ([the store](#the-store));
  - clashes kept with their three values ([merging](#the-versions-view)).

**5. A company's server.**
- **In music:**
  - Splice Studio (2013 to 2023):
    - kept a copy on its servers at each save, and showed which tracks changed;
    - had branches without merges;
    - attached the user's own bounce to a version.

    Splice said it hadn't been a focus since 2017 ([Splice](https://splice.com/blog/learn-with-splice/)).
  - BandLab keeps a mixdown with each revision, and lets anyone fork a song, without merges ([BandLab](https://help.bandlab.com/hc/en-us/articles/115002945173-What-is-a-Revision-on-BandLab-)).
  - Soundtrap restores an old version as a new project ([Soundtrap](https://support.soundtrap.com/hc/en-us/articles/115002725805-How-to-Restore-Previous-Project-Versions-in-Soundtrap)).
  - Avid's Cloud Collaboration avoids conflicts by giving each track one owner at a time.
  - VST Transit, Gobbler, Blend and Endlesss have closed ([Gobbler](https://magneticmag.com/2023/11/gobbler-media-shuts-down/), [Endlesss](https://cdm.link/2024/05/endlesss-discontinued/)).
- **Outside music:**
  - Onshape (CAD) keeps every edit. Its merges never stop on a conflict: what is merged in wins, tab by tab ([Onshape](https://www.onshape.com/en/blog/under-the-hood-how-collaboration-works)).
  - Figma reviews a branch side by side with the main file, then merges it whole ([Figma](https://help.figma.com/hc/en-us/articles/5691189138839/)). Its users complain that a conflict comes as a whole page or component.
- **What Phazite takes:**
  - merges that always finish, as Onshape's do;
  - changes shown on the song itself, as both companies' reviews show them on the design;
  - the history in the project folder, with any cloud only a copy.

**6. Every edit, merged live.**
- **What they do:**
  - Ohm Studio (2013 to 2020) was a DAW that several people edited at once, on a C++ data model with history and three-way merges. It worked, but closed for lack of demand ([Steam](https://steamcommunity.com/app/241660/discussions/0/364039785169696950)).
  - Soundtrap has had live editing together since 2022.
- **What research found.** Ink & Switch studied version control for writing ([Patchwork](https://www.inkandswitch.com/patchwork/notebook/2024-version-control/06/), [Upwelling](https://www.inkandswitch.com/upwelling/)):
  - people branch when a branch is quick to make and asks for nothing;
  - private drafts merged onto one main line suit creative work;
  - showing differences needs work for each kind of document.
- **What Phazite takes:** branches that ask for nothing. Editing together live is left for later. It would need IDs unique across computers ([merging by IDs](#merging-by-ids)).

**7. Bounces alone, with comments.**
- **What they do:** these tools keep each version as a bounce, with comments at times in it:
  - [untitled] ([untitled.stream](https://untitled.stream/)), at $49.99 a year;
  - Samply, which switches between versions of a mix while it plays ([Samply](https://apps.apple.com/us/app/samply-listen-share/id6463439253));
  - DISCO, Dropbox Replay and Filepass.
- **How it went:** people pay for these, so hearing versions matters. But a bounce:
  - is compared only by ear;
  - can't be worked on;
  - costs its full size each time. Re-rendered after one track's gain changed, a mix shared none of its bytes with the one before ([Wit](https://github.com/sep-lab/Wit)).
- **What Phazite takes:** a bounce when the user wants one, heard against the song while it plays, loudness-matched.

**Ruled out as well:**
- **Version control by patches** (Pijul, Darcs). Pijul is still in beta, and Darcs reduced its slow merges but never fixed them.
- **Music written as code** (Sonic Pi; LilyPond for scores). The history comes free, but a difference in the text misleads about the music, so musicdiff compares the notation instead.

**What the survey backs in the plan:**
- **Merging is where everyone stopped,** so V4, the riskiest step, comes last of the versions.
- **The history lives in the project folder,** in a documented format. A cloud would only hold a copy.
- **View state and counters stay out of the differences.** In Wit's measurements, 24% of Ableton's saves changed nothing musical, only what was in view and the set's internal counters.

## What the user sees

### The activity bar

```
+-----------------------------------------------------------------------+
| File Edit Add View  My Song   [<-][->][ Search commands ] [|][_][|][#] |
+-----------------------------------------------------------------------+
| Play  Rec   140 BPM   4/4                   master level   keyboard   |
+--+----------+------------------------------------------+-------------+
|Lb| Browser  | [Playlist] [Mixer]                       | Inspector   |
|  |  (or     |                                          |             |
|Vr| Versions)|                                          |             |
|  |          +------------------------------------------+             |
|  |          | [Piano roll] [Instrument] [Effects]      |             |
+--+----------+------------------------------------------+-------------+
| Saved  Main | hint: what the mouse and keys do here | 48 kHz | CPU 12% |
+-----------------------------------------------------------------------+
  Lb = Browser (Lucide "library")    Vr = Versions (Lucide "git-branch")
```

The bar is a column of icons along the window's left edge, from under the transport bar to the status bar. It behaves as VS Code's activity bar does.

**It is always there.** Hiding the left panel hides only the view in it, never the icons. That holds for its switch in the title bar, for Ctrl+Alt+B and for View > Panels. The bar also stays while a panel fills the window and in full screen.

**Clicking an icon:**
- on the icon of the view the left panel shows: hides the left panel;
- on any other icon, or while the panel is hidden: shows that view in it;
- while the left panel is in a window of its own: switches what that window shows, and brings it to the front.

**Its two icons:**
- **Browser**, the browser as it is now ([design section 11](design.md#11-the-browser)).
- **Versions** (below).

**At its foot,** as VS Code's Accounts and Manage icons:
- **Account** (Lucide "circle-user-round"): none yet. Its menu says so, and that projects and their versions stay on this computer.
- **Settings** (Lucide "settings"): a menu, as VS Code's Manage menu: Search Commands, then Settings, Audio Settings, MIDI Settings, Keyboard Shortcuts and Scan for Plug-ins, then Theme and Keymap.

Each view is a command, so it can have a key, its tooltip names that key, and it is in the View menu and the command palette:
- `view.browser`, Browser;
- `view.versions`, Versions.

Showing a view gives it the keyboard, as F5 to F9 do for the views in the centre ([design section 21](design.md#21-accessibility)).

**What the icons show:**
- the view on show: in full color, with a 2-pixel bar in the accent color along its left edge;
- the other icon: dimmed, but still at 3:1 contrast, which the theme tests check;
- while the panel is hidden, no icon is marked;
- while the song has changes since its branch's last version, a badge on the Versions icon counts them, as VS Code's Source Control icon does: the same count as the changes' heading, one for each track that changed (the master and the song's own count too), "99+" past 99.

**For screen readers,** each icon is a switch: "Versions, switch, on".

**Names in the window and the code:**
- The panel becomes the **Left Panel**, as the centre has the Upper and Lower Panels:
  - `Panel::browser` becomes `Panel::left`;
  - `view.toggleBrowser` becomes `view.toggleLeftPanel`, "Show the Left Panel" or "Hide the Left Panel", with Ctrl+Alt+B as now (Alt+F8 in FL Studio's keymap).
- The panel's width is the same for both views.
- The settings keep the width and which view the panel shows.

**Later icons** come only for views of the left panel; nothing that already has one way in gets a second. The account's menu fills once the cloud is there, and only if the user turns it on.

### The Versions view

The view is laid out as VS Code's Source Control view: the message, the button across the view, then the changes and the graph, each folding under its heading.

```
+------------------------------+
| VERSIONS                 ... |  ...: rename the branch, DAWproject, clean up
| Y Main v                     |  the branch: opens the branch picker
| [Message (Ctrl+Enter to save |  first line: the name; the rest: a note
|  a version on Main)        ] |  both optional
| [ v  Save Version      | v ] |  its menu: Bounce Each Version
| v CHANGES since "Chorus" (4) |
|   ~ Bass  volume, 12 notes M |  a click selects it in the song
|   ~ Drums 2 clips added   <M |  < : Put Back, under the pointer
|   ~ Strings               A  |  green A, amber M, red D (struck through)
|   = Master  Limiter       M  |
| v HISTORY                    |
|  o Now                       |
|  o Chorus drums [Main] 2 h > |  > : hear it (it has a bounce)
|  | o Half-time [Idea 2] 3 h >|  another branch's line, in its color
|  o-' Verse 2         Mon  >  |
|  o First idea        Mon     |  no bounce, nothing to hear
+------------------------------+
```

**Saving a version:**
- **Save Version** (`version.save`; Ctrl+Alt+S, Cubase's key for Save New Version; Ctrl+N in FL Studio's keymap, FL Studio's own key for it) saves the project and records a version.
- **One message,** as a commit's in VS Code: its first line is the version's name, the lines after it its note. Enter starts a new line, and Ctrl+Enter saves the version, as in VS Code.
- **Its name.** A name is optional. Left empty, the version is named by what changed ("Bass, Drums, + Strings").
- **A note,** optional too, says why: what the version tried, or what to try next. It shows under the name in the history. Cubase's users ask for one ([forum](https://forums.steinberg.net/t/save-as-new-version-add-note-for-version-information/1031505)). A study of CAD users found that numbered versions mean nothing once there are about fifty ([arXiv](https://arxiv.org/abs/2602.09236)).
- **Bounce Each Version** (`version.bounceEach`, in Save Version's menu, whose button then reads "Save Version and Bounce") renders the song into the version as Export Song does: the whole song with its tails, muted tracks silent, in a progress window with Cancel. Playback stops for it, as it does for an export. Cancelling leaves the version without a bounce. The settings keep it.
- **Not an undo step:** saving a version changes the history, not the song.

**Changes since the last version** lists what the song holds now that it didn't then, by track, as VS Code lists changed files:
- Each line is named in Phazite's own words: tracks added or removed, clips, notes, devices and their parameters, the mix, automation, sections, the tempo and the chord lane.
- View state and how the song plays (track heights, the loop, the playhead) never show. They use the same lists as the unsaved changes ([design section 18](design.md#18-projects-saving-and-export)).
- **Each line,** as VS Code's: an icon (a track, the master, the song's own), the name, what changed, muted, and a letter at its end, in VS Code's colors for it: A added (green), M changed (amber), D taken away (red, its name struck through). The heading counts the lines in a badge.
- A click on a line selects that thing in the song.
- **Put Back** takes that thing back from the last version, as one undo step: the arrow on the line under the pointer (or the line selected), as VS Code's Discard Changes, or the line's menu.
- **Marked in the playlist too.** While the Versions view shows, the playlist marks the clips and track headers that changed since the last version, so a change is seen where it is. Figma's and Onshape's reviews show a branch's changes on the design itself in the same way. A version chosen in the history marks what it changed instead.

**The history:**
- Every version of every branch, newest first. Each branch's line has a color of its own, as VS Code's Source Control graph draws its lines.
- A version's menu:
  - Hear It;
  - Bring Back This Version;
  - Take from This Version…;
  - New Branch from Here;
  - Merge into the current branch;
  - Bounce: only for the version the song matches now, until step V5 bounces old ones;
  - Delete Bounce;
  - Name and Note…;
  - Export as DAWproject (step D1).

**Hearing a version:**
- **Where it plays from.** A click on a version's play button plays the song from where playback starts, with that version's bounce heard in place of the song. "Now" plays the song itself.
- **Switching while playing.** A click on another version, or on Now, switches what is heard at once, at the same point. The bounce lines up beat for beat, by the version's own tempo.
- **Loudness matched.** Phazite plays both and measures both as they play (EBU R128, as exports are). At each switch it turns the louder one down to the other's loudness so far, so a version can't win by being louder.
- **Nothing hidden.** While a version is heard, the transport bar says so ("Hearing: Verse 2").
- **What plays is what is shown.** Any edit to the song switches back to Now, so that a change is heard as it is made.

**Going back:**
- **Bring Back This Version** makes the song what it was then, as one undo step ("Bring Back 'Verse 2'"). The history keeps every version, so the changes list then shows the difference, and Save Version records it.
- **Take from This Version…** shows that version next to the song as it is now, in the same list as the changes. Each thing on it (a track, a clip, a device, a section, the master, the tempo) has a Take. Each Take is one undo step.

**Branches** ("New Branch from Here", on a version or on Now):
- **No questions.** A new branch is named "Idea 2" (the next number free) and gets the next color. Phazite switches to it at once, with no window. Its menu's Name and Color… changes either later. Ink & Switch found that people branch when a branch is quick to make and asks for nothing ([Patchwork](https://www.inkandswitch.com/patchwork/notebook/2024-version-control/06/)).
- **Unsaved changes.** A branch made from Now takes the unsaved changes with it.
- **Switching** is done in the branch picker (Switch Branch…, `version.pickBranch`), as VS Code's: from the branch button at the top of the view, or from the branch's name in the status bar, beside the save state, which shows once the project has a version, with a star while the song has changes since its last version ("Main*"). The picker lists New Branch from Now first, named as typed (else "Idea 2"), then the branches, the one the song is on ticked, each with its last version and when; typing narrows them. Rename Branch… (`version.renameBranch`) names the current branch anew in the same picker; no two branches share a name.
  - The branch left behind is kept as it was, unsaved changes included.
  - The song becomes the other branch's in place, as it was left there, with its view state. What both branches have stays as it is: its plug-ins stay loaded and its audio stays read, and the song plays on.
  - The project is saved after the switch, so that its file is the branch's song.
  - Switching isn't an undo step, and the undo history starts again.
  - Not while the song records: the take isn't finished.
- **Main** is the first branch's name.

**Merging** ("Merge into Main…", on another branch or a version):
- **The window** lists what the other branch changed since the two lines parted, by track. Each change is ticked, to take it.
- **Changes on both sides** are clashes. Each shows both sides' values, and the current branch's value is chosen to start with, so a merge never stops on a question. Onshape's merges always finish too.
- **Merge** applies the ticked changes as one undo step ("Merge 'Half-time'"), so the result is heard at once, and Undo takes it back. Until the next version is saved, the view says "Merging 'Half-time': save a version to finish". That version has both lines as its parents, and the graph joins them.
- **Clashes stay open until then.** The Versions view lists each clash with a switch between its values:
  - this branch's;
  - the other branch's;
  - the value from before either changed it.

  A flip is an edit like any other, one undo step, so it is heard at once while the song plays. A dialog box can't offer that. Dolt keeps a conflict's three values until it is resolved in the same way ([Dolt](https://www.dolthub.com/blog/2020-07-15-three-way-merge/)).
- **The window warns** where both sides changed the same bars of one track. A merge can't hear that two bass lines don't go together.

**Words.** The view says:
- version, branch, merge, clash, bring back, take, hear, bounce;
- never commit, checkout, HEAD, stage, stash, rebase, cherry-pick or hash.

## How it works

### What a version holds

- **The song,** as saved, plug-in states included (`Song::storeDeviceStates()` first, as a save does).
- **The audio files it plays,** by their paths in the project folder, with each file's size and SHA-256. Files in `Audio/` and `Generated/` are written once and never changed: imports get new names, and recordings, bounces and generated sounds are new files. So a version names them rather than copying gigabytes. Nothing in Phazite deletes a file a version uses (Clean Up Unused Audio, [outside the steps](#the-steps), asks the store first). A version whose file was deleted by hand says which one is missing and plays the rest.
- **A bounce, or none.** The bounce is FLAC at 16 bits, dithered, at the song's rate: exact enough to compare mixes, and much smaller than 24 bits. That is 6 to 8 MB a minute, about 25 MB for a song of three and a half minutes, so one bounce takes the room of thousands of versions without one. Its integrated loudness and true peak are measured as an export's are, and its length is kept.
- **Its record:** its parents, its name and note, the summary of its changes, when it was saved, and the Phazite version that saved it. No user's or computer's name ([design section 22](design.md#22-privacy-licences-and-trademarks)).

### The store

```
Versions/
  objects/ab/cdef...    pieces of songs and versions' records, each named by the SHA-256 of what it
                        holds and compressed with zlib: written once and never changed
  branches/<n>.xml      a branch: its name, its color, its last version, and the song as it was left
                        there when that differs from its last version
  current               the branch the project file is
  bounces/<version>.flac
  files.xml             what each audio file hashes to, by its path, size and date, so each is read once
```

**Pieces.** The song is split at its tracks, clips, devices, containers, automation curves and sections:
- A plug-in's state is a piece of its own, in its bytes rather than base 64.
- **Beside its state, a plug-in keeps every parameter's value as text,** as the plug-in last reported it, by the plug-in's own IDs (`PluginInfo.h`). The song itself keeps only the parameters that Phazite's controls, automation or modulators touched; a version keeps them all. So a difference or a clash can name the knob ("Serum: Cutoff 40% → 55%") where the state's bytes name nothing.
- **Long lists are split into runs:** a clip's notes and a curve's points.
  - A run ends after a note or point whose hash ends in a chosen pattern, about one in 64, so where runs end depends only on what they hold.
  - A note changed or added in a clip of 5,000 stores its run alone, and the runs after it stay as they were.
  - This is how Dolt's prolly trees split a table ([Dolt](https://www.dolthub.com/blog/2020-04-01-how-dolt-stores-table-data/)).
- Each piece holds its element with its parts named by their hashes, down from the song's own piece, as git's trees are.
- A version that changed one note stores its run, its clip's piece, its track's and the song's: a few kilobytes. Plug-in states that didn't change are stored once for all versions.

**The same song always makes the same pieces.** Properties are written in the order of their names. Children whose order means nothing are written in the order of their IDs: Schema.h says which those are, such as clips and notes.

**Safe to write.** A piece is written whole to a temporary name, then renamed. A branch's file is replaced whole. A cut-off write leaves at most a piece that nothing names. Reading checks each piece's hash, so a damaged one marks its versions and no others.

**The project file stays the song alone:**
- It is the current branch's working copy, and everything that opens a project reads it.
- Without `Versions/`, the project opens as before, with no history.
- Save As and Save a Copy take `Versions/` with them: a copy of a project is a copy with its past.
- **One change to how it is written,** worth making first, as a step of its own: one property per line, in the order of their names.
  - That is how the golden tests already write the song ([GoldenFiles.cpp](../tests/GoldenFiles.cpp)), and how the pieces are written.
  - Today JUCE wraps lines at 60 characters and writes properties in the order they were set, so changing one value can rewrap the lines around it.
  - In the new form, anyone who keeps a project in git, or compares two in any text tool, sees just what changed.

**Off the message thread.** Saving a version takes no more of the message thread than a save does: the song is copied there, as an autosave copies it. Splitting, hashing, compressing and writing happen on a background thread, as the autosave's writer does. The changes list is also worked out in the background, once changes pause.

**The format is documented,** like the project file's (Schema.h, with the store's own names), so that other tools, and later the cloud, can read it.

### Differences

**How two songs are compared:**
- Pieces are compared by hash, from the top down, so only what changed is read.
- Then the changed elements are compared by their IDs. Elements without an ID go by a key Schema.h gives them: a curve by its device and parameter, a tempo or a chord by its beat.
- Each difference is described in the same words the inspector and the undo steps use.

**A plug-in's own settings count as changed only when the plug-in reported a change** since the last version. Some plug-ins save different bytes each time with nothing changed. Their bytes are still stored, and kept once where they are equal.

**A plug-in's change is named by its parameters:** the knobs whose values differ ([pieces](#the-store)). A change that moves no parameter, such as a sample loaded or a pattern drawn, shows as "settings changed".

### Making the song a version

Bring Back, Take, Put Back and Merge all work out the song they want and make the fewest edits that get there, through the song's undo history.

**So each is one undo step:**
- What plays follows it: the playback session hears the song change, as it does for an undo.
- A plug-in whose state is the same isn't touched.
- A plug-in whose state differs gets the version's state, as an undo of a preset gives it ([PluginInstances.h](../src/model/plugins/internal/PluginInstances.h)).
- References to what isn't there any more are removed, and the step's report says so: a send to a return that is gone, a sidechain from a track that is gone, a modulation of a device that is gone.

**View state stays as it is now,** for every step but a branch switch.

### Merging by IDs

**A three-way merge.** Phazite compares the branch's latest version, the other branch's, and the newest version both have: where the two lines parted.

**For each element, by its ID:**
- added on one side: added;
- removed on one side and unchanged on the other: removed;
- removed on one side and changed on the other: kept, and the window says so.

**For each property:**
- Changed on one side: that side's value.
- Changed on both sides to different values: a clash. It takes this branch's value, and keeps all three values (this branch's, the other's and where they parted) until the merge's version is saved.

**Notes** merge one by one inside a clip, and the clips of an alias group stay alike.

**Order** (tracks in a folder, devices in a chain) takes the other side's moves where this side made none. Where both sides moved things, this side's order is kept, and the other side's new items go after what they followed.

**Elements moved to another folder** keep their ID, so a moved track that was changed on the other side merges.

**IDs collide on purpose.** A song counts its IDs up (`Song::makeID()`), and two branches both count on from where they parted, so both give the same new IDs to different things. A merge gives a new ID to everything the other branch added: every ID above the count where they parted, notes included. Schema.h gets one list of every property that holds or names an ID, which `giveNewIDs()` (used for copies, which keep their notes' IDs) and the merge both go through. The project file needs no change for this. Editing together live, later, would need IDs unique across computers. That is a change of format to make then, if ever.

**Plug-in states, automation curves and the chord lane** merge whole:
- A curve both sides changed is a clash.
- So is a plug-in state both sides changed. Its clash names the parameters each side moved.

**View state and how the song plays** are always this branch's.

### Bounces and hearing them

**A bounce of the version being saved** is the open song. It renders through `OfflineRender` with the session's plug-ins, which the session lends it, exactly as an export does (`PlaybackSession::lendToRender()`).

**A bounce of an older version** (step V5) renders that version's song alone, with plug-in instances of its own in their helpers. It runs at a low priority and never while recording, and needs the version's plug-ins to still be installed.

**Hearing a version** is a part of the song's plan in Phazite's engine:
- The bounce is read ahead from disk as audio clips are (`engine::FileReader`).
- It plays after the master, in time with the playhead.
- A gain on it and on the master's output picks what is heard, both measured by `engine::LoudnessMeter`. Both stay sample-aligned, since they are one plan.
- Steady playback allocates nothing on the audio thread: a new guarded scenario in the [real-time audit](realtime-audit.md).

The same mechanism can later play a reference track from the browser against the song, loudness-matched.

### Where the code goes

The new code follows the layers in `CLAUDE.md`.

**The model,** `src/model/versions/`:
- `VersionsModel.h`: what the UI reaches by IDs and plain values: the versions, branches, changes, saving, hearing, taking, merging, and the commands' functions.
- `internal/VersionStore.h`: the folder, its pieces, branches and file hashes.
- `internal/SongPieces.h`: the song into pieces and back, in its one form.
- `internal/SongDiff.h`: the differences.
- `internal/SongMerge.h`: the three-way merge, with the IDs given anew.
- `internal/ApplySong.h`: the song made equal to another, or to a part of it, with the fewest edits.

**The names they write** go into Schema.h, as every name Phazite writes into a project does. That includes the lists the merge needs:
- what holds or names an ID;
- which children have no order;
- each element's key where it has no ID.

**The engine:** an audio file played after the master, in time, at a gain, measured, in `src/engine/`.

**The UI:**
- `src/ui/shell/ActivityBar.*`;
- the left panel's views in `PanelLayout.*` and `AppShell.*`;
- `src/ui/versions/VersionsView.*` and `MergeWindow.*`.

**Tokens** for everything that is drawn: the bar's width (40 pixels) and icons (20), the marker, the dot and the graph's lines.

## Decided against

**A git repository in the project folder** (libgit2, or git's own files):
- Git merges lines of text, which a song isn't, so Phazite would do its own merges anyway.
- Big audio needs Git LFS, which libgit2 doesn't speak.
- GitHub refuses files over 100 MB, so pushing a project there, the one thing a real repository gains, is the part that suits audio worst.
- Git rewrites its own files as it works, so a repository in a folder that Dropbox or OneDrive syncs between two computers can be damaged. The store only adds files, and each computer writes only its own branches' files.
- Git's words would leak in.
- libgit2's licence (GPL-2.0 with a linking exception) would allow it in Phazite. The reasons above are why it isn't used.

**Git, tried on 2026-11-04.** The test used a scratch repository and a made-up song of 100 tracks and 20,000 notes, written as the golden tests write songs: 2.7 MB of text.
- The whole song compresses to about 200 KB. Once git had packed them, each older version took 100 to 500 bytes. Until git packs them, each version is a whole compressed copy, 294 KB here.
- Two branches turned different knobs of one synth, which sit on neighbouring lines. Git stopped with a conflict that wasn't one.
- Two branches each added something new. Both got ID 73, and git merged them cleanly into a song with two things sharing one ID.

Phazite takes git's ideas, not its files: pieces named by their hashes, versions with parents, branches as names for versions, and three-way merges.

**Every save a version.** Ctrl+S is a habit, done every minute, so the history would be noise: in Wit's measurements, 24% of Ableton's saves changed nothing musical ([what others tried](#what-others-tried)). `Backups/` already keeps the last 10 saves, for a save made over good work. A version is a point the user chooses, as a commit is, and as Save New Version is elsewhere.

**A log of every edit, in place of the song's states.** A log could replay how a song was made, and it is the base of editing together live. But:
- a knob's drag is hundreds of values, and grouped into steps it is the undo history Phazite already keeps;
- seeing what a version holds would mean replaying from the start, so states would be stored anyway;
- a replay makes the same song only while Phazite's code does what it did then, quantizing for one;
- what the user does in a plug-in's own window isn't an edit Phazite sees.

It is worth it only for editing together live, which the one DAW built on it, Ohm Studio, found too little demand for.

**Versions kept on a server.** Every history kept only on a music company's servers has closed with it ([what others tried](#what-others-tried)). The history lives in the project folder, and a cloud only holds a copy ([later](#later-the-cloud-on-top)).

**Copies of the song in each version.** A big song's file is megabytes, mostly plug-in states that rarely change. Pieces store each state once.

**Copies of the audio in the store.** These would double the project's size, for files that never change.

**Every branch's plug-ins kept loaded.** Switching would be instant, but each branch would cost its plug-ins' memory and CPU. Switching makes the song the other branch's in place instead, which keeps the instances both branches have and loads the others.

**DAWproject as Phazite's own format** ([next](#dawproject-with-versions)).

## DAWproject with versions

DAWproject ([GitHub](https://github.com/bitwig/dawproject)) is an open exchange format under the MIT licence:
- **Who supports it:** Bitwig Studio 5.0.9, Studio One 6.5, Cubase 14, Cubasis 3.7.1, VST Live 2.2 and n-Track Studio 10.2.2, by its README. Logic Pro, Ableton Live, Pro Tools and FL Studio hadn't adopted it as of 2025.
- **What it is:** a zip of XML with the audio, embedded or referenced, and each plug-in's state, always embedded.

It fits with versions as an exchange at the edges, not as the form versions are kept in:
- **Not Phazite's own format.** Its own README says it doesn't aim to be a DAW's native format. It has no place for Phazite's modulators, chord lane, containers' macros or own devices: it knows only a generic EQ, compressor, gate and limiter. A zip can't be stored as differences, and its ids belong to each file, so they can't carry Phazite's identity from one version to the next.
- **Export any version** (D1), from a version's menu or File > Export.
  - **Plug-ins** go as themselves, each with its state as the VST3 SDK's preset file.
  - **Phazite's own instruments and effects** don't play in another DAW. A track that plays one goes as its notes on a track without an instrument, and beside it a bounce of its sound, so the other DAW hears it and can give the notes an instrument of its own. "Bounce or not" again, settled per track by what can play there.
  - **Its ids and its version.** Phazite writes its IDs as the file's ids, and the version the file came from where the format keeps it.
- **Import** (D2) opens a DAWproject as a new project, or as a branch of the project it came from.
  - **As a branch,** it starts from the version it came from. Phazite matches what the file holds to that version: by its ids where the other DAW kept them; otherwise by tracks' names and order, clips' places, and notes' pitches and times.
  - **What it shows:** only what the other DAW changed.
  - **What it leaves alone:** what DAWproject can't hold (modulators, the chord lane, Phazite's devices) stays as the version had it, instead of counting as removed.
  - **Merging** works as for any branch. This is how someone in Bitwig, Studio One or Cubase works on a Phazite song.

## Later: the cloud on top

These need designs of their own, after the conversation of 2026-11-04 about a Phazite cloud. The store is built for them: pieces never change once written, so copying them anywhere is safe, and branches are small files.
- **Backup to a folder of the user's choosing** (an external drive, a NAS, OneDrive, Dropbox): Phazite copies the pieces and audio files that aren't there yet, by their hashes. Each computer writes only its own branch files, so a syncing folder never has two writers on one file. Free, with no account.
- **Phazite Cloud:** the same store on a server, opt-in, priced from its cost.
- **Share links with comments** at times in a version's bounce: a web page anyone can open, with the comments coming back into the Versions view at their places in the song.
- **Collaboration:** a project shared through either. Each person works on a branch and merges the others' in Phazite. Each version lists its plug-ins, so a collaborator sees what they lack, and a bounce of each such track lets them hear it.

## The steps

**V1. The activity bar and versions saved (L)**
- **The bar:** the activity bar, and the Left Panel with its Browser and Versions views.
- **The store** and Save Version, the history, the changes since the last version, and the Versions icon's dot. Main is the only branch so far.
- *Done when:*
  - the bar behaves as VS Code's: icons, keys, the left panel's switch, a detached and a maximized left panel, the settings kept;
  - a version of every example song reopens in the history after the project closes;
  - a second version stores only what changed;
  - the changes list names each kind of change and never view state;
  - saving a version of the 100-track benchmark song takes no more of the message thread than a save does.

**V2. Going back (M-L)**
- Bring Back, Take from This Version and Put Back, on the fewest edits, as undo steps, with references repaired.
- *Done when:*
  - every example song, changed through the model and brought back, saves the same project file and renders the same golden fingerprint;
  - a plug-in that didn't change isn't set again;
  - Undo puts everything back, the plug-ins' states included.

**V3. Bounces, and hearing them (M)**
- "With a bounce" when saving a version, Bounce on the version that matches the song, Delete Bounce, and hearing versions against Now, loudness-matched.
- The room the history takes, shown in the Versions view, with the songs' versions and the bounces apart: nearly all of it is bounces.
- *Done when:*
  - switching between Now and a version while playing is instant and lands on the same beat;
  - after a switch, the two differ by under 0.5 LU;
  - hearing a version allocates nothing on the audio thread (the real-time audit);
  - deleting a bounce frees its file, and the room shown says so.

**V4. Branches and merging (L)**
- New Branch from Here, switching with each branch's unsaved changes kept, the graph, and merging with its window, its clashes and versions with two parents.
- *Done when:*
  - two branches that changed different tracks merge with no clashes;
  - a knob changed on both sides is a clash, and flipping it while the song plays is heard at once;
  - IDs added on both sides never collide;
  - seeded random edits on two branches always merge into a song that opens, plays and saves.

**V5. Bounces of old versions (M)**
- Bounce on any version: its song alone, rendered in the background with plug-in instances of its own.
- *Done when:*
  - an old version's bounce has the same render fingerprint as an export of that version's song;
  - the song playing meanwhile drops no block;
  - no bounce starts while recording.

The history needs no Forget Versions: a song's version costs bytes to kilobytes, and bounces, nearly all of the room, are deleted one by one (V3).

**D1. DAWproject export (M-L)**
- Any version as DAWproject: plug-ins with their states, and Phazite's own devices bounced beside their notes.
- Comes after V2.
- *Done when:* Phazite's example songs open in Bitwig Studio and Studio One with every track, clip, note, tempo and automation lane where it was, and sound as they do in Phazite.

**D2. DAWproject import (L)**
- As a new project, or as a branch matched to the version it came from.
- Comes after V4.
- *Done when:* a song exported, changed in Bitwig Studio (one track's notes and one fader) and imported back shows those two changes and nothing else, and merges.

**Outside these steps** are two small changes, which Phazite needs with or without versions:
- **The project file written one property per line,** in name order ([the store](#the-store)). It is worth doing first. It changes no golden file: the golden tests read the saved file and write it in that form themselves.
- **Clean Up Unused Audio… (S):** it lists the files in `Audio/` and `Generated/` that neither the song nor any version uses, and deletes the ticked ones. Takes, imports and generated sounds pile up today, versions or not.

**Where they go:** first among the steps after v1, because versions need the song's format to be kept. A version holds the song as it was saved. While Phazite has no backward compatibility, an update can make older versions unreadable, though their bounces would still play. So versions wait until compatibility starts, when the owner says ([design section 18](design.md#18-projects-saving-and-export)).

The DAWproject item in [After v1](roadmap.md#after-v1)'s list becomes D1 and D2.

If the owner wants versions to be v1's headline instead, V1 to V3 are the part to bring forward, and compatibility starts with them. They don't depend on anything in phase 4. V4 onward can follow v1.

## The tests on the way

- **The store:**
  - round trips;
  - the same song makes the same pieces, whatever the order of its properties and of its children that have no order;
  - a version stores only what changed;
  - a note changed or added in a long clip stores one run, and the runs after it stay the same;
  - a write cut off midway leaves the store readable;
  - a damaged piece affects only its versions.
- **Every example song:** saved as a version, changed through the model as the UI changes it, and brought back. The project file must be the same (`readNormalisedProject()`), and so must its golden render fingerprint.
- **Differences:**
  - a case for each kind of element in Schema.h, named as people read it;
  - a plug-in's change named by the parameters the tests' plug-in moved;
  - nothing for view state or the loop.
- **Merges:**
  - each kind of change on one side, on the other and on both;
  - each clash flipped to each of its three values, one undo step each;
  - IDs given anew where both branches added tracks, clips, notes, devices and curves;
  - references repaired;
  - aliases kept alike;
  - seeded random edits on two branches, checked for unique IDs and valid references;
  - merging what a branch already has changes nothing.
- **Applying:** the tests' plug-in counts how often its state is set, and undo restores everything.
- **Bounces:**
  - a bounce renders as an export does;
  - a version is heard aligned to the sample, checked with clicks through `EnginePlayback.h`;
  - its loudness is matched.
- **The UI:**
  - the activity bar's clicks, keys, the left panel detached and maximized, and the settings;
  - screen-reader names;
  - the Versions view's lists and the merge window;
  - the command goldens, changed on purpose.
- **Benchmarks:** Save Version and the changes list on the 100-track song.

## Risks, and what holds them

| Risk | What holds it |
|---|---|
| Bring Back or a merge changes the song wrongly | Each is one undo step; the project file and the versions stay; every example song goes through them in the tests |
| The history grows large | Pieces are shared and long lists split into runs, so a version costs what changed: bytes to kilobytes. Bounces, nearly all of the room, show in the room taken and are deleted one by one |
| An update makes older versions unreadable | Versions wait until compatibility starts; bounces still play |
| Big songs make versions slow | Pieces compared by hash, work off the message thread, and benchmarks with targets |
| Plug-ins that save different bytes with nothing changed | Their settings count as changed only when they reported a change; equal bytes are kept once |
| Git's words leak into the window | The list of words above; the texts kept in one place (phase A4's strings table) |
| A merge that is clean but sounds wrong | It is one undo step, heard at once; the window warns where both sides changed the same bars of a track |
| Audio files deleted by hand | The version names what is missing and plays the rest |

## As built

Every step above is built on the local branch `versions` (2026-11-04), in this order: the project file one property per
line, V1, V2, V4, V3, V5, Clean Up Unused Audio, then D1 and D2. Where the build differs from the plan:

- **The store** keeps pieces in packs, one file per saved version (`Versions/pieces/<sha>.pack`: the pieces it adds, then
  an index), not one file per piece: thousands of tiny files are slow on Windows and in sync tools. Pieces keep their
  children in their order, so a song comes back exactly; the differences and the merge match children without an order by
  their keys. Sections stay inside the song's piece: they are small. Pieces hold the whole song as it was saved, view
  state too; the differences, Bring Back and the merge leave out what isn't counted.
- **A plug-in's knobs** are read from its instance as a version is saved, and kept beside its state, so the changes can
  name them.
- **Switching branches** makes the song the other branch's in place with the fewest edits, view state included, outside
  the undo history, which then starts again (`ApplySong.h`: `makeSongTheSame()`). Its count of IDs becomes the larger of
  the two, and a DAWproject that comes back as a branch takes IDs past the open song's, so no two branches give one ID
  to two things. A merge waiting for its version stays with the branch it was made on, for the session. A frozen track
  stays frozen only where its file holds what it plays there: a track's frozen file is its own, whichever branch froze
  it last.
- **Merging** is a mode of the Versions view's list (Merge and Cancel under its heading), not a window of its own. A
  merge's notes (what it couldn't decide, where both sides changed a track's clips over the same beats) show while it waits
  for its version.
- **Bounces** are `Versions/bounces/<version id>.flac`, FLAC of 16 bits, dithered, at the rate the song plays at. The
  version the song is now renders as Export Song does, in its progress window; an older one renders its own song in the
  background, with plug-in instances of its own, beside the song playing.
- **Hearing** a version is a play mark on its row (Now's plays the song again) and Hear It in its menu. The engine plays
  the bounce after the master (`engine/nodes/Comparison.h`) and measures both. At each switch the louder one is turned
  down to the other's loudness so far, or as soon as both have one. The gains move over one block.
- **DAWproject:**
  - A VST3's state goes in as its `.vstpreset`, which the helper writes through JUCE.
  - What another DAW can't play is bounced beside its track ("Bass (bounce)"), the track itself muted with its notes
    kept.
  - Imported back into a branch, a file takes notes, faders, mutes and tracks added or taken away. Values are compared
    within what the file's numbers keep. Its devices, automation and audio clips stay the version's.

Not built yet:

- the playlist marking what changed since the last version;
- Name and Note… for a saved version, whose record is a piece and doesn't change;
- a branch's Color… (its name changes with Rename Branch…);
- the benchmark of Save Version and the changes list on the 100-track song;
- importing a DAWproject's plug-ins and sends into a new project.

**To check by hand:** an export opening in Bitwig Studio and Studio One, and a real VST3's preset going both ways. The
tests' plug-in is of a format of its own, so the tests bounce it rather than write its preset.
