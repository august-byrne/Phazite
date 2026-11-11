# Source control audit

*2026-11-11, the owner's request: "I want an audit on how our current source control works. I also want you to research
the most common ways people interact with source control as a concept. I then want us to polish up our implementation and
optimize for the most common use scenarios with our ux/ui, as well as make sure we are not missing any crucial functions of
source control." Section 5 says what was built from it, and section 6 what is left.*

The audit covers Phazite's source control as it stood on 2026-11-11: the Versions store, commits and branches, merging,
bounces and the Source Control view ([versions.md](versions.md)). Three research passes looked at how people use version
control: developers, measured where the data exists; producers and other creatives; and the version control systems
built to be simpler than git. Their sources are in section 7.

## 1. In short

- **Phazite's source control was strong where git is hard for a song.** It keeps a project's history in the project's
  folder, in pieces named by their content, so a commit costs what changed. It names changes per track in musical words,
  which producers ask for and no established DAW offers. Each branch keeps its own unfinished work, as the Gitless research
  recommends. Merges are three-way by ID and never stop on a conflict. Restoring is an undo step, and an old commit's bounce
  can be heard against the song, loudness-matched.
- **It was missing the operations people need most.** Undo was missing for the last commit and for a merge once taken. Nothing
  could delete a branch or rename a commit. Git users' most-viewed questions are about exactly these: undoing the most recent
  commit, deleting a branch, changing a commit's message.
