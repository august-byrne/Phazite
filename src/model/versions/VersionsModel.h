// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project's versions, as the Versions view, the activity bar, the status bar and the commands reach them
// (docs/design/versions.md): by plain values, versions and branches by their ids. What changes is told by the project context's
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
};

// A line of the changes: a track (a folder or a return too), the master, or the song's own.
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
};

// Whether the open project keeps versions (it has a project file), and whether they are still being read as it opens.
bool keepsVersions (const ProjectContext&);
bool isReadingVersions (const ProjectContext&);

// Waits until the versions being written are written (each on the history's thread, the initial version too), so that the
// project's folder can be copied whole, its versions with it: before a new project's first save moves it, and before Save
// As copies it.
void waitForVersionsWritten (const ProjectContext&);

// What the histories of projects that closed still write (a version or a snapshot given as each closed), each on its own
// thread, which a close never waits for: how many writes, the projects' names, and a wait for them all, however long it
// takes (as the program ends, once its window said so: app/ProjectSession.h, closeAndFinishThen()). Message thread.
int countVersionsBeingFinished();
juce::StringArray getProjectsBeingFinished();
void waitForVersionsFinished();

// Runs an action (a project's folder deleted: an unsaved one that closed) once no history that went writes into that folder
// or one inside it any more: at once if none does, else as the last write ends. Message thread.
void whenNoVersionsWriteInto (const juce::File& folder, std::function<void()> action);

// Every branch's versions, newest first; the branches, in the order they were made; the current one, which the project
// file is (none before the first version); and its last version.
std::vector<VersionInfo> getVersions (const ProjectContext&);
std::vector<BranchInfo> getBranches (const ProjectContext&);
std::optional<BranchInfo> getCurrentBranch (const ProjectContext&);
std::optional<VersionInfo> getLastVersion (const ProjectContext&);

// The song tied to its version (docs/design/versions.md: The song tied to its version). The version the song grew from (its
// base), which the changes are worked out against: the current branch's last version, unless the branch has newer versions
// the song didn't grow from (another computer saved them, or the project file came back from a backup); none before the first
// version.
std::optional<VersionInfo> getBaseVersion (const ProjectContext&);

// How many versions the branch has that the song didn't grow from (none while the song grew from its last); and as the
// Versions view says it, with what to do, none while there are none. Save Version is greyed meanwhile (canSaveVersion()), so
// that no version is saved over them.
int countNewerVersions (const ProjectContext&);
juce::String describeNewerVersions (const ProjectContext&);

// Take the Newer Versions (version.takeNewer): where the song has no changes of its own since its base, a plain update: the
// song made the branch's last version's, which it grows from then, as one undo step ("Take the Newer Versions"), read on the
// history's thread first. Where it has changes, Merge the Newer Versions: the branch's last version merged in by the merge
// that exists (startMerge(), its base the version the song grew from), whose version Save Version then saves, following both
// lines. What it is called now, and whether it can run.
bool takeNewerVersions (ProjectContext&);
bool canTakeNewerVersions (const ProjectContext&);
juce::String describeTakeNewer (const ProjectContext&);

// What couldn't be read of where the song's names said it stands (the version it grew from, damaged or let go of; a branch
// the Versions folder hasn't), as the Versions view says it once: the changes are worked out against the branch's last
// version then. None while there is nothing to say.
juce::String getBaseProblem (const ProjectContext&);

// Whether a version saved now may go on the branch: not while the branch has versions the song didn't grow from that only this
// computer holds, nor while the song's branch isn't in the folder (VersionHistory.h: canSaveOnBranch()).
bool canSaveOnBranch (const ProjectContext&);

// Why the last version given wasn't saved ("Nothing changed since the last version.", a folder that couldn't be written), as the
// Versions view says it, until the next version saved or the next edit; none while it was.
juce::String getSaveProblem (const ProjectContext&);

