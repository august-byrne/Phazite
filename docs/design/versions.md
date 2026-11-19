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
| A version opened live in the song's place, Back to Now, and A/B between them | Built | 2026-11-19 (A version open) |
| Bounces of versions, and Hear It for a version with one | Built | Optional: nothing bounces by itself |
| Local History | Built | |
| Undo History | Built | |
| Undo Last Version, Rename Version... | Built | |
| Verify History, with every version's audio files, Local History's songs and those left on branches | Built | S4.4 |
| A version off every branch offered 30 days by Recover Lost Versions..., with when and why it left | Built | S4.3 |
| The store's packs joined by size, losing nothing | Built | S4.4 |
| The store swept of what nothing names, older than 30 days | Built, off | Known bug 196: nothing is deleted yet |
| The song tied to the version it grew from; its changes worked out in the background | Built | S4.5 ([versions-cut.md](versions-cut.md)) |
| The playlist marking what changed | Planned | No roadmap step |
| Staging, a version checked out on its own (with no way back to Now), a merge of some lines | Removed | S4.2 ([versions-cut.md](versions-cut.md)); a version opens live again, with Now kept (A version open) |
| Tags, stash, rebase, squash, cherry-pick, blame, bisect | Declined | Not needed by one person working alone |
| Backup to another folder, a cloud copy, share links | Planned | No roadmap step |

## From the start

- **First version.** A project keeps versions from its start: as soon as its history is read and has no branch, the
  song as it is is saved as "First version", on the branch **main**. A new project's history moves with it at its
  first save.
- **Auto Save** keeps the project file up to date ([projects-saving-export.md](projects-saving-export.md)), so every
  change shows in the changes as it is made.
- The status bar names the branch beside the save state, with a star while the song has changes since the version it
  grew from ("main*"). A click opens the branch picker.
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

**Changes** lists what the song holds now that the version it grew from doesn't (the branch's last, unless the branch has
newer versions: below), a line for each track that changed, the master, and the song's own (its tempo, sections, chord
lane and track order).

- Each line says what changed in Phazite's own words ("volume, 12 notes"), with a letter: **A** added, **M**
  modified, **D** deleted; **C** marks a merge's conflict. The colours are VS Code's.
- View state and how the song plays never show. They are left out by the same lists as for unsaved changes.
- A plug-in's settings count as changed only when the plug-in reported a change or one of its knobs moved. A
  plug-in's change is named by the parameters whose values differ.
- A click on a line selects that thing in the song.
- **Discard Changes** (`version.putBack`) makes a line what the version the song grew from has again; **Discard All
  Changes** (`version.discardAll`) every line. From that version, not the branch's newest: what the user discards is
  their own changes; versions the branch has since are taken in apart (below), never slipped in by a discard. Each is
  one undo step. A clip moved between tracks since ends where that version has it, once, whichever of the two tracks'
  lines is discarded.
- **Worked out in the background** (Off the message thread, below): the badge, the star and the list show the last
  changes worked out, and follow an edit as soon as they are in. Save Version and Discard All Changes act on the song as
  it is, not on that list: they are there while the song may have changes (some listed, or an edit since), Discard makes
  the song the version's whatever the list said, and Save Version records the song as it is, named by what it changed,
  worked out from its copy.

### The song tied to its version

The project file says which branch the song is on and which version it grew from (`versionsBranch` and `versionsBase` on
the song: [project-format.md](../project-format.md)), and so do its autosave, its backups and the song left on a branch.
The changes are worked out against that version, not the branch's newest: so work saved elsewhere since is never shown as
removals of the song's, nor taken out by the next version.

- **Kept right by every command, in the same step:** Save Version moves them to the version saved; Undo Last Version to
  the version before; Create Branch to the new branch (from Now, the same version; from a version, that one); a switch to
  the branch entered and the version its song grew from (its song left there says, else its last version); Reset Branch
  to Last Readable Version to the version it resets to; a merge's version to it. Restore This Version and a line's restore
  make the song a version's but leave it growing from where it was, as git's restore leaves HEAD: the next version follows
  the branch's line, and the changes show what the restore took back. A merge taken, Cancel Merge, Discard and Recover
  Lost Versions leave them. They are no change to the song (no star, no unsaved project): moved without an undo step, and
  written into the project file at once while the song has no other unsaved change (with the next save otherwise).
