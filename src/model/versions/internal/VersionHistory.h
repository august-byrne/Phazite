// SPDX-License-Identifier: AGPL-3.0-or-later
// The open song's versions (docs/design/versions.md), as the model holds them while its project is open: what its Versions folder
// holds (VersionStore.h), read as the project opens; the version its branch is at, and the changes since then, worked out
// as they are asked for, from what changed since they were last; and versions saved, written on a thread of its own. A
// project context makes one for each song it opens (model/project/internal/ProjectContext.h): the versions of the project
// whose file the song has, in the folder beside it. Message thread, but for its own thread.
#pragma once

#include "ChangesWorker.h"
#include "SongDiff.h"
#include "SongMerge.h"
#include "StoreSweep.h"
#include "VersionStore.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
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
struct MergingRecord; // model/base/internal/SongOwnValue.h
struct SongStanding;  // model/base/internal/SongOwnValue.h
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
                       private SongValues::Listener,
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

    // The changes since the version the song grew from (its base: findBaseVersion(), below): from its song to the song as it
    // is, none before the first version, nor while the history is being read. Worked out on the history's thread from what
    // changed (ChangesWorker.h): the last list worked out, a round asked for if the song changed since, whose listeners are
    // told once it is in. And whether that list has any; whether it is the song's as it is now (nothing changed since its
    // round, which is in: the list, and the one against a version compared, if one is); and whether the song may have
    // changes, as far as is known now (some in the last list, or a change since).
    const std::vector<SongChange>& getChanges();
    bool hasChanges();
    bool areChangesWorkedOut() const;
    bool mayHaveChanges();

    // Saves a version: the song as it is now, its name and note as given (each may be empty), and calls back on the message
    // thread once it is written, or not: the song is copied here, then split and written on the history's thread. One at a
    // time, and not while the history is being read: false then, with done told why. The first, made as soon as the history
    // has read that it has none, is "First version", on main.
    bool saveVersion (const juce::String& name, const juce::String& note, std::function<void (juce::Result)> done = {});

    // Why the last version given wasn't saved (nothing changed since the version the song grew from, the folder couldn't be
    // written), as the Versions view says it, until the next version saved or the next edit; none while it was.
    juce::String saveProblem;
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

    // A version opened live (VersionsModel.h: openVersion()): the song made that version's in place, in a trial that holds the
    // song's history (model/commands/internal/Undo.h: Trial::holdHistory()), which Back to Now puts back. What it keeps
    // meanwhile: the version; the trial; the song as it was (Now: a copy taken as it opened, its plug-ins' settings stored
    // first, with their knobs), which what changed without an undo step is made again from; Now's piece in the store, split
    // and written on the history's thread as it opened (keepShownNow()), which the branch it is on names should an edit start a
    // branch (none until it is written, or if it couldn't be); the counts of the plug-ins' own changes as it opened; and what
    // the changes' getters said as it opened (mayHaveChanges() answers that meanwhile, no round of the changes runs and no
    // snapshot of Local History is taken: the song isn't Now). None while none is. Now never changes while it is open: what
    // changes meanwhile is the version's (an edit, which makes a branch of it; or what isn't an undo step, which Back to Now
    // makes as Now had it), so the piece written as it opened stays Now's.
    //
    // Also what is selected and the time range selected as it opened, which Back to Now puts back (the song's redo steps, done
    // again under it and undone with it, set the range they set and the one before); and an edit's branch waiting for Now's
    // piece (VersionsModel.cpp: the edit's step ended while the piece was still being written), with what it does once the
    // piece is in, written or not (whenNowIn, which holds the window meanwhile).
    struct Shown
    {
        juce::String version;
        std::unique_ptr<Trial> trial;
        juce::ValueTree now;
        PluginParameterValues nowParameters;
        juce::String nowPiece;
        bool nowPieceComing = false; // its write asked for (keepShownNow()), until it is in or failed
        int number = 0;              // which open from Now it is, for what comes back from the history's thread
        std::map<std::uint64_t, int> outsideAtOpen;
        bool mayHaveChanges = false;
        Selection::Kept selected;
        BeatRange range;
        std::vector<TrackID> rangeTracks;
        bool branchWaits = false;
        std::function<void (bool written)> whenNowIn;
    };

    std::optional<Shown> shown;
    int shownFromNow = 0; // opens from Now so far (Shown::number)

    // Now's piece written for the version open (shown), on the history's thread: once a first save's copy of the folder is
    // over, if one is under way.
    void keepShownNow();

    // The version opened last (A/B's B once none is open), and the songs of the versions opened, as they were read with their
    // knobs, kept for the next open (a version never changes): the last two.
    juce::String shownLast;

    struct ShownSong
    {
        juce::String version;
        juce::ValueTree song;
        PluginParameterValues parameters;
    };

    std::vector<ShownSong> shownSongs;
    static constexpr size_t shownSongsKept = 2;
    const ShownSong* findShownSong (const juce::String& versionID) const;
    void keepShownSong (ShownSong);

    // What an edit while a version is open makes of it (VersionsModel.cpp): a branch from the version open, made the current
    // one, the song growing from that version, as Create Branch from it makes one (named "idea-3", in the next colour); and
    // Now, by its piece written as the version opened (keepShownNow()), kept on the branch it leaves as a switch keeps the song
    // it leaves: only the branches' small records are written here. The song itself isn't touched: it is the version's
    // already. Called once the song shows the version no more (Song::isShowingOther()). The new branch's id; none, with
    // nothing changed, if it can't be made, and why in branchProblem ("busy": the history is, or Now's piece isn't written
    // yet; "write": a record couldn't be written).
    juce::String branchFromShown();
    juce::String branchProblem;

    // The whole song compared again at the next round of the changes: after Back to Now gave plug-ins changed in their
    // windows meanwhile Now's settings again, once they took them (their knobs, which the song doesn't hold, follow).
    void compareAllAgain() { markAllChanged(); }

    // What the versions tell without asking, once (VersionsModel.h: takeVersionsNotice()): none while there is nothing.
    juce::String noticeTitle, noticeText, noticeBranch;

    // Hear It's bounce: why the last press couldn't hear it, until the next press or an edit.
    juce::String compareProblem;

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
    std::vector<RenumberedID> findRenumbering (const juce::String& ours, const juce::String& theirs, const juce::String& parted,
                                               bool onlyAtParting = true) const;

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
        std::vector<RenumberedID> crossed;     // what theirs was translated through before that (crossRenumbering())
        std::vector<RenumberedID> renumbered;  // what it gave anew, which its version keeps
        std::vector<SongChange> lines;         // what the other line changed, which the merge takes whole
        std::vector<MergeClash> clashes;
        std::vector<int> chosen;
        juce::StringArray notes;
        juce::ValueTree before; // the song as it was before the merge was taken, which Abort Merge makes it again
        juce::String beforePiece;
        juce::int64 beforeChanges = -1; // the song's count of changes (countSongChanges()) as before was copied, for its piece
        juce::String head;              // the current branch's last version as the merge was taken
        // The song's numbering (Schema.h: idNumbering) as the merge's list was worked out against it: Merge takes the list into
        // no song numbered otherwise, as Restore This Version or its Undo can make the song while the list shows.
        juce::String numbering;

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
        juce::String head;        // and that commit, unless the song left there grew from another (SwitchRead::base)
    };

    struct SwitchRead
    {
        juce::String leftPiece;
        bool leftKept = false;
        juce::int64 leftChanges = 0; // the song's count of changes (countSongChanges()) as the copy split was taken
        juce::ValueTree other, last;
        PluginParameterValues lastParameters;

        // The version the other's song grew from, whose song last is (its names, where they say that branch and a version
        // that reads: Schema.h), and its record; none for the branch's last version.
        juce::String base;
        std::optional<VersionRecord> baseRecord;
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

    //==============================================================================
    // The song tied to its version (docs/design/versions.md: The song tied to its version; roadmap S4.5). The song says which
    // branch it is on and which version it grew from (Schema.h: versionsBranch, versionsBase), and goes with them into every
    // file it is saved into: the history takes them as it opens (where they name a branch it has and a version it can read;
    // else the branch the folder's current file names, and its last version, as a project opened without them), and moves
    // them with every command that makes the song grow from another version, in the same step. The changes are worked out
    // against the version the song grew from (its base), not the branch's newest: where the branch has versions the song
    // didn't grow from (another computer saved one; the project file came back from a backup), they are offered to be taken
    // or merged in, and no version is saved over them meanwhile.
    //
    // The base: none before the first version. Its song is the one the changes are from (getLastVersionSong()).
    const VersionRecord* findBaseVersion() const;
    const juce::String& getBaseVersionID() const noexcept { return baseVersion; }

    // Whether the branch has versions the song didn't grow from: its last version is neither the base nor one the base
    // follows (newer versions on the branch, or its line went elsewhere). How many of them the song's line doesn't hold.
    bool isBehind() const;
    int countNewerVersions() const;

    // Whether Save Version may save on the branch now: not while it is behind with versions this computer's own record of
    // the branch holds (a project file put back from a backup or an autosave, older than versions saved here since), which
    // a version on top of the base alone would leave on no branch, unless the merge waiting takes the branch's last version
    // in (its version then follows it). Versions another computer's record holds stay on the branch there: a version saved
    // here goes its own way, which the view shows to merge again (getDivergentVersions()). And why not, in the view's words.
    bool canSaveOnBranch() const;
    juce::String whyNotSaveOnBranch() const;

    // A version the song's names point at that can't be read (damaged, or let go of after it left every branch): said once,
    // the changes then worked out against the branch's last version; none while there is nothing to say.
    const juce::String& getBaseProblem() const noexcept { return baseProblem; }

    // Whether the branch the song's names say it is on isn't in the folder (missingBranch, below): no switch meanwhile.
    bool isBranchMissing() const noexcept { return missingBranch.isNotEmpty(); }

    // Take the Newer Versions (VersionsModel.cpp), where the song has no changes of its own since its base: the base moved to
    // the branch's last version, given its song (read on the history's thread, with its plug-ins' values), before the
    // command's undo step makes the song that version's and writes the names into it, so that the names it writes are the
    // history's already; its Undo puts the names back, and the history follows them (followNamesOfSong()). False if the branch
    // isn't ahead of the song any more, or that isn't its last version.
    bool takeBaseFrom (const juce::String& versionID, const juce::ValueTree& versionSong, PluginParameterValues);

    // After a switch, once the project was saved: the song left on the branch entered is let go of (its record no longer
    // names it), as the project file holds the song now. Not if the save failed: both branches stay as they were.
    void switchSaved();

    // The branch and the base a song's names say (Schema.h): none for what isn't one.
    struct Names
    {
        juce::String branch, base;
    };

    static Names readNames (const juce::ValueTree& song);
    static Names readNames (const SongStanding&);

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

    // The song of the version the song grew from, which the changes are from, never changed (the history's thread reads it
    // too), and its plug-ins' parameters' values.
    juce::ValueTree baseline;
    PluginParameterValues baselineParameters;

    // The song's counts of the changes its plug-ins said they made (Song::getChangesOutsideHistory()), by device, as they
    // were at the last version: a plug-in whose stored state differs counts as changed since only if its count moved, or a
    // knob of it did (SongDiff.h: PluginValues::reported).
    using ReportCounts = std::map<std::uint64_t, int>;
    ReportCounts reportedAtBaseline;
    static std::set<std::uint64_t> findReported (const ReportCounts& then, const ReportCounts& now);

    void baselineChanged (bool songIsIt = false); // songIsIt: the song is the baseline as it is (a version just saved of it)

    // The changes, worked out on the history's thread (ChangesWorker.h; docs/design/versions.md: Off the message thread): since
    // the base, and, while the changes are listed against another version, since that one; a round of each at a time, from
    // copies of what the song's notifications said changed since the last (askForChanges()), the last list kept until the next
    // is in (changesWorked()), whose listeners are told then. And what changed in the song as they took it in.
    ChangesWorker sinceBase, againstCompared;
    void askForChanges();
    void changesWorked (bool compared, const ChangesWorker::Result&);
    bool snapshotOnFirstChanges = false; // Local History's "As opened", once the first changes since the base are in
    juce::uint32 lastRoundAt = 0;        // when the last round was given (juce::Time::getMillisecondCounter()), 0 for none

    // After an Undo or a Redo while the branch has newer versions: whether the song is its last version's as it is (a Take of the
    // newer versions undone and redone round a version saved after it), which it then grows from; looked at on the history's
    // thread at the next tick, from a copy of the song.
    bool healPending = false;
    void healToLast();
    bool notifyPending = false;
    int changesOutsideHistory = 0; // the song's count, as the changes last took it in (a plug-in's own change)
    ReportCounts outsideSeen;      // and each device's, whose tracks are compared again
    int pluginsLoadingSeen = 0;    // the plug-ins still loading as the changes last took them in

    // While the changes are listed against another version: it, its song and its plug-ins' values.
    juce::String comparedVersion;
    juce::ValueTree comparedSong;
    PluginParameterValues comparedParameters;

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
                ReportCounts committed, juce::int64 changesAtCopy);
    void followFolder(); // the song's project moved (its first save asked where): its versions are beside it there
    BranchRecord makeBranchRecord (const juce::String& head) const; // "idea-2", the next number free, in the next colour

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

    // The song tied to its version: its base (the version it grew from), what can't be read of the one it named, the names
    // written into the song as they move (writeNames(), without an undo step), and the song's names followed where they moved
    // in an undo step (Take the Newer Versions, and its Undo and Redo): the base's song read on the history's thread, or the
    // one it was before, kept.
    juce::String baseVersion, baseProblem;

    // As it opens: the base, what to say of the one named if it couldn't be read (none for nothing), and every other
    // computer's record's last version of each branch, by the branch (headsElsewhere): versions there stay on the branch
    // whatever this computer's record says, so that a version saved over them here is a line of its own, which the view
    // offers to merge, and loses nothing.
    std::map<juce::String, juce::StringArray> headsElsewhere;
    void baseRead (int generation, const juce::String& base, const juce::String& unreadable,
                   std::map<juce::String, juce::StringArray> elsewhere, const juce::String& missing);

    // The branch the song's names say it is on where the folder hasn't it (its record not synced yet, or deleted): none while it
    // has. Meanwhile the song stays on the branch the folder names but grows from its own version where that reads, its names
    // kept as they are (writeNames() leaves them), no version is saved (canSaveOnBranch()), and no newer versions are offered;
    // Create Branch goes on from the song on a branch of its own, which ends it.
    juce::String missingBranch;
    void writeNames();
    void followNamesOfSong();
    int baseReads = 0;        // which read of a base's song is the last asked for
    bool movingNames = false; // while a switch makes the song another's, whose names it takes after

    struct KeptBaseline
    {
        juce::String version;
        juce::ValueTree song;
        PluginParameterValues parameters;
    };

    KeptBaseline previousBaseline;
    void moveBase (const juce::String& version, juce::ValueTree song, PluginParameterValues, ReportCounts, bool songIsIt = false);

    // The song as it is now, its plug-ins' settings stored, to be kept on the branch it leaves.
    void keepLeft (Switch&);
    void setBaseline (juce::ValueTree song, PluginParameterValues, ReportCounts, bool songIsIt = false);

    // What the song's tree says changed, marked for the next rounds of the changes (ChangesWorker.h): the song's own line (a
    // placeholder come or gone, its elements in another order), or the master's or a track's (an element a safety net
    // refused in a step, left there until the step's Undo).
    void markChanged (const juce::ValueTree&);
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);

    // A track's line or the master's, from the tree's callbacks by its element (the TRACK or the MASTER nearest what changed),
    // or by its ID (markLine(), the master's by its): the master's by one key whatever its ID, which a version
    // may give anew (ChangesWorker::masterKey; known bug 244).
    void markTrack (const juce::ValueTree& track);
    void markLine (TrackID);

    // The clips whose notes changed (the song holds them beside its tree: SongValues.h), heard as the values change and their
    // tracks marked (markTrack()) as the changes are next asked for: a listener of the values reads nothing of the song. With
    // them the master's line, where a modulator of its changed (masterHeard). And whether any waits.
    std::vector<ClipID> notesHeard;
    void markHeard();
    bool isHeardWaiting() const noexcept { return ! notesHeard.empty() || masterHeard; }
    void clipNotesChanged (const std::vector<ClipID>&) override;

    // The clips that changed (SongValues.h): their tracks' lines, before and after, marked by their IDs as they are told.
    void clipsChanged (const std::vector<SongValues::ClipChange>&, const std::vector<TrackID>& reordered) override;

    // The tempo map (SongValues.h), which the tree's callbacks below leave alone (isHeardAsKept()): the song's own line.
    void tempoMapChanged() override;

    // The chord lane (SongValues.h), what is in whose CHORDLANE the tree's callbacks below leave alone (isHeardAsKept()):
    // the song's own line, as a change of its element was. The element's own coming and going they mark as any element's.
    void chordLaneChanged() override;

    // The curves (SongValues.h), which the tree's callbacks below leave alone (isHeardAsKept()): each one's.
    void curvesChanged (const std::vector<ParameterRef>&) override;

    // The MIDI links (SongValues.h), what is in whose MIDILINKS the tree's callbacks below leave alone (isHeardAsKept()): the
    // song's own line, as a change of its element was. The element's own coming and going they mark as any element's.
    void midiLinksChanged() override;

    // The sections (SongValues.h), what is in whose SECTIONS the tree's callbacks below leave alone (isHeardAsKept()): the
    // song's own line, as a change of its element was. The element's own coming and going they mark as any element's.
    void sectionsChanged (const std::vector<SectionID>&) override;

    // The modulators and macros (SongValues.h), which the tree's callbacks below leave alone (isHeardAsKept()): the lines of
    // the channels each was on and is on, and of those whose modulators came in another order, by their tracks' IDs (a
    // container's macros' too: their channel's), as a change under one marked its channel's line; the master's by its own
    // ID, which it reads as the changes are next asked for (markHeard()), not as it is told.
    void modulationChanged (const std::vector<SongValues::ModulationChange>&, const std::vector<ModulatorPlace>& reordered) override;
    bool masterHeard = false; // the master's line, to mark by its ID (markHeard())

    // The sends (SongValues.h), which the song's tree holds none of: the lines of the tracks each was on and is on, and of
    // those whose sends came in another order, by their IDs, as a change of a SEND marked its track's line (none is the
    // master's: a send is a track's).
    void sendsChanged (const std::vector<SongValues::SendChange>&, const std::vector<TrackID>& reordered, const SongSends& now) override;

    // The devices, containers and chains (SongValues.h), what the devices' value covers of which the tree's callbacks below
    // leave alone (isHeardAsKept()): the lines of the channels each was on and is on, and of those whose devices came in
    // another order, by their tracks' IDs, as a change at or under a device marked its channel's line; the master's by its own
    // ID, as for modulation (masterHeard).
    void devicesChanged (const std::vector<SongValues::DeviceChange>&, const std::vector<DevicePlace>& reordered,
                         const SongDevices& now) override;

    // The devices' store (SongValues.h; the song's tree holds none of it): a change of the song's, counted as a store's property
    // or a VALUE was, and the line of the channel of each item whose song's entries changed, by its place as the change names
    // it (the devices' keeper's), as a change of a DEVICE's or a CHAIN's property marked its channel's line. A held value
    // alone, and what is written for an item the song doesn't hold, mark nothing.
    void deviceStoreChanged (const std::vector<SongValues::DeviceStoreChange>&, const DeviceStore& now) override;

    // The tracks (SongValues.h), what their values cover of which the tree's callbacks below leave alone (isHeardAsKept()): a
    // change of the song's, and the line of each track that changed, by its ID (each track in a folder added or taken out is
    // told, and marked, too), as a change of a TRACK's own property and a TRACK added or taken out marked it; the song's order
    // of them, for one added or taken out, come to another folder, renamed or of another kind, and for tracks in another
    // order (the lines' names, "Track 3"); the master's line by its key, all of them where its element came anew. Their
    // switches: the line of each track (the master's), as a switch's property marked it. Their view: a change of the song's
    // count alone (songChanges), as a view's property was, which marks no line (uncounted).
    void tracksChanged (const std::vector<TrackChange>&, const std::vector<TrackID>& reordered, const SongTracks& now) override;
    void trackSwitchesChanged (const std::vector<TrackSwitchChange>&, const TrackSwitches& now) override;
    void tracksViewChanged (const std::vector<TrackID>&, bool master) override;

    // What the song holds of its own (SongValues.h, told by its areas' tellers; the tree's callbacks below leave what its
    // keeper's safety net takes in alone, isHeardAsKept()), each a change of the song's count (songChanges), as each
    // property was: its own value (songOwnChanged()) and the key set last (songStandingChanged()), the song's own line, as
    // markChanged() marked them; a merge waiting taken, chosen in, finished or cancelled found again as it is asked for
    // (mergeDirty). Where the song stands in its versions, moved by an Undo or a Redo (not by the history itself:
    // movingNames), followed once the step is whole, at the next turn of the message loop (followNamesOfSong()). How it plays
    // and shows (songOutsideChanged()) the count alone, as a property of view state was.
    void songOwnChanged (const SongOwnChange&) override;
    void songStandingChanged (bool lastKey, bool versionsNames) override;
    void songOutsideChanged (bool plays) override;
    void markLineKey (std::uint64_t key); // a track's line by its ID, or the master's (ChangesWorker::masterKey)
    void markCurve (const juce::String& key);
    void markSongOwn();
    void markOrder();
    void markAllChanged();
    void changed();          // tells onChange at the next tick
    void changesMayDiffer(); // and asks for a round of the changes
    void edited();           // the song changed as the changes count it
    void takeChangesOutsideHistory();
    void takePluginsJoined();

