// SPDX-License-Identifier: AGPL-3.0-or-later
// The open song's versions (docs/design/versions.md), as the model holds them while its project is open: what its Versions folder
// holds (VersionStore.h), read as the project opens; the version its branch is at, and the changes since then, worked out
// as they are asked for, from what changed since they were last; and versions saved, written on a thread of its own. A
// project context makes one for each song it opens (model/project/internal/ProjectContext.h): the versions of the project
// whose file the song has, in the folder beside it. Message thread, but for its own thread.
#pragma once

#include "SongDiff.h"
#include "SongMerge.h"
#include "StoreSweep.h"
#include "VersionStore.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/Song.h"
#include "model/project/internal/Project.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace phazite
{
class ProjectContext;
class TrackRender;
struct MergeState;
struct VersionsAudio;

// What a look for the audio the versions play found (VersionHistory::makeAudioPathsFinder()): the files' paths; what it
// couldn't read whole (a commit's record whose pack can't be opened, a song another computer kept whose pieces haven't all
// arrived), each said, which leaves what they play unknown; the key of what named them as it began (findVersionsAudioKey());
// and whether it was told to stop before it was done.
struct VersionsAudioFound
{
    juce::StringArray paths, unreadable;
    juce::String key;
    bool stopped = false;
};

// The key of what names the versions' audio in a store: its packs' names, its kept songs and its branches' last versions,
// read without waiting for a join under way. Any thread.
juce::String findVersionsAudioKey (VersionStore&);

class VersionHistory : private juce::ValueTree::Listener,
                       private juce::Timer
{
public:
    // onChange is told, on the message thread, when the versions, the branches or the changes since the last version may
    // have changed, at most a few times a second.
    // It goes once what it was given to write (a commit, a snapshot) is written, waiting for it, so that a project closed at
    // once loses none of them and opens again with them; what it was given to read is let go.
    VersionHistory (Song&, std::function<void()> onChange);
    ~VersionHistory() override;

    // Whether the song keeps versions: it is a project's, which has a file. And whether the history's thread is reading or
    // writing, or what it read isn't taken in yet.
    bool keepsVersions() const;
    bool isBusy() const noexcept { return pendingJobs > 0; }
    bool isReading() const noexcept { return ! hasRead; }

    // Waits until the history's thread has done what it was given (a version written, the store read), so that the Versions
    // folder can be copied whole and nothing is written into the old one after it: before a new project's first save moves
    // it, and before Save As copies it. A sweep under way and Verify History stop first, rather than be waited for. What the
    // thread did still reaches the message thread later. Message thread.
    void waitForThread() const;

    // A first save's copy of the history's folder as the project moves (Project::moveTo(), VersionsCopier): run as a job of
    // the history's own thread, which is serial, so that it copies after every write given before it; from now until
    // endFolderCopy() the history writes nothing (isSaving() is true: a commit or a branch's change is refused, and what it
    // starts itself, the initial commit and Local History's snapshots, waits), so that nothing is written into the folder the
    // project leaves after it was copied: from the copy job's start, once what a history of the same folder that went before
    // still writes is done (FinishingHistories), the folder is held (WholeFile.h: holdFolder()), which every write into the
    // store checks. A write: it runs even on the thread the history hands on as it goes. Calls back on the message thread
    // with what came of it; when the history goes first, a failure, and nothing more is copied for the project that went. A bounce stopped by the copy is made again after it (bounceAgain,
    // given the version). endFolderCopy() after the move, moved or not: the folder let go of, the history follows the
    // project's folder, and what waited goes on there at the next turn of the message loop, unless the history goes first,
    // as the project closes. Message thread.
    void copyFolder (std::function<juce::Result (const BackgroundJobs::ShouldStop&)> copy, std::function<void (const juce::Result&)> copied,
                     std::function<void (const juce::String&)> bounceAgain = {});
    void endFolderCopy();
    bool isCopyingFolder() const noexcept { return copyingFolder; }

    // What writes into the store from the message thread when it may: at once, or once a first save's copy of the folder is over
    // (endFolderCopy(), at the next turn of the message loop), in the folder the project is in then; never if the history goes
    // first. Taking back what a command that read made, when it didn't finish (a branch it made, deleted again).
    void whenNotCopying (std::function<void()>);

    // The branches, in the order they were made; the current one (none before the first version); every branch's versions,
    // newest first; one by its id; and the current branch's last.
    std::vector<BranchRecord> getBranches() const;
    const BranchRecord* findCurrentBranch() const;
    std::vector<const VersionRecord*> getVersions() const;
    const VersionRecord* findVersion (const juce::String& id) const;
    const VersionRecord* findLastVersion() const;

    // The changes since the current branch's last version: from its song to the song as it is, none before the first version,
    // nor while the history is being read. And whether there are any.
    const std::vector<SongChange>& getChanges();
    bool hasChanges();

    // Saves a version: the song as it is now, its name and note as given (each may be empty), and calls back on the message
    // thread once it is written, or not: the song is copied here, then split and written on the history's thread. One at a
    // time, and not while the history is being read: false then, with done told why. The first, made as soon as the history
    // has read that it has none, is "First version", on main.
    bool saveVersion (const juce::String& name, const juce::String& note, std::function<void (juce::Result)> done = {});
    bool isSaving() const noexcept { return saving || copyingFolder; } // a commit, or a first save's copy (copyFolder())

    // What the next version would be named while it has no name of its own: what changed ("Update Bass and Drums"), or the
    // merge it finishes ("Merge idea-2").
    juce::String summariseNextCommit();

    // A version's song, read from its pieces, with its plug-ins' parameters' values if asked: invalid if one can't be read,
    // or, given somewhere to list what is missing, as much as can be read (SongPieces.h: joinSong()). And the current
    // branch's last version's song, which the changes are from (invalid before the first version).
    juce::ValueTree readSong (const juce::String& versionID, PluginParameterValues* parameters = nullptr,
                              std::vector<MissingPiece>* missing = nullptr) const;

    // A version's song's own piece alone, without its tracks, its master or its automation, which are pieces of their own
    // (stand-ins in their places): its tempo map, its sections and its chord lane, cheaply. Invalid if it can't be read.
    juce::ValueTree readSongOwn (const juce::String& versionID) const;

    // A version's song read on the history's thread for what goes on in the background meanwhile (an old commit's bounce, a
    // commit's export as DAWproject; known bug 126), told on the message thread: invalid for one that can't be read. Not told
    // if the history goes first.
    void readCommitInBackground (const juce::String& versionID, std::function<void (const juce::ValueTree&)> then);

    // What the last Restore from This Commit couldn't take, as the commit's pieces that couldn't be read left it: none
    // while it took everything; or why it took nothing (a line whose place in the commit can't be read). Told until the
    // next one.
    std::vector<MissingPiece> restoreMissing;
    juce::String restoreRefused;

    // Verify History (git's fsck): every piece of every pack read and checked against its name, every version's song walked
    // piece by piece, and so every song kept without a version (Local History's, those left on branches: VersionStore::
    // findKeptSongs()); every version's audio files checked against its record (there, of the size and the SHA-256 it
    // recorded), each file read once however many versions name it; and the versions found in the packs that no branch's
    // line holds. On the history's thread, how far it has come (from 0 to 1: the audio by its bytes, which take the time)
    // told meanwhile, and cancelCheck() stopping it between pieces and between files; done is told on the message thread,
    // and the report kept for the Versions view until the next check.
    struct AudioProblem
    {
        enum class Kind
        {
            missing,      // no file at its path
            otherSize,    // a file of another size
            otherContent, // of its size, but not what it held (changed in place)
            unreadable    // there, but it can't be read (another program holds it, a disk error)
        };

        juce::String path;
        Kind kind = Kind::missing;
    };

    struct DamagedSong
    {
        KeptSong song;
        std::vector<MissingPiece> missing; // what of it can't be read; its own piece alone, where none of it can
    };

    struct Check
    {
        int numPieces = 0, numVersions = 0, numSongs = 0, numAudioFiles = 0;
        juce::StringArray damagedPieces;                             // in a pack, but not what their names say
        std::map<juce::String, std::vector<MissingPiece>> damaged;   // by version: what of its song can't be read
        std::map<juce::String, std::vector<AudioProblem>> audio;     // by version: its audio files that aren't as it recorded
        std::vector<DamagedSong> damagedSongs;                       // songs kept without a version that can't be read whole
        std::map<juce::String, juce::StringArray> unreadableParents; // by version: the versions it follows that can't be read
        juce::StringArray unreadPacks;   // the pack files that don't read whole as packs (PieceStore::findDamagedPacks())
        std::vector<VersionRecord> lost; // versions no branch's line holds, newest first
        bool stopped = false;            // cancelled before it was done: none of it is sure
    };

    bool checkHistory (std::function<void()> done = {});
    bool isChecking() const noexcept { return checking; }
    float getCheckProgress() const noexcept { return checkProgress->load(); }
    void cancelCheck() { checkCancelled->store (true); }
    const std::optional<Check>& getLastCheck() const noexcept { return lastCheck; }

    // Why the store's sweep as the history opened left nothing out (StoreSweep.h: a file or a piece it needs can't be read),
    // which the view says where Verify History says what it found; none while it was sure.
    const juce::String& getSweepProblem() const noexcept { return sweepProblem; }

    // Every version the history shows (and those Recover Lost Versions offers), the merge waiting in the song and the songs
    // the branches name as left on them, made this computer's own (StoreSweep.h: adoptVersions()), on the history's thread,
    // a batch a job: as the history opens and after each command that starts naming one (a save on top, a merge put into the
    // song, a branch made, a switch, a recovery, a reset, an undo). Only while the sweep lets go (StoreSweep.h:
    // doesSweepLetGo()): with nothing deleted there is nothing to protect. Stops with the sweep (stopHousekeeping()). What
    // couldn't be copied, said where the sweep's problem is (none while all was); and, for tests, while set, the copy as the
    // history opens left out, so that a command's own is seen.
    void adoptLater();
    const juce::String& getAdoptionProblem() const noexcept { return adoptionProblem; }
    static std::atomic<bool> noAdoptionAtOpenForTests;

    // What Recover Lost Versions lists (docs/design/versions.md: Keeping the history whole): each line of versions that left
    // every branch within the last 30 days (keptByAge::dailyFor), by every computer's logs (Undo Last Version, Delete Branch,
    // Reset Branch to Last Readable Version), with when and why; a deleted branch's song left on it, which comes back with it,
    // even where every version of it is on another branch; and the lines the last check found that no log names (a record
    // damaged, another computer's whose log hasn't arrived), as found by the check. Newest first.
    struct LostLine
    {
        juce::String key;           // what names it among the lines: its last version and its song
        juce::String head;          // its last version
        juce::String branch;        // the branch it was on: its name then, none if no log names it
        juce::String why;           // what took it off: "undo", "delete", "reset", another move's; "found" by the check
        juce::int64 time = 0;       // when, milliseconds since 1970 (for one the check found, its last version's)
        juce::String song;          // the song left on the deleted branch, none for none
        juce::StringArray versions; // its versions no branch holds, newest first
        juce::String name;          // what its last version is called (getDisplayName(), as its message was edited)
    };

    std::vector<LostLine> getLostLines() const;

    // A line brought back as a branch at its last version, named after the branch it was on ("main (recovered)", or
    // "recovered"), with the song left on it as the branch's (so that switching to it gives the song as it was left), its
    // versions in the history again: the branch's id, or none if it can't be.
    juce::String recoverLine (const juce::String& key);
    const juce::ValueTree& getLastVersionSong() const noexcept { return baseline; }

    // A current branch whose last commit can't be read as the history opened (a piece of it damaged or missing): said, with
    // the newest commit of the branch's log on this computer that reads (VersionStore.h: readBranchLog()), offered; none
    // while it reads. No commit is made on top of it meanwhile, unless the log has none that reads: a commit then starts the
    // branch's line again. And the branch reset to that commit (git reset --soft), the song as it is, given that commit's
    // song read on the history's thread: false if it can't be. The commit offered.
    juce::String getHeadProblem() const;
    bool canResetToReadable() const;
    bool resetToReadable (juce::ValueTree then, PluginParameterValues parameters);
    juce::String getReadableCommit() const;

    // The Versions view's choices, which the commands act on: the version selected in its history, and the line of the
    // changes selected; and the version the changes are listed against while Take from This Version… shows one (none: the
    // last version). What differs from that one, as the changes are listed (from it to the song now), and its song.
    juce::String selectedVersion;
    std::optional<SongChange> selectedChange;
    std::optional<SongChange> selectedVersionChange; // a line of what the selected version changed, under it in the graph
    // Another version's song, read on the history's thread, with its plug-ins' values (none for the last version: the
    // changes' own); invalid for one that couldn't be read, which compares nothing.
    void compareWith (const juce::String& versionID, juce::ValueTree read = {}, PluginParameterValues readParameters = {});
    const juce::String& getComparedVersion() const noexcept { return comparedVersion; }
    const std::vector<SongChange>& getListedChanges();

    // What a line of the changes is taken back from: the last version's song (Discard Changes), or the version the changes
    // are listed against.
    const juce::ValueTree& getComparedSong() const noexcept { return comparedVersion.isEmpty() ? baseline : comparedSong; }

    // How many bytes the history takes on disk.
    juce::int64 getSize() const;

    // A finder of the audio files the versions still play (model/project/UnusedAudio.h), for another thread (a background
    // job's), which looks until shouldStop says it should stop: their paths as the songs hold them (findAudioFilePaths()),
    // each once, of every version in the store's packs, those no branch holds too (Recover Lost Versions brings them back
    // until the sweep lets them go: StoreSweep.h); of every computer's songs left on branches, in Local History (as thinned)
    // and left on branches deleted within 30 days; and of the songs before the merges those and the
    // song hold, which Abort Merge brings back; with what of them can't be read whole (VersionsAudioFound). What each piece
    // read whole names is kept, as a piece never changes, so that only what came since is read again. None for a song
    // that keeps no versions. And what names them, cheaply (findVersionsAudioKey(), message thread): another key than a
    // look's says that something came since it. And whether a commit or a snapshot is being written meanwhile, which names
    // files the store doesn't have yet.
    using AudioPathsFinder = std::function<VersionsAudioFound (const std::function<bool()>& shouldStop)>;
    AudioPathsFinder makeAudioPathsFinder() const;
    juce::String findVersionsAudioKey() const;
    bool isWriting() const; // its own, and a history's of the same folder that went before it (countVersionsBeingFinished())

    //==============================================================================
    // Bounces (docs/design/versions.md: Bounces and hearing them): a version's file, whether it has one or not, and whether it has
    // one; how many bytes they take, of the history's size; the bounces folder read again, after one was made or deleted (it
    // is read as the versions are); and whether Save Version bounces the version it saves, as the Versions view's box says.
    juce::File getBounceFile (const juce::String& versionID) const;
    bool hasBounce (const juce::String& versionID) const { return bounces.count (versionID) > 0; }
    juce::int64 getBouncesSize() const noexcept { return bouncesSize; }
    void readBounces();
    bool bounceWithVersion = false;
    std::set<juce::String> bouncesGoing; // deleted, whose files go once the engine lets go of them: none to read

    // An old version's bounce, its song rendered in the background beside the song playing (step V5), one at a time, which
    // stops as recording starts: the version, and the render; and why the last one failed, until the next starts.
    juce::String bouncingVersion;
    std::unique_ptr<TrackRender> backgroundBounce;
    juce::String bounceProblem;

    // Hearing a version: the version whose bounce plays beside the song in the song's session, compared with it (none while
    // none does), and whether it is heard rather than the song. An edit to the song switches back to the song, so that what
    // is heard is what is shown.
    juce::String bounceCompared;
    bool hearingBounce = false;

    // A/B's B waiting for its bounce, rendering in the background, to be heard as soon as it is: none while none waits. An
    // edit stops waiting. And why the bounce it waited for failed, until the next press or edit.
    juce::String hearWhenBounced, compareProblem;

    //==============================================================================
    // Branches (docs/design/versions.md: Branches). One made from a version, or from the current branch's last version (none), named
    // "idea-2" and on, in the next color; and made the current one, the song going on there, if asked: its id, or none if it
    // can't be made. And a branch named anew, as typed (model/versions/VersionsModel.h: toBranchName()), never as another is.
    juce::String makeBranch (const juce::String& fromVersion, bool makeCurrent);

    // A branch from a version, whose song is another (a DAWproject's that came back: model/project/internal/DawProject.h),
    // kept there as the song left on it (its piece, which the store holds already), to switch to: its id, or none.
    juce::String makeBranchWithSong (const juce::String& fromVersion, const juce::String& songPiece);

    // Switching to another branch in place (prepareSwitch(), below): the song as it is now, view state and unsaved changes
    // included, kept on the branch it leaves; then the other's song, as it was left there (or as its last version had it),
    // made the song's with the fewest edits (ApplySong.h: makeSongTheSame()), so that what both have stays as it is: its
    // plug-ins loaded, its audio read, the song playing on. Its changes since its last version from then on. A merge waiting
    // for its version stays with its branch.
    bool renameBranch (const juce::String& branchID, const juce::String& name);
    juce::String selectedBranch; // the one the Versions view's branch menu picked, which version.switchBranch switches to

    // Delete Branch (git branch -D): a branch but the current one, and the song left on it, gone from the history; its
    // versions that no other branch holds, and the song left on it, stay in the store for 30 days, which this computer's log
    // says with the branch's name, and Recover Lost Versions brings the branch back with them meanwhile. Its record says so
    // (Schema.h: branchDeleted), so that every computer leaves it out, until versions made on it elsewhere since bring it back.
    // Not while a merge of its line is under way. A branch a command made and takes away again as it didn't finish
    // (takenBack: Create Branch from a version, an import of a DAWproject, cancelled) leaves nothing to recover. And what
    // deleting one would take with it: how many versions no other branch holds, and whether the song left there had changes
    // since its last version.
    bool deleteBranch (const juce::String& branchID, bool takenBack = false);
    int countVersionsOnlyOn (const juce::String& branchID) const;
    enum class LeftSong
    {
        same,
        changed,
        unreadable // the song left there, or its last commit's, can't be read: it may hold anything
    };

    std::optional<LeftSong> hasChangesLeftOn (const juce::String& branchID); // none while it is being found out

    // Undo Last Version (as git reset --soft HEAD~): the current branch moved back to the version before its last, the song
    // as it is, so that what that version recorded shows in the changes again, and its name and note the draft again while
    // none is typed. Not for the first version or a merge's, nor while a version is being saved or a merge is under way. The
    // version stays in the store, so Recover Lost Versions brings it back while no branch holds it. Given the song of the
    // version before, read on the history's thread (startReading(): VersionsModel.cpp), with its plug-ins' values.
    bool canUndoLastCommit() const;
    bool undoLastCommit (juce::ValueTree before, PluginParameterValues beforeParameters);

    // What a commit changed: from the commit it follows (its first parent; nothing, for the first) to it, by line, worked
    // out once each (a commit never changes), its two songs read and compared on the history's thread the first time it is
    // asked for: empty while they are (isReadingVersionChanges(); onChange is told once they are), and for one that can't
    // be read.
    const std::vector<SongChange>& getVersionChanges (const juce::String& versionID);
    bool isReadingVersionChanges (const juce::String& versionID) const;

    // Edit Commit Message...: a commit's name and note set anew (either may be empty; the summary of its changes names one
    // without a name), kept beside its record, which never changes (VersionStore.h: writeMessage()), on every computer that
    // has the folder. False for a commit the history doesn't hold.
    bool editMessage (const juce::String& versionID, const juce::String& name, const juce::String& note);

    //==============================================================================
    // Local History (docs/design/versions.md: Local History): the song as it was at times while it differed from its last commit,
    // kept without commits in this computer's history in the store (VersionStore.h: Snapshot), newest first. Taken as the
    // project opens with changes since its last commit, every snapshotMinutes while the song changes, and, with a reason,
    // before what makes the song another (Restore from This Commit, Discard All Changes, a merge taken, a snapshot
    // restored); written on the history's thread, and only when the song differs from the snapshot before it. Thinned as it
    // grows (thinSnapshots()). A snapshot's song, by its time: invalid for none.
    static constexpr int snapshotMinutes = 10;
    const std::vector<Snapshot>& getSnapshots() const noexcept { return snapshots; }
    void takeSnapshot (const juce::String& reason);
    juce::ValueTree readSnapshot (juce::int64 time) const;
    juce::int64 selectedSnapshot = 0; // the one the Versions view selects, which version.restoreSnapshot restores

    // Where two versions' lines parted (findMergeBase()); none for two of no common line.
    juce::String findCommonVersion (const juce::String& a, const juce::String& b) const;

    // How many versions one version's line holds that another's doesn't (git rev-list --count other..version).
    int countVersionsNotIn (const juce::String& version, const juce::String& other) const;

    // The IDs earlier merges into this line gave anew to what the other line added (findRenumbering(), below).
    std::vector<RenumberedID> findRenumbering (const juce::String& ours, const juce::String& theirs, const juce::String& parted) const;

    // A version as a merge's own name names it ("Merge idea-2"): another branch's last version by the branch's name, else by
    // its own in quotes ("\"Verse 2\""), or its id's first seven characters for one the history doesn't hold.
    juce::String describeCommit (const juce::String& versionID) const;

    // A merge, from its window (preview) until the version that finishes it is saved (merging): the version merged, the
    // song it parted from and the other's, the other's changes by line, and the clashes; then which value of each the song
    // has (0, the one before either side changed it; 1, this branch's; 2, the other's) and the notes.
    struct Merge
    {
        juce::String version, otherBranch;
        juce::ValueTree base, theirs;
        PluginParameterValues theirParameters; // the other's plug-ins' knobs, which name a plug-in's clash
        std::vector<RenumberedID> translation; // what base and theirs were translated through (SongMerge.h: findTranslation())
        std::vector<RenumberedID> renumbered;  // what it gave anew, which its version keeps
        std::vector<SongChange> lines;         // what the other line changed, which the merge takes whole
        std::vector<MergeClash> clashes;
        std::vector<int> chosen;
        juce::StringArray notes;
        juce::ValueTree before; // the song as it was before the merge was taken, which Abort Merge makes it again
        juce::String beforePiece;
        juce::int64 beforeChanges = -1; // the song's count of changes (countSongChanges()) as before was copied, for its piece
        juce::String head;              // the current branch's last version as the merge was taken

        // Whether its three songs were read whole, so that its clashes are known (none while they are being read on the
        // history's thread, or where one can't be).
        bool readWhole = false, reading = false;
    };

    // The merge's list's merge, while it shows, which Merge takes whole.
    std::optional<Merge> preview;

    // The merge taken into the song that waits for its commit: the one the song's MERGING element says (Schema.h), until
    // a commit of this branch's line finishes it (one that merged its commit, on top of where it was taken, and recorded the
    // IDs it gave: findOwnCommit()), and while the branch's line doesn't hold its commit otherwise. Where the branch holds
    // it through another computer's merge of it that gave other IDs, it waits still, so that its commit records this
    // song's IDs beside that merge's (the IDs that merge gave and this one didn't, as they are here). Its clashes and notes
    // are found again from the same three songs on the history's thread where the history hasn't them already (a merge
    // taken here, undone and redone, or read as the project opened, has them): it waits meanwhile, being read. None when
    // there is none, or when it can't be read. The element goes with the song: into its file and autosave, onto the branch
    // it is left on, and in and out with Undo and Redo of the merge. Never reads a song on the message thread.
    const std::optional<Merge>& getMerging() const;

    // Why the merge the song holds doesn't wait as it was taken, for the Source Control view: it couldn't be taken (the
    // store couldn't keep the song before it); the branch moved on since, so that its commit follows the newest commit; the
    // branch holds the merged commit already, merged on another computer (so that it waits no more, or with that merge's
    // other IDs, still); it is being read; or it can't be read, so that Abort Merge takes it out. None while it waits as
    // taken.
    const juce::String& getMergeProblem() const;
    juce::String mergeFailure; // why the last Merge took nothing, until the next one, or Abort Merge

    // Whether the song holds a merge's element (Schema.h: MERGING), waiting or not.
    bool holdsMergeElement() const;

    // Whether deleting a branch would take from the graph the commit a merge waiting (here, or in a song left on another
    // branch) merged in: its line holds that commit, and no other branch's does.
    bool holdsCommitMergeWaits (const BranchRecord&) const;

    // The merge's element for a song (Schema.h: MERGING), with the song before it kept as a piece in the store first, as
    // a merge is taken: the history keeps the merge, so that the element added in the merge's step finds it. Invalid if the
    // store couldn't keep that song.
    juce::ValueTree keepMerge (const Merge&, std::vector<int> chosen);

    // Of another branch's song left on it (its working piece): the commit a merge it holds merged in; none for none.
    juce::String findMergeLeftOn (const BranchRecord&) const;

    // Tells onChange at once: what was read or written, a branch made, a merge begun.
    void tellNow();

    //==============================================================================
    // A command that reads whole songs before it changes anything (a switch to another branch, Merge's list, Restore This Version,
    // Restore from This Version, Restore from Local History, Compare with the Song, Undo Last Version, Reset Branch to Last
    // Readable Version: known bug 126), in two halves: what it reads, on the history's thread (read, given
    // what it needs copied first, never the song, whose tree is the message thread's), which returns what takes it in, on
    // the message thread once it is read (TakeIn: whether it did what the command does). One at a time, and not while the
    // history is being read; false, with nothing started, then. Cancelled meanwhile (cancelReading(): the progress window's
    // Cancel), nothing is taken in. What holds the window meanwhile (hold, the caller's: ProjectContext::holdForWork()) lets
    // go once it is over, after what was read was taken in; then done is told whether it did (not if the history went).
    // Whether one is under way, and what it is, as the progress window names it; and how the last one ended.
    // What taking it in came to (Taken): done or not, with why not (readingProblem, which the Source Control view says until
    // the next command that reads), or read again first: what it worked out from a copy of the song was out of date (the song
    // changed meanwhile), so another round reads on the history's thread from a new copy before it is taken in.
    struct Taken;
    using TakeIn = std::function<Taken()>;

    struct Taken
    {
        Taken (bool doneIt)
            : did (doneIt)
        {
        } // NOLINT: a take-in that did it, or not, says so

        static Taken refused (juce::String why)
        {
            Taken taken (false);
            taken.problem = std::move (why);
            return taken;
        }

        static Taken readAgain (std::function<TakeIn()> read)
        {
            Taken taken (false);
            taken.again = std::move (read);
            return taken;
        }

        bool did = false;
        juce::String problem;
        std::function<TakeIn()> again;
    };

    juce::String readingProblem;
    bool startReading (const juce::String& title, std::function<TakeIn()> read, std::shared_ptr<void> hold = {},
                       std::function<void (bool did)> done = {});
    bool isReadingForCommand() const noexcept { return commandRead.has_value(); }
    juce::String getCommandReadTitle() const { return commandRead.has_value() ? commandRead->title : juce::String(); }
    void cancelReading (const juce::String& why = {}); // why, said as the reading's problem: none for the user's Cancel
    bool didLastReading() const noexcept { return lastReadingDid; }
    int getReadingsStarted() const noexcept { return readingsStarted; }

    // The store, for what a command reads on the history's thread (startReading()); none for a song that keeps no versions.
    std::shared_ptr<VersionStore> getStore() const noexcept { return store; }

    // How many times the song changed (each property, child added, removed or moved, view state too, and each change its
    // plug-ins said they made): what a command that read on the history's thread from a copy of it compares as it takes
    // in what it read, to do again with the song as it is what it worked out from the copy.
    juce::int64 countSongChanges() const noexcept { return songChanges + song.getChangesOutsideHistory(); }

    // A switch to another branch, in its halves (startReading()): what it takes on the message thread first
    // (the song left copied, its plug-ins' settings stored), what it reads (the song left split and kept, the other's song and
    // its last version's read), and how it ends, the song made the other's. Invalid, with nothing done, if it can't be.
    struct Switch
    {
        juce::String branchID; // the branch entered
        juce::ValueTree left;  // the song as it is, kept on the branch it leaves
        PluginParameterValues leftParameters;
        juce::int64 changesAtCopy = 0;
        juce::String otherPiece;  // the other's song: as it was left there, or its last commit's
        bool otherIsHead = false; // its last commit's (none was left there): read once
        juce::String headPiece;   // its last commit's song, which its changes are from: none before its first
    };

    struct SwitchRead
    {
        juce::String leftPiece;
        bool leftKept = false;
        juce::int64 leftChanges = 0; // the song's count of changes (countSongChanges()) as the copy split was taken
        juce::ValueTree other, last;
        PluginParameterValues lastParameters;
    };

    std::optional<Switch> prepareSwitch (const juce::String& branchID, bool withinRead = false); // within: a command's read's next step
    static SwitchRead readSwitch (VersionStore&, const Switch&);
    Taken finishSwitch (const Switch&, SwitchRead);

    // Another round of the song left, for a switch whose song changed while the other was read: copied again (false if it
    // didn't change), and split and kept on the history's thread.
    bool copyLeftAgain (Switch&);
    static void keepLeftIn (VersionStore&, const Switch&, SwitchRead&);

    // The name and the note the Versions view holds for the next version, until it is saved with them.
    juce::String draftName, draftNote;

    // For tests: while set, every history's writes wait before they write, as a slow disk would make them.
    static std::atomic<bool> holdWritesForTests;
    static std::atomic<bool> holdReadsForTests; // and reads, each where it starts

    // For tests: while set, a history's first read, of its folder, waits once it has begun, where a read held as above
    // has not; and how many wait so.
    static std::atomic<bool> holdFirstReadsForTests;
    static std::atomic<int> firstReadsHeldForTests;

    // For tests: so many more reads start, and those after them are held where they start until this is set again (below
    // 0: none is held). A test that stops what reads in steps between two of them (an import cancelled once its branch is
    // made) counts the steps it lets through.
    static std::atomic<int> readsToStartForTests;

private:
    Song& song;
    juce::ValueTree state; // the song's, which this listens to
    std::function<void()> onChange;
    juce::File folder; // the store's
    std::shared_ptr<VersionStore> store;
    std::shared_ptr<BackgroundJobs> thread; // one at a time; finishing its writes after the history goes (~VersionHistory())
    std::shared_ptr<std::atomic<int>> writesLeft = std::make_shared<std::atomic<int>> (0);
    int pendingJobs = 0, generation = 0;       // generation: which store what comes back from the thread was read from
    std::shared_ptr<VersionsAudio> audioPaths; // what each piece names of audio (makeAudioPathsFinder())
    bool hasRead = false, saving = false, checking = false;
    std::optional<Check> lastCheck;
    std::shared_ptr<std::atomic<float>> checkProgress = std::make_shared<std::atomic<float>> (0.0f);
    std::shared_ptr<std::atomic<bool>> checkCancelled = std::make_shared<std::atomic<bool>> (false);
    float checkProgressTold = 0.0f;

    // Housekeeping stopped before anything waits for the history's thread (waitForThread(), copyFolder()): the sweep as the
    // store opens (StoreSweep.h), which changes nothing then and runs at a later open, and Verify History.
    std::shared_ptr<std::atomic<bool>> sweepStopped = std::make_shared<std::atomic<bool>> (false);
    void stopHousekeeping() const;

    std::vector<BranchRecord> branches;
    juce::String currentBranch; // its id
    std::map<juce::String, VersionRecord> versions;
    std::set<juce::String> bounces; // the versions that have one
    juce::int64 bouncesSize = 0;

    // The current branch's last version's song, which the changes are from: its tracks by their IDs, and its plug-ins'
    // parameters' values.
    juce::ValueTree baseline;
    std::map<std::uint64_t, juce::ValueTree> baselineTracks;
    PluginParameterValues baselineParameters;

    // The song's counts of the changes its plug-ins said they made (Song::getChangesOutsideHistory()), by device, as they
    // were at the last version: a plug-in whose stored state differs counts as changed since only if its count moved, or a
    // knob of it did (SongDiff.h: PluginValues::reported).
    using ReportCounts = std::map<std::uint64_t, int>;
    ReportCounts reportedAtBaseline;
    static std::set<std::uint64_t> findReported (const ReportCounts& then, const ReportCounts& now);

    void baselineChanged();

    // The changes, worked out a thing at a time: each track's (and the master's) by its ID, those with changes; the song's
    // own; the devices whose automation changed; and what changed in the song since.
    std::map<std::uint64_t, SongChange> trackChanges;
    SongChange songChange;
    std::set<std::uint64_t> automated, dirtyTracks;
    bool allDirty = true, songDirty = true, automationDirty = true, changesReady = false, notifyPending = false;
    std::vector<SongChange> changes;
    int changesOutsideHistory = 0; // the song's count, as the changes last took it in (a plug-in's own change)
    ReportCounts outsideSeen;      // and each device's, whose tracks are compared again
    int pluginsLoadingSeen = 0;    // the plug-ins still loading as the changes last took them in

    // While the changes are listed against another version: its song, its plug-ins' values, and what differs from it, worked
    // out again after each change to the song.
    juce::String comparedVersion;
    juce::ValueTree comparedSong;
    PluginParameterValues comparedParameters;
    std::vector<SongChange> comparedChanges;
    bool comparedReady = false;

    // What each commit asked for changed (getVersionChanges()), by its id, and those being read.
    std::map<juce::String, std::vector<SongChange>> versionChanges;
    std::set<juce::String> versionChangesBeingRead;
    void versionChangesRead (int generation, const juce::String& versionID, std::vector<SongChange>);

    // The commits' messages as they were edited (VersionStore.h: readMessages()), which name them in place of their records'.
    std::map<juce::String, MessageEdit> messageEdits;
    void applyMessageEdits();

    // Local History, newest first; whether the song changed since the last snapshot was taken, and when it was.
    std::vector<Snapshot> snapshots;
    bool editedSinceSnapshot = false;
    juce::int64 lastSnapshotTime = 0;
    void snapshotTaken (int generation, std::vector<Snapshot> oldestFirst);

    // Whether the songs left on branches had changes since their last commits (hasChangesLeftOn()), by their pieces, and those
    // being compared.
    std::map<juce::String, LeftSong> changesLeft;
    std::set<juce::String> changesLeftBeingRead;

    // The versions no branch's line holds any more (a branch deleted, a version undone) let go of from the history, kept
    // among those that left (departed), which Recover Lost Versions lists: they stay in the store for 30 days.
    void forgetUnreachable();

    // What left every branch: the records of the versions no branch holds that left within 30 days, read as the history
    // opened or kept as they left since; and every computer's logs' moves of the last 30 days, this computer's since too.
    std::map<juce::String, VersionRecord> departed;
    std::vector<BranchMove> recentMoves;

    // The time what the history writes now is stamped with (VersionStore::stamp()).
    juce::int64 stampNow() const;

    // A round of adoptLater()'s copy, a batch a job on the history's thread, and what came of it on the message thread: the
    // versions and songs that couldn't be copied whole aren't tried again until the history opens again, and are said.
    void adoptRound (Adoption);
    void adoptionDone (Adoption left, const Adopted&);
    bool adoptionQueued = false, adoptAgain = false;
    std::set<juce::String> adoptionFailed;
    juce::String sweepProblem, adoptionProblem;

    // A move of a branch's last version, written into this computer's log and kept among the recent moves.
    void logMove (const juce::String& branchID, const juce::String& from, const juce::String& to, const juce::String& why,
                  const juce::String& name, const juce::String& leftSong = {});

    // The branches whose last commits couldn't be read as the history opened (getHeadProblem()), by their ids: that commit,
    // and the newest commit of the branch's log that reads (none for none).
    struct UnreadableHead
    {
        juce::String head, readable;
    };

    std::map<juce::String, UnreadableHead> unreadableHeads;
    juce::StringArray getOfferedCommits() const; // the readable ones, which the graph keeps

    void open();   // the store beside the song's file, read on the history's thread
    struct Opened; // what the history's thread read as it opened
    void opened (int generation, Opened);

    // The merge the song's element says, found again (getMerging()): whether it must be, and those found, by the element
    // each was found from (its text but for the choices), a few, with why it doesn't wait as taken; those being read on the
    // history's thread, and one read there taken in.
    mutable bool mergeDirty = true;
    mutable std::optional<Merge> merging;
    mutable juce::String mergeProblem;
    mutable std::map<juce::String, Merge> mergesFound;
    mutable std::set<juce::String> mergesBeingRead;
    void findMerging() const;
    void keepFound (const juce::String& key, Merge) const;
    void mergeReadAgain (int generation, const juce::String& key, std::optional<Merge>);

    // This branch's own commit of a merge, on the branch's line (VersionHistory.h: getMerging()): none while there is none.
    const VersionRecord* findOwnCommit (const MergeState&) const;
    mutable std::map<juce::String, juce::String> mergesLeft; // findMergeLeftOn()'s, by the song's piece
    void saved (int generation, bool ok, const VersionRecord&, const BranchRecord&, const juce::ValueTree& song, PluginParameterValues,
                ReportCounts committed);
    void followFolder(); // the song's project moved (its first save asked where): its versions are beside it there

    // While the folder is copied (copyFolder()), and what the history started meanwhile, to start once it is over: with the
    // version whose bounce it stopped, and how to make it again. What the copy job and the message thread share.
    bool copyingFolder = false, initialCommitWaiting = false, openSnapshotWaiting = false;
    std::vector<std::function<void()>> waitingForCopy; // whenNotCopying()'s
    juce::String bounceWaiting;
    std::function<void (const juce::String&)> restartBounce;

    struct FolderCopyState
    {
        std::mutex lock;                  // over's change, why, and the hold taken or let go of
        std::atomic<bool> over { false }; // the copy stops, or doesn't begin
        juce::String why;                 // it is over: what its job says
        juce::File folder;
        std::function<void (const juce::Result&)> copied; // message thread only
    };

    std::shared_ptr<FolderCopyState> folderCopy;
    static void endCopy (FolderCopyState&, const juce::String& why); // the hold let go of
    void startWhatWaitedForTheCopy();

    // A job on the history's thread: one that writes (a commit, a snapshot), which is done before the history goes, or one
    // that reads, which is let go as it goes, a long one looking at closing between its steps.
    enum class Work
    {
        reads,
        writes
    };

    std::shared_ptr<std::atomic<bool>> closing = std::make_shared<std::atomic<bool>> (false);
    void run (std::function<void()> job, Work);
    void checked (int generation, Check);

    // A command reading on the history's thread (startReading()): what it is, what holds the window, what is told as it ends,
    // and its number among those started; how many started, and whether the last did what it does.
    struct CommandRead
    {
        juce::String title;
        std::shared_ptr<void> hold;
        std::function<void (bool)> done;
        int number = 0;
        juce::File folder; // the store's as it began, which what it read was read from
    };

    std::optional<CommandRead> commandRead;
    int readingsStarted = 0;
    bool lastReadingDid = false;
    void readOnThread (int number, std::function<TakeIn()> read);
    void readingDone (int generationThen, int number, const TakeIn&);
    void endReading (Taken);

    // How many times the song changed (countSongChanges()).
    juce::int64 songChanges = 0;

    // The song as it is now, its plug-ins' settings stored, to be kept on the branch it leaves.
    void keepLeft (Switch&);
    void setBaseline (juce::ValueTree song, PluginParameterValues, ReportCounts);

    void markChanged (const juce::ValueTree&, const juce::Identifier& property);
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);
    void markAllChanged();
    void changed();          // tells onChange at the next tick
    void changesMayDiffer(); // and works the changes out again, what is dirty of them
    void edited();           // the song changed as the changes count it
    void takeChangesOutsideHistory();
    void takePluginsJoined();

public:
    // The song's plug-ins' parameters' values as they are now, as their instances tell them, by their devices' IDs: what a
    // commit keeps beside their states, and what names a plug-in's knobs in a merge's clash.
    PluginParameterValues readPluginParameters() const;

private:
    void recordLoadedPlugins(); // the values of those that weren't known yet at the last commit, as they opened
    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        ++songChanges;
        markChildChanged (parent, child);
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        ++songChanges;
        markChildChanged (parent, child);
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (VersionHistory)
    JUCE_DECLARE_NON_COPYABLE (VersionHistory)
};