// What changed since the version the song grew from (docs/design/versions.md: Changes), a line a track, the master or the
// song's own; whether anything did; and how many lines (the activity bar's badge). Worked out on the history's thread from
// what changed in the song (docs/design/versions.md: Off the message thread): these read the last list worked out, and the
// listeners (ProjectContext::Listener::versionsChanged()) are told when a newer one is in. Whether the last list is the song's
// as it is now (nothing changed since its round, which is in; and the list against a version compared with the song, if
// one is), what a test waits for, which is never so while the history still reads the versions as its project opens (there
// is no list then: known bug 205, a test that read the list just after the project opened again found none); and whether
// the song may have changes, as far as is known now (some in the last list, or a
// change since it), which the commands that act on the song as it is (Discard All Changes, Save Version) are enabled by.
std::vector<VersionChange> getChangesSinceLastVersion (const ProjectContext&);
bool hasChangesSinceLastVersion (const ProjectContext&);
int countChanges (const ProjectContext&);
bool areChangesWorkedOut (const ProjectContext&);
bool mayHaveChangesSinceLastVersion (const ProjectContext&);

// Discard All Changes: the song made the last version's again, as one undo step.
bool discardAllChanges (ProjectContext&);

// The name and the note for the next version, as the Versions view holds them while they are typed.
void setVersionDraft (ProjectContext&, const juce::String& name, const juce::String& note);
juce::String getVersionDraftName (const ProjectContext&);
juce::String getVersionDraftNote (const ProjectContext&);

// Saves a version: the song as it is, with the draft's name and note (which it then clears), written in the background,
// and calls back once it is written, or not (why, then); false if it can't start (while one is being saved or the project's
// folder copied, while the versions are being read, or for a song that keeps none). Not an undo step: it changes the
// history, not the song. isSavingVersion() is true while either is under way.
bool saveVersion (ProjectContext&, std::function<void (juce::Result)> done = {});
bool isSavingVersion (const ProjectContext&);

// Whether a first save is copying the project's Versions folder as it moves (VersionHistory::copyFolder()): meanwhile nothing
// is saved, merged or switched, and the view says why ("Saving the project...").
bool isSavingProjectFolder (const ProjectContext&);

// Whether a version would record anything now (version.save is enabled while it would): changes, or a merge
// waiting for its version; as git, which has "nothing to commit" otherwise. And what it would be named while no message is
// typed, as the message field shows it: "Update Bass and Drums", "Merge idea-2"; none while there is nothing.
bool hasSomethingToCommit (const ProjectContext&);
juce::String describeNextCommit (const ProjectContext&);

// Rename Version... (version.editMessage, on the version the history selects): a version's name and note set anew, either
// may be empty (the summary of its changes names one without a name). Kept beside its record, which never changes, as
// Fossil amends a version: its id, its place in the history and its bounce stay. Not an undo step: it changes the history, not
// the song. False for a version the history doesn't hold.
bool editVersionMessage (ProjectContext&, const juce::String& versionID, const juce::String& name, const juce::String& note);
bool canEditSelectedMessage (const ProjectContext&);

// Undo Last Version (version.undoLast; as git reset --soft HEAD~): the current branch back at the version before its last,
// the song as it is, so that what that version recorded shows in the changes again, and its name and note back in the field
// while none is typed. Not for the first version or a merge's, nor while one is being saved or a merge is under way. Not an
// undo step: it changes the history, not the song. The version stays in the store for 30 days, while Recover Lost Versions
// brings it back.
bool canUndoLastCommit (const ProjectContext&);
bool undoLastCommit (ProjectContext&);

// How many bytes the project's versions take on disk.
juce::int64 getVersionsSize (const ProjectContext&);

