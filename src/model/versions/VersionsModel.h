// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project's versions, as the Versions view, the activity bar, the status bar and the commands reach them
// (docs/versions.md): by plain values, versions and branches by their ids. What changes is told by the project context's
// listeners (ProjectContext::Listener::versionsChanged()).
#pragma once

#include "model/base/IDs.h"
#include "model/project/ProjectContext.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;
class TrackRender;
class TrackRenderHost;

struct VersionInfo
{
    juce::String id;
    juce::String name; // the one it was given, or the summary of its changes
    juce::String note;
    juce::String summary; // what it changed, as its message says it while it has none of its own: "Update Bass and Drums"
    bool hasOwnName = false;
    juce::Time saved;
    juce::String branch;       // its branch's id
    juce::StringArray parents; // the versions it follows: none for the first, two for a merge
    bool hasBounce = false;    // the song as it was then, rendered to be heard
};

struct BranchInfo
{
    juce::String id, name;
    juce::Colour colour;
    juce::String head; // its last version's id; none before its first

    // The current one only: a commit checked out on its own (Checkout (Detached)), the song on no branch, named as the commit
    // is; its head that commit.
    bool detached = false;
};

// A line of the changes, as git lists a file's: a track (a folder or a return too), the master, or the song's own; in the
// staged changes, or in the working tree's.
struct VersionChange
{
    enum class Kind
    {
        added,
        removed,
        changed
    };

    Kind kind = Kind::changed;
    TrackID track; // the track's, or the master's; none for the song's own
    bool isMaster = false, isSong = false;
    juce::String name; // "Bass", "Master", "Song"
    juce::String what; // "volume, 12 notes"; empty for a track added or taken away
    bool staged = false;
};

// Whether the open project keeps versions (it has a project file), and whether they are still being read as it opens.
bool keepsVersions (const ProjectContext&);
bool isReadingVersions (const ProjectContext&);

// Waits until the versions being written are written (each on the history's thread, the initial commit too), so that the
// project's folder can be copied whole, its versions with it: before a new project's first save moves it, and before Save
// As copies it.
void waitForVersionsWritten (const ProjectContext&);

// Every branch's versions, newest first; the branches, in the order they were made; the current one, which the project
// file is (none before the first version); and its last version.
std::vector<VersionInfo> getVersions (const ProjectContext&);
std::vector<BranchInfo> getBranches (const ProjectContext&);
std::optional<BranchInfo> getCurrentBranch (const ProjectContext&);
std::optional<VersionInfo> getLastVersion (const ProjectContext&);

// What changed since the current branch's last commit, as git sees it (docs/versions.md: Staging): the changes in the
// working tree, from the staged song to the song, and whether anything changed since the commit, staged or not; the staged
// changes, what the next commit records; and how many things changed, staged or not, each once (the activity bar's badge).
std::vector<VersionChange> getChangesSinceLastVersion (const ProjectContext&);
bool hasChangesSinceLastVersion (const ProjectContext&);
std::vector<VersionChange> getStagedChanges (const ProjectContext&);
int countChanges (const ProjectContext&);

// Staging (git add, git restore --staged): a line, with its plug-ins as they are now, or every line, into the staged song,
// or out of it again. Not undo steps: they change the staged song, not the song. False for a line neither has.
bool stageChange (ProjectContext&, const VersionChange&);
bool unstageChange (ProjectContext&, const VersionChange&);
void stageAllChanges (ProjectContext&);
void unstageAllChanges (ProjectContext&);

// Discard All Changes (git restore): the song made the staged song again, as one undo step.
bool discardAllChanges (ProjectContext&);

// The name and the note for the next version, as the Versions view holds them while they are typed.
void setVersionDraft (ProjectContext&, const juce::String& name, const juce::String& note);
juce::String getVersionDraftName (const ProjectContext&);
juce::String getVersionDraftNote (const ProjectContext&);

// Commits: the staged changes, or every change while none is staged, with the draft's name and note (which it then
// clears), written in the background, and calls back once it is written, or not (why, then); false if it can't start (while
// one is being made, while the commits are being read, or for a song that keeps none). Not an undo step: it changes the
// history, not the song.
bool saveVersion (ProjectContext&, std::function<void (juce::Result)> done = {});
bool isSavingVersion (const ProjectContext&);

