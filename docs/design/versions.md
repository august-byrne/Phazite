# Versions

*Design section 18; in full here.*

A project keeps a history of its song, as git keeps one of code: commits on branches, in the project's own folder,
with no account and no network. The **Source Control** view in the left panel shows it, in git's words as VS Code's
Source Control says them. A commit can carry a bounce, so that an earlier version can be heard against the song.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Commit, with a message or named by what changed | Built | |
| The changes since the last commit, by track | Built | |
| The graph of every branch's commits | Built | |
| Restore the song, or one track, from a commit | Built | |
| Discard Changes, for a line or for all | Built | |
| Branches: create, check out, rename, delete | Built | |
| A branch's colour chosen by hand | Planned | No roadmap step |
| Merging another branch or commit, with conflicts | Built | To be simplified: see [To be removed](#to-be-removed) |
| Bounces of commits, Hear It and A/B | Built | |
| Local History | Built | |
| Undo History | Built | |
| Undo Last Commit, Edit Commit Message... | Built | |
| Verify History, Recover Lost Commits | Built | |
| Staging | Built | To be removed |
| Checkout (Detached) | Built | To be removed |
| The playlist marking what changed | Planned | No roadmap step |
| Tags, stash, rebase, squash, cherry-pick, blame, bisect | Declined | Not needed by one person working alone |
| Backup to another folder, a cloud copy, share links | Planned | No roadmap step |

## From the start

- **Initial commit.** A project keeps commits from its start: as soon as its history is read and has no branch, the
  song as it is is committed as "Initial commit", on the branch **main**. A new project's history moves with it at its
  first save.
- **Auto Save** keeps the project file up to date ([projects-saving-export.md](projects-saving-export.md)), so every
  change shows in the changes as it is made.
- The status bar names the branch beside the save state, with a star while the song has changes since its last
  commit ("main*"). A click opens the branch picker.
- The Source Control icon in the activity bar carries a badge that counts the changed lines.

## The Source Control view

From the top: the view's title with its menu (Commit, Changes, Branch, the DAWproject commands, Clean Up Unused
Audio, Verify History, Recover Lost Commits); the branch button; the message field; the **Commit** button with its
menu; then the sections **Staged Changes**, **Changes**, **Graph**, **Local History** and **Undo History**, each
folding under its heading.

### Committing

- **Commit** (`version.save`, Ctrl+Alt+S) saves the project and commits the staged changes, or every change while
  none is staged.
- It runs only with something to commit: changes, staged or not, or a merge waiting for its commit. Until then it
  is greyed.
- **One message:** its first line is the commit's name, the lines after it a note. Enter starts a new line and
  Ctrl+Enter commits. Both are optional. Left empty, the commit is named by what changed ("Update Bass and Drums"),
  and the empty field shows that name before it is made.
