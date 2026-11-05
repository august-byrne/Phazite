// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/VersionHistory.h"
#include "model/arrangement/TrackColours.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AppInfo.h"
#include "model/plugins/internal/PluginInstances.h"

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr int tickMilliseconds = 250; // how soon what changed is told, at most

    // The plug-ins' elements in a part of the song, their containers' too.
    void findPlugins (const juce::ValueTree& element, std::vector<juce::ValueTree>& found)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::DEVICE) && child[schema::deviceType].toString() == schema::deviceIsPlugin)
                found.push_back (child);

            findPlugins (child, found);
        }
    }

    std::map<std::uint64_t, juce::ValueTree> indexTracks (const juce::ValueTree& song)
    {
        std::map<std::uint64_t, juce::ValueTree> tracks;

        for (const auto& track : getTrackElements (song))
            tracks.emplace (readID (track[schema::trackID]), track);

        return tracks;
    }
} // namespace

juce::File getVersionsFolder (const Song& song)
{
    return song.getFile() != juce::File() ? song.getFile().getSiblingFile ("Versions") : juce::File();
}

juce::String getDisplayName (const VersionRecord& version)
{
    return version.name.isNotEmpty() ? version.name : version.summary;
}

//==============================================================================
VersionHistory::VersionHistory (Song& s, std::function<void()> changeCallback)
    : song (s),
      state (s.getState()),
      onChange (std::move (changeCallback)),
      thread (juce::ThreadPoolOptions {}.withThreadName ("Phazite versions").withNumberOfThreads (1))
{
    changesOutsideHistory = song.getChangesOutsideHistory();
    state.addListener (this);
    open();
    startTimer (tickMilliseconds);
}

VersionHistory::~VersionHistory()
{
    stopTimer();
    state.removeListener (this);
    thread.removeAllJobs (false, 30000); // a version being written is finished, never cut short
}

bool VersionHistory::keepsVersions() const
{
    return store != nullptr;
}

void VersionHistory::run (std::function<void()> job)
{
    ++pendingJobs;
    thread.addJob (std::move (job));
}

void VersionHistory::open()
{
    ++generation;
    folder = getVersionsFolder (song);
    store = folder != juce::File() ? std::make_shared<VersionStore> (folder) : nullptr;
    branches.clear();
    versions.clear();
    currentBranch.clear();
    setBaseline ({}, {});
    hasRead = store == nullptr;

    if (store == nullptr)
        return;

    // Its branches, every version their last ones follow, and the current branch's last version's song.
    run (
        [store = store, number = generation, weak = juce::WeakReference<VersionHistory> (this)]
        {
            auto branchesRead = store->readBranches();
            const auto current = store->readCurrentBranch();
            std::map<juce::String, VersionRecord> versionsRead;
            juce::StringArray toRead;

            for (const auto& branch : branchesRead)
                if (branch.head.isNotEmpty())
                    toRead.add (branch.head);

            while (! toRead.isEmpty())
            {
                const auto id = toRead[toRead.size() - 1];
                toRead.remove (toRead.size() - 1);

                if (versionsRead.count (id) > 0)
                    continue;

                if (auto version = store->readVersion (id))
                {
                    toRead.addArray (version->parents);
                    versionsRead.emplace (id, std::move (*version));
                }
            }

            juce::ValueTree head;
            PluginParameterValues parameters;

            for (const auto& branch : branchesRead)
                if (branch.id == current)
                    if (const auto found = versionsRead.find (branch.head); found != versionsRead.end())
                        head = store->readSong (found->second.song, &parameters);

            store->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, branchesRead, current, versionsRead, head, parameters]() mutable
                {
                    if (auto* self = weak.get())
                        self->opened (number, std::move (branchesRead), current, std::move (versionsRead), head, std::move (parameters));
                });
        });
}

void VersionHistory::opened (int number, std::vector<BranchRecord> branchesRead, const juce::String& current,
                             std::map<juce::String, VersionRecord> versionsRead, const juce::ValueTree& head,
                             PluginParameterValues parameters)
{
    --pendingJobs;

    if (number != generation)
        return;

    branches = std::move (branchesRead);
    versions = std::move (versionsRead);
    currentBranch = current;

    // A current branch that isn't there any more: the first one made.
    if (findCurrentBranch() == nullptr && ! branches.empty())
        currentBranch = branches.front().id;

    setBaseline (head, std::move (parameters));
    hasRead = true;
    tellNow();
}