// Whether a commit would record anything now (version.save is enabled while it would): changes, staged or not, or a merge
// waiting for its commit; as git, which has "nothing to commit" otherwise. And what it would be named while no message is
// typed, as the message field shows it: "Update Bass and Drums", "Merge branch 'idea-2'"; none while there is nothing.
bool hasSomethingToCommit (const ProjectContext&);
juce::String describeNextCommit (const ProjectContext&);

// Edit Commit Message... (version.editMessage, on the commit the graph selects): a commit's name and note set anew, either
// may be empty (the summary of its changes names one without a name). Kept beside its record, which never changes, as
// Fossil amends a commit: its id, its place in the graph and its bounce stay. Not an undo step: it changes the history, not
// the song. False for a commit the history doesn't hold.
bool editVersionMessage (ProjectContext&, const juce::String& versionID, const juce::String& name, const juce::String& note);
bool canEditSelectedMessage (const ProjectContext&);

// Undo Last Commit (version.undoCommit; git reset --soft HEAD~, as VS Code's): the current branch back at the commit before
// its last, what that commit recorded staged again, the song as it is, and its message back in the field while none is
// typed. Not for the first commit or a merge's, nor while one is being made or a merge is under way. Not an undo step: it
// changes the history, not the song. The commit stays in the store: while no branch holds it, Verify History finds it and
// Recover Lost Commits brings it back.
bool canUndoLastCommit (const ProjectContext&);
bool undoLastCommit (ProjectContext&);

// How many bytes the project's versions take on disk.
juce::int64 getVersionsSize (const ProjectContext&);

//==============================================================================
// Going back (docs/versions.md: Going back): each a named undo step, which what plays follows as it follows an undo. What
// isn't counted (view state, how the song plays) stays as it is now, and the history keeps every version.

// Bring Back This Version: the song made what it was then ("Bring Back "Verse 2""). False for a version it can't read.
bool bringBackVersion (ProjectContext&, const juce::String& versionID);

// What the Versions view lists against the song: the changes since the last version, or, while Take from This Version...
// compares the song with another (compareWithVersion(); none for the last version again), what differs from that one. And
// one of its lines taken back from that version: Put Back from the last version ("Put Back Bass"), Take from another ("Take
// Bass from "Verse 2""). What it named that isn't there then is taken away: a sidechain or a modulation of what is gone.
void compareWithVersion (ProjectContext&, const juce::String& versionID);
juce::String getComparedVersion (const ProjectContext&);
std::vector<VersionChange> getListedChanges (const ProjectContext&);
bool takeBack (ProjectContext&, const VersionChange&);

// What a commit changed, from the commit it follows (nothing, for the first), by line, as the graph lists them under it
// (git show --stat); none for a commit that can't be read. And one of them restored from that commit (git restore --source):
// the line made what it was then, its track there again or taken away as it was or wasn't then, one undo step ("Restore
// Bass from "Verse 2""). What it named that isn't there then is taken away: a sidechain or a modulation of what is gone.
std::vector<VersionChange> getVersionChanges (const ProjectContext&, const juce::String& versionID);
bool restoreFromVersion (ProjectContext&, const juce::String& versionID, const VersionChange&);

// A line of what a commit changed, selected under it in the graph (with the commit; none for none), which
// version.restoreChange restores from that commit: what it is called now ("Restore Bass from "Verse 2""), and whether it can.
void selectVersionChange (ProjectContext&, const juce::String& versionID, std::optional<VersionChange>);
std::optional<VersionChange> getSelectedVersionChange (const ProjectContext&);
juce::String describeRestoreChange (const ProjectContext&);
bool canRestoreSelectedChange (const ProjectContext&);
void restoreSelectedChange (ProjectContext&);

// The Versions view's choices, which the commands act on: the version selected in its history, and the line of the changes
// selected (none for neither).
void selectVersion (ProjectContext&, const juce::String& versionID);
juce::String getSelectedVersion (const ProjectContext&);
void selectChange (ProjectContext&, std::optional<VersionChange>);
std::optional<VersionChange> getSelectedChange (const ProjectContext&);

