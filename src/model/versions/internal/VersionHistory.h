// SPDX-License-Identifier: AGPL-3.0-or-later
// The open song's versions (docs/versions.md), as the model holds them while its project is open: what its Versions folder
// holds (VersionStore.h), read as the project opens; the version its branch is at, and the changes since then, worked out
// as they are asked for, from what changed since they were last; and versions saved, written on a thread of its own. A
// project context makes one for each song it opens (model/project/internal/ProjectContext.h): the versions of the project
// whose file the song has, in the folder beside it. Message thread, but for its own thread.
#pragma once

#include "SongDiff.h"
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

    // The changes since the current branch's last version: none before the first version, nor while it is being read.
    const std::vector<SongChange>& getChanges();
    bool hasChanges();

    // Saves a version of the song as it is now, its name and note as given (each may be empty), and calls back on the
    // message thread once it is written, or not: the song is copied here, then split and written on the history's thread.
    // One at a time, and not while the history is being read: false then, with done told why.
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
    void compareWith (const juce::String& versionID);
    const juce::String& getComparedVersion() const noexcept { return comparedVersion; }
    const std::vector<SongChange>& getListedChanges();
    const juce::ValueTree& getComparedSong() const noexcept { return comparedVersion.isEmpty() ? baseline : comparedSong; }

    // How many bytes the history takes on disk.
    juce::int64 getSize() const;

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

    // The current branch's last version's song, which the changes are from: its tracks by their IDs, and its plug-ins'
    // parameters' values.
    juce::ValueTree baseline;
    std::map<std::uint64_t, juce::ValueTree> baselineTracks;
    PluginParameterValues baselineParameters;

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
    void setBaseline (juce::ValueTree song, PluginParameterValues);

    void markChanged (const juce::ValueTree&, const juce::Identifier& property);
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);
    void markAllChanged();
    void changed(); // tells onChange at the next tick
    void tellNow(); // what was read or written, at once
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