void VersionHistory::followFolder()
{
    // The project's folder came with it (Project::moveTo()), its versions too: the same history, in the new folder.
    if (const auto moved = getVersionsFolder (song); moved != folder && store != nullptr && moved != juce::File())
    {
        folder = moved;
        store = std::make_shared<VersionStore> (folder);
    }
    else if (moved != folder)
    {
        open();
    }
}

std::vector<BranchRecord> VersionHistory::getBranches() const
{
    // In the order they were made: by their first versions' times, the ones without any last.
    auto sorted = branches;
    const auto firstSaved = [this] (const BranchRecord& branch)
    {
        auto first = std::numeric_limits<juce::int64>::max();

        for (const auto& [id, version] : versions)
            if (version.branch == branch.id)
                first = std::min (first, version.saved);

        return first;
    };

    std::stable_sort (sorted.begin(), sorted.end(), [&] (const auto& a, const auto& b) { return firstSaved (a) < firstSaved (b); });
    return sorted;
}

const BranchRecord* VersionHistory::findCurrentBranch() const
{
    for (const auto& branch : branches)
        if (branch.id == currentBranch)
            return &branch;

    return nullptr;
}

std::vector<const VersionRecord*> VersionHistory::getVersions() const
{
    std::vector<const VersionRecord*> sorted;

    for (const auto& [id, version] : versions)
        sorted.push_back (&version);

    std::sort (sorted.begin(), sorted.end(),
               [] (const VersionRecord* a, const VersionRecord* b) { return a->saved != b->saved ? a->saved > b->saved : a->id < b->id; });
    return sorted;
}

const VersionRecord* VersionHistory::findVersion (const juce::String& id) const
{
    const auto found = versions.find (id);
    return found != versions.end() ? &found->second : nullptr;
}

const VersionRecord* VersionHistory::findLastVersion() const
{
    const auto* branch = findCurrentBranch();
    return branch != nullptr ? findVersion (branch->head) : nullptr;
}

juce::ValueTree VersionHistory::readSong (const juce::String& versionID) const
{
    const auto* version = findVersion (versionID);

    if (version == nullptr || store == nullptr)
        return {};

    auto read = store->readSong (version->song);
    store->getPieces().closeFiles();
    return read;
}

juce::int64 VersionHistory::getSize() const
{
    return store != nullptr ? store->getPieces().getSize() : 0;
}

//==============================================================================
bool VersionHistory::saveVersion (const juce::String& name, const juce::String& note, std::function<void (juce::Result)> done)
{
    followFolder();

    const auto refuse = [&done] (const juce::String& why)
    {
        if (done != nullptr)
            done (juce::Result::fail (why));

        return false;
    };

    if (store == nullptr)
        return refuse ("This song isn't a project's, so it keeps no versions.");

    if (! hasRead)
        return refuse ("Phazite is still reading the project's versions.");

    if (saving)
        return refuse ("A version is being saved.");

    // The song as a save stores it, its plug-ins' own settings included, and their knobs' values.
    song.storeDeviceStates();
    auto copy = song.getState().createCopy();
    auto parameters = readPluginParameters();

    // On the current branch, or the first: Main.
    BranchRecord branch;

    if (const auto* current = findCurrentBranch())
        branch = *current;
    else
        branch = { juce::Uuid().toString(), "Main", getTrackColours()[1].colour, {}, {} };

    VersionRecord version;
    version.name = name.trim();
    version.note = note.trim();
    version.summary = branch.head.isEmpty() ? juce::String ("First version") : summarise (getChanges());
    version.saved = juce::Time::currentTimeMillis();
    version.branch = branch.id;
    version.phazite = getVersionString();

    if (branch.head.isNotEmpty())
        version.parents.add (branch.head);

    saving = true;
    tellNow(); // "Saving..."

    run (
        [store = store, number = generation, copy, parameters, version, branch, projectFolder = song.getFile().getParentDirectory(),
         weak = juce::WeakReference<VersionHistory> (this), done]() mutable
        {
            const auto pieces = splitSong (copy, parameters);
            version.files = store->hashFiles (findAudioFilePaths (copy), projectFolder);
            version.id = store->writeVersion (version, pieces);
            version.song = pieces.song;
            auto ok = version.id.isNotEmpty();

            if (ok)
            {
                branch.head = version.id;
                branch.working = {};
                ok = store->writeBranch (branch) && store->writeCurrentBranch (branch.id);
            }

            store->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, ok, version, branch, copy, parameters, done]() mutable
                {
                    if (auto* self = weak.get())
                        self->saved (number, ok, version, branch, copy, std::move (parameters));

                    if (done != nullptr)
                        done (ok ? juce::Result::ok()
                                 : juce::Result::fail ("Phazite couldn't write the version into the project's Versions folder."));
                });
        });

    return true;
}