public:
    // The song's plug-ins' parameters' values as they are now, as their instances tell them, by their devices' IDs: what a
    // commit keeps beside their states, and what names a plug-in's knobs in a merge's clash.
    PluginParameterValues readPluginParameters() const;

private:
    // Of the plug-ins of these devices, for a round of the changes (ChangesWorker.h): their knobs' values as their instances
    // tell them now; and the base's, where it came without those of one still loading as it was saved or read the values it
    // opened with, which are its values then, kept from the first round that finds it loaded.
    PluginParameterValues readKnobs (const std::set<std::uint64_t>& devices) const;
    PluginParameterValues readBaseKnobs (const std::set<std::uint64_t>& devices);
    void timerCallback() override;

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (isHeardAsKept (parent, child))
            return; // the areas': tempoMapChanged(), chordLaneChanged(), curvesChanged(), midiLinksChanged()

        ++songChanges;
        markChildChanged (parent, child);
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        if (isHeardAsKept (parent, child))
            return; // the areas': tempoMapChanged(), chordLaneChanged(), curvesChanged(), midiLinksChanged()

        ++songChanges;
        markChildChanged (parent, child);
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override;

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
// With onlyAtParting, only the merges whose version merged in the parting is or follows: those after which the parting's song
// is numbered as the other line (a parting on this line before its merge holds this line's IDs, which no translation
// touches); without, every one, as crossRenumbering() takes the other line's.
std::vector<RenumberedID> findRenumbering (const std::map<juce::String, VersionRecord>& versions, const juce::String& ours,
                                           const juce::String& theirs, const juce::String& parted, bool onlyAtParting = true);