// The commands on the selected version and the selected line (version.bringBack, version.takeFrom, version.putBack,
// version.showChanges), what they are called now, and whether they can run.
juce::String describeBringBack (const ProjectContext&);
juce::String describePutBack (const ProjectContext&);
bool canBringBack (const ProjectContext&);
bool canTakeFrom (const ProjectContext&);
bool canPutBack (const ProjectContext&);
void takeFromSelectedVersion (ProjectContext&);
void bringBackSelectedVersion (ProjectContext&);
void putBackSelectedChange (ProjectContext&);

// The staging commands on the selected line, and on every line (version.stage, version.unstage, version.stageAll,
// version.unstageAll), and whether they can run: a line of the working tree's is staged, a staged one unstaged.
bool canStageSelected (const ProjectContext&);
bool canUnstageSelected (const ProjectContext&);
void stageSelectedChange (ProjectContext&);
void unstageSelectedChange (ProjectContext&);
bool canStageAll (const ProjectContext&);
bool canUnstageAll (const ProjectContext&);

//==============================================================================
// The app's save, which asks where a project goes the first time, and calls back with whether it saved: Save Version saves
// the project first, and a switch to another branch saves it after.
using SaveProjectThen = std::function<void (std::function<void (bool saved)>)>;

// Branches (docs/versions.md: Branches). Switching makes the song the other branch's in place, as it was left there: the
// song as it is now, its view state and unsaved changes included, is kept on the branch it leaves; what both branches have
// stays as it is (its plug-ins loaded, the song playing on); the undo history starts again, and the project is saved, so
// that its file is the branch's song, as the versions say. Not undo steps.

// Create Branch: a branch named "idea-2" (the next number free), in the next color, from a commit, or from Now (none): from
// the current branch's last commit, the song with its changes going on there. From a commit, the song becomes that
// commit's, a checkout. False if it can't be made (before the first commit, while one is being made).
bool newBranch (ProjectContext&, const juce::String& fromVersion, const SaveProjectThen&);
bool switchBranch (ProjectContext&, const juce::String& branchID, const SaveProjectThen&);

// Whether the song can switch to another branch now: not while it records (a take not finished would be lost), while a
// render has its plug-ins (an export, a freeze, a bounce), while a version is being saved or the versions are read.
bool canSwitchBranch (const ProjectContext&);
bool canMakeBranch (const ProjectContext&);

// Checkout (Detached) (version.checkout, a commit's in the graph, which a double-click runs too; git switch --detach, as VS
// Code's graph names it; docs/versions.md: Checking out a commit): the song made a commit's in place, as a switch to a branch
// makes it, to see and hear it whole as it was then; the branch's song kept on the branch as it is, to come back to. Meanwhile
// the song is on no branch (getCurrentBranch() is detached): it plays and can change, but nothing is committed or merged;
// Create Branch makes a branch here that takes what changed, and going back keeps what changed in Local History first. Back to
// the branch (version.backToBranch, "Back to main") makes the song the branch's again, as it was left; checking out another
// branch from the picker goes there alike. The project is saved after each, so that its file is the song, as after a switch.
// Not undo steps: the undo history starts again, as a switch's does.
bool canCheckout (const ProjectContext&, const juce::String& versionID);
bool checkoutVersion (ProjectContext&, const juce::String& versionID, const SaveProjectThen&);
bool isDetached (const ProjectContext&);
bool canGoBackToBranch (const ProjectContext&);
bool backToBranch (ProjectContext&, const SaveProjectThen&);
juce::String describeBackToBranch (const ProjectContext&); // "Back to main"
void checkoutSelectedVersion (ProjectContext&, const SaveProjectThen&);
bool canCheckoutSelected (const ProjectContext&);

// A branch named anew, as toBranchName() makes the name: false for an empty one, or one another branch has (in any case).
// Not an undo step. And whether Rename Branch... can name the current one: the song is on a branch, not a commit checked out.
bool renameBranch (ProjectContext&, const juce::String& branchID, const juce::String& name);
bool canRenameBranch (const ProjectContext&);