void VersionHistory::saved (int number, bool ok, const VersionRecord& version, const BranchRecord& branch, const juce::ValueTree& copy,
                            PluginParameterValues parameters)
{
    --pendingJobs;
    saving = false;

    if (! ok || number != generation)
    {
        tellNow();
        return;
    }

    versions[version.id] = version;
    currentBranch = branch.id;

    if (auto found = std::find_if (branches.begin(), branches.end(), [&branch] (const auto& b) { return b.id == branch.id; });
        found != branches.end())
        *found = branch;
    else
        branches.push_back (branch);

    setBaseline (copy, std::move (parameters));
    tellNow();
}

PluginParameterValues VersionHistory::readPluginParameters() const
{
    PluginParameterValues values;
    std::vector<juce::ValueTree> plugins;
    findPlugins (song.getState(), plugins);

    for (const auto& plugin : plugins)
    {
        const auto id = readID (plugin[schema::deviceID]);

        if (auto* instance = findPluginInstance (song, DeviceID::fromRaw (id)))
        {
            auto& parameters = values[id];
            const auto& infos = instance->getParameterInfos();

            for (int i = 0; i < (int) infos.size(); ++i)
                parameters.push_back ({ infos[(size_t) i].id, infos[(size_t) i].name, instance->getParameterValue (i) });
        }
    }

    return values;
}

//==============================================================================
void VersionHistory::setBaseline (juce::ValueTree baselineSong, PluginParameterValues parameters)
{
    baseline = std::move (baselineSong);
    baselineParameters = std::move (parameters);
    baselineTracks = indexTracks (baseline);
    markAllChanged();
}

