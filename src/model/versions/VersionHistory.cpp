// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/VersionHistory.h"
#include "internal/ApplySong.h"
#include "model/arrangement/TrackColours.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AppInfo.h"
#include "model/mixer/FreezeModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/VersionsModel.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace phazite
{
namespace
{
    constexpr int tickMilliseconds = 250; // how soon what changed is told, at most
    constexpr int maxPacks = 50;          // packs past which they are joined into one as the history opens

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

    // Branches in the order they were made: by their first versions' times, the ones without any last.
    std::vector<BranchRecord> sortByFirstVersion (std::vector<BranchRecord> branches, const std::map<juce::String, VersionRecord>& versions)
    {
        const auto firstSaved = [&versions] (const BranchRecord& branch)
        {
            auto first = std::numeric_limits<juce::int64>::max();

            for (const auto& [id, version] : versions)
                if (version.branch == branch.id)
                    first = std::min (first, version.saved);

            return first;
        };

        std::stable_sort (branches.begin(), branches.end(), [&] (const auto& a, const auto& b) { return firstSaved (a) < firstSaved (b); });
        return branches;
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
    backgroundBounce = nullptr; // cancelled, before what it renders goes
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

void VersionHistory::waitForThread() const
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Its jobs tell the message thread what they did with callAsync and never wait for it, so it can wait for them, as long
    // as the destructor does at most.
    const auto until = juce::Time::getMillisecondCounter() + 30000;

    while (thread.getNumJobs() > 0 && juce::Time::getMillisecondCounter() < until)
        juce::Thread::sleep (1);
}

void VersionHistory::open()
{
    ++generation;
    folder = getVersionsFolder (song);
    store = folder != juce::File() ? std::make_shared<VersionStore> (folder, getThisComputer()) : nullptr;
    branches.clear();
    versions.clear();
    currentBranch.clear();
    setBaseline ({}, {}, {});
    hasRead = store == nullptr;

    if (store == nullptr)
        return;

    // Its branches, every version their last ones follow, and the current branch's last version's song.
    run (
        [store = store, number = generation, weak = juce::WeakReference<VersionHistory> (this)]
        {
            // Its packs joined into one once there are many, as each is read as the store opens.
            if (store->getPieces().getNumPacks() > maxPacks)
                store->getPieces().joinPacks();

            // Every computer's record of each branch, and every version their last ones follow; then each branch as this
            // computer takes them together.
            const auto records = store->readBranches();
            auto current = store->readCurrentBranch();
            std::map<juce::String, VersionRecord> versionsRead;
            juce::StringArray toRead;

            for (const auto& record : records)
                if (record.head.isNotEmpty())
                    toRead.add (record.head);

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

            auto branchesRead = joinBranchRecords (records, versionsRead, store->getComputer());
            juce::ValueTree head;
            PluginParameterValues parameters;

            // A current branch that isn't there any more: the first one made.
            if (! branchesRead.empty()
                && std::none_of (branchesRead.begin(), branchesRead.end(),
                                 [&current] (const auto& branch) { return branch.id == current; }))
                current = sortByFirstVersion (branchesRead, versionsRead).front().id;

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
    setBaseline (head, std::move (parameters), song.getDeviceChangesOutsideHistory());
    hasRead = true;
    readBounces(); // and tells

    // A project keeps versions from its start, as a repository from its first commit: the song as it is now, on main.
    if (versions.empty())
        saveVersion ("Initial commit", {});
}

void VersionHistory::followFolder()
{
    // The project's folder came with it (Project::moveTo()), its versions too: the same history, in the new folder.
    if (const auto moved = getVersionsFolder (song); moved != folder && store != nullptr && moved != juce::File())
    {
        folder = moved;
        store = std::make_shared<VersionStore> (folder, getThisComputer());
    }
    else if (moved != folder)
    {
        open();
    }
}

std::vector<BranchRecord> VersionHistory::getBranches() const
{
    return sortByFirstVersion (branches, versions);
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

juce::ValueTree VersionHistory::readSong (const juce::String& versionID, PluginParameterValues* parameters,
                                          std::vector<MissingPiece>* missing) const
{
    const auto* version = findVersion (versionID);

    if (version == nullptr || store == nullptr)
        return {};

    auto read = store->readSong (version->song, parameters, missing);
    store->getPieces().closeFiles();
    return read;
}

bool VersionHistory::checkHistory (std::function<void()> done)
{
    if (store == nullptr || ! hasRead || saving || checking)
        return false;

    checking = true;
    tellNow(); // "Verifying..."

    run (
        [store = store, number = generation, known = versions, weak = juce::WeakReference<VersionHistory> (this), done]
        {
            // Every pack the folder holds now, those written since the history opened too.
            store->getPieces().readNewPacks();

            Check check;
            const auto names = store->getPieces().getNames();
            check.numPieces = names.size();
            check.numVersions = (int) known.size();

            // Every piece read, which checks it against its name, and the commits among them.
            std::vector<VersionRecord> found;

            for (const auto& name : names)
            {
                const auto bytes = store->getPieces().read (name);

                if (! bytes.has_value())
                {
                    check.damagedPieces.add (name);
                    continue;
                }

                constexpr const char* versionStart = "<VERSION ";

                if (bytes->getSize() > std::strlen (versionStart)
                    && std::memcmp (bytes->getData(), versionStart, std::strlen (versionStart)) == 0)
                    if (auto record = toVersionRecord (readElement (*bytes), name))
                        found.push_back (std::move (*record));
            }

            // Each commit's song walked piece by piece; the commits no branch's line holds.
            for (const auto& [id, version] : known)
            {
                std::vector<MissingPiece> missing;
                store->readSong (version.song, nullptr, &missing);

                if (! missing.empty())
                    check.damaged[id] = std::move (missing);
            }

            for (auto& record : found)
                if (known.count (record.id) == 0)
                    check.lost.push_back (std::move (record));

            std::sort (check.lost.begin(), check.lost.end(), [] (const auto& a, const auto& b) { return a.saved > b.saved; });
            store->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, check, done]() mutable
                {
                    if (auto* self = weak.get())
                        self->checked (number, std::move (check));

                    if (done != nullptr)
                        done();
                });
        });

    return true;
}

void VersionHistory::checked (int number, Check check)
{
    --pendingJobs;
    checking = false;

    if (number == generation)
        lastCheck = std::move (check);

    tellNow();
}

int VersionHistory::recoverLostVersions()
{
    if (store == nullptr || ! lastCheck.has_value() || lastCheck->lost.empty())
        return 0;

    // Taken into the history; then a branch at each line's newest commit, which no other lost one follows.
    std::set<juce::String> followed;

    for (const auto& version : lastCheck->lost)
    {
        versions.emplace (version.id, version);

        for (const auto& parent : version.parents)
            followed.insert (parent);
    }

    const auto isTaken = [this] (const juce::String& name)
    {
        return std::any_of (branches.begin(), branches.end(), [&name] (const auto& branch) { return branch.name.equalsIgnoreCase (name); });
    };

    int made = 0;

    for (const auto& version : lastCheck->lost)
    {
        if (followed.count (version.id) > 0)
            continue;

        juce::String base = "recovered";

        for (const auto& branch : branches)
            if (branch.id == version.branch)
                base = branch.name + "-recovered";

        auto name = base;

        for (int number = 2; isTaken (name); ++number)
            name = base + "-" + juce::String (number);

        BranchRecord branch;
        branch.id = juce::Uuid().toString();
        branch.name = name;
        branch.colour = getTrackColours()[(branches.size() + 1) % getTrackColours().size()].colour;
        branch.head = version.id;
        branch.computer = getThisComputer();
        branch.changed = juce::Time::currentTimeMillis();

        if (store->writeBranch (branch))
        {
            store->logBranchMove (branch.id, {}, version.id, "recover");
            branches.push_back (branch);
            ++made;
        }
    }

    lastCheck->lost.clear();
    tellNow();
    return made;
}

juce::int64 VersionHistory::getSize() const
{
    return store != nullptr ? store->getPieces().getSize() : 0;
}

juce::StringArray VersionHistory::findVersionsAudioPaths() const
{
    juce::StringArray paths;

    for (const auto& [id, version] : versions)
        for (const auto& file : version.files)
            paths.addIfNotAlreadyThere (file.path);

    if (store != nullptr)
        for (const auto& branch : branches)
            if (branch.working.isNotEmpty())
                for (const auto& path : findAudioFilePaths (store->readSong (branch.working)))
                    paths.addIfNotAlreadyThere (path);

    return paths;
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
        return refuse ("This song isn't a project's, so it keeps no commits.");

    if (! hasRead)
        return refuse ("Phazite is still reading the project's commits.");

    if (saving)
        return refuse ("A commit is being made.");

    // The staged song; or, with nothing staged or a merge being made, the song as a save stores it, its plug-ins' own
    // settings included, and their knobs' values.
    const auto commitsStaged = ! merging.has_value() && ! getStagedChanges().empty();
    juce::ValueTree copy;
    PluginParameterValues parameters;
    ReportCounts committed; // the changes the plug-ins said they made, as far as the commit holds them

    if (commitsStaged)
    {
        copy = staged.createCopy();
        parameters = stagedParameters;
        committed = reportedAtStaged;
    }
    else
    {
        song.storeDeviceStates();
        copy = song.getState().createCopy();
        parameters = readPluginParameters();
        committed = song.getDeviceChangesOutsideHistory();
    }

    // On the current branch, or the first: main.
    BranchRecord branch;

    if (const auto* current = findCurrentBranch())
    {
        branch = *current;
    }
    else
    {
        branch.id = juce::Uuid().toString();
        branch.name = "main";
        branch.colour = getTrackColours()[1].colour;
        branch.changed = juce::Time::currentTimeMillis();
    }

    branch.computer = getThisComputer();
    const auto mergesAnother = merging.has_value() && merging->version.isNotEmpty();

    VersionRecord version;
    version.name = name.trim();
    version.note = note.trim();
    version.summary =
        branch.head.isEmpty() ? juce::String ("Initial commit") : summarise (commitsStaged ? getStagedChanges() : getChanges());
    version.saved = juce::Time::currentTimeMillis();
    version.branch = branch.id;
    version.phazite = getVersionString();

    if (branch.head.isNotEmpty())
        version.parents.add (branch.head);

    // A merge's commit follows both lines, and is named as git names one while it has no name of its own: "Merge branch
    // 'idea-2'" for a branch's last commit, else "Merge commit '1a2b3c4'".
    if (merging.has_value() && merging->version.isNotEmpty() && ! version.parents.contains (merging->version))
    {
        version.parents.add (merging->version);
        version.summary = "Merge " + describeCommit (merging->version);
        version.renumbered = merging->renumbered;
    }

    saving = true;
    tellNow(); // "Saving..."

    run (
        [store = store, number = generation, copy, parameters, committed, version, branch, mergesAnother,
         projectFolder = song.getFile().getParentDirectory(), weak = juce::WeakReference<VersionHistory> (this), done]() mutable
        {
            const auto pieces = splitSong (copy, parameters);
            version.files = store->hashFiles (findAudioFilePaths (copy), projectFolder);
            version.id = store->writeVersion (version, pieces);
            version.song = pieces.song;
            auto ok = version.id.isNotEmpty();

            if (ok)
            {
                const auto before = branch.head;
                branch.head = version.id;
                branch.working = {};
                branch.divergent.clear(); // a merge's commit follows the other line; any other leaves it as it was
                ok = store->writeBranch (branch) && store->writeCurrentBranch (branch.id);
                store->logBranchMove (branch.id, before, version.id, mergesAnother ? "merge" : "commit");
            }

            store->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, ok, version, branch, copy, parameters, committed, done]() mutable
                {
                    if (auto* self = weak.get())
                        self->saved (number, ok, version, branch, copy, std::move (parameters), std::move (committed));

                    if (done != nullptr)
                        done (ok ? juce::Result::ok()
                                 : juce::Result::fail ("Phazite couldn't write the commit into the project's Versions folder."));
                });
        });

    return true;
}

void VersionHistory::saved (int number, bool ok, const VersionRecord& version, const BranchRecord& branch, const juce::ValueTree& copy,
                            PluginParameterValues parameters, ReportCounts committed)
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
    merging.reset();         // its version finishes it
    comparedVersion.clear(); // the changes since the new version
    comparedSong = {};

    if (auto found = std::find_if (branches.begin(), branches.end(), [&branch] (const auto& b) { return b.id == branch.id; });
        found != branches.end())
        *found = branch;
    else
        branches.push_back (branch);

    setBaseline (copy, std::move (parameters), std::move (committed));
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
void VersionHistory::setBaseline (juce::ValueTree baselineSong, PluginParameterValues parameters, ReportCounts reported)
{
    // Nothing staged: the staged song is the last commit's.
    baseline = std::move (baselineSong);
    baselineParameters = std::move (parameters);
    reportedAtBaseline = std::move (reported);
    staged = baseline.isValid() ? baseline.createCopy() : juce::ValueTree();
    stagedParameters = baselineParameters;
    reportedAtStaged = reportedAtBaseline;
    stagedChanged();
}

std::set<std::uint64_t> VersionHistory::findReported (const ReportCounts& then, const ReportCounts& now)
{
    const auto countIn = [] (const ReportCounts& counts, std::uint64_t device)
    {
        const auto found = counts.find (device);
        return found != counts.end() ? found->second : 0;
    };

    std::set<std::uint64_t> reported;

    for (const auto* counts : { &then, &now })
        for (const auto& [device, count] : *counts)
            if (countIn (then, device) != countIn (now, device))
                reported.insert (device);

    return reported;
}

void VersionHistory::stagedChanged()
{
    stagedTracks = indexTracks (staged);
    stagedReady = false;
    markAllChanged();
}

juce::String VersionHistory::describeCommit (const juce::String& versionID) const
{
    for (const auto& branch : branches)
        if (branch.head == versionID && branch.id != currentBranch)
            return "branch '" + branch.name + "'";

    return "commit '" + versionID.substring (0, 7) + "'";
}

const std::vector<SongChange>& VersionHistory::getStagedChanges()
{
    if (! stagedReady)
    {
        const auto reported = findReported (reportedAtBaseline, reportedAtStaged);
        stagedChanges = hasRead && baseline.isValid() && staged.isValid()
                            ? findChanges (baseline, staged, { &baselineParameters, &stagedParameters, &reported })
                            : std::vector<SongChange>();
        stagedReady = true;
    }

    return stagedChanges;
}

void VersionHistory::takeLine (const SongChange& line, const juce::ValueTree& from, const PluginParameterValues& fromParameters,
                               const ReportCounts& fromReported)
{
    // The line's part of the staged song made what it is in from, and its plug-ins' knobs taken with it.
    const auto findIn = [&line] (const juce::ValueTree& songTree)
    {
        if (line.where == SongChange::Where::master)
            return songTree.getChildWithName (schema::MASTER);

        for (const auto& track : getTrackElements (songTree))
            if (readID (track[schema::trackID]) == line.trackID)
                return track;

        return juce::ValueTree();
    };

    auto devices = getAutomatableIDs (findIn (staged));
    devices.merge (getAutomatableIDs (findIn (from)));

    if (line.where == SongChange::Where::song)
        makeSongOwnLike (staged, from);
    else
        makeTrackLike (staged, TrackID::fromRaw (line.trackID), from);

    for (const auto device : devices)
    {
        if (const auto found = fromParameters.find (device); found != fromParameters.end())
            stagedParameters[device] = found->second;
        else
            stagedParameters.erase (device);

        if (const auto found = fromReported.find (device); found != fromReported.end())
            reportedAtStaged[device] = found->second;
        else
            reportedAtStaged.erase (device);
    }

    stagedChanged();
    tellNow();
}

bool VersionHistory::stage (const SongChange& line)
{
    if (! hasRead || ! staged.isValid())
        return false;

    // Its plug-ins' own settings as they are now go in with it.
    song.storeDeviceStates();
    takeLine (line, song.getState(), readPluginParameters(), song.getDeviceChangesOutsideHistory());
    return true;
}

bool VersionHistory::unstage (const SongChange& line)
{
    if (! hasRead || ! staged.isValid() || ! baseline.isValid())
        return false;

    takeLine (line, baseline, baselineParameters, reportedAtBaseline);
    return true;
}

void VersionHistory::stageAll()
{
    if (! hasRead || ! staged.isValid())
        return;

    song.storeDeviceStates();
    staged = song.getState().createCopy();
    stagedParameters = readPluginParameters();
    reportedAtStaged = song.getDeviceChangesOutsideHistory();
    stagedChanged();
    tellNow();
}

void VersionHistory::unstageAll()
{
    if (! hasRead || ! baseline.isValid())
        return;

    staged = baseline.createCopy();
    stagedParameters = baselineParameters;
    reportedAtStaged = reportedAtBaseline;
    stagedChanged();
    tellNow();
}

const std::vector<SongChange>& VersionHistory::getChanges()
{
    takeChangesOutsideHistory();

    if (changesReady)
        return changes;

    changesReady = true;
    changes.clear();

    if (! hasRead || ! staged.isValid())
        return changes;

    const auto now = song.getState();
    const auto current = indexTracks (now);
    const auto masterBefore = staged.getChildWithName (schema::MASTER), masterNow = now.getChildWithName (schema::MASTER);
    const auto masterID = readID (masterNow[schema::trackID]);

    if (allDirty)
    {
        trackChanges.clear();
        dirtyTracks.clear();

        for (const auto& [id, track] : current)
            dirtyTracks.insert (id);

        for (const auto& [id, track] : stagedTracks)
            dirtyTracks.insert (id);

        dirtyTracks.insert (masterID);
        songDirty = automationDirty = true;
        allDirty = false;
    }

    const auto findElements = [&] (std::uint64_t id) -> std::pair<juce::ValueTree, juce::ValueTree>
    {
        if (id == masterID)
            return { masterBefore, masterNow };

        const auto before = stagedTracks.find (id);
        const auto after = current.find (id);
        return { before != stagedTracks.end() ? before->second : juce::ValueTree(),
                 after != current.end() ? after->second : juce::ValueTree() };
    };

    if (! dirtyTracks.empty())
    {
        const auto parameters = readPluginParameters();
        const auto reported = findReported (reportedAtStaged, song.getDeviceChangesOutsideHistory());
        const PluginValues values { &stagedParameters, &parameters, &reported };

        for (const auto id : dirtyTracks)
        {
            const auto [before, after] = findElements (id);

            if (! before.isValid() && ! after.isValid())
            {
                trackChanges.erase (id);
                continue;
            }

            if (auto change = findTrackChange (before, after, staged, now, values);
                change.kind != SongChange::Kind::changed || ! change.what.isEmpty())
                trackChanges[id] = std::move (change);
            else
                trackChanges.erase (id);
        }

        dirtyTracks.clear();
    }

    if (songDirty)
    {
        songChange = findSongChange (staged, now);
        songDirty = false;
    }

    if (automationDirty)
    {
        automated = findAutomatedDevicesChanged (staged, now);
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

        auto line = found != trackChanges.end() ? found->second : findTrackChange (before, after, staged, now);

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

    for (const auto& track : getTrackElements (staged))
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
    return ! getChanges().empty() || ! getStagedChanges().empty();
}

//==============================================================================
void VersionHistory::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! schema::isCounted (tree, property))
        return;

    edited();

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

    edited();

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
    {
        edited();
        markAllChanged();
    }
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
    changesReady = comparedReady = false;
    notifyPending = true;
}