- **Bounce Each Commit** (`version.bounceEach`, in Commit's menu, whose button then reads "Commit and Bounce")
  renders the song into each commit as Export Song renders it. The settings keep it.
- **Undo Last Commit** (`version.undoCommit`): the branch goes back to the commit before its last, what that commit
  recorded is staged again, the song stays as it is, and the message goes back into the field. Not for the first
  commit, nor a merge's. The commit stays in the store, where Recover Lost Commits finds it.
- **Edit Commit Message...** (`version.editMessage`) names any commit anew. A commit never changes, so the message
  is kept beside it; its id, its place in the graph and its bounce stay.
- Committing is not an undo step: it changes the history, not the song.

### Changes

**Changes** lists what the song holds now that the last commit doesn't, a line for each track that changed, the
master, and the song's own (its tempo, sections, chord lane and track order).

- Each line says what changed in Phazite's own words ("volume, 12 notes"), with a letter as git marks a file: U
  untracked (a track added), M modified, D deleted, A added (staged), C a merge's conflict.
- View state and how the song plays never show. They are left out by the same lists as for unsaved changes.
- A plug-in's settings count as changed only when the plug-in reported a change or one of its knobs moved. A
  plug-in's change is named by the parameters whose values differ.
- A click on a line selects that thing in the song.
- **Discard Changes** (`version.putBack`) makes a line what the last commit has again; **Discard All Changes**
  (`version.discardAll`) every line. Each is one undo step. A clip moved between tracks since ends where the last
  commit has it, once, whichever of the two tracks' lines is discarded.

### The graph

- Every commit of every branch, newest first, each on its branch's line in the branch's colour, under a row for the
  **Uncommitted Changes**.
- **A click on a commit opens it:** under it, a row for each line it changed since the commit before it. One commit
  is open at a time.
- **Restore from This Commit** on one of those rows (`version.restoreChange`) makes that line what it was in the
  commit, as one undo step ("Restore Bass from "Verse 2""). This is how a track is brought back from an earlier
  version.
- **Restore from This Commit** on the commit itself (`version.bringBack`) makes the whole song what it was then, as
  one undo step. The history keeps every commit, so the changes then show what was taken back.
- **Compare with Working Tree** (`version.takeFrom`) lists what differs between a commit and the song, each line to
  restore on its own; **Close the Comparison** lists the changes since the last commit again.
- A commit's menu also has Hear It, Create Branch, Merge into Current Branch..., Edit Commit Message..., Bounce,
  Delete Bounce and Export as DAWproject....

## Branches

- **Create Branch** (`version.newBranch`) asks nothing: the branch is named "idea-2" (the next number free), takes
  the next colour, and the song goes on there at once. Made from the current commit, it takes the uncommitted
  changes with it. Made from an older commit, the song becomes that commit's.
- **Checkout to...** (`version.pickBranch`, the branch button and the status bar's branch) opens the branch picker:
  the branches, the current one ticked, each with its last commit; typing narrows them or names a new one.
- **Checking out a branch** makes the song the other branch's in place, with the fewest edits. What both branches
  have stays as it is: its plug-ins stay loaded and the song plays on. The branch left keeps its song as it was,
  uncommitted changes and view state included. The project is saved after, so that its file is the branch's song.
  It is not an undo step, and the undo history starts again. Not while the song records or renders.
- **Rename Branch...** (`version.renameBranch`) names the current branch anew. Names are as git takes them: spaces
  become dashes, and no two branches share a name.
- **Delete Branch...** (`version.deleteBranch`) lists the other branches. When the branch has commits no other
  branch has, or changes left on it uncommitted, it asks first, saying so. Its commits stay in the store. Not while a
  merge waiting (here, or in the song left on another branch) took in a commit only its line holds.

## Merging

**Merge Branch...** (`version.mergeBranch`) lists the other branches, each with how many commits it would bring;
**Merge into Current Branch...** on a commit merges that commit.

- The Source Control view then lists what the other line changed since the newest commit both lines have, by track,
  each line ticked. **Merge** takes the ticked lines as one undo step, so the result is heard at once.
- **A three-way merge by IDs.** An element added on one side is added. One removed on one side and unchanged on the
  other goes; removed on one side and changed on the other, it stays, and a note says so. A property changed on one
  side takes that side's value.
- **Conflicts.** A property both sides changed to different values is a conflict. It takes this branch's value to
  start with, so a merge never stops on a question, and it keeps all three values until the merge is committed:
  **Accept Current Change**, **Accept Incoming Change**, **Accept Base**. Each switch is one undo step, heard at
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
  knows them again by its commit's record, even where the other branch, on another computer, gave one of those IDs to
  something it made since. No ID is in the song twice after a merge, a restore or a discard, and every one of them
  leaves the song's count of IDs past every ID it holds.
- **The commit that finishes it** has both lines as its parents, and is named as git names one ("Merge branch
  'idea-2'") unless a message is typed.
- **Abort Merge** (`version.cancelMerge`) closes the list, taking nothing; once the merge is taken and waits for its
  commit, it makes the song what it was before, as one undo step, the song as it is kept in Local History first (the
  branch's last commit, where the song before it can't be read, as git's abort resets to `HEAD`). A merge's record
  in the song that waits for nothing (one that can't be read, one of another history, as in a project file opened
  without its Versions folder, or one the branch holds already) it takes out alone, the rest of the song as it is.
- **A merge the Versions folder can't keep** (the song before it can't be written, a full or locked drive) takes
  nothing: its list stays, and the view says why.
- **One merge at a time.** While a merge waits for its commit, no other starts, as git refuses one: Merge Branch...
  and Merge into Current Branch... are greyed until it is committed or aborted.
- **A merge waiting is part of the song,** as git keeps `MERGE_HEAD` beside the files it merged: it goes with the song
  into its file, its autosave and the song left on a branch, so that whichever of them opens holds the merge state its
  song is in. A song closed without saving after a merge holds none; the autosave restored after a crash holds the one
  it was saved with. It goes in and out with the merge's undo step: Undo of the merge leaves none waiting, so the next
  commit follows this line alone, and Redo brings it back; Undo of Abort Merge makes it wait again, its conflicts as
  they were chosen. It counts as an unsaved change, but is no line of the changes, nor part of a commit.
- **The branch moved on underneath it.** Where the branch's last commit is no longer the one the merge was taken at
  (another computer committed on it, or a commit was made after the merge's Undo and before its Redo), the merge still
  waits, so that its commit follows the newest commit and the merged one and records the IDs it gave, rather than the
  merged work coming into the branch as new items that a later merge would bring again; the view says so. Its own
  commit finishes it, known by the graph (a commit of the line that merged the same commit, on top of where it was
  taken, recording the IDs it gave), so that nothing is written into the song after a commit. Where the branch holds the
  merged commit through another computer's merge of it that gave what it added other IDs, it waits still, its commit
  recording this song's IDs beside that merge's, so that later merges of that line find this song's items; where the
  branch holds it otherwise, it waits no more, and the view says so.
- **Read in the background.** A merge's conflicts are found again from its three songs on the history's thread (as
  the project opens, or a branch is switched to whose song holds one); meanwhile it waits, "being read", and Abort
  Merge waits for it.
- View state and how the song plays are always this branch's.

## Hearing a commit

- **A bounce** is the song as it was in a commit, rendered as Export Song renders it, as FLAC at 16 bits. **Bounce**
  (`version.bounce`) makes one for a commit that has none: the current commit's from the open song, playback
  stopped; an older one's in the background, with plug-in instances of its own, while the song plays on. **Delete
  Bounce** frees its file. Bounces are nearly all of the room a history takes.
- **Hear It** (`version.hear`) plays a commit's bounce in place of the song, from the engine's next block, at the
  same beat. The transport bar says "Hearing: *name*". An edit to the song switches back to the song.
- **Loudness matched:** both are measured as they play, and at each switch the louder one is turned down to the
  other's loudness so far, so that a version can't win by being louder.
- **A/B** (`version.compare`, N, and the transport bar's A/B) switches between the song and B, over plug-in windows
  too. B is the commit heard last, or else the last commit, so that work not committed yet is heard against what
  was. A commit without a bounce is bounced in the background first. A/B is greyed while B is the song itself.

## Local History

Local History keeps the song as it was at times while it differed from its last commit, without asking.

- **When:** as the project opens with changes since its last commit ("As opened"); every 10 minutes while the song
  changes ("Auto-saved"), never while it records; and before what makes the song another: a restore, Discard All
  Changes, a merge taken, Abort Merge, a snapshot restored.
- **What it costs:** a snapshot is taken only when the song differs from the one before it, and is stored as
  commits are, so it costs what changed.
- **How long:** every snapshot of the last two hours, then the newest of each hour for a day, then the newest of
  each day for 30 days.
- **Restore from Local History** (`version.restoreSnapshot`) makes the song what it was then, as one undo step, the
  song as it is kept first.
- It is this computer's own, and folded at first.

**Undo History**, under it, lists the song's undo steps; a click goes back or forth to one. The history keeps the last
1000 steps (the setting `phaziteUndoSteps`, which no window shows yet: `src/services/Settings.h`).

## Keeping the history whole

- **Verify History** (`version.verify`) reads every piece and checks it against its name, walks every commit's
  song, and finds the commits no branch holds.
- **Recover Lost Commits** (`version.recover`) makes a branch of each line of those commits, named after the branch
  they were made on ("main-recovered").
- A commit with a damaged piece is restored in part, and the view says what it couldn't take, which stays as it is
  now. **Restore from This Commit** on the commit (the whole song) takes every element it can read, and a track the
  song hasn't now comes back without what of it can't be read (a clip). On one of its lines, it takes that line where
  the commit holds it whole, leaves what of it can't be read as it is, and takes nothing where the line's place in the
  commit can't be read: it isn't in the commit where it reads, and a track whose piece is damaged could have held it (a
  folder it is in now, the nearest named; a folder now; or a track gone since, whose kind is unknown), saying
  which piece. In the window, only the first is reached: the graph lists no lines under a commit it can't read whole,
  nor does Compare with Working Tree list any, so a damaged commit's lines show nowhere to restore one by one. Verify
  History names such commits. A commit whose own piece can't be read restores nothing.
- **A branch whose last commit can't be read** (a piece of its record damaged or missing) says so in the view, and
  offers the newest commit of the branch's log on this computer that reads; **Reset Branch to Last Readable Commit**
  (`version.resetToReadable`, git reset --soft) takes the branch back to it, the song as it is. Nothing is committed on
  top of the commit that can't be read meanwhile; where the log holds none that reads, a commit starts the branch's line
  again.
- **A damaged piece is read from another pack** that holds it, and a copy found damaged (by a read, or Verify History)
  is forgotten, so that the next commit holding the piece writes it again.
- A project file that can't be read offers its branch's last commit
  ([projects-saving-export.md](projects-saving-export.md)).

## The store

The history is the `Versions` folder beside the project file. Its format is written down for other tools in
[../versions-format.md](../versions-format.md).

- **Pieces.** A song is split at its tracks, clips, devices, containers and automation curves. Each piece is named
  by the SHA-256 of what it holds, so a commit stores only the pieces no commit before it has. A clip's notes and a
  curve's points are split into runs, so one note changed in a long clip stores one run. A plug-in's state is a
  piece of its own, in its bytes.
- **The same song always makes the same pieces:** each value is written in one way, as in the project file.
- **Packs.** Pieces are kept in packs, written once and never changed, but where every piece of one was found damaged:
  the next commit that holds them writes it again whole under its own name, in its place (where another program holds the
  damaged file, the commit fails and says so). More than 50 packs are joined into one as the history opens.
- **Audio files** are named by their paths, sizes and hashes, not copied. Clean Up Unused Audio keeps every file a
  commit plays (on a branch, or one Recover Lost Commits brings back), and every song left on a branch or kept in Local
  History, on any computer, and the song before a merge any of them holds. The Project view finds how many are unused
  in the background, reading each piece once.
- **Written safely:** every file is written whole or not at all. Reading checks each piece's hash, so a damaged one
  affects only the commits that hold it.
- **Each computer writes only files of its own:** its record of each branch, its log of each branch's moves, its
  edited messages, the songs it left on branches and its Local History. A computer is known by an ID made at
  random, never by a name.
- **Off the message thread:** the song is copied on the message thread; splitting, hashing, compressing and writing
  run on the history's own thread, as does reading it when the project opens. A commit or a snapshot given as the project
  closes is written after it, on the history's thread, which the close doesn't wait for; the project opened again (or
  another history of the same folder) reads the folder only after it, and quitting waits for it behind a window that
  says what is being finished. What the history reads is let go.
- Save Project As copies the history with the project. Without a `Versions` folder, a project opens with no history
  and makes its initial commit.

## DAWproject

A commit, or the song as it is, is written as a DAWproject for other DAWs, and one that was exported from a commit
comes back as a branch from that commit, showing only what the other DAW changed
([projects-saving-export.md](projects-saving-export.md#dawproject)).

## To be removed

The feature is to be made simpler for producers. Commit, the graph, restoring the song or one track from a commit,
branches, A/B and Local History stay. Merging stays as a whole merge, for later collaboration. These parts are in
the code now and are to be removed; this page is to be cut down when they are.

- **Staging.** The **Staged Changes** section, **Stage Changes** and **Unstage Changes** on a line (`version.stage`,
  `version.unstage`), **Stage All Changes** and **Unstage All Changes** (`version.stageAll`, `version.unstageAll`),
  and the letter A. Today the staged song is what the next commit records: the last commit with the lines staged
  since, as they were when staged. Discard Changes restores a line from the staged song; Undo Last Commit stages
  what the commit recorded. Staging is kept while the project is open, for its branch.
- **A commit checked out on its own.** **Checkout (Detached)** (`version.checkout`, also a double-click on a
  commit) makes the song a commit's in place, on no branch: the branch button and the status bar read
  "*name* (detached)", and a bar offers **Back to main** (`version.backToBranch`) and **Create Branch Here**.
  Nothing is committed or merged meanwhile. What changed there goes into Local History ("Before Leaving a
  Checkout") on the way back. The project opens again where it was left.
- **A merge's ticked lines.** Today each line of a merge can be unticked, so that only some tracks are taken. A
  merge is to take every line.
- **Per-computer branch records** are parked: each computer's own record of each branch
  (`branches/<branch>/<computer>.xml`), with what follows from it: a branch's lines that went their own way on
  another computer shown to merge again, a deleted branch kept out on every computer, and the newest computer's
  word winning for an edited message.

## Decisions

- The history lives in the project's folder, in a documented format. Every history kept only on a company's
  servers has closed with it; a cloud would only hold a copy.
- Git's ideas without git's files: git merges lines of text, which a song isn't, and two branches that each add
  something give the same new ID to different things.
- A commit is a point the user chooses, not every save; Local History covers the time between commits.
- A commit stores what changed, in pieces; audio files are named, not copied; bounces are optional and deleted one
  by one.
- Differences and merges go by the song's IDs, and are said in Phazite's own words, by track.
- A merge never stops on a question: a conflict takes this branch's value and stays open to switch.
- Restoring, discarding and merging are each one undo step, so what plays follows and Undo takes them back.
- Checking out a branch changes the song in place rather than loading it anew, so plug-ins stay loaded.
- A new branch asks nothing.
- The words are git's, as VS Code's Source Control says them; hear, bounce and A/B are Phazite's own.
- No stash: each branch keeps its own unfinished work, and A/B hears it against the last commit.
- While Phazite keeps no backward compatibility, an update can make older commits unreadable; their bounces still
  play.

## Not built

- The playlist marking the clips and tracks that changed since the last commit: Planned.
- A branch's colour chosen by hand: Planned.
- One undo for every source control operation (a checkout, a deleted branch): Planned.
- Reverting one old commit while keeping later ones: Planned; restoring a line from a commit covers most of it.
- Showing only named commits in a long graph, and exports tied to commits: Planned.
- Backup to a folder of the user's choosing, a cloud copy, share links with comments, and working together on
  branches: Planned.
- Tags, stash, rebase, squash, cherry-pick, blame and bisect: Declined.
- A benchmark of committing and of the changes list on a 100-track song: Planned.

## In the code

- `src/model/versions/VersionsModel.h`: everything the UI reaches, by plain values and ids, and the commands.
- `src/model/versions/internal/VersionHistory.h`: the open song's history, its branches, the merge under way, Local
  History.
- `src/model/versions/internal/VersionStore.h`, `PieceStore.h`, `SongPieces.h`: the folder, the packs and the pieces.
- `src/model/versions/internal/SongDiff.h`: what differs between two songs.
- `src/model/versions/internal/ApplySong.h`: the song made like another with the fewest edits (`makeSongLike()`,
  `makeTrackLike()`, `makeSongTheSame()`).
- `src/model/versions/internal/SongMerge.h`: the three-way merge (`mergeSongs()`).
- `src/model/commands/UndoHistory.h`: the undo history as the view lists it.
- `src/engine/nodes/Comparison.h`: a bounce played after the master.
- `src/ui/versions/VersionsView.h`, `BranchPicker.h`; `src/ui/shell/ActivityBar.h`, `StatusBar.h`, `TopBar.h` (A/B).
- Tests: `tests/VersionsTests.cpp`, `VersionsBranchTests.cpp`, `VersionsMergeTests.cpp`,
  `VersionsCheckoutTests.cpp`, `VersionsGoingBackTests.cpp`, `VersionsHearingTests.cpp`, `VersionStoreTests.cpp`,
  `tests/golden/format/`.