// What the other side's song (theirSong, as read) is translated through first (translateIDs()), into the parting's (base)
// numbering, before the translation this line's records make takes it into this line's (SongMerge.h: findTranslation(),
// which counts none of these targets as a collision): one numbering a stage, so that nothing is translated twice. Where the
// other line's own merges gave anew what this line holds too: this line merged in by a merge back (main merged idea-2 and
// gave its Pad a new ID; idea-2 merges main), or another computer's merge of a version this line merged too, which gave an
// item another ID than this line's merge did (the review of S4.5; known bug 199). theirMerges are those merges' records
// (findRenumbering() from that line's side, without onlyAtParting: those of versions this line's follows), and
// theirMergesAtParting those of them whose version the parting follows (with onlyAtParting), which holds that version's items
// under their own IDs (from); ourMerges are this line's alike, and ourMergesAtParting those findTranslation() translates by.
// Each of the other line's latest records whose to its song holds crosses: to from, the parting's ID, where ourMergesAtParting
// give it a word that findTranslation() takes (an item the parting holds; a note in its clip), which moves it on; else straight
// to this line's ID, the one this line's latest merge gave the same from (ourMerges, by clip and from), else from itself; to
// itself where it is that ID already, so that it counts as this line's too. And, given one of its own first (from makeID, one
// neither song nor any record holds), each of the other line's own items in the way: under an ID one of those crosses to,
// unless it crosses away itself or is the parting's own item there (one the parting holds that none of that line's words gave
// another ID: two items then, which the merge refuses: holdsEachIDOnce()); and under an ID one of its merges at the parting
// gave away (a from of theirMergesAtParting) that the parting holds too, which would be taken for the parting's item it is
// not, whether or not this line has a word for it. A note by its clip (its group) as that line's song has it, looked up in
// the parting by its clip as the parting numbers it, and given here by its clip as the items' translation numbers it, as
// translateIDs() looks it up. All of them, simultaneous; where they aren't one to one, those new IDs alone (one no item has
// never makes two items one). The merge's commit records them as recordCrossings() gives them. Any thread.
std::vector<RenumberedID> crossRenumbering (const std::vector<RenumberedID>& theirMerges, const std::vector<RenumberedID>& ourMerges,
                                            const std::vector<RenumberedID>& theirMergesAtParting,
                                            const std::vector<RenumberedID>& ourMergesAtParting, const juce::ValueTree& base,
                                            const juce::ValueTree& theirSong, const std::function<std::uint64_t()>& makeID);