const std::vector<SongChange>& VersionHistory::getChanges()
{
    takeChangesOutsideHistory();

    if (changesReady)
        return changes;

    changesReady = true;
    changes.clear();

    if (! hasRead || ! baseline.isValid())
        return changes;

    const auto now = song.getState();
    const auto current = indexTracks (now);
    const auto masterBefore = baseline.getChildWithName (schema::MASTER), masterNow = now.getChildWithName (schema::MASTER);
    const auto masterID = readID (masterNow[schema::trackID]);

    if (allDirty)
    {
        trackChanges.clear();
        dirtyTracks.clear();

        for (const auto& [id, track] : current)
            dirtyTracks.insert (id);

        for (const auto& [id, track] : baselineTracks)
            dirtyTracks.insert (id);

        dirtyTracks.insert (masterID);
        songDirty = automationDirty = true;
        allDirty = false;
    }

    const auto findElements = [&] (std::uint64_t id) -> std::pair<juce::ValueTree, juce::ValueTree>
    {
        if (id == masterID)
            return { masterBefore, masterNow };

        const auto before = baselineTracks.find (id);
        const auto after = current.find (id);
        return { before != baselineTracks.end() ? before->second : juce::ValueTree(),
                 after != current.end() ? after->second : juce::ValueTree() };
    };

    if (! dirtyTracks.empty())
    {
        const auto parameters = readPluginParameters();
        const PluginValues values { &baselineParameters, &parameters };

        for (const auto id : dirtyTracks)
        {
            const auto [before, after] = findElements (id);

            if (! before.isValid() && ! after.isValid())
            {
                trackChanges.erase (id);
                continue;
            }

            if (auto change = findTrackChange (before, after, baseline, now, values);
                change.kind != SongChange::Kind::changed || ! change.what.isEmpty())
                trackChanges[id] = std::move (change);
            else
                trackChanges.erase (id);
        }

        dirtyTracks.clear();
    }

    if (songDirty)
    {
        songChange = findSongChange (baseline, now);
        songDirty = false;
    }

    if (automationDirty)
    {
        automated = findAutomatedDevicesChanged (baseline, now);
        automationDirty = false;
    }

    // The lines in the song's order: its tracks, those it hasn't any more, the master, then the song's own; a track's
    // automation named on its line.
    const auto addLine = [this, &findElements, &now] (std::uint64_t id)
    {
        const auto found = trackChanges.find (id);
        auto touchesAutomation = false;
        const auto [before, after] = findElements (id);

        if (! automated.empty() && before.isValid() && after.isValid())
        {
            auto ids = getAutomatableIDs (before);
            ids.merge (getAutomatableIDs (after));
            touchesAutomation = std::any_of (ids.begin(), ids.end(), [this] (auto device) { return automated.count (device) > 0; });
        }

        if (found == trackChanges.end() && ! touchesAutomation)
            return;

        auto line = found != trackChanges.end() ? found->second : findTrackChange (before, after, baseline, now);

        if (touchesAutomation)
            line.what.addIfNotAlreadyThere ("automation");

        changes.push_back (std::move (line));
    };

    std::set<std::uint64_t> shown;

    for (const auto& track : getTrackElements (now))
    {
        const auto id = readID (track[schema::trackID]);
        shown.insert (id);
        addLine (id);
    }

    for (const auto& track : getTrackElements (baseline))
        if (const auto id = readID (track[schema::trackID]); shown.count (id) == 0)
            addLine (id);

    if (masterBefore.isValid() && masterNow.isValid())
        addLine (masterID);

    if (! songChange.what.isEmpty())
        changes.push_back (songChange);

    return changes;
}

bool VersionHistory::hasChanges()
{
    return ! getChanges().empty();
}

//==============================================================================
void VersionHistory::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! schema::isCounted (tree, property))
        return;

    // The track (or the master) it is in, nearest first, or else the song's own.
    for (auto element = tree; element.isValid(); element = element.getParent())
    {
        if (element.hasType (schema::AUTOMATION))
        {
            automationDirty = true;
            changed();
            return;
        }

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
        {
            dirtyTracks.insert (readID (element[schema::trackID]));
            changed();
            return;
        }
    }

    songDirty = true;
    changed();
}

void VersionHistory::markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (schema::isUncountedElement (parent, child) || ! schema::isCounted (parent, {}))
        return;

    // A track added or taken away changes the others' names ("Track 3") and the song's order of them.
    if (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS) || child.hasType (schema::MASTER))
    {
        allDirty = true;
        changed();
        return;
    }

    if (child.hasType (schema::AUTOMATION) || child.hasType (schema::CURVE))
    {
        automationDirty = true;
        changed();
        return;
    }

    markChanged (parent, {});
}

void VersionHistory::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // A track's ID or name changes what the lines are called.
    if (tree.hasType (schema::TRACK) && (property == schema::trackID || property == schema::trackName || property == schema::trackKind))
        markAllChanged();

    markChanged (tree, property);
}

void VersionHistory::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex)
{
    // Tracks in another order: the song's line, and their numbers.
    if (parent.getChild (newIndex).hasType (schema::TRACK))
        markAllChanged();
    else
        markChanged (parent, {});
}

void VersionHistory::markAllChanged()
{
    allDirty = true;
    changed();
}

void VersionHistory::changed()
{
    changesReady = false;
    notifyPending = true;
}

void VersionHistory::tellNow()
{
    changed();
    notifyPending = false;

    if (onChange != nullptr)
        onChange();
}

void VersionHistory::takeChangesOutsideHistory()
{
    // A plug-in's own change (in its window): its knobs, which the song doesn't hold.
    if (const auto outside = song.getChangesOutsideHistory(); outside != changesOutsideHistory)
    {
        changesOutsideHistory = outside;
        markAllChanged();
    }
}

void VersionHistory::timerCallback()
{
    followFolder();
    takeChangesOutsideHistory();

    if (std::exchange (notifyPending, false) && onChange != nullptr)
        onChange();
}
} // namespace phazite
