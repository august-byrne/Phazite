---
title: Versions
description: Save versions of a song, see what changed, go back to one, hear old mixes against the song, try ideas on branches, and get back work you didn't save as a version.
sidebar:
  order: 12
---

Phazite keeps the versions of each song inside its project folder. Open them with the branch icon down the left of the window (**Versions**). Everything here works without an account or a network.

## Save a version

Save a version whenever the song is at a point you might want back: before a risky change, after a good session, or when you send a mix to someone.

1. Type a name, if you like: the field's first line names the version, and the lines after it are a note. Left empty, the version is named by what changed, such as "Update Bass and Drums", which the empty field shows before you save.
2. Click **Save Version**, or press **Ctrl+Enter** in the field, or **Ctrl+Alt+S** anywhere (**Ctrl+N** with FL Studio's shortcuts).

A version holds the whole song as it is. **Save Version** is greyed out while nothing has changed since the last one.

**Saved too early?** **Undo Last Version** (in the **...** menu at the top of the view, under Version, or on the last version's menu) takes the version off the branch. The song stays as it is, so what that version held shows in the changes again, and its name returns to the field. For 30 days, **Recover Lost Versions...** brings it back (below).

## See what changed

**Changes** lists what changed since the version the song grew from (the last version, normally: see **Newer versions** below), a line per track (and one for the master and one for the song's own tempo, sections and chords), each with a letter: **A** added, **M** changed, **D** deleted. Click a line to select its track.

Changed your mind? A line's arrow (**Discard Changes**) makes that track what that version has, and the arrow on the **Changes** heading (**Discard All Changes**) does it for the whole song. Either is one undo step, so Ctrl+Z brings the change back.

## Go back

**History** lists every version, newest first, each on its branch's line. Click a version to unfold it: what it changed shows under it, a line per track. Click it again to fold it.

- **One track:** right-click the version and choose **Compare with the Song**: every track that differs from it is listed. Click the arrow on the track's line to make it what it was then; the rest of the song stays as it is. (Unfolding a version lists only what it changed, with the same arrows.)
- **The whole song:** right-click the version and choose **Restore This Version**.
- **Compare first:** right-click a version and choose **Compare with the Song** to list what differs from the song now, each line with its own arrow. **Done** goes back to the changes.

Each restore is one undo step, so Ctrl+Z takes it back, and restoring the whole song keeps the song as it was as an automatic save first (below). Every version stays in the history, so nothing is lost by going back. Save a version afterwards to keep the song as it is now.

To rename a version, for example to mark the one you sent to the label, right-click it and choose **Rename Version...**.

## Hear an old version against the song

- **Open a version:** double-click it in **History**, or right-click it and choose **Open Version**. The song becomes that version right away, live, with no bounce, and keeps playing at the same beat. A bar across the top of the view says **Looking at "Verse 2" (3 days ago)**, and the transport bar and the status bar say it too. Your song as it was, unsaved changes and all, is kept: click **Back to Now** (or press **Escape** in the view) and it comes back exactly as it was, Undo and Redo included. Nothing is saved while you look: saving, exporting or saving a version goes back to Now first.
- **A/B:** press **N**, or click **A/B** in the transport bar, to open the last version (or the one you opened last) in place of the song, and press it again to go back to Now. This is the quick way to hear whether a change is better.
- **Change something while a version is open, or record there,** and Phazite starts a branch there for you (**idea-3**) as the change is made (automation you write while it plays counts too, from Stop), so it never touches the old version or your song: a notice says so, with **Back to main** to return to your song as you left it. While automation is being written, stop first: no version opens, and Back to Now waits, until you press Stop. To start a branch without changing anything, click **Create Branch Here** in the bar.
- **Bounces** are optional. A version with a bounce has a play mark: click it, or right-click the version and choose **Hear It**, to hear the bounce in place of the song, matched in loudness so the louder one doesn't win just by being louder. Bounce a version that uses a plug-in you don't always have installed (right-click it and choose **Bounce**, or turn on **Bounce Each Version** in **Save Version**'s menu): the bar offers its bounce when that plug-in is missing.

## Try an idea on a branch

A **branch** is an alternate version of the song, such as a radio edit, a remix or a different chorus, that you can switch to and from.

- **Make one:** click the branch name at the top of the view, or in the status bar, and choose **Create new branch** (type a name, spaces and all, or leave it as "idea-2"). The song carries on there, changes included. To start one from an older version, right-click that version and choose **Create Branch**.
- **Switch:** pick another branch in the same list. Each branch keeps its own unfinished changes, so nothing is lost when you switch, and the song goes on playing.
- **Bring an idea back:** **Merge Branch...** (in the **...** menu, under Branch) lists what the other branch changed, a line per track. Click **Merge** to take all of it, or **Cancel**. Where both branches changed the same thing, each conflict is listed with a choice of value (**Keep This Branch's**, **Take the Other Branch's** or **Take What Was Before**), heard as you switch it. Save a version to finish the merge, or click **Cancel** to go back to how the song was. Undo takes the merge back as well. A merge you haven't saved a version of yet is part of the project: save the project, and it still waits when you open it again.
- **One track from another branch:** right-click that branch's last version in History, choose **Compare with the Song**, then click the arrow on the track's line.
- **Clean up:** **Delete Branch...** removes a branch you no longer need. If it holds versions no other branch has, or changes left on it, Phazite asks first. Either way, the branch can come back for 30 days (below).

## Get back what you took away

Nothing you take off a branch is gone at once. A version you undid, and a branch you deleted with its versions and the changes left on it, are offered for 30 days by **Recover Lost Versions...** (in the **...** menu), which lists them newest first, each with when and why it went: "Undone on 12 Nov, from main", "On idea-3, deleted on 14 Nov, with the changes left on it". Click one to bring it back as a branch, named after the one it was on ("idea-3 (recovered)"). Switch to it, and the song is as you left it there.

After 30 days, they are no longer listed. Phazite still keeps them in the project's `Versions` folder, with their bounces: it deletes nothing there by itself, so the folder only grows. A command that frees that room by hand, listing what it would remove, is planned.

## Automatic saves: a history without asking

You don't have to save versions to have a history. While the song differs from its last version, Phazite keeps it as it was, without asking. It takes an automatic save:

- when the project opens;
- every 10 minutes while you work;
- just before anything that replaces the song, such as a restore, discarding every change, or a merge.

They show in **History**, in grey, between the versions, by when each was taken: with a clock, why it was taken ("Auto-saved", "Before Restore") and when. Several between two versions fold into one row ("6 automatic saves"): click it to unfold them, and again to fold them. Point at one and click its arrow to make the song what it was then, as one undo step.

Saving a version, then, is naming a point of that history: one you want to find again, compare with, or branch from.

Phazite lists every automatic save of the last two hours, then one an hour for a day, then one a day for 30 days. To see the versions alone, untick **Show Automatic Saves** in the **...** menu.

**Undo History**, at the very bottom and folded until you click it, lists each change you made since the project opened, newest first, by name ("Move Clip", "Change Volume"). The one the song is at says "now". Click an older change to go back to it, as if you had pressed Undo that many times; the undone changes stay in the list, dimmed, until a new change clears them.

## Newer versions

The project file remembers which branch the song is on and which version it grew from, so the changes always show your own work. If the branch has versions the song doesn't have yet, the view says so under **Save Version** ("'main' has 1 newer version than the song grew from"), with one button:

- **Take the Newer Versions**, when you have no changes of your own: the song becomes the branch's newest version. One undo step, so Ctrl+Z takes it back.
- **Merge the Newer Versions**, when you do: the newer work is merged with yours, as a merge of another branch is, and you hear it at once. Save a version afterwards to keep both.

This happens when another computer saved versions on the branch (below), or when the project file was put back from a backup or an older autosave. In the second case, **Save Version** waits until you take the newer versions in, so that it never saves over them; if you want to go on from the older song instead, **Create Branch** starts a branch there.

## In a synced folder

A project folder can live in a synced folder (Dropbox, OneDrive and the like) and be opened on a studio PC and a laptop, one at a time. Versions saved on the other computer show as newer versions when the project opens (above). If versions were saved on both before the sync caught up, a version you save goes its own way beside the other computer's, and the view says so and names the other computer's version: right-click it in History and choose **Merge into** the branch to bring that work in. Nothing is lost either way.

If the project file arrives before the other computer's record of its branch (the branch it is on isn't in this folder yet), the view says so and shows only the song's own changes. **Save Version** waits, so that nothing is saved onto another branch; once the sync finishes and the project opens again, all is as usual, or **Create Branch** keeps working from the song on a branch of its own.

Each computer writes files of its own into the project's `Versions` folder, so the two never write the same file. Neither deletes anything the other saved. If a computer's clock is set back (a dead battery, say), what it saves meanwhile is never dated earlier than what it saved before.

## If the history is damaged

- **A version that can't be read:** if a piece of the branch's last version is missing or damaged in the project's `Versions` folder (a sync that hasn't finished, a disk error), the view says so and names the newest version of the branch that reads. No version is saved on top of the damaged one. Choose **Reset Branch to Last Readable Version** (in the **...** menu) to take the branch back to it: the song stays as it is, and its changes since that version show as usual. If the damaged version's pieces arrive within 30 days, **Recover Lost Versions...** brings it back.
- **The version the song grew from can't be read** (damaged, or not synced yet): the view says so once, and the changes are shown against the branch's newest version instead. Nothing of the song is lost.
- **Verify History** (in the **...** menu) reads every piece of the history, every version's song, and the songs the automatic saves and your branches keep, and checks that the audio files every version plays are still there and unchanged. It says which version misses which file ("Audio/Kick.wav missing", "changed since", "can't be read"), what is damaged, which versions follow one that can't be read, and which versions no branch holds, which **Recover Lost Versions...** then lists too. Checking the audio takes a while on a big project: the view shows how far it has come, and choosing **Verify History** again (now **Stop Verifying History**) stops it. Saving a version, switching branches or anything else that reads a version stops it too, rather than waiting for it, and the view then says it was stopped, never that the history is whole.
- **A file that doesn't read whole:** if a file of the history that says what it keeps doesn't read whole, Phazite says so where Verify History says what it found: "Phazite is keeping everything in the Versions folder, because ...".
- Versions don't copy your audio: they name the files in the project's `Audio` folder. **Clean Up Unused Audio...** never offers a file that a version you can still get back plays.