//==============================================================================
// Going back (docs/design/versions.md: Going back): each a named undo step, which what plays follows as it follows an undo. What
// isn't counted (view state, how the song plays) stays as it is now, and the history keeps every version.
//
// The commands that read a whole song (Restore This Version, a line's Restore, Compare with the Song, Restore from
// Local History, Undo Last Version, Reset Branch to Last Readable Version, and the switches and the merge's list below) read it
// on the history's thread and change the song once it is in, the window held meanwhile (ProjectContext::holdForWork();
// known bug 126): true for one started (or done at once), false for one that can't start. One at a time.

// Restore This Version: the song made what it was then (the undo step "Restore "Verse 2""), as much of it as its pieces hold whole:
// what a damaged piece held stays as it is now (not there, for what the song hasn't now), and getRestoreProblem() says what.
// False for a version it can't read at all.
bool bringBackVersion (ProjectContext&, const juce::String& versionID);

// What the Versions view lists against the song: the changes since the last version, or, while Take from This Version...
// compares the song with another (compareWithVersion(); none for the last version again), what differs from that one. And
// one of its lines taken back from that version: Put Back from the last version ("Put Back Bass"), Take from another ("Take
// Bass from "Verse 2""). What it named that isn't there then is taken away: a sidechain or a modulation of what is gone.
bool compareWithVersion (ProjectContext&, const juce::String& versionID);
juce::String getComparedVersion (const ProjectContext&);
std::vector<VersionChange> getListedChanges (const ProjectContext&);
bool takeBack (ProjectContext&, const VersionChange&);

// What a version changed, from the version it follows (nothing, for the first), by line, as the history lists them under it
// (git show --stat), read in the background the first time it is asked for (none listed while it is:
// isReadingVersionChanges(); the versions tell their listeners once it is); none for a version that can't be read. And one
// of them restored from that version (git restore --source): the line made what it was then, its track there again or taken
// away as it was or wasn't then, one undo step ("Restore Bass from "Verse 2""). What it named that isn't there then is taken away: a sidechain or a modulation of what is gone. As
// much of the line as the version's pieces hold whole, as Restore This Version takes the song: what a damaged piece held stays as it is now,
// and getRestoreProblem() says what.
std::vector<VersionChange> getVersionChanges (const ProjectContext&, const juce::String& versionID);
bool isReadingVersionChanges (const ProjectContext&, const juce::String& versionID);
bool restoreFromVersion (ProjectContext&, const juce::String& versionID, const VersionChange&);

// A line of what a version changed, selected under it in the history (with the version; none for none), which
// version.restoreChange restores from that version: what it is called now ("Restore Bass from "Verse 2""), and whether it can.
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

//==============================================================================
// The app's save, which asks where a project goes the first time, and calls back with whether it saved: Save Version saves
// the project first, and a switch to another branch saves it after.
using SaveProjectThen = std::function<void (std::function<void (bool saved)>)>;

// Branches (docs/design/versions.md: Branches). Switching makes the song the other branch's in place, as it was left there: the
// song as it is now, its view state and unsaved changes included, is kept on the branch it leaves; what both branches have
// stays as it is (its plug-ins loaded, the song playing on); the undo history starts again, and the project is saved, so
// that its file is the branch's song, as the versions say. Not undo steps.

// Create Branch: a branch named "idea-2" (the next number free), in the next color, from a version, or from Now (none): from
// the current branch's last version, the song with its changes going on there. From a version, the song becomes that
// version's, as a switch to the branch makes it. False if it can't be made (before the first version, while one is being
// saved).
bool newBranch (ProjectContext&, const juce::String& fromVersion, const SaveProjectThen&);

// The branch picker's "Create new branch": a branch from Now, as newBranch() makes one, named as typed (toBranchName(); the
// next "idea-" name for an empty one or one another branch has). Its ID, or empty if none could be made: then nothing else
// changes, the branch the song is on keeping its name.
juce::String newBranchNamed (ProjectContext&, const juce::String& name);

// Started, it reads on the history's thread (above, Going back), and tells done once it is over whether it switched.
bool switchBranch (ProjectContext&, const juce::String& branchID, const SaveProjectThen&, std::function<void (bool switched)> done = {});