// What crossRenumbering() gave (crossed) as the merge's commit records it (VersionRecord::renumbered), so that a later merge
// translates by it at once: each made the ID the merged song holds after the translation that follows it (translation:
// SongMerge.h, findTranslation()'s all), a note keyed by its clip as the merged song numbers it.
std::vector<RenumberedID> recordCrossings (const std::vector<RenumberedID>& crossed, const std::vector<RenumberedID>& translation);

// Whether a song holds each item's ID once, and each clip each note's: the other side's song and the parting's as a merge
// translated them (two items under one ID would be made one: the merge is refused), or as a merge waiting says (which a
// hand-made one could make hold one twice).
bool holdsEachIDOnce (const juce::ValueTree& song);

// A command that reads whole songs before it changes anything, on the history's thread (VersionHistory::startReading()),
// the window held meanwhile (ProjectContext::holdForWork(): its title for the progress window, and its Cancel cancelling the
// read), as the versions' commands start one (VersionsModel.cpp); false, with nothing started, while another is under way.
// And what takes a switch's reads in (VersionHistory::Switch), the song made the other's in place; read again from a new
// copy of the song left, rounds times at most, if it changed meanwhile.
bool startCommandRead (ProjectContext&, VersionHistory&, const juce::String& title, std::function<VersionHistory::TakeIn()> read,
                       std::function<void (bool did)> done = {}, std::shared_ptr<std::atomic<float>> progress = {});