// The versions folder of a song's project, beside its file; none for a song without one.
juce::File getVersionsFolder (const Song&);
// Where the lines of two versions parted, as git merge-base finds it: of the versions both are or follow, one that no other
// of them follows. Chosen by the graph, not by when versions were saved, so that computers whose clocks differ agree. Where
// merges crossed and there are several, the one saved last (then the greater id). None for versions of no common line, or
// whose records aren't among those given.
juce::String findMergeBase (const std::map<juce::String, VersionRecord>& versions, const juce::String& a, const juce::String& b);

// Whether a version is another or follows it, among the versions given.
bool follows (const std::map<juce::String, VersionRecord>& versions, const juce::String& later, const juce::String& earlier);

// The IDs earlier merges into one line (ours) gave anew to what another line (theirs) added (RenumberedID), oldest first:
// those of the merge commits ours follows and where the lines parted doesn't, whose version merged in theirs follows. The
// other side's song and the parting's, translated through them (SongMerge.h: translateIDs()), are numbered as this side's.
std::vector<RenumberedID> findRenumbering (const std::map<juce::String, VersionRecord>& versions, const juce::String& ours,
                                           const juce::String& theirs, const juce::String& parted);

// A command that reads whole songs before it changes anything, on the history's thread (VersionHistory::startReading()),
// the window held meanwhile (ProjectContext::holdForWork(): its title for the progress window, and its Cancel cancelling the
// read), as the versions' commands start one (VersionsModel.cpp); false, with nothing started, while another is under way.
// And what takes a switch's reads in (VersionHistory::Switch), the song made the other's in place; read again from a new
// copy of the song left, rounds times at most, if it changed meanwhile.
bool startCommandRead (ProjectContext&, VersionHistory&, const juce::String& title, std::function<VersionHistory::TakeIn()> read,
                       std::function<void (bool did)> done = {}, std::shared_ptr<std::atomic<float>> progress = {});