// Whether the song can switch to another branch now: not while it records (a take not finished would be lost), while a
// render has its plug-ins (an export, a freeze, a bounce), while a version is being saved or the versions are read.
bool canSwitchBranch (const ProjectContext&);

// Why a switch, Create Branch from a version (a switch to it), a merge, Undo Last Version, Reset Branch or a DAWproject's
// import can't be made: "Create Branch first" while the song's branch isn't in the folder (VersionHistory.h: missingBranch),
// whose way out is Create Branch from the song as it is. None while it can.
juce::String whyNotSwitchBranch (const ProjectContext&);
bool canMakeBranch (const ProjectContext&);

// A branch named anew, as toBranchName() makes the name: false for an empty one, or one another branch has (in any case).
// Not an undo step. And whether Rename Branch... can name the current one: not while a version is being saved.
bool renameBranch (ProjectContext&, const juce::String& branchID, const juce::String& name);
bool canRenameBranch (const ProjectContext&);

// Delete Branch... (version.deleteBranch, the branch picker's; git branch -D): a branch but the current one gone, and the
// song left on it. Its versions that no other branch holds leave the history, and they and the song left on it stay in the
// store for 30 days, while Recover Lost Versions brings them back. Whether one can be deleted (not the current branch, nor
// while a merge of its line is under way); and what deleting it would take with it, as the question before it says it ("3
// versions that no other branch has, and the changes left on it. Recover Lost Versions brings them back for 30 days."),
// none for nothing. Not an undo step.
bool canDeleteBranch (const ProjectContext&, const juce::String& branchID);
bool canDeleteAnyBranch (const ProjectContext&); // Delete Branch...'s: one of the branches can be
juce::String describeBranchDeletion (const ProjectContext&, const juce::String& branchID);
bool deleteBranch (ProjectContext&, const juce::String& branchID);

// Merge Branch... (version.mergeBranch, the branch picker's; git merge <branch>): another branch's last version merged into
// the current branch, its lines listed first (startMerge()). Whether it can be (it has versions the current branch's line
// doesn't), and how many of its versions the current branch doesn't have yet.
bool canMergeBranch (const ProjectContext&, const juce::String& branchID);
bool canMergeAnyBranch (const ProjectContext&); // Merge Branch...'s: one of the branches can be
int countCommitsToMerge (const ProjectContext&, const juce::String& branchID);
bool mergeBranch (ProjectContext&, const juce::String& branchID);

// A branch's name as typed, spaces and all: trimmed, each run of white space one space, without control characters or the
// format characters that change how text shows without showing themselves (zero-width ones, bidirectional marks), and at
// most maxBranchName characters (a branch is stored by its id, so its name needs nothing more to be safe). Empty if nothing
// is left. Names read from the store, which anyone may have written, are made so as they are read.
inline constexpr int maxBranchName = 100;
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
// Merging (docs/design/versions.md: Merging). A version of another line (another branch's, or one the current branch's past doesn't
// hold) into the song: its list shows what it changed since the newest version both have, by line, each with whether it
// clashes with what the song changed; Merge takes every line as one undo step ("Merge idea-2"). Then, until the
// next version is saved (which follows both lines), each clash switches between its three values, each switch an undo step.
// The merge waiting is part of those steps (and of Cancel Merge's): Undo of the merge leaves none waiting, Redo brings it
// back. It is part of the song (Schema.h: MERGING), so it waits still in whichever file of the song opens: the project file, its
// autosave, the song left on a branch.
struct MergeLine
{
    VersionChange change;
    bool clashes = false; // with what this side changed
};