void VersionHistory::edited()
{
    // What is heard is what is shown: an edit is heard as it is made, in the song.
    if (! hearingBounce)
        return;

    hearingBounce = false;

    if (auto* session = findSession (song))
        if (auto* comparison = session->getPlayer().getComparisonState())
            comparison->hear (false);

    changed();
}

//==============================================================================
juce::File VersionHistory::getBounceFile (const juce::String& versionID) const
{
    return store != nullptr && versionID.isNotEmpty() ? store->getBounceFile (versionID) : juce::File();
}

void VersionHistory::readBounces()
{
    bounces.clear();
    bouncesSize = 0;
    std::erase_if (bouncesGoing, [this] (const juce::String& id) { return ! getBounceFile (id).existsAsFile(); });

    if (store != nullptr)
    {
        // Those of versions the history has (not a render cut off midway, which left its temporary file).
        for (const auto& file : store->getBouncesFolder().findChildFiles (juce::File::findFiles, false, "*.flac"))
        {
            if (const auto id = file.getFileNameWithoutExtension(); versions.count (id) > 0 && bouncesGoing.count (id) == 0)
            {
                bounces.insert (id);
                bouncesSize += file.getSize();
            }
        }
    }

    tellNow();
}

void VersionHistory::compareWith (const juce::String& versionID)
{
    // The last version is the changes' own; another is read once, to list what differs from it while the song changes.
    const auto* last = findLastVersion();
    const auto other = versionID.isNotEmpty() && (last == nullptr || last->id != versionID) ? versionID : juce::String();

    if (other == comparedVersion)
        return;

    comparedVersion = other;
    comparedParameters.clear();
    comparedSong = other.isNotEmpty() ? readSong (other, &comparedParameters) : juce::ValueTree();

    if (other.isNotEmpty() && ! comparedSong.isValid())
        comparedVersion.clear(); // it can't be read

    selectedChange.reset();
    tellNow();
}