// Delete Branch... (version.deleteBranch, the branch picker's; git branch -D): a branch but the current one gone, and the
// song left on it. Its commits that no other branch holds leave the graph and stay in the store, where Verify History finds
// them and Recover Lost Commits brings them back; the song left on it doesn't come back. Whether one can be deleted (not
// the current branch, nor while a merge of its line is under way); and what deleting it would take with it, as the
// question before it says it ("Its 3 commits that no other branch has, and its uncommitted changes"), none for nothing.
// Not an undo step.
bool canDeleteBranch (const ProjectContext&, const juce::String& branchID);
bool canDeleteAnyBranch (const ProjectContext&); // Delete Branch...'s: one of the branches can be
juce::String describeBranchDeletion (const ProjectContext&, const juce::String& branchID);
bool deleteBranch (ProjectContext&, const juce::String& branchID);

// Merge Branch... (version.mergeBranch, the branch picker's; git merge <branch>): another branch's last commit merged into
// the current branch, its lines listed to tick (startMerge()). Whether it can be (it has commits the current branch's line
// doesn't), and how many of its commits the current branch doesn't have yet.
bool canMergeBranch (const ProjectContext&, const juce::String& branchID);
bool canMergeAnyBranch (const ProjectContext&); // Merge Branch...'s: one of the branches can be
int countCommitsToMerge (const ProjectContext&, const juce::String& branchID);
bool mergeBranch (ProjectContext&, const juce::String& branchID);

// A name as git takes it for a branch: spaces as dashes (as VS Code puts them), without what a ref can't hold (control
// characters, ~ ^ : ? * [ \, "..", "@{", a leading dash or dot, a trailing dot, slash or ".lock"). Empty if nothing is left.
juce::String toBranchName (const juce::String& typed);

// This computer, as projects' histories know it (docs/versions-format.md: each computer writes only files of its own): an
// ID the app makes once and keeps in its settings (services/Settings.h), and sets as it starts, before any project opens.
// Until then (tests, and songs outside the app), one made for the process. Never a name: nothing of the user or the
// computer goes into a project. Message thread.
void setThisComputer (const juce::String& id);
juce::String getThisComputer();

// The branch the branch picker picked (ui/versions/BranchPicker.h), which version.switchBranch switches to.
void selectBranch (ProjectContext&, const juce::String& branchID);

//==============================================================================
// Merging (docs/versions.md: Merging). A version of another line (another branch's, or one the current branch's past doesn't
// hold) into the song: its window lists what it changed since the newest version both have, by line, each with whether it
// clashes with what the song changed; Merge takes the lines ticked as one undo step ("Merge "Half-time""). Then, until the
// next version is saved (which follows both lines), each clash switches between its three values, each switch an undo step.
struct MergeLine
{
    VersionChange change;
    bool clashes = false; // with what this side changed
    bool ticked = true;   // to be taken: each to start with
};

bool canMerge (const ProjectContext&);                  // the version selected can be: not the current branch's last, nor in its past
juce::String describeMergeInto (const ProjectContext&); // version.merge's name: "Merge into Main..."
bool startMerge (ProjectContext&, const juce::String& versionID);
std::vector<MergeLine> getMergeLines (const ProjectContext&); // the merge window's, while it shows
juce::String describeMerge (const ProjectContext&);           // "Half-time" (the version's name), while it shows or is merging
juce::StringArray getMergeNotes (const ProjectContext&);      // what the merge couldn't decide, once it is made

// Abort Merge (version.cancelMerge; git merge --abort): the merge's list closed, taking nothing; or, once it was taken and
// waits for its commit, the song made what it was before it, one undo step ("Abort Merge"), so that no commit says the other
// line is merged in while the song doesn't hold it. Whether there is a merge to abort.
void cancelMerge (ProjectContext&);
bool canCancelMerge (const ProjectContext&);
bool finishMerge (ProjectContext&, const std::vector<VersionChange>& lines);
bool isMerging (const ProjectContext&); // made, until its version is saved

// The merge window's ticks, and its Merge with the lines ticked (version.finishMerge).
void tickMergeLine (ProjectContext&, int line, bool ticked);
void finishTickedMerge (ProjectContext&);

struct ClashInfo
{
    juce::String line, what;
    juce::String values[3]; // before both changed it, this branch's, the other's: as text
    juce::String otherBranch;
    int chosen = 1;
};

std::vector<ClashInfo> getClashes (const ProjectContext&);
bool chooseClash (ProjectContext&, int clash, int value); // 0 before both, 1 this branch's, 2 the other's