bool canMerge (const ProjectContext&);                  // the version selected can be: not the current branch's last, nor in its past
juce::String describeMergeInto (const ProjectContext&); // version.merge's name: "Merge into Main..."
bool startMerge (ProjectContext&, const juce::String& versionID);
std::vector<MergeLine> getMergeLines (const ProjectContext&); // the merge's list's, while it shows
juce::String describeMerge (const ProjectContext&);           // "Half-time" (the version's name), while it shows or is merging
juce::StringArray getMergeNotes (const ProjectContext&);      // what the merge couldn't decide, once it is made

// Why the merge the song holds doesn't wait as it was taken (VersionHistory.h: getMergeProblem()), as the Source Control view
// says it: the branch moved on since, or holds the merged version already, or the merge can't be read. None while it does.
juce::String getMergeProblem (const ProjectContext&);

// Cancel Merge (version.cancelMerge; as git merge --abort): the merge's list closed, taking nothing; or, once it was taken
// and waits for its version, the song made what it was before it, one undo step ("Cancel Merge"), so that no version says the
// other line is merged in while the song doesn't hold it. Whether there is a merge to cancel.
void cancelMerge (ProjectContext&);
bool canCancelMerge (const ProjectContext&);
bool isMerging (const ProjectContext&); // made, until its version is saved

// Merge (version.finishMerge): every line of the merge's list taken, as one undo step. Enabled while the list shows, but not
// while a first save copies the project's Versions folder (finishMerge() refuses then, saying why).
bool finishMerge (ProjectContext&);
bool canFinishMerge (const ProjectContext&);

struct ClashInfo
{
    juce::String line, what;
    juce::String values[3]; // before both changed it, this branch's, the other's: as text
    juce::String otherBranch;
    int chosen = 1;
};

std::vector<ClashInfo> getClashes (const ProjectContext&);
bool chooseClash (ProjectContext&, int clash, int value); // 0 before both, 1 this branch's, 2 the other's

// A conflict's three choices: "Take What Was Before" (0), "Keep This Branch's" (1), "Take the Other Branch's" (2).
juce::String getClashChoiceName (int value);

//==============================================================================
// Bounces, and hearing them (docs/design/versions.md: Hearing a version). A version's bounce is the song as it was then, rendered as
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
// starts: how far it has come, while it renders; and why the last one failed, until the next. Only when asked: nothing bounces
// by itself.
bool canBounce (const ProjectContext&, const juce::String& versionID);
std::unique_ptr<TrackRender> startBounce (ProjectContext&, const juce::String& versionID,
                                          std::function<void (const juce::Result&)> onFinished);
bool startBackgroundBounce (ProjectContext&, const juce::String& versionID);
std::optional<float> getBounceProgress (const ProjectContext&, const juce::String& versionID); // from 0 to 1
juce::String getBounceProblem (const ProjectContext&);

//==============================================================================
// The automatic saves (docs/design/versions.md: Automatic saves; in the code and the format, Local History and its snapshots),
// as Figma's and Google Docs' histories keep them beside the versions that were named: the song as it was at times while it
// differed from its last version, taken without asking (as the project opens with changes, every few minutes while it
// changes, and before Restore This Version, Discard All Changes, a merge taken or an automatic save restored), kept in the
// project's Versions folder on this computer, newest first, thinned as they grow old. The history shows them between the
// versions (ui/versions/VersionsView.h). Each restored as one undo step, the song as it is taken first.
struct SnapshotInfo
{
    juce::int64 time = 0;  // when it was taken, which names it
    juce::String branch;   // the name of the branch the song was on, none for one deleted since
    juce::String reason;   // why it was taken, as the view says it: "Auto-saved", "Before Restore"
    juce::String branchID; // that branch, whose line the history shows it on: none for one deleted since
};

std::vector<SnapshotInfo> getSnapshots (const ProjectContext&);
bool restoreSnapshot (ProjectContext&, juce::int64 time); // "Restore the Automatic Save of 14:02"