const std::vector<SongChange>& VersionHistory::getListedChanges()
{
    if (comparedVersion.isEmpty())
        return getChanges();

    takeChangesOutsideHistory();

    if (! comparedReady)
    {
        const auto parameters = readPluginParameters();
        comparedChanges = findChanges (comparedSong, song.getState(), { &comparedParameters, &parameters });
        comparedReady = true;
    }

    return comparedChanges;
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
        edited();
        markAllChanged();
    }
}

void VersionHistory::timerCallback()
{
    followFolder();
    takeChangesOutsideHistory();

    // An old version's bounce goes on, its progress shown, unless recording starts: it never runs while the song records.
    if (backgroundBounce != nullptr)
    {
        if (isRecording (song))
        {
            const auto* version = findVersion (bouncingVersion);
            bounceProblem = "Bouncing \"" + (version != nullptr ? getDisplayName (*version) : juce::String()) + "\" stopped for recording.";
            backgroundBounce = nullptr;
            bouncingVersion.clear();
            readBounces();
        }

        changed();
    }

    if (std::exchange (notifyPending, false) && onChange != nullptr)
        onChange();
}

//==============================================================================
juce::String VersionHistory::makeBranch (const juce::String& fromVersion, bool makeCurrent)
{
    followFolder();

    if (store == nullptr || ! hasRead || saving)
        return {};

    const auto* current = findCurrentBranch();
    const auto head = fromVersion.isNotEmpty() ? fromVersion : current != nullptr ? current->head : juce::String();

    if (findVersion (head) == nullptr)
        return {};

    // "idea-2", the next number no branch has, in the next color of the tracks' palette (main took the second).
    const auto isTaken = [this] (const juce::String& name)
    {
        return std::any_of (branches.begin(), branches.end(), [&name] (const auto& branch) { return branch.name.equalsIgnoreCase (name); });
    };

    auto number = 2;

    while (isTaken ("idea-" + juce::String (number)))
        ++number;

    BranchRecord branch;
    branch.id = juce::Uuid().toString();
    branch.name = "idea-" + juce::String (number);
    branch.colour = getTrackColours()[(branches.size() + 1) % getTrackColours().size()].colour;
    branch.head = head;
    branch.computer = getThisComputer();
    branch.changed = juce::Time::currentTimeMillis();

    if (! store->writeBranch (branch) || (makeCurrent && ! store->writeCurrentBranch (branch.id)))
        return {};

    store->logBranchMove (branch.id, {}, head, "branch");

    branches.push_back (branch);

    if (makeCurrent)
        currentBranch = branch.id; // the same last version: the changes since stay as they are

    tellNow();
    return branch.id;
}

