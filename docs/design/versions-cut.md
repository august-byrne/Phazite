# Versions, cut down to what producers use

*A design note (roadmap step S4.1), approved by the owner on 2026-11-17 with both of its recommendations. Steps S4.2
to S4.5 build it, and [versions.md](versions.md) is rewritten from it as they do.*

The owner's decision (2026-11-13): versions serve one person's song first, and "there is git-style functionality
that is really not needed for music makers and can be removed and the whole system can be simplified and optimized
to the specific workflows that producers would actually use". Collaboration comes later, "either live, or by merging
each person's work in from their own branches".

This note says what stays, what goes, what the words become, and what is built so that a history cannot lose work.
Each part ends with what it changes for the person using it.

## What a producer does with versions

Six things, and the feature is these six:

1. **Save a version** at a point worth keeping, with a name or without.
2. **See what changed** since the last version, by track.
3. **Go back**: the whole song, or one track, as it was in a version.
4. **Hear a version** against the song, at the same loudness.
5. **Try an idea** on a branch without touching the song as it stands, and bring it in if it works.
6. **Be saved from a mistake** without having asked: Local History.

## What stays as it is

Saving a version, the changes by track, the history's graph, restoring the song or one track from a version,
Discard Changes, comparing a version with the song, branches (create, switch, rename, delete), whole merges with
their conflicts, bounces, Hear It and A/B, Local History, Undo History, Undo Last Commit, renaming a version,
Verify History, Recover Lost Commits, and DAWproject export and import. The store and its format stay.

Merging stays (open question 51). It is built, it was fixed with the audit's priorities, and it is what the owner
named for collaboration. It becomes simpler in one way, below.

## What goes

### Staging

- **Gone:** the **Staged Changes** section, Stage Changes and Unstage Changes on a line, Stage All and Unstage All
  (`version.stage`, `version.unstage`, `version.stageAll`, `version.unstageAll`), the letter A as "staged", and the
  staged song in the model.
- **After it:** saving a version records the song as it is. Discard Changes makes a line what the last version has.
  Undo Last Commit moves the branch back one version and leaves the song alone, so what that version recorded
  shows in the changes again.
- **Who loses what:** someone who wanted a version of the drums alone while the bass was half done. They save both,
  or put the bass on a branch.

### A commit checked out on its own

- **Gone:** Checkout (Detached) (`version.checkout`), Back to Branch (`version.backToBranch`), the bar that offers
  Back to main and Create Branch Here, the "(detached)" state of the branch button and the status bar, and the
  record that reopens a project there.
- **After it:** an old version is looked at in the three ways that stay. Hear It plays it. Restore from it makes
  the song that version as one undo step, so Ctrl+Z comes back. Create Branch from it starts an idea there.
- **A double-click on a version** did the checkout. It becomes **Hear It**: it changes nothing, and it answers the
  question a producer has about an old version.
- **Who loses what:** nobody. The song was never on "no branch" for a reason a producer has.

### A merge's ticked lines

- **Gone:** the ticks. A merge takes every line the other branch changed.
- **After it:** Merge Branch... still shows the list first, as what will come in, with Merge and Cancel. One track
  from another branch is brought over as it already can be: open the branch's version, and restore that line.
- **Why:** a merge with lines unticked is recorded as a whole merge, so what was left out could never be merged
  later. Taking one track is a restore, not a merge, and the restore already exists.

### The letters

U (untracked), A (staged) and "Index Added" go. A line is **A** added, **M** modified, **D** deleted or **C** in
conflict, in the colours it has now.

## What stays, against the first plan: each computer's own records

The first plan parked them: each computer writes its own record of every branch, and from that follow a branch
whose lines went their own way on two computers shown to merge again, a deleted branch staying deleted everywhere,
and the newest computer's word winning for a renamed version.

This part stays as it is, and nothing more is built on it until collaboration is designed (the owner, 2026-11-17).

- A project folder kept in a synced folder and opened on a studio PC and a laptop is something producers do today.
  The per-computer files are what makes that safe: no file ever has two writers.
- It is built, tested, and its faults from the audit are fixed. Taking it out is a rewrite of the store and of
  [versions-format.md](../versions-format.md) with nothing for the user at the end of it.
- "A history that cannot lose work" needs it anyway: when two computers each saved a version on the same branch,
  one of the two must not drop out of sight.

It stays out of the guide as a feature: the guide says a project may live in a synced folder, used on one computer
at a time, and that work saved on two at once is offered to merge.

## The words

Until now the view spoke git, as VS Code's Source Control does (the owner, 2026-11-05). Now that the feature is for
producers, it takes plain words for what a producer touches, and keeps **branch** and **merge** (the owner,
2026-11-17).

- Every DAW that keeps versions says "version": Studio One's Versions, Cubase's and FL Studio's Save New Version,
  Logic's Alternatives. So do Figma and Google Docs ("version history").