// The snapshot the Versions view selects (0 for none), which version.restoreSnapshot restores: what it is called now, and
// whether it can. They are rows of one list: selecting a version (selectVersion(), selectVersionChange()) selects no snapshot.
void selectSnapshot (ProjectContext&, juce::int64 time);
juce::int64 getSelectedSnapshot (const ProjectContext&);
juce::String describeRestoreSnapshot (const ProjectContext&);
bool canRestoreSelectedSnapshot (const ProjectContext&);
void restoreSelectedSnapshot (ProjectContext&);

//==============================================================================
// Keeping the history whole (docs/archive/format-audit.md, R3 and R4).

// Verify History (version.verify, git's fsck): every piece read and checked, every version's song and every song kept
// without a version (Local History's, those left on branches) walked, every version's audio files checked against its record
// (there, of its size and its SHA-256), and the versions no branch holds found, in the background; whether it is under way;
// and what it found, as the Versions view says it ("Verifying the history... 40%"; "Verified 12 versions, ... : all whole";
// ""Verse 2" (Audio/Kick.wav missing)"), until the next check; none before the first.
bool verifyHistory (ProjectContext&);
bool canVerifyHistory (const ProjectContext&);
bool isVerifyingHistory (const ProjectContext&);
juce::String describeHistoryCheck (const ProjectContext&);

// While it runs (version.verify, then named "Stop Verifying History"), stopped between pieces and between audio files:
// "Verify History was stopped before it was done." And the command's three: the check, or its stop; its name now.
void stopVerifyingHistory (ProjectContext&);
void verifyOrStopVerifying (ProjectContext&);
juce::String describeVerify (const ProjectContext&);

// Recover Lost Versions... (version.recover, the branch picker's): what can come back, newest first (VersionHistory.h:
// getLostLines()): each line of versions that left every branch within the last 30 days, with when and why (undone, its
// branch deleted, a reset), a deleted branch's song left on it, and the lines Verify History found that no log names. One
// brought back as a branch at its last version, named after the branch it was on ("main (recovered)"), with the song left
// on it; its id, none if it couldn't be. Not an undo step: it changes the history, not the song. Whether there is any to
// bring back now (not while the history is read or a version saved).
struct LostVersionsInfo
{
    juce::String key;      // which line
    juce::String branch;   // the branch it was on, as it was called; none if no log names it
    juce::String why;      // "undo", "delete", "reset", "found" by Verify History, or another move's
    juce::Time time;       // when it left (for one Verify History found, when its last version was saved)
    int numVersions = 0;   // its versions no branch holds
    juce::String lastName; // what its last version is called
    bool withSong = false; // a deleted branch's song left on it comes back with it
};

std::vector<LostVersionsInfo> getLostVersions (const ProjectContext&);
bool canRecoverLostVersions (const ProjectContext&);
juce::String recoverLostVersions (ProjectContext&, const juce::String& key);

// A current branch whose last version can't be read (VersionHistory.h: getHeadProblem()), as the Versions view says it, with
// the newest version of the branch's log that reads, offered; none while it reads. Reset Branch to Last Readable Version
// (version.resetToReadable, git reset --soft to it): the branch moved to that version, the song as it is, its changes since
// that version. Whether it can be.
juce::String getHeadProblem (const ProjectContext&);
bool canResetToReadable (const ProjectContext&);
bool resetToReadable (ProjectContext&);

// What the last Restore This Version couldn't take from a damaged version, which stayed as it is, as the Versions view
// says it ("Bass: a clip"): none while it took everything, and why when it could take nothing.
juce::String getRestoreProblem (const ProjectContext&);

// Why the last command that read the versions first (a switch, a merge's list, a restore: above) did nothing,
// as the Versions view says it: a song that can't be read, the song recording, a render started, the branch moved on, the
// folder that can't be written to. None while it did, or was cancelled; until the next one starts.
juce::String getReadingProblem (const ProjectContext&);