juce::String VersionHistory::makeBranchWithSong (const juce::String& fromVersion, const juce::ValueTree& branchSong)
{
    const auto id = makeBranch (fromVersion, false);

    if (id.isEmpty())
        return {};

    const auto pieces = splitSong (branchSong.createCopy());

    if (! store->writeSong (pieces))
        return {};

    for (auto& branch : branches)
    {
        if (branch.id == id)
        {
            auto withSong = branch;
            withSong.working = pieces.song;

            if (! store->writeBranch (withSong))
                return {};

            branch = withSong;
        }
    }

    return id;
}

juce::ValueTree VersionHistory::leaveFor (const juce::String& branchID)
{
    followFolder();
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });
    const auto current =
        std::find_if (branches.begin(), branches.end(), [this] (const auto& branch) { return branch.id == currentBranch; });

    if (store == nullptr || ! hasRead || saving || found == branches.end() || current == branches.end() || branchID == currentBranch)
        return {};

    // The song as it is now, its view state and unsaved changes too, its plug-ins' settings stored, on the branch it leaves.
    song.storeDeviceStates();
    const auto pieces = splitSong (song.getState().createCopy(), readPluginParameters());

    if (! store->writeSong (pieces))
        return {};

    auto left = *current;
    left.working = pieces.song;

    // The other's, as it was left there, or as its last version had it.
    const auto* head = findVersion (found->head);
    auto other = found->working.isNotEmpty() ? store->readSong (found->working)
                 : head != nullptr           ? store->readSong (head->song)
                                             : juce::ValueTree();
    store->getPieces().closeFiles();

    if (! other.isValid() || ! store->writeBranch (left))
        return {};

    auto entered = *found;
    entered.working = {};

    if (! store->writeBranch (entered) || ! store->writeCurrentBranch (entered.id))
        return {};

    *current = left;
    *found = entered;
    currentBranch = entered.id;
    merging.reset();
    preview.reset();
    return other;
}

