// SPDX-License-Identifier: AGPL-3.0-or-later
// The open song's versions (docs/versions.md), as the model holds them while its project is open: what its Versions folder
// holds (VersionStore.h), read as the project opens; the version its branch is at, and the changes since then, worked out
// as they are asked for, from what changed since they were last; and versions saved, written on a thread of its own. A
// project context makes one for each song it opens (model/project/internal/ProjectContext.h): the versions of the project
// whose file the song has, in the folder beside it. Message thread, but for its own thread.
#pragma once

#include "SongDiff.h"
#include "SongMerge.h"
#include "VersionStore.h"
#include "model/base/Song.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace phazite
{
class TrackRender;

class VersionHistory : private juce::ValueTree::Listener,
                       private juce::Timer
{
public:
    // onChange is told, on the message thread, when the versions, the branches or the changes since the last version may
    // have changed, at most a few times a second.
    VersionHistory (Song&, std::function<void()> onChange);
    ~VersionHistory() override;

    // Whether the song keeps versions: it is a project's, which has a file. And whether the history's thread is reading or
    // writing, or what it read isn't taken in yet.
    bool keepsVersions() const;
    bool isBusy() const noexcept { return pendingJobs > 0; }
    bool isReading() const noexcept { return ! hasRead; }

    // The branches, in the order they were made; the current one (none before the first version); every branch's versions,
    // newest first; one by its id; and the current branch's last.
    std::vector<BranchRecord> getBranches() const;
    const BranchRecord* findCurrentBranch() const;
    std::vector<const VersionRecord*> getVersions() const;
    const VersionRecord* findVersion (const juce::String& id) const;
    const VersionRecord* findLastVersion() const;

    // The changes in the working tree, as git's: from the staged song (getStagedSong()) to the song, none before the first
    // commit, nor while the history is being read. And whether anything changed since the current branch's last commit,
    // staged or not.
    const std::vector<SongChange>& getChanges();
    bool hasChanges();

    // Staging, as git's index: the staged song is what the next commit records, the current branch's last commit with the
    // lines staged since taken from the song as they were when they were staged. Its changes from the last commit; a line
    // staged (its track, the master or the song's own, with its plug-ins' settings and knobs as they are now) or unstaged
    // (as the last commit has it again); every line at once. False for a line neither has. Kept while the project is open,
    // for its branch: a switch, or the project opening again, starts with nothing staged.
    const std::vector<SongChange>& getStagedChanges();
    const juce::ValueTree& getStagedSong() const noexcept { return staged; }
    bool stage (const SongChange&);
    bool unstage (const SongChange&);
    void stageAll();
    void unstageAll();

    // Commits: the staged song, or, with nothing staged or a merge being made, the song as it is now (git's smart commit),
    // its name and note as given (each may be empty), and calls back on the message thread once it is written, or not: the
    // song is copied here, then split and written on the history's thread. One at a time, and not while the history is
    // being read: false then, with done told why. The first, made as soon as the history has read that it has none, is
    // "Initial commit", on main.
    bool saveVersion (const juce::String& name, const juce::String& note, std::function<void (juce::Result)> done = {});
    bool isSaving() const noexcept { return saving; }

    // A version's song, read from its pieces, with its plug-ins' parameters' values if asked: invalid if one can't be read.
    // And the current branch's last version's song, which the changes are from (invalid before the first version).
    juce::ValueTree readSong (const juce::String& versionID, PluginParameterValues* parameters = nullptr) const;
    const juce::ValueTree& getLastVersionSong() const noexcept { return baseline; }

    // The Versions view's choices, which the commands act on: the version selected in its history, and the line of the
    // changes selected; and the version the changes are listed against while Take from This Version… shows one (none: the
    // last version). What differs from that one, as the changes are listed (from it to the song now), and its song.
    juce::String selectedVersion;
    std::optional<SongChange> selectedChange;
    bool selectedChangeIsStaged = false;
    void compareWith (const juce::String& versionID);
    const juce::String& getComparedVersion() const noexcept { return comparedVersion; }
    const std::vector<SongChange>& getListedChanges();

    // What a line of the changes is taken back from: the staged song (Discard Changes, as git restores from its index), or
    // the version the changes are listed against.
    const juce::ValueTree& getComparedSong() const noexcept { return comparedVersion.isEmpty() ? staged : comparedSong; }

    // How many bytes the history takes on disk.
    juce::int64 getSize() const;

    // The audio files every version plays, and every branch's song as it was left: their paths as the songs hold them
    // (findAudioFilePaths()), each once (model/project/UnusedAudio.h).
    juce::StringArray findVersionsAudioPaths() const;

    //==============================================================================
    // Bounces (docs/versions.md: Bounces and hearing them): a version's file, whether it has one or not, and whether it has
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

    //==============================================================================
    // Branches (docs/versions.md: Branches). One made from a version, or from the current branch's last version (none), named
    // "idea-2" and on, in the next color; and made the current one, the song going on there, if asked: its id, or none if it
    // can't be made. And a branch named anew, as git names one (model/versions/VersionsModel.h: toBranchName()), never as
    // another is.
    juce::String makeBranch (const juce::String& fromVersion, bool makeCurrent);

    // A branch from a version, whose song is another (a DAWproject's that came back: model/project/internal/DawProject.h),
    // kept there as the song left on it, to switch to: its id, or none.
    juce::String makeBranchWithSong (const juce::String& fromVersion, const juce::ValueTree& song);

    // Switching to another branch in place: the song as it is now, view state and unsaved changes included, kept on the branch
    // it leaves; then the other's song, as it was left there (or as its last version had it), made the song's with the fewest
    // edits (ApplySong.h: makeSongTheSame()), so that what both have stays as it is: its plug-ins loaded, its audio read, the
    // song playing on. Its changes since its last version from then on. A merge waiting for its version stays with its
    // branch. False if it can't be.
    bool switchTo (const juce::String& branchID);
    bool renameBranch (const juce::String& branchID, const juce::String& name);
    juce::String selectedBranch; // the one the Versions view's branch menu picked, which version.switchBranch switches to

    // The newest version that two versions both are or follow; none for two of no common line.
    juce::String findCommonVersion (const juce::String& a, const juce::String& b) const;

    // A commit as git names one in a merge's message: "branch 'idea-2'" for another branch's last commit, else
    // "commit '1a2b3c4'" (its id's first seven characters).
    juce::String describeCommit (const juce::String& versionID) const;

    // A merge, from its window (preview) until the version that finishes it is saved (merging): the version merged, the
    // song it parted from and the other's, the other's changes by line, and the clashes; then which value of each the song
    // has (0, the one before either side changed it; 1, this branch's; 2, the other's) and the notes.
    struct Merge
    {
        juce::String version, otherBranch;
        juce::ValueTree base, theirs;
        std::vector<SongChange> lines;
        std::vector<MergeClash> clashes;
        std::vector<int> chosen;
        juce::StringArray notes;
    };

    std::optional<Merge> preview, merging;
    std::vector<bool> ticked;                    // the merge window's lines that Merge takes
    std::map<juce::String, Merge> mergesWaiting; // by branch: a merge made there, waiting for its version while another is on

    // Tells onChange at once: what was read or written, a branch made, a merge begun.
    void tellNow();

    // The name and the note the Versions view holds for the next version, until it is saved with them.
    juce::String draftName, draftNote;

private:
    Song& song;
    juce::ValueTree state; // the song's, which this listens to
    std::function<void()> onChange;
    juce::File folder; // the store's
    std::shared_ptr<VersionStore> store;
    juce::ThreadPool thread;
    int pendingJobs = 0, generation = 0; // generation: which store what comes back from the thread was read from
    bool hasRead = false, saving = false;

    std::vector<BranchRecord> branches;
    juce::String currentBranch; // its id
    std::map<juce::String, VersionRecord> versions;
    std::set<juce::String> bounces; // the versions that have one
    juce::int64 bouncesSize = 0;

    // The current branch's last version's song, which the staged changes are from, and its plug-ins' parameters' values;
    // and the staged song, which the changes are from: its tracks by their IDs, its plug-ins' values, and its changes.
    juce::ValueTree baseline;
    PluginParameterValues baselineParameters;
    juce::ValueTree staged;
    std::map<std::uint64_t, juce::ValueTree> stagedTracks;
    PluginParameterValues stagedParameters;
    std::vector<SongChange> stagedChanges;
    bool stagedReady = false;
    void takeLine (const SongChange&, const juce::ValueTree& from, const PluginParameterValues& fromParameters);
    void stagedChanged();

    // The changes, worked out a thing at a time: each track's (and the master's) by its ID, those with changes; the song's
    // own; the devices whose automation changed; and what changed in the song since.
    std::map<std::uint64_t, SongChange> trackChanges;
    SongChange songChange;
    std::set<std::uint64_t> automated, dirtyTracks;
    bool allDirty = true, songDirty = true, automationDirty = true, changesReady = false, notifyPending = false;
    std::vector<SongChange> changes;
    int changesOutsideHistory = 0; // the song's count, as the changes last took it in (a plug-in's own change)

    // While the changes are listed against another version: its song, its plug-ins' values, and what differs from it, worked
    // out again after each change to the song.
    juce::String comparedVersion;
    juce::ValueTree comparedSong;
    PluginParameterValues comparedParameters;
    std::vector<SongChange> comparedChanges;
    bool comparedReady = false;

    void open(); // the store beside the song's file, read on the history's thread
    void opened (int generation, std::vector<BranchRecord>, const juce::String& current, std::map<juce::String, VersionRecord>,
                 const juce::ValueTree& head, PluginParameterValues);
    void saved (int generation, bool ok, const VersionRecord&, const BranchRecord&, const juce::ValueTree& song, PluginParameterValues);
    void followFolder(); // the song's project moved (its first save asked where): its versions are beside it there
    void run (std::function<void()> job);

    // Leaving the current branch for another: the song as it is now kept on it, and the other's as it was left there (or as
    // its last version had it), to make the song's: invalid if it can't be.
    juce::ValueTree leaveFor (const juce::String& branchID);
    void setBaseline (juce::ValueTree song, PluginParameterValues);

    void markChanged (const juce::ValueTree&, const juce::Identifier& property);
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);
    void markAllChanged();
    void changed(); // tells onChange at the next tick
    void edited();  // the song changed as the changes count it
    void takeChangesOutsideHistory();
    PluginParameterValues readPluginParameters() const;

    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { markChildChanged (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { markChildChanged (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (VersionHistory)
    JUCE_DECLARE_NON_COPYABLE (VersionHistory)
};

// The versions folder of a song's project, beside its file; none for a song without one.
juce::File getVersionsFolder (const Song&);

// What a version is called: the name it was given, or the summary of its changes.
juce::String getDisplayName (const VersionRecord&);
} // namespace phazite