// The current branch's lines that went their own way on another computer (docs/archive/format-audit.md, R8): their last
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
// turned down to the other's. An edit to the song switches back to it. False for a version without a bounce: Open Version
// plays one live. Nothing bounces by itself.
bool hearVersion (ProjectContext&, const juce::String& versionID);
juce::String getHeardVersion (const ProjectContext&); // none while the song itself is heard
juce::String describeHearing (const ProjectContext&); // "Hearing: Verse 2" while a version's bounce is heard, for the transport bar

//==============================================================================
// A version open (docs/design/versions.md: A version open): the song made that version's in place, live, with no bounce, to
// listen and look, and Now (the song as it was, its unsaved changes, undo and redo steps and what is selected included) back
// exactly at Back to Now.
//
// Open Version (version.open, a double-click in the history): the version's song read on the history's thread (once: kept for
// the next open of it), the window held meanwhile as a switch's read holds it, then made the song's with the fewest edits
// (ApplySong.h: makeSongLike()) in a trial that holds the song's history (model/commands/internal/Undo.h: Trial::
// holdHistory()), the song as it is kept in Local History first and copied, its plug-ins' settings stored. What both have stays
// as it is, the song plays on, and a plug-in the version has and Now lacks loads in the background as any added does. From one
// version open to another directly, one way back to Now still. The version Now is (the last, with no changes since) opens
// nothing, and while another is open goes Back to Now. True for one opened or being read.
//
// While one is open: its project's unsaved mark, the Changes list, the star and the title are Now's (the song isn't Now); Undo
// and Redo do nothing (greyed, "Back to Now first"); a freeze and Clean Up Unused Audio wait for Back to Now; and every save
// (Auto Save waits), export, Save Version, switch, merge, restore, bounce of the song and Local History's snapshot goes Back to
// Now first, so that nothing is written that says the song is the version's. What isn't an undo step (mute and solo, the loop,
// view state, a plug-in's settings changed in its window) may change meanwhile, and is put back as Now had it.
//
// An edit while one is open makes a branch there as its step ends with changes (an undo step, a gesture, Swap's or another
// trial kept; a press that changed nothing, a drag ended with Escape or a trial put back leave the version open, Now whole):
// as Create Branch from the version makes one (VersionHistory::branchFromShown()), the branch named "idea-3", the song
// growing from the version on it, Now with its unsaved changes kept on the branch it was on as a switch keeps the song it
// leaves, by its piece written on the history's thread as the version opened; the branch's undo history starts with the
// edit, which Undo takes back, nothing under it reachable; then the project is saved, and a notice says so with Back to
// "main" (takeVersionsNotice()). Recording makes the branch as it starts, before anything is recorded, and doesn't start
// where it can't be made, saying why. Where an edit's branch can't be made (the history busy, Now's piece not written yet,
// a record that can't be written), the version is kept as a restore on Now's branch instead ("Restore "Verse 2"", the
// edit after it, which Undo takes back), and the notice says why and how many Undos take it back.
bool openVersion (ProjectContext&, const juce::String& versionID, const SaveProjectThen& saveProjectThen = {});
bool canOpenVersion (const ProjectContext&, const juce::String& versionID);
juce::String whyNotOpenVersion (const ProjectContext&, const juce::String& versionID); // none while it can

// Back to Now: the version open put back; nothing while none is. Not while a render has the song (Bounce in Place,
// Consolidate), whose step lands in the version open as it ends: false then, the song still the version's, which whatever
// would write it refuses. Its command greyed with why (whyNotBackToNow(): also during a drag on the version, whose step ends
// first).
bool backToNow (ProjectContext&);
juce::String whyNotBackToNow (const ProjectContext&); // none while it can
bool canGoBackToNow (const ProjectContext&);
juce::String describeBackToNow (const ProjectContext&);   // its name as menus show it: "Back to Now", or with why not
juce::String getOpenVersion (const ProjectContext&);      // none while none is
juce::String describeOpenVersion (const ProjectContext&); // "Looking at "Verse 2" (3 days ago)", none while none is open