bool VersionHistory::switchTo (const juce::String& branchID)
{
    // A merge waiting for its version stays with the branch it was made on.
    const auto leaving = currentBranch;
    auto waiting = std::move (merging);
    merging.reset();
    const auto other = leaveFor (branchID);

    if (! other.isValid())
    {
        merging = std::move (waiting);
        return false;
    }

    if (waiting.has_value())
        mergesWaiting.insert_or_assign (leaving, std::move (*waiting));

    if (const auto kept = mergesWaiting.find (currentBranch); kept != mergesWaiting.end())
    {
        merging = std::move (kept->second);
        mergesWaiting.erase (kept);
    }

    // The other branch's song, in place; its changes since its last version from here on.
    makeSongTheSame (song, other);
    const auto* last = findLastVersion();
    PluginParameterValues parameters;
    auto lastSong = last != nullptr ? readSong (last->id, &parameters) : juce::ValueTree();
    setBaseline (std::move (lastSong), std::move (parameters), song.getDeviceChangesOutsideHistory());
    comparedVersion.clear();
    comparedSong = {};
    selectedChange.reset();
    tellNow();
    return true;
}

bool VersionHistory::renameBranch (const juce::String& branchID, const juce::String& name)
{
    // As git names a branch: no spaces (a dash in their place, as VS Code puts one), nor what refs can't hold.
    const auto newName = toBranchName (name);
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });
    const auto taken = std::any_of (branches.begin(), branches.end(), [&branchID, &newName] (const auto& branch)
                                    { return branch.id != branchID && branch.name.equalsIgnoreCase (newName); });

    if (store == nullptr || found == branches.end() || newName.isEmpty() || taken)
        return false;

    auto renamed = *found;
    renamed.name = newName;
    renamed.changed = juce::Time::currentTimeMillis(); // the newest name, on every computer

    if (! store->writeBranch (renamed))
        return false;

    *found = renamed;
    tellNow();
    return true;
}