- **Uncommitted work had no safety net that anyone could see.** Every creative tool studied keeps automatic versions beside
  the named ones (Figma, Google Docs, Logic's backups), and developers' biggest pain is losing work before it is committed.
  Phazite's Backups folder kept 10 copies of the project file, which only the recovery of an unreadable file used.
- **The history said little about each commit.** A commit given a name hid what it changed, and nothing listed a commit's own
  changes. Looking at history is the second most frequent thing developers do.
- **A merge could be recorded falsely.** After Merge was taken, Ctrl+Z took it back, but the merge kept waiting. The next
  commit then said the other branch was merged in, so its changes would be skipped by every later merge (section 3.2).
- **Built from this audit (section 5):**
  - Undo Last Commit, Edit Commit Message..., Delete Branch... and Merge Branch...;
  - Abort Merge after a merge is taken;
  - a commit opened in the graph to show what it changed, each line restorable from it;
  - Local History: automatic snapshots of uncommitted work, restorable as one undo step;
  - Commit only while there is something to commit, its field showing the name it will get.

## 2. How people use source control

### 2.1 Developers: what they do, and what they need help with

No vendor publishes per-action telemetry: GitHub Desktop counts events such as commits undone, partial commits and changes
taken to a new branch, but publishes no totals. The evidence is surveys of what people do, and Stack Overflow's views of what
they need help with.

**What they do (self-reported):**
- **Commit everything that changed.** In the 2012 Git User's Survey (4,474 answers), 75% often or always add and then
  commit, and 56% commit every changed file at once. Committing only parts of files is rare: 56% never do it [git-2012].
- **Look at history.** 61% of 217 developers look at history at least several times a day, and 85% call it important
  [codoban].
- **Stash** is the most used of the "advanced" features (69.7% of 6,307 in 2011), then interactive rebase (44%) [git-2011].
- **The command line dominates:** 82% use it, and 8% use only a GUI [gitbutler].

**What they need help with** (views of the most-voted git questions on Stack Overflow, read 2026-11-11):

| Question | Views |
|---|---|
| Undo the most recent local commits | 16.8M |
| Delete a branch | 13.6M |
| Revert the repository to a previous commit | 13.0M |
| Rename a local branch | 6.5M |
| Undo `git add` | 6.4M |
| Discard unstaged changes | 4.7M |
| Change a commit message | 4.4M |
| Resolve merge conflicts | 3.8M |
| Revert a file to a specific revision | 3.4M |
| Undo an unpushed merge | 3.5M |
| Fix a detached HEAD | 2.9M |
| Abort a merge | 2.8M |
| See the differences between two branches | 2.8M |

Grouped, undoing and going back are about a third of the views of the 40 top questions, and branches a quarter. A study of
80,370 git questions found that the commands with the most views per question are revert, reflog, stash, clean and reset,
"except for git clean, all related to recovery" [yang].

**What hurts:**
1. **Fear of losing work.** Interviews at Google and nine months of watching a team at Autodesk found narrow, ritual
   workflows, backup copies made by hand before git commands, and "fear of losing everything" [church].
2. **Uncommitted work is what gets lost.** "You cannot get back uncommitted changes in general", says the accepted answer
   to a question with 458k views; VS Code's Discard All Changes once deleted "3 months of work" [vscode-32405]. JetBrains'
   Local History rescues people after `git reset --hard`.
3. **Switching branches with unfinished work.** GitHub Desktop's team names two jobs: "I started work on the wrong branch",
   and "I was interrupted, and want to come back with my changes right where I left them" [desktop-6107].
4. **The staging area** offers "inessential functionality" for most users [jackson-2013]; "undo git add" has 6.4M views.
5. **Detached HEAD**: every one of the seven detached-HEAD questions studied came from users who got there by accident
   [jackson-2016].

**What the GUI clients put one click away:** VS Code: Ctrl+Enter commits, Undo Last Commit, the branch in the status bar,
and Local History in its Timeline [vscode-docs]. GitHub Desktop: an Undo button under the commit box, and "Leave my changes"
or "Bring my changes" when switching branches [desktop-docs]. GitKraken and Tower: Undo for almost every operation, which
reviewers praise most [gitkraken], [tower].

### 2.2 Producers and other creatives

No survey measures how producers version their work. The sources are DAW manuals, DAW makers' forums, two papers and
articles (section 7).

**What producers do,** ranked by how many of about 45 sources mention it:
1. **Save a copy before a risky change, or at a milestone**: "My Song 2.2 (pre drum comp)", a new version every hour or
   two [liveschool]. Interviews with creative professionals found the main benefit "psychological": a state saved made
   risks feel safe [sterman].
2. **Recover after an overwrite, a crash or a damaged file**: Ctrl+S instead of Save As over a set; a crash that cost a
   day [ableton-forum].
3. **Keep alternates**: mixes, a radio edit, a remix [musictech], [steinberg-radio].
4. **Find the right version among many**: users ask for audio previews because they can't tell projects apart
   [il-preview], [steinberg-preview].
5. **Compare by ear**: renders made after every few saves, to A/B [liveschool].
6. **Get back what a client heard**: "can we go back to the mix from three weeks ago?" [puremix].
7. **Clean up** copies and unused files.

**What DAWs offer:** Logic's alternatives and 10 backups each, restored by File > Revert to; Cubase's Save New Version and
.bak files, which users find confusing; Studio One's versions with descriptions, praised because a version "restores the
whole world"; Ableton Live's last 10 saves, whose automatic deletion "has screwed me multiple times"; FL Studio's Save new
version and autosave backups; REAPER's undo history saved with the project. Splice Studio, which kept every save as a
revision, closed in 2023.

**Outside music, the common core** of Figma, Google Docs, Notion, macOS and OneDrive:
- **Capture without asking**: Figma every 30 minutes, Notion every 10, macOS at least hourly.
- **One timeline**, autosaves grouped, named versions marked: Google's "Only show named versions"; Adobe deletes unmarked
  versions and keeps marked ones.
- **Look first, restore safely**: Figma's restore adds a checkpoint first; Google offers Make a copy.
- **No staging and no merging** for one person: Unity's Gluon is for artists who "don't want to merge or branch".

**What producers ask for that no established DAW has:** hearing an old version without opening it; a list of what changed
("Track 3, Pan updated from 20L to 20R"); notes on versions, branches, and keeping parts of both; a deep timed history
instead of ten saves; undo that survives closing.

### 2.3 Designs that set out to simplify git

| Design | What it simplified | How it went |
|---|---|---|
| Gitless | no staging area; each branch keeps its own unfinished work; no stash; no detached HEAD | In a study with 11 git users: more satisfying (5.09 vs 3.91) and less frustrating (2.91 vs 4.73), switching branches mid-task faster (p=0.003) [jackson-2016]. Stayed a research prototype. |
| Jujutsu (jj) | the working copy is a commit, snapshotted by most commands; an operation log with undo for every operation; conflicts kept in commits, never blocking; any message editable | 32k GitHub stars; Google funds its lead developer [jj]. |
| Sapling (Meta) | a "smartlog" of where you are; undo, uncommit, unamend, hide; no staging area | Ten years at Meta, tens of thousands of developers [sapling]. |
| Fossil | timeline; `amend` adds a correction record instead of rewriting a commit; no rebase | Built for SQLite [fossil]. |
| Perforce, Plastic (Gluon) | shelves, locks, one branch for artists | The games industry's standard for assets [perforce], [gluon]. |

The ideas lasted where they shipped inside tools people already used (jj on git repositories, Sapling at Meta); standalone
tools and services did not.

### 2.4 The functions of source control, and which matter here

| Function | Why it matters | Phazite before this |
|---|---|---|
| Commit with a message | the core act; "checkpoint, then dare" | yes, Ctrl+Alt+S, named by its changes when no message is typed |
| History | the second most frequent act | the graph |
| See what a commit changed | "what's in this version?" | no |
| Diff: now against the last commit, or any commit | review before committing, compare | Changes; Compare with Working Tree |
| Discard changes, all or one | back out of an experiment | yes, each an undo step |
| Restore an old version, whole or one part | the producers' "go back" | Restore from This Commit; one track through Compare with Working Tree |
| Undo the last commit | the most-viewed git question | no |
| Edit a commit's message | 4.4M views; Figma renames versions | no ("Name and Note…" planned) |
| Branches: create, switch, rename, delete | alternates | no delete |
| Merge, with conflicts | combine ideas | yes, never blocking |
| Abort a merge | 2.8M views | only before it was taken |
| Automatic snapshots | the safety net creatives rely on | not visible: Backups/ only |
| Hear an old version | producers' A/B | yes, with a bounce |
| Recovery and integrity | survive mistakes and damage | Verify History, Recover Lost Commits |
| Backup to another place | survive a lost disk | planned with the cloud |
| Stash, rebase, squash, cherry-pick, blame, bisect, locks | developers' and teams' tools | not needed for one person: each branch keeps its work, there is one author, and hearing replaces bisect |

## 3. Phazite before this (2026-11-11)

### 3.1 What works well, and stays

- **The store** (format audit, 2026-11-09): content-addressed pieces in packs, per-computer records, safe writes, Verify
  History and Recover Lost Commits. A commit that changed one note costs a few kilobytes.
- **Changes in Phazite's own words, per track**, with git's letters and VS Code's colors. Producers ask for this.
- **Each branch keeps its own work** and switches in place, as Gitless recommends; there is no detached HEAD.
- **Restore and Discard are undo steps**, so a mistaken click is one Ctrl+Z away.
- **Merges never block**: clashes keep their three values and are switched while the song plays.
- **Hearing a commit's bounce** against the song, loudness-matched, beat for beat: what producers ask for and no
  established DAW has.
- **Commit everything by default** (smart commit), with staging there only when it is used: right for the 56% who commit
  everything and the 56% who never pick hunks.

### 3.2 Findings

1. **Commit ran with nothing to commit**, making empty "No changes" commits that cluttered the graph. Git refuses ("nothing
   to commit, working tree clean").
2. **No Undo Last Commit.** VS Code has it in its Commit menu, and GitHub Desktop shows an Undo button after every commit.
3. **No way to name a commit after it is made.** "Name and Note…" was planned (versions.md) but not built. Producers name
   versions after the fact ("the one I sent to the label"), and the creative tools all allow it.
4. **A commit's own changes couldn't be seen.** The history showed each commit's name. A commit given a name of its own hid
   the summary of what it changed, and nothing listed a commit's changes; Compare with Working Tree compares a commit with
   the song now, not with the commit before it.
5. **No Delete Branch.** Branches made in a few clicks ("idea-2", "idea-3") could only pile up.
6. **Merging a branch took a right-click on its last commit in the graph.** VS Code's Merge Branch... picks a branch by
   name; Phazite had no such command, and its Branch menu's merge acted on whatever commit the graph had selected.
7. **A merge, once taken, could only be left by Ctrl+Z, and that left it waiting** (a bug). The view went on saying "commit
   to finish", and the next commit followed both lines. Every later merge of that branch then took its changes as merged
   already, and skipped them. Abort worked only while the merge's list showed.
8. **No visible safety net for uncommitted work.** The undo history ends with the session and starts again at a branch
   switch. Backups/ keeps 10 copies of the project file, at most one every 10 minutes, used only to open a damaged file.
9. **The graph's selection could drift.** It kept its row number when a commit was added at the top, so the highlighted row
   and the commit the commands acted on could differ.
10. **Commands in two categories.** Checkout to..., Rename Branch... and Bounce Each Commit were "Project" commands, the rest
   "Source Control".
11. **versions.md said VS Code commits everything by default when nothing is staged.** VS Code's smart commit ships off,
   and VS Code asks first [vscode-settings]. Phazite's choice to commit everything stays, on the survey's numbers; the text is
   corrected.

## 4. The plan

Each item answers a finding, and keeps to the owner's principles: one way per task, visible controls rather than hidden
keys, and git's words as VS Code says them (the owner, 2026-11-05).

- **Commit only with something to commit** (finding 1); its field shows what the commit will be named, as GitHub Desktop
  shows "Update README.md".
- **Undo Last Commit** (2), as VS Code's: `git reset --soft`, so the song stays, what the commit recorded is staged again,
  and its message comes back to the field. Not for the first commit, nor a merge's.
- **Edit Commit Message...** (3), on any commit. A commit is a piece named by its content and never changes, so the new
  message is kept beside it, as Fossil amends a commit, in a file each computer writes for itself.
- **Open a commit in the graph** (4): a click shows what it changed under it, as a tree's row opens, each line with its
  letter and a Restore from This Commit: "bring back this track from version X", which the research names the producers'
  everyday merge.
- **Delete Branch...** and **Merge Branch...** (5, 6) in the branch picker, as VS Code's quick picks. Deleting asks first only
  when commits no other branch has, or changes left on it, would go.
- **Abort Merge after a merge is taken** (7): the song made what it was before the merge, one undo step.
- **Local History** (8), named as VS Code's: snapshots taken without asking while the song differs from its last commit,
  listed under the graph and restored as one undo step. The two tiers the research found everywhere: automatic snapshots
  for recovery, named commits for the story.
- **Smaller fixes** (9-11): the graph's selection follows the commit, and every command is in Source Control.

## 5. What was built (2026-11-11)

### 5.1 Committing

- **Commit** (`version.save`) runs only while something is staged or changed, or a merge waits for its commit
  (`hasSomethingToCommit()`). The button is greyed until then.
- While nothing is typed, the message field shows the name the commit will get: "Update Bass and Drums (Ctrl+Enter to
  commit on 'main')" (`describeNextCommit()`); with nothing to commit, VS Code's "Message (Ctrl+Enter to commit on 'main')".
- **Undo Last Commit** (`version.undoCommit`), in the view's menu (... > Commit) and on the last commit's menu in the graph:
  - the branch moves back to the commit before;
  - what the commit recorded is staged again, and the song stays as it is;
  - the commit's message goes back into the field, unless one is being typed.
  - Its log line says `undo`. The commit stays in the store, so Verify History finds it and Recover Lost Commits brings it
    back. A plug-in whose settings differ between the two commits counts as changed in what is staged.
- **Edit Commit Message...** (`version.editMessage`) on a commit's menu opens a picker with its message. As in the commit
  field, the first line is the name and the lines after it the note, and Ctrl+Enter keeps it. An empty name leaves the
  commit named by what it changed. The message is kept in `Versions/messages/<computer>.xml`; the newest computer's word
  wins, and the commit's id, place and bounce stay.

### 5.2 The graph

- **A click on a commit opens it**, as a tree's row opens: under it, a row for each line it changed since the commit before
  it, with its icon, what changed, and git's letter (A, M, D). Another click folds it; one commit is open at a time; Enter
  opens the selected one. A chevron beside each commit's name shows which is open.
- **Each line restores from that commit**, by its arrow under the pointer or Restore from This Commit in its menu
  (`version.restoreChange`). That is one undo step: "Restore Bass from "Verse 2"".
- **A commit with a name of its own** says what it changed after its name and note, to screen readers and in its tooltip.
- **The commit's menu** is grouped as VS Code's: Hear It; Restore and Compare; Create Branch and Merge; Edit Commit
  Message... (and Undo Last Commit on the last one); Bounce and Delete Bounce; Export as DAWproject.
- The row selected follows its commit as commits are added.

### 5.3 Branches and merges

- **Merge Branch...** (`version.mergeBranch`, in the Branch menu) shows Source Control and opens a picker of the other
  branches. Each row says how many commits it would bring ("1 commit to merge; last Quieter, 5 min"), and a branch with
  nothing new is greyed. The one picked lists its lines to tick, as Merge into does.
- **Delete Branch...** (`version.deleteBranch`) opens a picker of the other branches; the current one can't be deleted, as
  in git.
  - When the branch has commits that no other branch has, or changes left on it uncommitted, a question comes first: "This
    also deletes 1 commit that no other branch has, and the changes left on it uncommitted. Recover Lost Commits brings the
    commits back."
  - Its commits leave the graph and stay in the store.
  - This computer's record of the branch says `deleted` with the time, so every computer leaves the branch out. Commits
    another computer made on it since bring it back.
- **Abort Merge** works while a merge waits for its commit too: the song becomes what it was before the merge, one undo
  step ("Abort Merge"), and nothing waits any more. Its button shows beside the conflicts.

### 5.4 Local History

- **What it keeps:** the song as it was, while it differs from its last commit.
- **When it takes one:**
  - as the project opens with changes since its last commit;
  - every 10 minutes while the song changes, never while it records;
  - before Restore from This Commit, Discard All Changes, a merge taken, and a snapshot restored.
- **What it costs:** a snapshot is only taken when the song differs from the one before it. Its pieces go in the store as
  a branch's working song does, so one costs what changed.
- **How it thins:** every snapshot of the last two hours, then the newest of each hour for a day, then the newest of each
  day for 30 days; none older. Time Machine and Kdenlive thin their backups the same way.
- **Where it shows:** a section under the graph, folded at first, its count on its badge. Each row has a clock, why it was
  taken ("Auto-saved", "Before Discard All Changes", "As opened"), the branch it was on, and when.
- **Restoring:** its arrow under the pointer, or Restore from Local History in its menu (`version.restoreSnapshot`), makes
  the song what it was, one undo step ("Restore Local History of 14:02"). The song as it is goes into Local History first.
- **Where it is kept:** `Versions/local/<computer>/history`, this computer's own.

### 5.5 Tests

- **VersionsTests:**
  - Commit only with something to commit, and the name it will get;
  - Undo Last Commit: the branch, what is staged, the draft, the reread history, the lost commit, never the first commit;
  - Edit Commit Message, kept as the project opens;
  - a commit's changes, and one line restored as one undo step;
  - Local History before Discard All and Restore, kept once for the same song, taken as the project opens with changes,
    restored as one undo step;
  - Local History's thinning over 40 days.
- **VersionsBranchTests:**
  - Delete Branch: what goes said first; gone after reopening; the commit recovered;
  - a branch deleted on one computer comes back where another committed on it since, or named it since;
  - Merge Branch, Abort Merge after Merge, and nothing to merge once it is merged.
- **VersionStoreTests:** messages from two computers, the newest each; Local History's file, a cut-off line left out.
- **ActivityBarTests** (the whole window):
  - Commit greyed with nothing to commit, and the field's text;
  - a commit opened by a click, its line restored by its arrow, folded again;
  - Merge Branch and Delete Branch in the picker, with the question;
  - Edit Commit Message in the picker;
  - Local History's row and its arrow.
- **The command goldens** list the six new commands.

## 6. What is left

- **One undo for every source control operation**, as jj's operation log and GitKraken's Undo have it. Operations that
  change the song are undo steps now; Undo Last Commit, Recover Lost Commits and the question before Delete Branch cover
  the rest. A log of operations to undo (a branch switch, a deletion) is the next step if mistakes show up.
- **A branch switch starts the undo history again.** The song is made the other branch's in place, and undo steps recorded
  on one branch can't apply to the other's song. Each branch keeps its own work, and Local History keeps what the song was.
- **Revert one old commit while keeping later ones** (`git revert`). Restoring a line from a commit covers the producers'
  case; a full revert can reuse the merge (the commit as the base, the commit before it as the other side).
- **Undo Last Commit on a merge's commit.** It would drop what the merge recorded of IDs given anew, so the next merge of
  that branch could double its items. Restore from the commit before the merge does the same safely.
- **Backup to another folder**, then the cloud ([versions.md](versions.md#later-the-cloud-on-top)): the store is built for it.
- **Showing only named commits**, as Google Docs does, once histories grow long. Named commits are drawn brighter than
  those named by their changes.
- **Changes marked in the playlist**, still planned in versions.md: a commit opened in the graph could mark what it changed
  on the song itself.
- **Exports tied to commits**: "the mix we sent on the 3rd" one click away, as the research suggests.
- **Git's words or plain ones** ([design.md](design.md#25-open-questions), question 25): the research on non-programmers
  favours plain verbs, and the owner chose git's words. Staging stays out of sight until it is used.

## 7. Sources

**Measured or documented:**
- [git-2012] Git User's Survey 2012: https://survs.com/results/QPESOB10/ME8UTHXM4M
- [git-2011] Git User's Survey 2011: https://www.survs.com/results/Q5CA9SKQ/P7DE07F0PL
- [codoban] Codoban et al., "Software history under the lens" (2015): https://www.cs.kent.edu/~jmaletic/cs63902/Papers/codoban15.pdf
- [gitbutler] GitButler, Git zeitgeist survey (2024): https://blog.gitbutler.com/git-zeitgeist
- [yang] Yang et al., TOSEM 2022: https://dl.acm.org/doi/10.1145/3494518
- Stack Overflow, the top `git` questions, read through the Stack Exchange API on 2026-11-11:
  https://api.stackexchange.com/2.3/questions?order=desc&sort=votes&tagged=git&site=stackoverflow
- [church] Church, Söderberg and Elango, "A case of computational thinking" (PPIG 2014): https://research.google.com/pubs/archive/42942.pdf
- [jackson-2013] Perez De Rosso and Jackson, "What's Wrong with Git?" (Onward! 2013): https://dspace.mit.edu/handle/1721.1/105233
- [jackson-2016] Perez De Rosso and Jackson, "Purposes, Concepts, Misfits, and a Redesign of Git" (OOPSLA 2016): https://spderosso.github.io/oopsla16.pdf
- [sterman] Sterman et al., "Towards Creative Version Control" (CSCW 2022): https://creativity.web.illinois.edu/assets/pdfs/towards_creative_version_control.pdf
- [vscode-32405] https://github.com/microsoft/vscode/issues/32405
- [desktop-6107] https://github.com/desktop/desktop/issues/6107
- [vscode-docs] https://code.visualstudio.com/docs/sourcecontrol/staging-commits, https://code.visualstudio.com/updates/v1_66
- [vscode-settings] https://github.com/microsoft/vscode/blob/main/extensions/git/package.json
- [desktop-docs] https://docs.github.com/en/desktop/managing-commits/undoing-a-commit-in-github-desktop,
  https://docs.github.com/en/desktop/making-changes-in-a-branch/managing-branches-in-github-desktop
- [gitkraken] https://help.gitkraken.com/gitkraken-client/undo-and-redo
- [tower] https://www.git-tower.com/features/undo
- [jj] https://docs.jj-vcs.dev/latest/operation-log/, https://github.com/jj-vcs/jj, https://lwn.net/Articles/958468
- [sapling] https://engineering.fb.com/2022/11/15/open-source/sapling-source-control-scalable/
- [fossil] https://fossil-scm.org/home/doc/trunk/www/fossil-v-git.wiki
- [perforce] https://stackoverflow.blog/2023/01/09/beyond-git-the-other-version-control-systems-developers-use
- [gluon] https://docs.unity.com/en-us/unity-version-control/gluon/gluon

**Creative tools:**
- [liveschool] https://liveschool.net/blog/workflow-tutorial-archive-ableton-projects/
- [ableton-forum] https://forum.ableton.com/viewtopic.php?p=1746629, https://forum.ableton.com/viewtopic.php?p=1814417
- [musictech] https://musictech.com/tutorials/logic-pro-x-project-alternatives/
- [steinberg-radio] https://forums.steinberg.net/t/need-to-make-radio-edit-of-song/661419
- [il-preview] https://forum.image-line.com/viewtopic.php?p=2026826
- [steinberg-preview] https://forums.steinberg.net/t/generate-a-small-mp3-file-of-the-project-a-sort-of-preview/990450
- [puremix] https://www.puremix.com/library/tutorials/explained/mix-versioning
- Logic Pro: https://help.apple.com/logicpro/mac/10.1/en.lproj/lgcpa158ef77.html
- Cubase: https://archive.steinberg.help/cubase_pro/v12/en/cubase_nuendo/topics/project_handling/project_handling_saving_project_files_as_new_version_t.html
- Ableton Live's backups: https://help.ableton.com/hc/en-us/articles/360000377870-Backup-Sets
- FL Studio: https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/menu_file.htm
- Figma: https://help.figma.com/hc/en-us/articles/360038006754
- Google Docs: https://support.google.com/docs/answer/190843
- Adobe: https://helpx.adobe.com/creative-cloud/help/versioning-faq.html
- Kdenlive's backups: https://wikisandbox.kde.org/Kdenlive/Manual/Projects_and_Files/Backup
- Asked for in DAWs: https://forum.ableton.com/viewtopic.php?p=1820916, https://forums.steinberg.net/t/feature-request-modern-version-handling/676271