VersionHistory::TakeIn takeInSwitch (ProjectContext&, VersionHistory::Switch, std::shared_ptr<VersionHistory::SwitchRead>, int rounds);

// A version's song read from a store, without a merge's element (Schema.h: MERGING), which no version's song holds, whoever
// wrote it: what the song is made like (a switch, Restore This Version) never brings one. As VersionStore::readSong() reads
// it. Any thread.
juce::ValueTree readCommitSong (const VersionStore&, const juce::String& songPiece, PluginParameterValues* parameters = nullptr,
                                std::vector<MissingPiece>* missing = nullptr, bool* idsGivenAnew = nullptr);

// The song of the current branch's last commit in a project's history, read from its Versions folder without opening the
// project (when its file can't be read, ProjectSession offers it, read on a thread of its own: known bug 184), and the
// commit's name; invalid for none, or one that can't be read whole. computer is this computer's (getThisComputer(), read on
// the message thread). Any thread.
std::pair<juce::ValueTree, juce::String> readLastCommitSong (const juce::File& projectFile, const juce::String& computer);

// Every computer's record of every branch (VersionStore.h) taken together, a record a branch, as this computer has them:
// its name and color the newest record's; its last version the one that follows every other computer's, or, where the
// lines went their own ways, this computer's (else the newest record's), with the others' last versions it doesn't follow
// as divergent; and the song this computer left on it. Versions whose records aren't among those given count as none.
std::vector<BranchRecord> joinBranchRecords (const std::vector<BranchRecord>& records,
                                             const std::map<juce::String, VersionRecord>& versions, const juce::String& computer);