juce::String VersionHistory::findCommonVersion (const juce::String& a, const juce::String& b) const
{
    return findMergeBase (versions, a, b);
}

std::vector<RenumberedID> VersionHistory::findRenumbering (const juce::String& ours, const juce::String& theirs,
                                                           const juce::String& parted) const
{
    return phazite::findRenumbering (versions, ours, theirs, parted);
}

std::vector<RenumberedID> findRenumbering (const std::map<juce::String, VersionRecord>& versions, const juce::String& ours,
                                           const juce::String& theirs, const juce::String& parted)
{
    // This line's merge commits since the lines parted that merged in a version the other line's follows, oldest first.
    std::vector<const VersionRecord*> merges;
    std::set<juce::String> seen;
    juce::StringArray toVisit (ours);

    while (! toVisit.isEmpty())
    {
        const auto id = toVisit[toVisit.size() - 1];
        toVisit.remove (toVisit.size() - 1);
        const auto found = versions.find (id);
        const auto* version = found != versions.end() ? &found->second : nullptr;

        if (version == nullptr || ! seen.insert (id).second || (parted.isNotEmpty() && follows (versions, parted, id)))
            continue;

        if (version->parents.size() > 1 && ! version->renumbered.empty() && follows (versions, theirs, version->parents[1]))
            merges.push_back (version);

        toVisit.addArray (version->parents);
    }

    std::sort (merges.begin(), merges.end(), [] (const auto* a, const auto* b) { return a->saved < b->saved; });
    std::vector<RenumberedID> renumbered;

    for (const auto* merge : merges)
        renumbered.insert (renumbered.end(), merge->renumbered.begin(), merge->renumbered.end());

    return renumbered;
}

