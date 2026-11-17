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

**Saved too early?** **Undo Last Version** (in the **...** menu at the top of the view, under Version, or on the last version's menu) takes the version off the branch. The song stays as it is, so what that version held shows in the changes again, and its name returns to the field.

## See what changed

**Changes** lists what changed since the last version, a line per track (and one for the master and one for the song's own tempo, sections and chords), each with a letter: **A** added, **M** changed, **D** deleted. Click a line to select its track.

Changed your mind? A line's arrow (**Discard Changes**) makes that track what the last version has, and the arrow on the **Changes** heading (**Discard All Changes**) does it for the whole song. Either is one undo step, so Ctrl+Z brings the change back.

## Go back

**History** lists every version, newest first, each on its branch's line. Click a version to open it: what it changed shows under it, a line per track. Click it again to fold it.

- **One track:** right-click the version and choose **Compare with the Song**: every track that differs from it is listed. Click the arrow on the track's line to make it what it was then; the rest of the song stays as it is. (Opening a version lists only what it changed, with the same arrows.)
- **The whole song:** right-click the version and choose **Restore This Version**.
- **Compare first:** right-click a version and choose **Compare with the Song** to list what differs from the song now, each line with its own arrow. **Done** goes back to the changes.

Each restore is one undo step, so Ctrl+Z takes it back, and restoring the whole song keeps the song as it was in Local History first. Every version stays in the history, so nothing is lost by going back. Save a version afterwards to keep the song as it is now.

To rename a version, for example to mark the one you sent to the label, right-click it and choose **Rename Version...**.

## Hear a version against the song

- **A/B:** press **N**, or click **A/B** in the transport bar, to hear the last version in place of the song, at the same point and just as loud. Press it again to hear the song. This is the quick way to hear whether a change is better. The first time, Phazite bounces the version in the background while the song plays on, and switches as soon as it's ready. Any edit switches back to the song.
- **An older version:** double-click it, or right-click it and choose **Hear It**, to hear it in place of the song. A version without a bounce is bounced first, in the background while the song plays on, and heard as soon as it's ready; one with a bounce has a play mark, which plays it at once. To bounce versions ahead, right-click one and choose **Bounce**, or turn on **Bounce Each Version** in **Save Version**'s menu. A/B then switches between the song and the version you heard last.

Both are matched in loudness as they play, so the louder one doesn't win just by being louder.

## Try an idea on a branch

A **branch** is an alternate version of the song, such as a radio edit, a remix or a different chorus, that you can switch to and from.

- **Make one:** click the branch name at the top of the view, or in the status bar, and choose **Create new branch** (type a name, spaces and all, or leave it as "idea-2"). The song carries on there, changes included. To start one from an older version, right-click that version and choose **Create Branch**.
- **Switch:** pick another branch in the same list. Each branch keeps its own unfinished changes, so nothing is lost when you switch, and the song goes on playing.
- **Bring an idea back:** **Merge Branch...** (in the **...** menu, under Branch) lists what the other branch changed, a line per track. Click **Merge** to take all of it, or **Cancel**. Where both branches changed the same thing, each conflict is listed with a choice of value (**Keep This Branch's**, **Take the Other Branch's** or **Take What Was Before**), heard as you switch it. Save a version to finish the merge, or click **Cancel** to go back to how the song was. Undo takes the merge back as well. A merge you haven't saved a version of yet is part of the project: save the project, and it still waits when you open it again.
- **One track from another branch:** right-click that branch's last version in History, choose **Compare with the Song**, then click the arrow on the track's line.
- **Clean up:** **Delete Branch...** removes a branch you no longer need. If it holds versions no other branch has, or changes left on it, Phazite asks first.

## Local History: saved without asking

While the song differs from its last version, Phazite keeps snapshots of it without asking. It takes one:

- when the project opens;
- every 10 minutes while you work;
- just before anything that replaces the song, such as a restore, discarding every change, or a merge.

Open **Local History** near the bottom of the view. Each row's arrow makes the song what it was then, as one undo step. Snapshots are kept for 30 days, fewer as they grow older.

**Undo History**, at the very bottom, lists each change you made since the project opened, newest first, by name ("Move Clip", "Change Volume"). The one the song is at says "now". Click an older change to go back to it, as if you had pressed Undo that many times; the undone changes stay in the list, dimmed, until a new change clears them.

## In a synced folder

A project folder can live in a synced folder (Dropbox, OneDrive and the like) and be opened on a studio PC and a laptop, one at a time. If versions were saved on both before the sync caught up, the view says so and names the other computer's version: right-click it in History and choose **Merge into** the branch to bring that work in.

## If the history is damaged

- **A version that can't be read:** if a piece of the branch's last version is missing or damaged in the project's `Versions` folder (a sync that hasn't finished, a disk error), the view says so and names the newest version of the branch that reads. No version is saved on top of the damaged one. Choose **Reset Branch to Last Readable Version** (in the **...** menu) to take the branch back to it: the song stays as it is, and its changes since that version show as usual. If the damaged version's pieces arrive later, **Verify History** and **Recover Lost Versions** bring it back.
- **Verify History** (in the **...** menu) reads every piece of the history and says what is damaged, and which versions no branch holds. **Recover Lost Versions** makes a branch of each of those.
