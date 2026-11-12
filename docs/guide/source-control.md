---
title: Source control
description: Save versions of a song, go back to one, hear old mixes, try ideas on branches, and get back work you didn't commit.
sidebar:
  order: 2
---

Phazite keeps a history of each song inside its project folder, the way git keeps a history of code. Open it with the branch icon down the left of the window (**Source Control**). Everything here works without an account or a network.

## Save a version before you try something

Make a **commit** whenever the song is at a point you might want back: before a risky change, after a good session, or when you send a mix to someone.

1. Type a message, if you like: its first line names the commit, and the lines after it are a note. Left empty, the commit is named by what changed, such as "Update Bass and Drums", which the empty message box shows before you commit.
2. Click **Commit**, or press **Ctrl+Enter** in the message box, or **Ctrl+Alt+S** anywhere (**Ctrl+N** with FL Studio's shortcuts).

Commit is greyed out while nothing has changed since the last commit.

**Changes** lists what changed since the last commit, a line per track, each with a letter: **U** added, **M** changed, **D** deleted. Click a line to select its track. Its arrow puts that track back as it was, and the arrow on the **Changes** heading puts everything back. Either is one undo step, so Ctrl+Z brings the change back.

## See and hear what an old version was

The **Graph** lists every commit, newest first.

- **Click a commit** to open it: what it changed shows under it, a line per track. Click it again to fold it.
- **Check it out:** double-click a commit, or right-click it and choose **Checkout (Detached)**. The song becomes that commit, on no branch, so you can play it, solo its tracks and open its devices. Your branch keeps your work as it was. Click **Back to main** in the bar at the top of Source Control to return to it. Changes you make while checked out aren't committed. They go into Local History when you leave, or click **Create Branch Here** to keep them on a branch of their own.
- **A/B:** press **N**, or click **A/B** in the transport bar, to hear the last commit in place of the song, at the same point and just as loud. Press it again to hear the song. This is the quick way to hear a change before you commit it. The first time, Phazite bounces the commit in the background while the song plays on, and switches as soon as it's ready. Any edit switches back to the song.
- **Hear an older one:** a commit with a bounce has a play mark, which plays it in place of the song. Right-click a commit and choose **Bounce** to give it one. A/B then switches between the song and the commit you heard last.
- **Name it later:** right-click a commit and choose **Edit Commit Message...**, for example to mark the version you sent to the label.

## Go back

- **One track:** open the commit in the Graph, then click the arrow on the track's line. That track becomes what it was in that commit, and the rest of the song stays as it is.
- **The whole song:** right-click the commit and choose **Restore from This Commit**.

Both are one undo step, and the song as it was goes into Local History first. Then commit, to keep the song as it is now.

**Committed too early?** **Undo Last Commit** (in the **...** menu at the top of Source Control, under Commit, or on the last commit's menu) takes the commit back. The song stays as it is, and its message returns to the message box.

## Local History: work you didn't commit

While the song differs from its last commit, Phazite keeps snapshots of it without asking. It takes one:

- when the project opens;
- every 10 minutes while you work;
- just before anything that replaces the song, such as Restore, discarding every change, or leaving a commit you checked out.

Open **Local History** at the bottom of Source Control. Each row's arrow makes the song what it was then, as one undo step. Snapshots are kept for 30 days, fewer as they grow older.

## Try an idea on a branch

A **branch** is an alternate version of the song, such as a radio edit, a remix or a different chorus, that you can switch to and from.

- **Make one:** click the branch name at the top of Source Control, or in the status bar, and choose **Create new branch** (type a name, or leave it as "idea-2"). The song carries on there, changes included.
- **Switch:** pick another branch in the same list. Each branch keeps its own unfinished changes, so nothing is lost when you switch.
- **Bring an idea back:** **Merge Branch...** (in the **...** menu at the top of Source Control, under Branch) lists what the other branch changed, a line per track. Untick what you don't want, then click **Merge**. Where both branches changed the same thing, each conflict is listed with a choice of value, heard as you switch it. Commit to finish, or **Abort** to go back to how the song was.
- **Clean up:** **Delete Branch...** removes a branch you no longer need. If it holds commits no other branch has, Phazite asks first.