- Git itself added `switch` and `restore` in 2019 because `checkout` confused its own users.
- "Commit", "working tree" and "checkout" mean nothing in a studio. "Branch" and "merge" are plain English that
  say what they do, and they are the owner's own words for collaboration.
- The folder, the model and the command ids already say `version`.

The layout stays VS Code's: the message field over the button, the sections that fold, the graph.

| Before | Now |
| --- | --- |
| Source Control (the view, the menu's title) | Versions |
| Commit; Commit and Bounce | Save Version; Save Version and Bounce |
| a commit | a version |
| "Message (Ctrl+Enter to commit on 'main')" | "Name this version (Ctrl+Enter to save it on 'main')" |
| Graph | History |
| Uncommitted Changes (the graph's top row) | Changes Since the Last Version |
| Restore from This Commit | Restore This Version (on a line: Restore from This Version) |
| Compare with Working Tree | Compare with the Song |
| Checkout to... | Switch Branch... |
| Undo Last Commit | Undo Last Version |
| Edit Commit Message... | Rename Version... |
| Bounce Each Commit | Bounce Each Version |
| Accept Current Change, Accept Incoming Change, Accept Base | Keep This Branch's, Take the Other Branch's, Take What Was Before |
| Abort Merge | Cancel Merge |
| "Merge branch 'idea-2'" (a merge's own name) | "Merge idea-2" |
| Recover Lost Commits; Reset Branch to Last Readable Commit | Recover Lost Versions; Reset Branch to Last Readable Version |
| Create Branch, Rename Branch..., Delete Branch..., Merge Branch..., Discard Changes, Verify History, Local History, Hear It, A/B | unchanged |

- Git's words stay as search words in the command palette, so "commit" and "checkout" still find the commands.
- Tooltips say what a command does in the song's terms; "git reset --soft" and the like go.
- A branch is named as typed. Spaces no longer become dashes.
- Command ids follow where they still say `commit` or `checkout`.

## A history that cannot lose work (S4.3)

After the cut, three commands take a version off every branch: Undo Last Version, Delete Branch, and Reset Branch
to Last Readable Version. Today such a version stays in the store for ever, because nothing is ever removed, and
only Recover Lost Versions finds it. Once the store is swept (below), that would no longer hold. So:

- **A version that leaves every branch is kept for 30 days**, the time Local History keeps. Each computer's log of
  a branch's moves already records when and why it left.
- **Recover Lost Versions lists them** with that: "Undone on 12 Nov", "On idea-3, deleted on 14 Nov". One click
  brings a line back as a branch.
- **Delete Branch still asks** when the branch holds versions no other branch has, or changes left on it, and its
  question says they can be recovered for 30 days.
- **The song left on a deleted branch** is kept the same 30 days.
- Nothing else a command does can end a version: restores, discards, merges and their cancelling each keep the
  song as it was in Local History first, as they do now.

Done when: no command leaves a version out of reach without asking, and a test deletes a branch, undoes a version,
lets 29 days pass on a clock it sets, and recovers both.

## Housekeeping (S4.4)

- **The store is swept.** When the packs are joined as a history opens, pieces that nothing names are left out:
  nothing on a branch, in a song left on a branch, in Local History, in a merge that waits, or in a version kept
  for its 30 days, on any computer. Today the store only grows: thinned snapshots, deleted branches and plug-ins
  that write other bytes at every save keep their room for ever.
- **Verify History checks what it now skips:** every version's audio files (there, of the recorded size, and of
  the recorded content), the Local History's songs and the songs left on branches. It says which version misses
  which file. It runs on the history's thread with progress and a Cancel, as now.
- **Clean Up Unused Audio** already keeps what Local History and recoverable versions play. It follows the same
  30 days.

## The song tied to its version (S4.5)

- **The project file says which branch it is on and which version the song grew from.** Today the changes are
  always worked out against the branch's newest version. When that moved without the song (another computer saved
  a version; a project file brought back from a backup), the other work shows as removals of yours, and one Save
  Version takes it out.
- **After it:** the changes are worked out against the version the song grew from. Where the branch has newer
  versions, the view says so and offers to merge them in, by the merge that exists.
- **Differences are worked out on the history's thread.** The badge on the activity bar and the changes list ask
  for them and are told when they are in; the message thread works out none. A benchmark on the 100-track song
  goes with it. Done when the view never stalls the window on a large song.

## What this does not touch

- Auto Save stays as it is (open question 52 is its own).
- Not built now, as before: the playlist marking what changed, a branch's colour by hand, one old version
  reverted alone, backup of the history to another folder, the cloud.
- Declined, as before: tags, stash, rebase, squash, cherry-pick, blame, bisect.

## What the owner decided (2026-11-17)

1. **The words:** plain ones, as in the table.
2. **Each computer's records:** left as they are.
3. The rest as written: a double-click hears a version, a version off every branch is kept 30 days, a merge takes
   every line.