// The plug-ins of the version open that aren't installed here, by name, which play nothing (as in any project); and whether the
// version has a bounce to hear in their place. None while none is open, or all are.
juce::StringArray getOpenVersionMissingPlugins (const ProjectContext&);

// version.open on the version the history selects; its name as menus show it ("Open Version", or why it can't).
void openSelectedVersion (ProjectContext&, const SaveProjectThen& = {});
bool canOpenSelected (const ProjectContext&);
juce::String describeOpenSelected (const ProjectContext&);

// What the versions tell without asking (an edit that started a branch while a version was open), once each: the title, the
// text, and the branch its button goes back to ("Back to main"; none without one). None while there is nothing.
struct VersionsNotice
{
    juce::String title, text, actionName, branch;
};

std::optional<VersionsNotice> takeVersionsNotice (ProjectContext&);

// A/B (version.compare, N, the transport bar's A/B; docs/design/versions.md: Hearing a version), live: a press opens B
// (openVersion()), another goes back to Now, at the same beat, the song playing on. B is the version open, else the version
// opened last, else the version whose bounce was heard last, else the one the song grew from (the current branch's last
// version), so that the changes not saved in a version yet are heard against it. While a version's bounce is heard (Hear It),
// a press hears the song again. Nothing to compare while B is the song itself: the last version with no changes since. The
// loudness isn't matched live (the two never play at once); it is for a bounce heard.
juce::String getCompareVersion (const ProjectContext&); // B, none without a version
bool canCompare (const ProjectContext&);
void toggleCompare (ProjectContext&, const SaveProjectThen& = {});
juce::String getCompareProblem (const ProjectContext&); // why the last press couldn't open B, until the next press or an edit
juce::String describeCompare (const ProjectContext&);   // the A/B button's tooltip: what a press does now, or why it can't

// Export as DAWproject... (step D1; model/project/internal/DawProject.h): the version the Versions view selects, or the song
// as it is (Now), written as a DAWproject into the project's Renders folder ("<project> - <version>.dawproject"), in the host,
// which says where it went. What it is called now ("Export "Verse 2" as DAWproject..."), and whether it can be.
juce::String describeDawProjectExport (const ProjectContext&);
bool canExportDawProject (const ProjectContext&);
void exportDawProject (ProjectContext&, TrackRenderHost&);

// The commands on the version the Versions view selects (version.hear: Now while none is; version.bounce: the last while none
// is; version.deleteBounce), each render ending in the host, as Freeze Track's does. Hear It only on a version with a bounce
// (its name says why not otherwise: "No bounce: Open Version plays it live").
void hearSelectedVersion (ProjectContext&);
bool canHearSelected (const ProjectContext&);
juce::String describeHearSelected (const ProjectContext&);
void bounceSelectedVersion (ProjectContext&, TrackRenderHost&);
bool canBounceSelected (const ProjectContext&);
void deleteSelectedBounce (ProjectContext&);
bool canDeleteSelectedBounce (const ProjectContext&);

// Save Version (version.save): the project saved first (saveProjectThen), then a version recorded. Then its bounce, with a
// bounce (getBounceWithVersion()), in the host. Greyed while the history is read or a version is saved, while nothing has
// changed, and while the branch's last version can't be read and one that reads is offered (canResetToReadable()).
void saveProjectAndVersion (ProjectContext&, const SaveProjectThen&, TrackRenderHost&);
bool canSaveVersion (const ProjectContext&);

// The versions' commands: version.save, the commands on the version and the line the Versions view selects, those of
// branches and merges, and those of bounces, which render in the host.
void addVersionCommands (CommandRegistry&, ProjectContext&, SaveProjectThen, TrackRenderHost&);
void newBranchFromSelection (ProjectContext&, const SaveProjectThen&);
void switchToSelectedBranch (ProjectContext&, const SaveProjectThen&);
void mergeSelectedVersion (ProjectContext&);
} // namespace phazite
