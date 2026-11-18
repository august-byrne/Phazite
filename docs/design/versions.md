# Versions

*Design section 18; in full here. Cut down to what producers use by [versions-cut.md](versions-cut.md) (the owner,
2026-11-17).*

A project keeps the versions of its song: saved at points the user chooses, on branches, in the project's own folder,
with no account and no network. The **Versions** view in the left panel shows them, in plain words: a version, the
history, a branch, a merge. A version can carry a bounce, so that it can be heard against the song.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Save Version, named as typed or by what changed | Built | |
| The changes since the last version, by track | Built | |
| The history of every branch's versions | Built | |
| Restore the song, or one track, from a version | Built | |
| Discard Changes, for a line or for all | Built | |
| Branches: create, switch, rename, delete | Built | |
| A branch's colour chosen by hand | Planned | No roadmap step |
| Merging another branch or version, whole, with conflicts | Built | |
| Bounces of versions, Hear It and A/B | Built | |
| Local History | Built | |
| Undo History | Built | |
| Undo Last Version, Rename Version... | Built | |
| Verify History, with every version's audio files, Local History's songs and those left on branches | Built | S4.4 |
| A version off every branch offered 30 days by Recover Lost Versions..., with when and why it left | Built | S4.3 |
| The store's packs joined by size, losing nothing | Built | S4.4 |
| The store swept of what nothing names, older than 30 days | Built, off | Known bug 196: nothing is deleted yet |
| The song tied to the version it grew from | Planned | S4.5 ([versions-cut.md](versions-cut.md)) |
| The playlist marking what changed | Planned | No roadmap step |
| Staging, a version checked out on its own, a merge of some lines | Removed | S4.2 ([versions-cut.md](versions-cut.md)) |
| Tags, stash, rebase, squash, cherry-pick, blame, bisect | Declined | Not needed by one person working alone |
| Backup to another folder, a cloud copy, share links | Planned | No roadmap step |

## From the start

- **First version.** A project keeps versions from its start: as soon as its history is read and has no branch, the
  song as it is is saved as "First version", on the branch **main**. A new project's history moves with it at its
  first save.
- **Auto Save** keeps the project file up to date ([projects-saving-export.md](projects-saving-export.md)), so every
  change shows in the changes as it is made.
- The status bar names the branch beside the save state, with a star while the song has changes since its last
  version ("main*"). A click opens the branch picker.
- The Versions icon in the activity bar carries a badge that counts the changed lines.

## The Versions view

From the top: the view's title with its menu (Version, Changes, Branch, the DAWproject commands, Clean Up Unused
Audio, Verify History, Recover Lost Versions, Reset Branch to Last Readable Version); the branch button; the name field; the **Save Version** button with its
menu; then the sections **Changes**, **History**, **Local History** and **Undo History**, each folding under its
heading. The layout is VS Code's Source Control view's; the words are a producer's.

### Saving a version

- **Save Version** (`version.save`, Ctrl+Alt+S) saves the project and a version of the song as it is: every change,
  the whole song.
- It runs only with something to save: changes, or a merge waiting for its version. Until then it is greyed.
- **One field:** its first line is the version's name, the lines after it a note. Enter starts a new line and
  Ctrl+Enter saves. Both are optional. Left empty, the version is named by what changed ("Update Bass and Drums"), and
  the empty field shows that name before it is saved ("Update Bass and Drums (Ctrl+Enter to save it on 'main')", or
  "Name this version (...)" with nothing to save).