// What a version is called: the name it was given, or the summary of its changes.
juce::String getDisplayName (const VersionRecord&);

// A merge waiting, as the song's MERGING element holds it (Schema.h), read as what anyone may have written: none for an
// element whose values don't hold together (a commit's or a piece's id that isn't one, a line or a choice that isn't one,
// IDs given anew that are 0 or not one to one: SongMerge.h, areOneToOne()). And the element of one.
struct MergeState
{
    juce::String version, head, before;
    std::vector<int> chosen;
    std::vector<RenumberedID> renumbered, translation;
};

std::optional<MergeState> readMergeState (const juce::ValueTree& element);
juce::ValueTree writeMergeState (const MergeState&);

// A merge waiting found again from what its element says, in a store with these commits: the song before it, read on its
// own (Abort Merge needs nothing else), each read as a whole song (PHAZITE); and, where they can be, the parting's and the
// other line's songs translated as they were, with the clashes and notes merging the three gives (Merge::readWhole). For
// any thread.
VersionHistory::Merge findMergeAgain (const VersionStore&, const std::map<juce::String, VersionRecord>&, const MergeState&);

// A merge's clashes on plug-ins named by the knobs whose values differ between this side and the other ("Serum settings:
// Cutoff, Mix"), and by the versions of the plug-in that made each side's state, where they differ.
void nameKnobs (std::vector<MergeClash>&, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                const PluginParameterValues& ourValues, const PluginParameterValues& theirValues);

// A local history thinned as it grows, as of a time (docs/design/versions.md: Local History): every snapshot of the last two hours,
// then the newest of each hour for a day, then the newest of each day for 30 days, and none older; oldest first, as given.
std::vector<Snapshot> thinSnapshots (const std::vector<Snapshot>& oldestFirst, juce::int64 now);
// How a first save copies the open project's Versions folder (Project::VersionsCopier): by the history of the song the
// context holds as each is called (copyFolder(), endFolderCopy()); with none, the copy runs on the message thread (a first
// save gives a project without a history none: ProjectSession::moveUntitledTo()).
class ProjectContext;
Project::VersionsCopier makeVersionsCopier (ProjectContext&);
} // namespace phazite