std::pair<juce::ValueTree, juce::String> readLastCommitSong (const juce::File& projectFile)
{
    const VersionStore store (projectFile.getSiblingFile ("Versions"), getThisComputer());
    const auto records = store.readBranches();
    const auto current = store.readCurrentBranch();

    // The versions every record's last one follows, read; then the branches taken together, the current one's last.
    std::map<juce::String, VersionRecord> versions;
    juce::StringArray toRead;

    for (const auto& record : records)
        toRead.add (record.head);

    while (! toRead.isEmpty())
    {
        const auto id = toRead[toRead.size() - 1];
        toRead.remove (toRead.size() - 1);

        if (id.isNotEmpty() && versions.count (id) == 0)
        {
            if (auto version = store.readVersion (id))
            {
                toRead.addArray (version->parents);
                versions.emplace (id, std::move (*version));
            }
        }
    }

    for (const auto& branch : joinBranchRecords (records, versions, store.getComputer()))
        if (branch.id == current)
            if (const auto found = versions.find (branch.head); found != versions.end())
                return { store.readSong (found->second.song), getDisplayName (found->second) };

    return {};
}

bool follows (const std::map<juce::String, VersionRecord>& versions, const juce::String& later, const juce::String& earlier)
{
    std::set<juce::String> seen;
    juce::StringArray toVisit (later);

    while (! toVisit.isEmpty())
    {
        const auto id = toVisit[toVisit.size() - 1];
        toVisit.remove (toVisit.size() - 1);

        if (id == earlier)
            return true;

        if (const auto version = versions.find (id); version != versions.end() && seen.insert (id).second)
            toVisit.addArray (version->second.parents);
    }

    return false;
}