- **Bounce Each Version** (`version.bounceEach`, in Save Version's menu, whose button then reads "Save Version and
  Bounce") renders the song into each version as Export Song renders it. The settings keep it.
- **Undo Last Version** (`version.undoLast`): the branch goes back to the version before its last and the song stays as
  it is, so what that version recorded shows in the changes again; its name and note go back into the field. Not for the
  first version, nor a merge's. For 30 days, Recover Lost Versions... brings it back.
- **Rename Version...** (`version.editMessage`) names any version anew, and its note. A version never changes, so the
  name is kept beside it; its id, its place in the history and its bounce stay.
- Saving a version is not an undo step: it changes the history, not the song.

### Changes

**Changes** lists what the song holds now that the last version doesn't, a line for each track that changed, the
master, and the song's own (its tempo, sections, chord lane and track order).

- Each line says what changed in Phazite's own words ("volume, 12 notes"), with a letter: **A** added, **M**
  modified, **D** deleted; **C** marks a merge's conflict. The colours are VS Code's.
- View state and how the song plays never show. They are left out by the same lists as for unsaved changes.
- A plug-in's settings count as changed only when the plug-in reported a change or one of its knobs moved. A
  plug-in's change is named by the parameters whose values differ.
- A click on a line selects that thing in the song.
- **Discard Changes** (`version.putBack`) makes a line what the last version has again; **Discard All Changes**
  (`version.discardAll`) every line. Each is one undo step. A clip moved between tracks since ends where the last
  version has it, once, whichever of the two tracks' lines is discarded.

### History

- Every version of every branch, newest first, each on its branch's line in the branch's colour, under a row for the
  **Changes Since the Last Version**.
- **A click on a version opens it:** under it, a row for each line it changed since the version before it. One version
  is open at a time.
- **A double-click hears it** (Hear It, below): it changes nothing. A version without a bounce is bounced in the
  background first and heard as soon as it is, as A/B does.
- **Restore from This Version** on one of those rows (`version.restoreChange`) makes that line what it was in the
  version, as one undo step ("Restore Bass from "Verse 2""). A version lists only the lines it changed since the version
  before it, so a track it didn't change isn't there: Compare with the Song lists every line that differs.
- **Restore This Version** on the version itself (`version.bringBack`) makes the whole song what it was then, as one
  undo step ("Restore "Verse 2""). The history keeps every version, so the changes then show what was taken back.
- **Compare with the Song** (`version.takeFrom`) lists what differs between a version and the song, each line to
  restore on its own (Restore from "Verse 2", one undo step); **Close the Comparison** (Done) lists the changes since the
  last version again. This is how a track is brought back from any version, or from another branch: its last version
  compared with the song, then the track's line restored.
- A whole version's restore keeps the song as it is in Local History first; a line's restore, as a line's discard, is an
  undo step alone.
- A version's menu also has Hear It, Create Branch, Merge into Current Branch..., Rename Version..., Undo Last Version
  (on the last), Bounce, Delete Bounce and Export as DAWproject....

## Branches

- **Create Branch** (`version.newBranch`) asks nothing: the branch is named "idea-2" (the next number free), takes
  the next colour, and the song goes on there at once. Made from the song as it is, it takes the changes with it. Made
  from an older version, the song becomes that version's, as a switch makes it.
- **Switch Branch...** (`version.pickBranch`, the branch button and the status bar's branch) opens the branch picker:
  the branches, the current one ticked, each with its last version; typing narrows them or names a new one.
- **Switching to a branch** (`version.switchBranch`) makes the song the other branch's in place, with the fewest edits.
  What both branches have stays as it is: its plug-ins stay loaded and the song plays on. The branch left keeps its song
  as it was, changes and view state included. The project is saved after, so that its file is the branch's song. It is
  not an undo step, and the undo history starts again. Not while the song records or renders.
- **A branch is named as typed**, spaces and all (`toBranchName()`): trimmed, each run of white space one space, no
  control characters. A branch is stored by its id, so its name needs nothing more to be safe. No two branches share a
  name, whatever its case.
- **Rename Branch...** (`version.renameBranch`) names the current branch anew.
- **Delete Branch...** (`version.deleteBranch`) lists the other branches. When the branch has versions no other
  branch has, or changes left on it, it asks first, saying so ("This also deletes 1 version that no other branch has,
  and the changes left on it. Recover Lost Versions brings them back for 30 days."). Recover Lost Versions offers its
  versions and the song left on it for 30 days (Keeping the history whole, below). Not while a merge waiting (here, or
  in the song left on another branch) took in a version only its line holds. A branch a command made and takes back as
  it doesn't finish (Create Branch from a version, an import of a DAWproject, cancelled) leaves nothing to recover.

## Merging

**Merge Branch...** (`version.mergeBranch`) lists the other branches, each with how many versions it would bring;
**Merge into Current Branch...** (`version.merge`) on a version merges that version.

- The Versions view then lists what the other line changed since the newest version both lines have, by track: what
  will come in, each line marked where it conflicts. **Merge** (`version.finishMerge`) takes every line as one undo
  step ("Merge idea-2"), so the result is heard at once; **Cancel** takes nothing. A merge never takes some lines and
  leaves others: it is recorded whole, so what was left out could never be merged later. One track from another branch
  is a restore, not a merge: Compare with the Song on that branch's last version, then the track's line restored (above).
- **A three-way merge by IDs.** An element added on one side is added. One removed on one side and unchanged on the
  other goes; removed on one side and changed on the other, it stays, and a note says so. A property changed on one
  side takes that side's value.
- **Conflicts.** A property both sides changed to different values is a conflict. It takes this branch's value to
  start with, so a merge never stops on a question, and it keeps all three values until the merge's version is saved:
  **Keep This Branch's**, **Take the Other Branch's**, **Take What Was Before**. Each switch is one undo step, heard at
  once; what the value chosen names that this branch took away (a track deleted here) goes with it, as after any
  edit. Automation curves, the chord lane and a plug-in's state merge whole.
- **Moves.** An item moved between tracks, or into a folder or a container, is treated as a move, not as a removal
  and an addition: it goes where that side put it, with the other side's changes, and stays where this branch put
  it if both moved it. Moved on one side and deleted on the other, it stays where it was moved, and a note says so,
  as for any change against a removal.
- **What names the gone goes.** A merge, as every restore and discard, leaves out what names something the song
  hasn't (a send to a return this branch deleted, a modulation of a device gone), and lets go of a sidechain that
  the two lines' changes made into a loop (one side's sidechain, the other's send), as the edit that makes one does.
- **A folder removed leaves its tracks.** A folder taken away by a merge or a discard goes without the tracks in
  it, which stay where it was; each track follows its own line.
- **IDs never collide.** Two branches both count their IDs on from where they parted, so a merge gives new IDs to
  what the other branch added wherever this branch has something else with them, and a later merge of that branch
  knows them again by its version's record, even where the other branch, on another computer, gave one of those IDs
  to something it made since. No ID is in the song twice after a merge, a restore or a discard, and every one of them
  leaves the song's count of IDs past every ID it holds.
- **The version that finishes it** has both lines as its parents, and is named for what it merged ("Merge idea-2",
  or "Merge "Verse 2"" for a version that isn't a branch's last) unless a name is typed.
- **Cancel Merge** (`version.cancelMerge`) closes the list, taking nothing; once the merge is taken and waits for its
  version, it makes the song what it was before, as one undo step, the song as it is kept in Local History first (the
  branch's last version, where the song before it can't be read). A merge's record in the song that waits for nothing
  (one that can't be read, one of another history, as in a project file opened without its Versions folder, or one
  the branch holds already) it takes out alone, the rest of the song as it is.
- **A merge the Versions folder can't keep** (the song before it can't be written, a full or locked drive) takes
  nothing: its list stays, and the view says why.
- **One merge at a time.** While a merge waits for its version, no other starts: Merge Branch... and Merge into
  Current Branch... are greyed until its version is saved or it is cancelled.
- **A merge waiting is part of the song** (as git keeps `MERGE_HEAD` beside the files it merged): it goes with the song
  into its file, its autosave and the song left on a branch, so that whichever of them opens holds the merge state its
  song is in. A song closed without saving after a merge holds none; the autosave restored after a crash holds the one
  it was saved with. It goes in and out with the merge's undo step: Undo of the merge leaves none waiting, so the next
  version follows this line alone, and Redo brings it back; Undo of Cancel Merge makes it wait again, its conflicts as
  they were chosen. It counts as an unsaved change, but is no line of the changes, nor part of a version.
- **The branch moved on underneath it.** Where the branch's last version is no longer the one the merge was taken at
  (another computer saved one on it, or one was saved after the merge's Undo and before its Redo), the merge still
  waits, so that its version follows the newest version and the merged one and records the IDs it gave, rather than
  the merged work coming into the branch as new items that a later merge would bring again; the view says so. Its own
  version finishes it, known by the history (a version of the line that merged the same version, on top of where it was
  taken, recording the IDs it gave), so that nothing is written into the song after a version is saved. Where the branch
  holds the merged version through another computer's merge of it that gave what it added other IDs, it waits still,
  its version recording this song's IDs beside that merge's, so that later merges of that line find this song's items;
  where the branch holds it otherwise, it waits no more, and the view says so.
- **Read in the background.** A merge's conflicts are found again from its three songs on the history's thread (as
  the project opens, or a branch is switched to whose song holds one); meanwhile it waits, "being read", and Cancel
  Merge waits for it.
- View state and how the song plays are always this branch's.

## Hearing a version

- **A bounce** is the song as it was in a version, rendered as Export Song renders it, as FLAC at 16 bits. **Bounce**
  (`version.bounce`) makes one for a version that has none: the current version's from the open song, playback
  stopped; an older one's in the background, with plug-in instances of its own, while the song plays on. **Delete
  Bounce** frees its file. Bounces are nearly all of the room a history takes.
- **Hear It** (`version.hear`, the play mark, a double-click in the history) plays a version's bounce in place of the
  song, from the engine's next block, at the same beat. A version without a bounce is bounced in the background first,
  the song playing on, and heard as soon as it is, as A/B does (meanwhile A/B names it, "Bouncing to hear it"); not the
  version the song is now, which has nothing to be heard against. The transport bar says "Hearing: *name*". An edit to
  the song switches back to the song.
- **Loudness matched:** both are measured as they play, and at each switch the louder one is turned down to the
  other's loudness so far, so that a version can't win by being louder.
- **A/B** (`version.compare`, N, and the transport bar's A/B) switches between the song and B, over plug-in windows
  too. B is the version heard last, or else the last version, so that the changes since are heard against what was. A
  version without a bounce is bounced in the background first. A/B is greyed while B is the song itself.

## Local History

Local History keeps the song as it was at times while it differed from its last version, without asking.

- **When:** as the project opens with changes since its last version ("As opened"); every 10 minutes while the song
  changes ("Auto-saved"), never while it records; and before what makes the song another: a restore, Discard All
  Changes, a merge taken, Cancel Merge, a snapshot restored.
- **What it costs:** a snapshot is taken only when the song differs from the one before it, and is stored as
  versions are, so it costs what changed.
- **How long:** the list shows every snapshot of the last two hours, then the newest of each hour for a day, then the
  newest of each day for 30 days. Those it no longer shows are still in the store (known bug 196).
- **Restore from Local History** (`version.restoreSnapshot`) makes the song what it was then, as one undo step, the
  song as it is kept first.
- It is this computer's own, and folded at first.

**Undo History**, under it, lists the song's undo steps; a click goes back or forth to one. The history keeps the last
1000 steps (the setting `phaziteUndoSteps`, which no window shows yet: `src/services/Settings.h`).

## Keeping the history whole

- **Nothing a command does ends a version at once.** Three commands take a version off every branch: Undo Last Version,
  Delete Branch and Reset Branch to Last Readable Version. Each computer's log of a branch's moves says when, and why,
  with the branch's name, and a deleted branch's log line names the song left on it; a version that left every branch
  is offered by Recover Lost Versions for 30 days from then (`keptByAge::dailyFor`, the time Local History keeps), with
  the song left on a deleted branch, whichever computer it left on. What is older is no longer offered, but is still in
  the store: nothing is deleted from it yet (The store, below; known bug 196). Nothing else takes a version off a
  branch: a rename, a switch, Cancel Merge and a cancelled command move no branch, and a branch a command made and takes
  back as it doesn't finish (Create Branch from a version, an import of a DAWproject) held only versions other branches
  hold. Restores, Discard All Changes, a
  merge taken and Cancel Merge keep the song as it was in Local History first.
- **Recover Lost Versions...** (`version.recover`, the branch picker's) lists what can come back, newest first, each
  line of versions with when and why it left, as the history says dates: "Undone on 12 Nov, from main", "On idea-3,
  deleted on 14 Nov, with the changes left on it"; a deleted branch whose versions other branches hold, for the changes
  left on it alone; and the lines Verify History found that no log names (a record damaged, or another computer's whose
  log hasn't arrived), "Found by Verify History". A click brings a line back as a branch at its last version, named
  after the branch it was on ("idea-3 (recovered)", "recovered" where no log names it), with the song left on it as the
  branch's, so that switching to it gives the song as it was left. Not an undo step.
- **Verify History** (`version.verify`) reads every piece and checks it against its name; walks every version's song,
  Local History's songs and the songs left on branches piece by piece; checks every version's audio files against its
  record (the file is there, of the recorded size, and of the recorded SHA-256), each file read once however many
  versions name it, in parts; and finds the versions no branch holds. It says which version misses which file ("Verse 2"
  (Audio/Kick.wav missing; Audio/Snare.wav changed since)) and which snapshot or song left on a branch can't be read
  whole. It runs on the history's thread, the view showing how far it has come (the audio by its bytes), and the command,
  named Stop Verifying History meanwhile, stops it between pieces and between parts of a file. Saving a version, or a
  command that reads songs (a switch, a restore), stops it too and goes ahead rather than waiting behind it.
- A version with a damaged piece is restored in part, and the view says what it couldn't take, which stays as it is
  now. **Restore This Version** (the whole song) takes every element it can read, and a track the song hasn't now comes
  back without what of it can't be read (a clip). On one of its lines, it takes that line where the version holds it
  whole, leaves what of it can't be read as it is, and takes nothing where the line's place in the version can't be
  read: it isn't in the version where it reads, and a track whose piece is damaged could have held it (a folder it is
  in now, the nearest named; a folder now; or a track gone since, whose kind is unknown), saying which piece. In the
  window, only the first is reached: the history lists no lines under a version it can't read whole, nor does Compare
  with the Song list any, so a damaged version's lines show nowhere to restore one by one. Verify History names such
  versions. A version whose own piece can't be read restores nothing.
- **A branch whose last version can't be read** (a piece of its record damaged or missing) says so in the view, and
  offers the newest version of the branch's log on this computer that reads; **Reset Branch to Last Readable Version**
  (`version.resetToReadable`) takes the branch back to it, the song as it is. No version is saved on top of the one
  that can't be read meanwhile; where the log holds none that reads, a version saved starts the branch's line again.
- **A damaged piece is read from another pack** that holds it, and a copy found damaged (by a read, or Verify History)
  is forgotten, so that the next version holding the piece writes it again.
- A project file that can't be read offers its branch's last version
  ([projects-saving-export.md](projects-saving-export.md)).

## The store

The history is the `Versions` folder beside the project file. Its format is written down for other tools in
[../versions-format.md](../versions-format.md).

- **Pieces.** A song is split at its tracks, clips, devices, containers and automation curves. Each piece is named
  by the SHA-256 of what it holds, so a version stores only the pieces no version before it has. A clip's notes and a
  curve's points are split into runs, so one note changed in a long clip stores one run. A plug-in's state is a
  piece of its own, in its bytes.
- **The same song always makes the same pieces:** each value is written in one way, as in the project file.
- **Packs.** Pieces are kept in packs, written once and never changed. Each piece's entry says when it was written, and
  each pack which computer wrote it; a pack is named by its index and its writer, so two computers' packs never share
  a name. Reading finds a piece in any pack; writing writes the pieces this computer's own packs don't hold yet.
- **Nothing is deleted yet** (known bug 196). The store keeps every piece, bounce and edited message it was given: an
  undone version, a deleted branch with the song left on it, and the snapshots Local History no longer lists keep their
  room. Recover Lost Versions offers what left within 30 days; what is older is still in the store.
- **Joined** (git's repack; `StoreSweep.h`). As the history opens, on its thread before anything else reads the store,
  once a week (the keeping time: now, but no more than a week past its last sweep, its first sweep noting the time),
  this computer joins its own packs by size, as git's geometric repacking does: those under 1 MiB, and of each size
  four times the one before, once four of a size gather, and packs a crash left with the same pieces twice. Every piece
  of every pack it joins is copied into the new pack from a copy it read whole, never left to a copy elsewhere it didn't
  read, so the count of its packs grows with the log of their size, not with the versions and
  snapshots it takes (a year of daily snapshots stays under 50), and a piece is rewritten a few times at most. The joined
  pack is written whole beside the others, its bytes checksummed as they are written and read back against that before
  the packs it replaces go; a computer deletes only packs it wrote and a join replaced (a pack's writer read again
  first), so nothing another computer wrote is ever deleted. A write that fails (a full disk), a crash, or a pack another
  program holds leaves every piece readable. What a join a crash cut off leaves beside the packs is counted in the room
  the view shows until it goes, a day later.
- **The walk.** At a weekly sweep, the history also walks what names a piece, on any computer (each computer's records
  are files of its own, all read as far as they have arrived): every branch's last version and the versions its line
  reaches, the lines gone their own way, the songs left on branches, Local History's snapshots (thinned as of the
  keeping time, as the view shows them), the songs before the merges waiting in the open song and in those, what left
  every branch within 30 days by the logs, where a computer's record of a branch is gone its log's last place for it,
  every piece written within those 30 days, and what each of those names. Nothing is left out of a join by it yet; a
  file that names something and doesn't read whole, a sync program's copy of one, or a named piece that can't be read
  makes it unsure, and the Versions view says so where Verify History says what it found ("Phazite is keeping everything
  in the Versions folder, because ..."). Logs are read as bytes: a line a crash cut off, a line of another shape, or zero
  bytes a power loss left at a log's end make it unsure only while they are recent (a time within 30 days, or recent
  lines beside the zero bytes), and the log is written again whole at its next move; the files a system puts in folders
  (`desktop.ini`, `Thumbs.db`, `.DS_Store`, `.directory`, dot-files) are passed over.
- **Times that never go back.** What the store stamps (a version, a branch's record and log, a snapshot, an edited
  message, a piece's entry) is always later than the stamp before it, and never older than the newest stamp this
  computer ever made: a floor kept in its own folder (`stamped`) and taken again from its logs, its Local History and its
  packs as the history opens (a time more than ten years past the clock passed over, as a damaged file's), never
  lowered, apart from the sweep's time. So a clock set back, even at every start (a dead battery), stamps nothing that
  would read as old, and no two snapshots share a time. A clock that was ahead keeps this computer's stamps ahead until
  real time passes them: new versions show as saved "now" meanwhile, and Local History isn't thinned (known bug 196). A
  noted sweep time after now is written again as now, so that once the clock is put right the weekly sweeps go on.
- **Nothing waits for it.** The sweep copies without holding the store's lock, so the window's room text and Clean Up's
  look don't wait for it; the Versions view says it is reading meanwhile, as it does while the history opens, and
  commands that change the history wait. A save into a new folder (Save As, a first save) stops it rather than waiting,
  changing nothing, and it runs at a later open; so is Verify History stopped, and by a save of a version or a command
  that reads songs, saying it was stopped and never that the history is whole.
- **Audio files** are named by their paths, sizes and hashes, not copied. Clean Up Unused Audio keeps every file that a
  version the store still holds plays (on a branch, or one Recover Lost Versions... or Verify History can bring back),
  and every song left on a branch, in Local History or left on a deleted branch, on any computer, and the song before a
  merge any of them holds; while one of those songs, or a version a log names from the last 30 days, can't be read (its
  pack held, or not arrived), or the folder holds a pack the store can't read (held, damaged, or written by another
  build), it offers nothing and says why. It never goes by the clock.
  The Project view finds how many are unused in the background, reading each piece once.
- **Written safely:** every file is written whole or not at all. Reading checks each piece's hash, so a damaged one
  affects only the versions that hold it.
- **Each computer writes only files of its own:** its record of each branch, its log of each branch's moves, its
  renamed versions, the songs it left on branches and its Local History. A computer is known by an ID made at random,
  never by a name. So a project folder kept in a synced folder and opened on two computers, one at a time, never has a
  file with two writers. From each computer's records follow a branch whose line went its own way on two computers,
  which the view shows to merge again ("main went another way on another computer too"), a deleted branch staying
  deleted everywhere, and the newest computer's word winning for a renamed version or branch. Nothing more is built on
  them until working together is designed (the owner, 2026-11-17).
- **Off the message thread:** the song is copied on the message thread; splitting, hashing, compressing and writing
  run on the history's own thread, as does reading it when the project opens. A version or a snapshot given as the
  project closes is written after it, on the history's thread, which the close doesn't wait for; the project opened
  again (or another history of the same folder) reads the folder only after it, and quitting waits for it behind a
  window that says what is being finished. What the history reads is let go.
- **Commands that read whole songs read them there too** (known bug 126): a switch to another branch, Create Branch from a
  version, Import DAWproject back as a branch, Merge's list, Restore This Version and Restore from This Version, Compare
  with the Song, Restore from Local History, Undo Last Version and Reset Branch to Last Readable Version read on the
  history's thread, and change the song on the message thread once what they read is in (one named undo step where the
  command is one). A switch also splits and keeps the song it leaves there, from a copy taken as it starts; the merge's
  list is worked out there against a copy of the song, which is kept in the store there too, for Cancel Merge (Merge
  takes that piece, unless the song changed since: then it is kept on the history's thread as Merge is clicked, the
  window held). The import of a DAWproject as a branch unpacks its audio there, with progress. If the song changed while
  it was read (a plug-in's own change, the automation written as it plays), that part goes round again on the history's
  thread from a new copy, twice at most; what changed after that (the hold keeps every edit of the user's out) is worked
  in on the message thread as the command ends. Bouncing an old version, exporting one as DAWproject and Delete
  Branch's question (whether the song left there had changes) read on the history's thread too, without holding the
  window; hearing a version's bounce reads its song's own piece alone, for its tempo map.
- **The window held meanwhile** (`ProjectContext::holdForWork()`): from the command's start the window takes no input but
  the keys that work while a mouse button is down (play and stop, the tools, the editors' zoom): the window's one hold
  (`src/app/InputHold.h`), the same as a command waiting for the plug-ins' states and an audio file's work take, and the
  message loop runs on; no other command that changes the project runs, nor does a file dropped on the window. A read
  of more than a second shows in the progress window, saying what is being read, with progress where it is known, in
  the window's queue of progress windows, after an audio file's work's if that shows first. Its Cancel, or Escape
  (before its window shows too), and a quit or the computer's session ending, cancel it
  (a quit says so where the view says its problems: "Stopped, as Phazite was closing"), and what each leaves is:
  - a switch: the song and the branch as they were; the song left may already be kept in the store as a piece that
    nothing names, which takes only its room;
  - Create Branch from a version, Import DAWproject as a branch: nothing; a branch made for it is taken away again (the
    import's audio unpacked by then is deleted again);
  - Merge's list, the restores, Compare, Undo Last Version, Reset Branch: nothing changed.
  A command that can't finish says why in the Versions view (`getReadingProblem()`): a song that can't be read, the
  song recording or a render begun meanwhile, the branch moved on, a folder that can't be written to, the project
  saved into a new folder meanwhile. None of them starts while a first save copies the `Versions` folder, and one that
  read before the copy is refused as it is taken in, so that nothing is written into the folder the project leaves; a
  branch a cancelled command made is taken away once the copy is over. Delete Branch's question says when what was left
  on the branch can't be read.
- **Songs from elsewhere:** a song read from the pieces (a version's, a song left on a branch, a snapshot) is made
  consistent as a project file is as it opens (`makeConsistent()`), whoever wrote it: what no Phazite writes (a number
  that is none or out of its range, an ID twice, a send to nothing, a sound file outside the project's folder) never
  reaches the song. Of a song Phazite wrote the checks change nothing but what a project opened leaves out too: the
  curves of a device deleted, which the song keeps while an Undo could bring the device back. That loses nothing: a
  switch starts the undo history again; Restore This Version and Restore from Local History are undo steps, whose Undo
  puts the curves back with the rest; a line's restore leaves them as they are; a merge counts them as this side's. A
  song read in part (a version with damaged pieces) keeps what names what it couldn't read (a send to a lost return, a
  lost device's curves and modulations), which the song it is taken into judges (`removeBrokenReferences()`, and a
  curve it brought in of a device the song hasn't taken away again, in the same step), so that the song is what its
  file would open as. A merge, Restore This Version and a line's restore refuse a song that held an ID twice, which its
  line's IDs no longer match once the checks give it new ones (an item would come in as new).
- Save Project As copies the history with the project. Without a `Versions` folder, a project opens with no history
  and saves its first version.

## DAWproject

A version, or the song as it is, is written as a DAWproject for other DAWs, and one that was exported from a version
comes back as a branch from that version, showing only what the other DAW changed
([projects-saving-export.md](projects-saving-export.md#dawproject)).

## Decisions

- The history lives in the project's folder, in a documented format. Every history kept only on a company's
  servers has closed with it; a cloud would only hold a copy.
- Git's ideas without git's files: git merges lines of text, which a song isn't, and two branches that each add
  something give the same new ID to different things.
- A version is a point the user chooses, not every save; Local History covers the time between versions.
- A version stores what changed, in pieces; audio files are named, not copied; bounces are optional and deleted one
  by one.
- A version records the whole song: no staging, as no producer saves the drums alone while the bass is half done
  (they save both, or put the bass on a branch).
- The song is always on a branch: an old version is looked at by hearing it, restoring it (an undo step) or starting a
  branch there, never by checking it out on its own.
- A merge takes every line: one track from another branch is a restore of its line, after Compare with the Song.
- Differences and merges go by the song's IDs, and are said in Phazite's own words, by track.
- A merge never stops on a question: a conflict takes this branch's value and stays open to switch.
- Restoring, discarding and merging are each one undo step, so what plays follows and Undo takes them back.
- Switching to a branch changes the song in place rather than loading it anew, so plug-ins stay loaded.
- A new branch asks nothing, and is named as typed.
- The words are plain ones (version, history, switch, restore), keeping branch and merge (the owner, 2026-11-17); git's
  words stay as search words for the commands. The layout stays VS Code's Source Control view's.
- No stash: each branch keeps its own unfinished work, and A/B hears it against the last version.
- While Phazite keeps no backward compatibility, an update can make older versions unreadable; their bounces still
  play.

## Not built

- What S4.5 builds next ([versions-cut.md](versions-cut.md)): the song tied to the version it grew from: Planned.
- The playlist marking the clips and tracks that changed since the last version: Planned.
- A branch's colour chosen by hand: Planned.
- One undo for every versions operation (a switch, a deleted branch): Planned.
- Reverting one old version while keeping later ones: Planned; restoring a line from a version covers most of it.
- Showing only named versions in a long history, and exports tied to versions: Planned.
- Backup to a folder of the user's choosing, a cloud copy, share links with comments, and working together on
  branches: Planned.
- Tags, stash, rebase, squash, cherry-pick, blame and bisect: Declined.
- A benchmark of saving a version and of the changes list on a 100-track song: Planned (S4.5).
- Letting go of what nothing names (known bug 196), built and tested but off in the product (`sweepLetsGo`,
  `StoreSweep.h`): the sweep leaving out each piece of this computer's own that nothing names and that it wrote more than
  30 days before the keeping time, at most a week's worth at one open, with the bounces and this computer's edited
  messages of the versions that go, only when that frees at least 4 MiB and a tenth of this computer's packs. Waits for
  what must come first:
  - **Each computer copying what it names** (`adoptVersions()`, built, off with it): whatever another computer's
    history names (a merge of its version, a version saved on top of it, a branch from it, a switch to its branch, an
    undo back onto it, a line or a song recovered from its log, the merge a song holds) copied into this computer's own
    packs before the other may delete it, in the background, a batch a job, never making a save or Save As wait.
  - **One computer id per machine:** an id carried to a second machine (a roaming profile, a restored backup) would let
    one computer's sweep take what the other needs; binding the id to the machine must keep every existing install's
    id, its songs left on branches and its Local History.

## In the code

- `src/model/versions/VersionsModel.h`: everything the UI reaches, by plain values and ids, and the commands.
- `src/model/versions/internal/VersionHistory.h`: the open song's history, its branches, the merge under way, Local
  History.
- `src/model/versions/internal/VersionStore.h`, `PieceStore.h`, `SongPieces.h`: the folder, the packs and the pieces;
  `StoreSweep.h`: the sweep (its letting go off) and each computer's copy (off with it).
- `src/model/versions/internal/SongDiff.h`: what differs between two songs.
- `src/model/versions/internal/ApplySong.h`: the song made like another with the fewest edits (`makeSongLike()`,
  `makeTrackLike()`, `makeSongTheSame()`).
- `src/model/versions/internal/SongMerge.h`: the three-way merge of every line (`mergeSongs()`).
- `src/model/commands/UndoHistory.h`: the undo history as the view lists it.
- `src/engine/nodes/Comparison.h`: a bounce played after the master.
- `src/ui/versions/VersionsView.h`, `BranchPicker.h`; `src/ui/shell/ActivityBar.h`, `StatusBar.h`, `TopBar.h` (A/B).
- Tests: `tests/VersionsTests.cpp`, `VersionsBranchTests.cpp`, `VersionsMergeTests.cpp`, `VersionsMergeWaitingTests.cpp`,
  `VersionsFolderCopyTests.cpp` (a first save's copy of the folder), `VersionsGoingBackTests.cpp`,
  `VersionsHearingTests.cpp` (and A/B), `VersionsKeptWholeTests.cpp`, `VersionsRecoveryTests.cpp` (the 30 days),
  `VersionsSweepTests.cpp` (the sweep, with its letting go on but for the product's own test, and Verify History),
  `VersionsTwoComputersTests.cpp` (each computer's copy, clocks, Verify's limits), `VersionStoreTests.cpp`, `tests/golden/format/`.