// A conflict's three choices as a merge editor names them: "Accept Base" (0), "Accept Current Change" (1), "Accept Incoming
// Change" (2).
juce::String getClashChoiceName (int value);

//==============================================================================
// Bounces, and hearing them (docs/versions.md: Hearing a version). A version's bounce is the song as it was then, rendered as
// Export Song renders it (model/project/Export.h: the whole song with its tails, muted tracks silent; playback stops for it),
// into the project's versions, while a progress window shows it (TrackRender, model/mixer/FreezeModel.h). Not undo steps:
// they change the history, not the song.

// The room the versions take on disk: the songs' (their pieces and the branches) and the bounces', apart.
struct VersionsRoom
{
    juce::int64 songs = 0, bounces = 0;
};

VersionsRoom getVersionsRoom (const ProjectContext&);

// Bounce: a version that has none yet, while nothing else renders the song or records. The version the song is now (the
// current branch's last version, with no changes since) is the open song rendered, playback stopped for it, in a progress
// window: started, or nothing (with no call back) if it can't be. An older one is its song alone, rendered in the
// background with plug-in instances of its own while the song plays on (step V5), one at a time, which stops if recording
// starts: how far it has come, while it renders; and why the last one failed, until the next.
bool canBounce (const ProjectContext&, const juce::String& versionID);
std::unique_ptr<TrackRender> startBounce (ProjectContext&, const juce::String& versionID,
                                          std::function<void (const juce::Result&)> onFinished);
bool startBackgroundBounce (ProjectContext&, const juce::String& versionID);
std::optional<float> getBounceProgress (const ProjectContext&, const juce::String& versionID); // from 0 to 1
juce::String getBounceProblem (const ProjectContext&);

//==============================================================================
// Local History (docs/versions.md: Local History), as VS Code's and JetBrains' keep a file's saves: the song as it was at
// times while it differed from its last commit, taken without asking (as the project opens with changes, every few minutes
// while it changes, and before Restore from This Commit, Discard All Changes, a merge taken or a snapshot restored), kept in
// the project's Versions folder on this computer, newest first, thinned as they grow old. Each restored as one undo step,
// the song as it is taken first.
struct SnapshotInfo
{
    juce::int64 time = 0; // when it was taken, which names it
    juce::String branch;  // the name of the branch the song was on, none for one deleted since
    juce::String reason;  // why it was taken, as the view says it: "Auto-saved", "Before Restore"
};

std::vector<SnapshotInfo> getSnapshots (const ProjectContext&);
bool restoreSnapshot (ProjectContext&, juce::int64 time); // "Restore Local History of 14:02"

// The snapshot the Versions view selects (0 for none), which version.restoreSnapshot restores: what it is called now, and
// whether it can.
void selectSnapshot (ProjectContext&, juce::int64 time);
juce::int64 getSelectedSnapshot (const ProjectContext&);
juce::String describeRestoreSnapshot (const ProjectContext&);
bool canRestoreSelectedSnapshot (const ProjectContext&);
void restoreSelectedSnapshot (ProjectContext&);

//==============================================================================
// Keeping the history whole (docs/format-audit.md, R3 and R4).

// Verify History (version.verify, git's fsck): every piece read and checked, every commit's song walked, and the commits no
// branch holds found, in the background; whether it is under way; and what it found, as the Versions view says it ("12
// commits and 340 pieces, all whole"), until the next check; none before the first.
bool verifyHistory (ProjectContext&);
bool canVerifyHistory (const ProjectContext&);
bool isVerifyingHistory (const ProjectContext&);
juce::String describeHistoryCheck (const ProjectContext&);

// Recover Lost Commits (version.recover): each line of the commits the last check found that no branch holds made a branch
// of its own, named after the branch it was made on ("main-recovered"); how many the check found, and how many branches it
// made.
int countLostVersions (const ProjectContext&);
int recoverLostVersions (ProjectContext&);

// What the last Restore from This Commit couldn't take from a damaged commit, which stayed as it is, as the Versions view
// says it ("Bass: a clip"): none while it took everything, and why when it could take nothing.
juce::String getRestoreProblem (const ProjectContext&);