- **The branch has newer versions** (another computer saved one; the project file came back from a backup or an older
  autosave): the view says so under Save Version ("'main' has 2 newer versions than the song grew from...") with one
  button, **Take the Newer Versions** (`version.takeNewer`): where the song has no changes of its own, the song becomes
  the branch's last version, which it grows from then, as one undo step whose Undo takes both back; with changes of its
  own, **Merge the Newer Versions**, the merge that exists, from the version the song grew from, its version then
  following both lines.
- **Save Version meanwhile:** where the newer versions are on another computer's record of the branch, a version saved
  goes its own way beside them, which loses nothing: the branch shows the two lines to merge again, as any branch that
  went its own way on two computers. Where only this computer's record holds them (a project file put back from a backup
  older than versions saved here since), a version saved on top of the song's own would leave them on no branch: Save
  Version is greyed, saying why, until they are taken in, or Create Branch goes on from the version the song grew from.
- **A version the names point at that can't be read** (damaged, or its pieces not synced yet): the changes are worked
  out against the branch's last version, which the song grows from then, and the view says so once; nothing of the song is
  lost. The same after an Undo or a Redo of Take the Newer Versions that moves it to a version that can't be read.
- **A branch the names point at that the folder hasn't** (another computer's record of it not synced yet, or deleted, or
  a backup from before it was): the song keeps growing from its own version where that reads (so another branch's work
  never lists as removals), its names stay as they are, the view says so, and no version is saved meanwhile, which would
  land on the branch the folder names and take that branch's work out; **Create Branch** goes on from the song on a branch
  of its own. Where its version doesn't read either, the changes are against the folder's branch's last version, and the same
  holds; and a switch to another branch waits for it too, as it would leave the song on that branch. A song without names (a
  first version not saved yet, a DAWproject's song) grows from its branch's last version.
- **A version the branch moved back from** (Undo Last Version, or Reset Branch, on this computer, the project file not
  saved after: Don't Save at the close): the file names a version that follows the branch's last; the song grows from the
  branch's last version as it opens, and the work of the one undone shows as its changes, as Undo Last Version left it.
- **Nothing saved for no change:** Save Version is there while the song may have changes (an edit since the last list),
  and a version of a song that holds none since the version it grew from (an edit and its Undo, Discard All Changes, the
  version restored) isn't written: the view says "Nothing changed since the last version.", and the name and note typed
  for it stay in their fields, as they do when a version's write fails. A merge's version is saved
  however little it changes; one of a version that follows the song's own already, and gave nothing a new ID (Merge the
  Newer Versions', or a branch merged that was ahead of the song), follows that version alone, as the graph needs no other.
- **A version saved as the project closed** (its write finishes after the close: The store, below), whose names the file
  couldn't take: the project opens growing from that version where the song is that version's as it is, and from the one
  its names say otherwise.
- **A switch** keeps the song left on the branch entered named by that branch's record until the project file holds the
  song after it: a save that fails leaves both branches' songs as they were.
- **During a drag** no round of the changes runs: one runs once it is committed or put back, and at most one a tick
  (250 ms) otherwise.

### History

- Every version of every branch, newest first, each on its branch's line in the branch's colour, under a row for the
  **Changes Since the Last Version**.
- **A click on a version unfolds it:** under it, a row for each line it changed since the version before it. One version
  is unfolded at a time.
- **A double-click opens it** in the song's place, live, with no bounce (Open Version, A version open, below); on the
  changes' row, it goes Back to Now.
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
- A version's menu has Open Version first (Back to Now on the version open), then Restore This Version, Compare with the
  Song, Create Branch, Merge into Current Branch..., Rename Version..., Undo Last Version (on the last), Hear It (greyed
  without a bounce), Bounce, Delete Bounce and Export as DAWproject....

## Branches

- **Create Branch** (`version.newBranch`) asks nothing: the branch is named "idea-2" (the next number free), takes
  the next colour, and the song goes on there at once. Made from the song as it is, it takes the changes with it. Made
  from an older version, the song becomes that version's, as a switch makes it.
- **Switch Branch...** (`version.pickBranch`, the branch button and the status bar's branch) opens the branch picker:
  the branches, the current one ticked, each with its last version; typing narrows them or names a new one.
- **Switching to a branch** (`version.switchBranch`) makes the song the other branch's in place, with the fewest edits.
  What both branches have stays as it is: its plug-ins stay loaded and the song plays on. The branch left keeps its song
  as it was, changes and view state included. The project is saved after, so that its file is the branch's song; the
  branch entered lets go of the song left on it only once that save is done, so a save that fails loses neither. It is
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
  to something it made since. Where another computer merged the same version and gave what it added other IDs, a later
  merge of that computer's line takes them back to this line's by both merges' records, so an item both took in is one item,
  and an item that line made under an ID its merge gave away stays its own, with an ID of this line's; a line merged back
  into the line that merged it (main merged idea-2, then idea-2 merges main) takes its own items back from main's IDs
  alike, by main's merge's record. The other line's song is taken first to the IDs the version both lines have gives
  them, then to this line's, one step each, so that no item is moved twice. The version of that merge records both, by
  the IDs the merged song holds, so that a merge after it knows them again. Where the two songs, so matched, would still
  hold one ID for two things (records another tool wrote, or a history the records don't cover), nothing is merged, and
  the merge says why: two things are never made one. No ID is in the song twice after a merge, a restore or a discard,
  and every one of them leaves the song's count of IDs past every ID it holds.
- **The version that finishes it** has both lines as its parents (the one merged alone where it follows the song's own
  already and the merge gave nothing a new ID), and is named for what it merged ("Merge idea-2",
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

## A version open

Listening to an old version is opening it: its song, live, in the song's place, with no bounce, and the song as it was
(Now) back exactly at Back to Now. A song's changes are text, so the switch is an edit of the song like any other, which
the engine follows as it follows an edit.

- **Open Version** (`version.open`, a double-click on a version, first in its menu) reads the version's song on the
  history's thread (once: the last two read are kept for the next open), the window held meanwhile as a switch holds it,
  then makes the song that version's in place with the fewest edits (`makeSongLike()`). What both have stays as it is:
  its plug-ins loaded, the song playing on at the same beat. A plug-in the version has and Now lacks loads in the
  background as one added does. The song as it is goes into Local History first ("Before Opening a Version"), and the
  plug-ins' own settings are stored before it opens (the command asks them, as Save Version does).
- **Kept whole:** the version is made the song's in a trial that holds the song's undo history (`Trial::holdHistory()`,
  `model/commands/internal/Undo.h`), one step that Back to Now puts back, leaving nothing to redo: the song's own undo and
  redo steps are as they were. The song's redo steps are done again first, out of sight, under the version's step, and
  Back to Now undoes them with it, as JUCE can't take a step back alone once another began on top of it (JUCE workaround
  W56); the history keeps two steps more meanwhile (the version's and one begun on top of it), and one more for each step
  begun inside another on top of it, so that none of the song's goes. What isn't an undo step may change while a version
  is open (mute and solo, the loop, view state, a plug-in's settings in its window): Back to Now makes each as Now had it
  again, from a copy of the song taken as the version opened, while the song still shows it, so that none of it is a
  change of the project's, and gives the plug-ins changed meanwhile Now's settings again. A freeze waits for Back to Now.
  What is selected and the time range selected are kept as the version opens and put back at Back to Now, whatever was
  selected meanwhile, and whatever range Now's redo steps, done again under the version and undone with it, set.
- **Redo never reaches a step thrown away.** JUCE appends the redo steps it put aside whenever a step is taken back alone
  (a drag's Escape, a trial put back, a version closed), and those it put aside long before where there were none: Phazite
  counts them, and Redo, its name and Undo History reach only the song's own (`Song::History::staleRedoSteps`; JUCE
  workaround W56).
- **Now's meanwhile:** the project's unsaved mark, the title's star, the status bar's star and the Changes list are
  Now's: the version is no change of the song's, nor are the plug-ins' own changes meanwhile, and a save in the background
  under way as it opened ends with the project saved. The view says so under its bar ("The changes below are the song's
  own"). Undo and Redo are greyed ("Back to Now first"); Undo History lists Now's steps alone.
- **The bar,** across the Versions view's top: "Looking at "Verse 2" (3 days ago)", with **Back to Now**
  (`version.backToNow`, Escape while the view has the keys, a double-click on the changes' row) and **Create Branch Here**
  (Create Branch from the version, from Now). The version's row is marked "Open". With the view closed, the transport bar
  says the same beside A/B, and the status bar beside the branch.
- **From one version to another** directly: still one step back to Now. The version the song is already (the last, with no
  changes since) opens nothing, and its command says why.
- **Nothing writes the old version as the song.** Save, Save As, an export of the song, Save Version, a switch, a merge,
  a restore, a bounce of the current version, an import of a DAWproject, Clean Up, closing the project and quitting each go
  Back to Now first, silently (it is lossless), then do their work. Auto Save and the autosave wait until it is back. No
  snapshot of Local History is taken of it, no round of the changes runs, nothing heals its base, and a project never
  reopens in a version. And should a caller be missed: whatever in the history copies or writes the song as Now (a switch's
  song left, a branch from Now, a merge's song before, Save Version's copy) refuses while a version is open, as does every
  write of the project file.
- **Back to Now waits for a render, for Stop and for a branch being made.** While Bounce in Place, Consolidate or another
  render has the song's plug-ins, its step lands in the song as it ends: Back to Now and A/B are greyed then ("Not while a
  render has the song"), and what would write the song refuses (a save says why). The same while automation is being
  written ("Stop first: automation is being written": a pass's points are the song's it began on, and the undos of Back to
  Now would be recorded into it), and while the branch an edit makes waits for Now's piece (below). During a drag on the
  version they are greyed too, until its step ends; while a swap is tried on it, "Finish swapping first" (a save puts the
  swap back, then the version).
- **No version opens while automation is being written** on Now (a pass under way in Touch, Latch or Write, or one waiting
  for a drag to end): "Stop first: automation is being written", and A/B with it; Stop gives Now its pass.
- **An edit starts a branch there, as its step ends.** A step that begins while a version is open (an undo step, a
  gesture, Swap's trial, the step automation is recorded in, an imported file's or Normalize's or Quantize Audio's step as
  its work ends) goes on top of the version's as one of its own, and steps begun inside it go on top too. As the last of
  them ends with changes, it makes a branch from the version, as Create Branch from it does ("idea-3", asking nothing): the
  song grows from that version on the new branch, and Now, with its unsaved changes, is kept on the branch it was on, as a
  switch keeps the song it leaves. A step that ends with none (a knob pressed and let go, the wheel on a knob at its end, a
  click of a tool that moved nothing, a drag ended with Escape, Swap put back, a play with no automation written) did
  nothing: the version stays open, and Back to Now brings Now back whole. Automation written while a version plays is an
  edit as any other: a knob moved in Touch or Latch makes the branch as its drag ends, and playing a version in Write mode
  makes a branch at Stop (earlier where a punch-in at Play sets a knob to its lane, or where the playhead is put elsewhere
  while it writes, as the pass is written there). Now's piece is split and written on
  the history's thread as the version opens, so the branch writes only the branches' small records. The branch's undo
  history starts with the edit: Undo takes it back to the version, and nothing under it is reachable (JUCE drops those
  steps as the branch is made: workaround W56). The project is saved after it, as after a switch; until that save lands,
  the project file holds the song saved last, which names the branch it was on, so a save cancelled and a close with
  Don't Save open that song on that branch, whole, Now's unsaved changes kept on its record as a switch keeps them. A
  notice says "You changed "Verse 2", so this is now the branch idea-3." with **Back to main** (a switch back). The
  version's record never changes.
- **An edit made before Now's piece is written waits for it.** Right after an open, Now's piece may still be on the
  history's thread (seconds for a large song, behind the "Before Opening a Version" snapshot): the edit's step ends, and
  the branch waits for the piece, the version shown with the edit on it and the window held as a switch holds it ("Making
  a branch from "Verse 2"", with Cancel after a second); Back to Now, A/B, saves and closes wait too, and Auto Save. Once
  the piece is in, the branch is made as it would have been at once, its notice shown and the project saved.
- **Where the branch can't be made** (the history busy, Now's piece or a record that can't be written, the wait above
  cancelled, or Now's piece waiting behind a first save's copy of the folder, which takes minutes), the version is kept
  as a restore on Now's branch instead ("Restore "Verse 2"", the edit after it), and the notice says which and how many
  Undos take the song back as it was ("Undo twice"; more where Now had steps to redo, which are done again under it, or
  where the edit was steps begun inside one another; one fewer where the version's song was Now's). The song is then the
  version with the edit, an unsaved change of the project's (a save in the background under way since before it opened
  ends with the project still unsaved).
- **Recording** makes the branch as it starts, before the take takes its tracks, so that nothing pulls the song from under
  it (a save during the take saves the branch's song). Where the branch can't be made at once (Now's piece not in yet, the
  history busy, a record that can't be written), recording doesn't start, and the notice says why; where a record
  couldn't be written, the version is kept as a restore on Now's branch, as above.
- **Plug-ins not installed here:** a version whose plug-in isn't installed opens as any project with a missing plug-in
  does (it plays nothing); the bar says which, and offers **Hear Its Bounce** where the version has one.
- **How fast** (a Debug build, 2026-11-19; `tests/VersionsOpenTests.cpp` and `tests/Vst3PluginTests.cpp` say them): a small
  song's first open about 35 ms (its read), Back to Now under 1 ms, the next open about 1 ms; with the tests' real VST3 that
  only the version has, the open as fast and the plug-in loaded in the background some 25 ms after. A plug-in instance
  that the open takes away (Now's alone) is let go of, and loads again in the background at Back to Now, as an undo brings
  one back: keeping those alive meanwhile is not built.

## Hearing a version

- **A/B** (`version.compare`, N, and the transport bar's A/B, over plug-in windows too) switches between the song and B
  live: a press opens B (Open Version), another goes Back to Now, at the same beat, the song playing on. B is the version
  open, else the version opened last, else the version whose bounce was heard last, else the version the song grew from,
  so that the changes since are heard against what was. A/B shows while there is a B to open, and is greyed while B is the
  song itself. The two never play at once, so their loudness isn't matched live.
- **A bounce** is the song as it was in a version, rendered as Export Song renders it, as FLAC at 16 bits: an extra,
  for a version that can't open whole (its plug-ins missing here), and for an instant switch in a heavy song. **Bounce**
  (`version.bounce`) makes one for a version that has none: the current version's from the open song, playback
  stopped; an older one's in the background, with plug-in instances of its own, while the song plays on. **Bounce Each
  Version** bounces each version saved. Nothing bounces by itself. **Delete Bounce** frees its file. Bounces are nearly
  all of the room a history takes.
- **Hear It** (`version.hear`, the play mark on a version with a bounce) plays its bounce in place of the song, from the
  engine's next block, at the same beat. Greyed for a version without one ("No bounce: Open Version plays it live"). The
  transport bar says "Hearing: *name*". An edit to the song, or A/B, switches back to the song.
- **Loudness matched,** for a bounce: both are measured as they play, and at each switch the louder one is turned down to
  the other's loudness so far, so that a version can't win by being louder.

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
- **Verify History** (`version.verify`) reads every piece and checks it against its name, and every pack file again as
  a pack (one held, cut off, or with a byte of its index or trailer changed is named); walks every version's song,
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
- **The changes, worked out there too** (S4.5; `ChangesWorker.h`): the song's notifications mark what changed (a track,
  the master, an automation curve, the song's own, the tracks' order and names); a round copies only those parts on the
  message thread (a track's own parts, without the tracks in a folder; a light outline of the song's own and its tracks'
  names and order) and the knobs of the plug-ins in them, and the history's thread compares them with the version's song,
  which it keeps between rounds with what each part compared to, and hands back the whole list, once a turn of the message
  loop. A track that didn't change is never copied or compared again; the message thread compares nothing. One round at a
  time: what changes meanwhile goes into the next, and a list of another version's (the song switched, a version saved,
  Undo Last Version) is dropped. After Save Version the changes are none, known without a round where the song didn't change
  while it was saved; a switch to a branch's last version, the same. Where everything must be compared again (the project
  opened, a switch to a song left on a branch, a plug-in joining), the whole song is copied once. Compare with the Song's
  list is worked out alike, against the version compared. Local History's snapshot as the project opens waits for the first
  list, and every snapshot checks on the history's thread that the song differs from its version.
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
- The song is always on a branch: an old version is opened live in its place, which Back to Now takes back exactly, and an
  edit there starts a branch from it (the owner, 2026-11-19); it is never checked out on its own, with no way back to Now.
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

- The playlist marking the clips and tracks that changed since the last version: Planned.
- A branch's colour chosen by hand: Planned.
- One undo for every versions operation (a switch, a deleted branch): Planned.
- Reverting one old version while keeping later ones: Planned; restoring a line from a version covers most of it.
- Showing only named versions in a long history, and exports tied to versions: Planned.
- Backup to a folder of the user's choosing, a cloud copy, share links with comments, and working together on
  branches: Planned.
- Tags, stash, rebase, squash, cherry-pick, blame and bisect: Declined.
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
- A version saved over newer versions only this computer holds (a project file put back from a backup), as a line of its
  own kept on a branch of its own: Not built; Save Version is greyed then, and Create Branch goes on from the song's
  version.
- Noticing another computer's newer versions while the project is open: Not built; they show as the project opens again,
  and a version saved meanwhile goes its own way beside them, losing nothing.

## In the code

- `src/model/versions/VersionsModel.h`: everything the UI reaches, by plain values and ids, and the commands.
- `src/model/versions/internal/VersionHistory.h`: the open song's history, its branches, the merge under way, Local
  History.
- `src/model/versions/internal/VersionStore.h`, `PieceStore.h`, `SongPieces.h`: the folder, the packs and the pieces;
  `StoreSweep.h`: the sweep (its letting go off) and each computer's copy (off with it).
- `src/model/versions/internal/SongDiff.h`: what differs between two songs; `ChangesWorker.h`: the changes of the open
  song worked out on the history's thread from what changed.
- `src/model/versions/internal/ApplySong.h`: the song made like another with the fewest edits (`makeSongLike()`,
  `makeTrackLike()`, `makeSongTheSame()`).
- `src/model/versions/internal/SongMerge.h`: the three-way merge of every line (`mergeSongs()`).
- `src/model/commands/UndoHistory.h`: the undo history as the view lists it.
- `src/engine/nodes/Comparison.h`: a bounce played after the master.
- `src/ui/versions/VersionsView.h`, `BranchPicker.h`; `src/ui/shell/ActivityBar.h`, `StatusBar.h`, `TopBar.h` (A/B).
- Tests: `tests/VersionsTests.cpp`, `VersionsBranchTests.cpp`, `VersionsMergeTests.cpp`, `VersionsMergeWaitingTests.cpp`,
  `VersionsFolderCopyTests.cpp` (a first save's copy of the folder), `VersionsGoingBackTests.cpp`,
  `VersionsHearingTests.cpp` (bounces, and A/B), `VersionsOpenTests.cpp` (a version open, Back to Now, an edit there),
  `VersionsKeptWholeTests.cpp`, `VersionsRecoveryTests.cpp` (the 30 days),
  `VersionsSweepTests.cpp` (the sweep, with its letting go on but for the product's own test, and Verify History),
  `VersionsTiedTests.cpp` (the song tied to its version), `VersionsChangesTests.cpp` (the changes worked out in the
  background), `VersionsTwoComputersTests.cpp` (each computer's copy, clocks, Verify's limits), `VersionStoreTests.cpp`,
  `tests/golden/format/`.