VersionHistory::TakeIn takeInSwitch (ProjectContext&, VersionHistory::Switch, std::shared_ptr<VersionHistory::SwitchRead>, int rounds);

// A version's song read from a store, without a merge's element (Schema.h: MERGING) or the names of where a song stands
// (versionsBranch, versionsBase), which no version's song holds, whoever wrote it: what the song is made like (a switch,
// Restore This Version) never brings them. As VersionStore::readSong() reads it. Any thread.
juce::ValueTree readCommitSong (const VersionStore&, const juce::String& songPiece, PluginParameterValues* parameters = nullptr,
                                std::vector<MissingPiece>* missing = nullptr, bool* idsGivenAnew = nullptr);

// The versions a project's files name as the ones their songs grew from (Schema.h: versionsBase): its project file's, its
// autosave's and each of its backups', each once, as a project put back from any of them grows from its version. Only the
// start of each file is read. Any thread. Nothing calls it yet: the store's sweep is to take what it answers as named once
// its letting go is turned on (known bug 196), and keeps everything until then.
juce::StringArray findVersionsNamedByProject (const juce::File& projectFile);

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
    std::vector<RenumberedID> renumbered, translation, crossed;
};

std::optional<MergeState> readMergeState (const juce::ValueTree& element);
std::optional<MergeState> readMergeState (const MergingRecord&);
juce::ValueTree writeMergeState (const MergeState&);

// Sets the merge waiting in a song as its element says (a copy of it: writeMergeState()'s, or one a file holds, read as above),
// in place of the one the song holds, none for an invalid element, outside the song's history: as a song opened from a file
// that holds one has it. The merge's own steps set theirs in the step (model/versions/VersionsModel.cpp). Only for a song
// whose history holds no step to undo and none to redo (one just opened or made: asserted): the merge waiting is a field of a
// plain area's value, whose Undo and Redo put back the whole value their step held, so that an Undo of an earlier step would
// put back a song without it.
void setWaitingMerge (Song&, const juce::ValueTree& merging);

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