// The current branch's lines that went their own way on another computer (docs/format-audit.md, R8): their last
// versions, which Merge into the current branch joins again; and as the Versions view says it. None while the computers
// agree.
juce::StringArray getDivergentVersions (const ProjectContext&);
juce::String describeDivergence (const ProjectContext&);

// The one or the other: in a host, which runs the song's and is told how it ended, or why a bounce couldn't start.
void bounceVersion (ProjectContext&, const juce::String& versionID, TrackRenderHost&);

// Delete Bounce: its file goes, and the room it took with it; one rendering in the background stops.
bool deleteBounce (ProjectContext&, const juce::String& versionID);

// "With a bounce": whether Save Version bounces the version it saves, as the Versions view's box says.
void setBounceWithVersion (ProjectContext&, bool);
bool getBounceWithVersion (const ProjectContext&);

// Hear It: a version's bounce heard in place of the song from the engine's next block, at the same beat, the song playing
// from its playhead if it was stopped; none, the song itself (Now). Each switch matches the two's loudness so far, the louder
// turned down to the other's. An edit to the song switches back to it. False for a version without a bounce.
bool hearVersion (ProjectContext&, const juce::String& versionID);
juce::String getHeardVersion (const ProjectContext&); // none while the song itself is heard
juce::String describeHearing (const ProjectContext&); // "Hearing: Verse 2" while a version is heard, for the transport bar

// A/B (version.compare, the transport bar's A/B; docs/versions.md: Hearing a version), as a reference plug-in switches a mix
// and its earlier version: a press hears B, a commit's bounce, in place of the song, A, at the same point and as loud; another,
// the song again. B is the commit heard last (Hear It), else the one the song comes from: the current branch's last commit,
// or the one checked out, so that the changes not committed yet are heard against it. A commit without a bounce is bounced
// in the background first (startBackgroundBounce()), the song playing on, and heard as soon as it is; a press meanwhile, or
// an edit, stops waiting for it. Nothing to compare while B is the song itself: the last commit with no changes since.
juce::String getCompareVersion (const ProjectContext&); // B, none without a commit
bool canCompare (const ProjectContext&);
void toggleCompare (ProjectContext&);
bool isWaitingToCompare (const ProjectContext&);        // B is being bounced to be heard
juce::String getCompareProblem (const ProjectContext&); // why that bounce failed, until the next press or an edit
juce::String describeCompare (const ProjectContext&);   // the A/B button's tooltip: what a press does now, or why it can't

// Export as DAWproject... (step D1; model/project/internal/DawProject.h): the version the Versions view selects, or the song
// as it is (Now), written as a DAWproject into the project's Renders folder ("<project> - <version>.dawproject"), in the host,
// which says where it went. What it is called now ("Export "Verse 2" as DAWproject..."), and whether it can be.
juce::String describeDawProjectExport (const ProjectContext&);
bool canExportDawProject (const ProjectContext&);
void exportDawProject (ProjectContext&, TrackRenderHost&);

// The commands on the version the Versions view selects (version.hear: Now while none is; version.bounce: the last while none
// is; version.deleteBounce), each render ending in the host, as Freeze Track's does.
void hearSelectedVersion (ProjectContext&);
bool canHearSelected (const ProjectContext&);
void bounceSelectedVersion (ProjectContext&, TrackRenderHost&);
bool canBounceSelected (const ProjectContext&);
void deleteSelectedBounce (ProjectContext&);
bool canDeleteSelectedBounce (const ProjectContext&);

// Save Version (version.save): the project saved first (saveProjectThen), then a version recorded. Then its bounce, with a
// bounce (getBounceWithVersion()), in the host.
void saveProjectAndVersion (ProjectContext&, const SaveProjectThen&, TrackRenderHost&);
bool canSaveVersion (const ProjectContext&);

// The versions' commands: version.save, the commands on the version and the line the Versions view selects, those of
// branches and merges, and those of bounces, which render in the host.
void addVersionCommands (CommandRegistry&, ProjectContext&, SaveProjectThen, TrackRenderHost&);
void newBranchFromSelection (ProjectContext&, const SaveProjectThen&);
void switchToSelectedBranch (ProjectContext&, const SaveProjectThen&);
void mergeSelectedVersion (ProjectContext&);
} // namespace phazite