std::vector<BranchRecord> joinBranchRecords (const std::vector<BranchRecord>& records,
                                             const std::map<juce::String, VersionRecord>& versions, const juce::String& computer)
{
    std::map<juce::String, std::vector<const BranchRecord*>> byBranch;

    for (const auto& record : records)
        byBranch[record.id].push_back (&record);

    const auto legalComputer = juce::File::createLegalFileName (computer);
    std::vector<BranchRecord> joined;

    for (const auto& [id, group] : byBranch)
    {
        // The newest name and color (this computer's, of two set at once).
        const BranchRecord* newest = nullptr;
        const BranchRecord* mine = nullptr;

        for (const auto* record : group)
        {
            const auto isMine = record->computer == legalComputer;

            if (isMine)
                mine = record;

            if (newest == nullptr || record->changed > newest->changed || (record->changed == newest->changed && isMine))
                newest = record;
        }

        auto branch = *newest;
        branch.computer = computer;
        branch.working = mine != nullptr ? mine->working : juce::String();
        branch.divergent.clear();

        // The last version that follows every computer's, else this computer's own.
        juce::StringArray heads;

        for (const auto* record : group)
            if (versions.count (record->head) > 0)
                heads.addIfNotAlreadyThere (record->head);

        juce::String head;

        for (const auto& candidate : heads)
            if (std::all_of (heads.begin(), heads.end(), [&] (const juce::String& other) { return follows (versions, candidate, other); }))
                head = candidate;

        if (head.isEmpty())
        {
            head = mine != nullptr && versions.count (mine->head) > 0 ? mine->head : newest->head;

            for (const auto& other : heads)
                if (! follows (versions, head, other))
                    branch.divergent.add (other);
        }

        branch.head = head;
        joined.push_back (std::move (branch));
    }

    return joined;
}

juce::String findMergeBase (const std::map<juce::String, VersionRecord>& versions, const juce::String& a, const juce::String& b)
{
    // Every version one is or follows, of those whose records are known.
    const auto ancestors = [&versions] (const juce::String& from)
    {
        std::set<juce::String> found;
        juce::StringArray toVisit (from);

        while (! toVisit.isEmpty())
        {
            const auto id = toVisit[toVisit.size() - 1];
            toVisit.remove (toVisit.size() - 1);

            if (const auto version = versions.find (id); version != versions.end() && found.insert (id).second)
                toVisit.addArray (version->second.parents);
        }

        return found;
    };

    const auto ofA = ancestors (a);
    std::set<juce::String> common;

    for (const auto& id : ancestors (b))
        if (ofA.count (id) > 0)
            common.insert (id);

    // The ones no other common version follows. A version that another common one follows is itself the parent of a common
    // one, as every version a common one follows is common too.
    auto best = common;

    for (const auto& id : common)
        for (const auto& parent : versions.at (id).parents)
            best.erase (parent);

    juce::String chosen;
    juce::int64 chosenSaved = std::numeric_limits<juce::int64>::min();

    for (const auto& id : best)
    {
        if (const auto saved = versions.at (id).saved; saved > chosenSaved || (saved == chosenSaved && id > chosen))
        {
            chosen = id;
            chosenSaved = saved;
        }
    }

    return chosen;
}
} // namespace phazite
