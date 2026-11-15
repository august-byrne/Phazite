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
#include <cmath>
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

    // The versions the branches' lines hold: each branch's last version and those it follows, and the lines of it that went
    // their own way on other computers. What a deleted branch held alone, or a commit undone, isn't among them.
    void keepReachable (std::map<juce::String, VersionRecord>& versions, const std::vector<BranchRecord>& branches)
    {
        std::set<juce::String> reachable;
        juce::StringArray toVisit;

        for (const auto& branch : branches)
        {
            toVisit.add (branch.head);
            toVisit.addArray (branch.divergent);
        }

        while (! toVisit.isEmpty())
        {
            const auto id = toVisit[toVisit.size() - 1];
            toVisit.remove (toVisit.size() - 1);

            if (id.isEmpty() || ! reachable.insert (id).second)
                continue;

            if (const auto found = versions.find (id); found != versions.end())
                toVisit.addArray (found->second.parents);
        }

        std::erase_if (versions, [&reachable] (const auto& entry) { return reachable.count (entry.first) == 0; });
    }

    // The plug-ins' stored settings in a song, by their devices' IDs.
    std::map<std::uint64_t, juce::String> getPluginStates (const juce::ValueTree& song)
    {
        std::vector<juce::ValueTree> plugins;
        findPlugins (song, plugins);
        std::map<std::uint64_t, juce::String> states;

        for (const auto& plugin : plugins)
            states[readID (plugin[schema::deviceID])] = plugin[schema::pluginState].toString();

        return states;
    }

    // A commit's song, without a merge's element (Schema.h: MERGING), which no commit's song holds, whoever wrote it: what
    // the song is made like (a checkout, a switch, Bring Back) never brings one.
    juce::ValueTree readCommitSong (const VersionStore& store, const juce::String& songPiece, PluginParameterValues* parameters = nullptr,
                                    std::vector<MissingPiece>* missing = nullptr)
    {
        auto read = store.readSong (songPiece, parameters, missing);
        read.removeChild (read.getChildWithName (schema::MERGING), nullptr);
        return read;
    }

    // A commit's or a piece's id: 64 lowercase hexadecimal digits.
    bool isPieceID (const juce::String& text)
    {
        return text.length() == 64 && text.containsOnly ("0123456789abcdef");
    }

    // An ID as a MERGING element writes it (Schema.h): decimal digits, no more than an ID can have.
    std::optional<std::uint64_t> readMergeID (const juce::String& text)
    {
        if (text.isEmpty() || text.length() > 18 || ! text.containsOnly ("0123456789"))
            return std::nullopt;

        const auto id = (std::uint64_t) text.getLargeIntValue();
        return id > 0 && id < schema::maxID ? std::optional (id) : std::nullopt;
    }

    // IDs given anew as a MERGING element writes them: "from>to", or "<group>/from>to" for a note, separated by commas.
    juce::String writeMergeIDs (std::vector<RenumberedID> ids)
    {
        std::sort (ids.begin(), ids.end(),
                   [] (const auto& a, const auto& b) { return a.clip != b.clip ? a.clip.compare (b.clip) < 0 : a.from < b.from; });
        juce::StringArray entries;

        for (const auto& id : ids)
            entries.add ((id.clip.isNotEmpty() ? id.clip + "/" : juce::String()) + juce::String ((juce::uint64) id.from) + ">"
                         + juce::String ((juce::uint64) id.to));

        return entries.joinIntoString (",");
    }

    std::optional<std::vector<RenumberedID>> readMergeIDs (const juce::String& text)
    {
        std::vector<RenumberedID> ids;

        for (const auto& entry : juce::StringArray::fromTokens (text, ",", {}))
        {
            const auto group = entry.containsChar ('/') ? entry.upToLastOccurrenceOf ("/", false, false) : juce::String();
            const auto pair = entry.fromLastOccurrenceOf ("/", false, false);
            const auto from = readMergeID (pair.upToFirstOccurrenceOf (">", false, false));
            const auto to = readMergeID (pair.fromFirstOccurrenceOf (">", false, false));
            const auto groupID = readMergeID (group.fromFirstOccurrenceOf (" ", false, false));

            if (! from.has_value() || ! to.has_value()
                || (group.isNotEmpty() && (! (group.startsWith ("clip ") || group.startsWith ("link ")) || ! groupID.has_value())))
                return std::nullopt;

            ids.push_back ({ group, *from, *to });
        }

        if (! areOneToOne (ids))
            return std::nullopt;

        return ids;
    }

    // What names a merge's element but for its choices, which change while it waits: by which the merge found is known again.
    juce::String getMergeKey (const juce::ValueTree& element)
    {
        // Its properties by their names, whatever their order (a song read back from its pieces holds them sorted).
        juce::StringArray properties;

        for (int i = 0; i < element.getNumProperties(); ++i)
            if (const auto name = element.getPropertyName (i); name != schema::mergingChosen)
                properties.add (name.toString() + "=" + element[name].toString());

        properties.sort (false);
        return properties.joinIntoString (";");
    }

    // Whether a song holds each item's ID once, and each clip each note's: as a commit's, translated through what a merge
    // waiting says (which a hand-made one could make hold one twice).
    bool holdsEachIDOnce (const juce::ValueTree& song)
    {
        std::set<std::uint64_t> items;
        auto once = true;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::CLIP))
                {
                    std::set<std::uint64_t> notes;

                    for (const auto& note : child)
                        if (note.hasType (schema::NOTE))
                            once = once && notes.insert (readID (note[schema::noteID])).second;
                }

                if (! child.hasType (schema::NOTE) && child.hasProperty (schema::trackID) && schema::holdsID (child, schema::trackID))
                    once = once && items.insert (readID (child[schema::trackID])).second;

                visit (child);
            }
        };

        visit (song);
        return once;
    }
} // namespace

std::optional<MergeState> readMergeState (const juce::ValueTree& element)
{
    if (! element.hasType (schema::MERGING))
        return std::nullopt;

    MergeState state;
    state.version = element[schema::mergingVersion].toString();
    state.head = element[schema::mergingHead].toString();
    state.before = element[schema::mergingBefore].toString();

    if (! isPieceID (state.version) || ! isPieceID (state.head) || ! isPieceID (state.before))
        return std::nullopt;

    for (const auto& line : juce::StringArray::fromTokens (element[schema::mergingLines].toString(), " ", {}))
    {
        if (line == "song")
            state.taken.songOwn = true;
        else if (const auto id = readMergeID (line))
            state.taken.tracks.insert (*id);
        else
            return std::nullopt;
    }

    for (const auto chosen : element[schema::mergingChosen].toString())
    {
        if (chosen < '0' || chosen > '2')
            return std::nullopt;

        state.chosen.push_back ((int) (chosen - '0'));
    }

    const auto renumbered = readMergeIDs (element[schema::mergingRenumbered].toString());
    const auto translation = readMergeIDs (element[schema::mergingTranslated].toString());

    if (! renumbered.has_value() || ! translation.has_value())
        return std::nullopt;

    state.renumbered = *renumbered;
    state.translation = *translation;
    return state;
}

juce::ValueTree writeMergeState (const MergeState& state)
{
    juce::ValueTree element (
        schema::MERGING,
        { { schema::mergingVersion, state.version }, { schema::mergingHead, state.head }, { schema::mergingBefore, state.before } });
    juce::StringArray lines, chosen;

    if (state.taken.songOwn)
        lines.add ("song");

    for (const auto id : state.taken.tracks)
        lines.add (juce::String ((juce::uint64) id));

    for (const auto value : state.chosen)
        chosen.add (juce::String (value));

    if (! lines.isEmpty())
        element.setProperty (schema::mergingLines, lines.joinIntoString (" "), nullptr);

    if (! chosen.isEmpty())
        element.setProperty (schema::mergingChosen, chosen.joinIntoString ({}), nullptr);

    if (! state.renumbered.empty())
        element.setProperty (schema::mergingRenumbered, writeMergeIDs (state.renumbered), nullptr);

    if (! state.translation.empty())
        element.setProperty (schema::mergingTranslated, writeMergeIDs (state.translation), nullptr);

    return element;
}

VersionHistory::Merge findMergeAgain (const VersionStore& store, const std::map<juce::String, VersionRecord>& versions,
                                      const MergeState& state)
{
    VersionHistory::Merge merge;
    merge.version = state.version;
    merge.head = state.head;
    merge.taken = state.taken;
    merge.translation = state.translation;
    merge.renumbered = state.renumbered;
    merge.chosen = state.chosen;
    merge.beforePiece = state.before;

    // The song before it, whatever else can be read, for Abort Merge: a whole song, never a piece of another kind (a
    // track's) named here, which would make Abort Merge empty the song.
    if (auto before = store.readSong (state.before); before.hasType (schema::PHAZITE))
        merge.before = std::move (before);

    // The parting's and the other line's, translated as they were, each a whole song holding each ID once.
    const auto merged = versions.find (state.version);

    if (! merge.before.isValid() || merged == versions.end() || versions.count (state.head) == 0)
        return merge;

    const auto parted = findMergeBase (versions, state.head, state.version);
    const auto partedFound = versions.find (parted);
    merge.base = parted.isEmpty()                ? juce::ValueTree (schema::PHAZITE)
                 : partedFound != versions.end() ? store.readSong (partedFound->second.song)
                                                 : juce::ValueTree();
    merge.theirs = store.readSong (merged->second.song, &merge.theirParameters);

    for (auto* song : { &merge.base, &merge.theirs })
    {
        song->removeChild (song->getChildWithName (schema::MERGING), nullptr); // no part of a commit's song
        translateIDs (*song, merge.translation);
    }

    if (! merge.base.hasType (schema::PHAZITE) || ! merge.theirs.hasType (schema::PHAZITE) || ! holdsEachIDOnce (merge.base)
        || ! holdsEachIDOnce (merge.theirs))
    {
        merge.base = merge.theirs = {};
        return merge;
    }

    merge.theirParameters = translateKnobs (std::move (merge.theirParameters), merge.translation);

    // Its clashes and notes, as merging the same three songs gave them when it was taken; the IDs this merge gives anew are
    // of no account here, and none of them reaches the song.
    auto next = juce::jmax (findNextID (merge.before), findNextID (merge.theirs));
    const auto result = mergeSongs (merge.base, merge.before, merge.theirs, merge.taken, [&next] { return next++; });
    merge.clashes = result.clashes;
    merge.notes = result.notes;
    merge.readWhole = true;
    return merge;
}

juce::File getVersionsFolder (const Song& song)
{
    return song.getFile() != juce::File() ? song.getFile().getSiblingFile ("Versions") : juce::File();
}

juce::String getDisplayName (const VersionRecord& version)
{
    return version.name.isNotEmpty() ? version.name : version.summary;
}

void nameKnobs (std::vector<MergeClash>& clashes, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                const PluginParameterValues& ourValues, const PluginParameterValues& theirValues)
{
    for (auto& clash : clashes)
    {
        if (clash.property != schema::pluginState)
            continue;

        const auto ourDevice = findByPath (ours, clash.path), theirDevice = findByPath (theirs, clash.path);
        const auto id = readID ((ourDevice.isValid() ? ourDevice : theirDevice)[schema::deviceID]);
        juce::StringArray details;

        if (const auto mine = ourValues.find (id), other = theirValues.find (id); mine != ourValues.end() && other != theirValues.end())
        {
            std::map<juce::String, float> there;

            for (const auto& parameter : other->second)
                there.emplace (parameter.id, parameter.value);

            juce::StringArray knobs;

            for (const auto& parameter : mine->second)
                if (const auto found = there.find (parameter.id);
                    found != there.end() && std::abs (found->second - parameter.value) > 0.0005f)
                    knobs.add (parameter.name);

            if (knobs.size() > 3)
                details.add (knobs[0] + ", " + knobs[1] + " and " + juce::String (knobs.size() - 2) + " more");
            else if (! knobs.isEmpty())
                details.add (knobs.joinIntoString (", "));
        }

        const auto ourVersion = ourDevice[schema::pluginVersion].toString();
        const auto theirVersion = theirDevice[schema::pluginVersion].toString();

        if (ourVersion.isNotEmpty() && theirVersion.isNotEmpty() && ourVersion != theirVersion)
            details.add ("version " + ourVersion + " here, " + theirVersion + " there");

        if (! details.isEmpty())
            clash.what += ": " + details.joinIntoString ("; ");
    }
}

//==============================================================================
VersionHistory::VersionHistory (Song& s, std::function<void()> changeCallback)
    : song (s),
      state (s.getState()),
      onChange (std::move (changeCallback)),
      thread ("Phazite versions", 1, juce::Thread::Priority::normal)
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
    thread.removeAll (false); // a version being written is finished, never cut short, however long that takes
}

bool VersionHistory::keepsVersions() const
{
    return store != nullptr;
}

void VersionHistory::run (std::function<void()> job)
{
    ++pendingJobs;
    thread.add ([work = std::move (job)] (const BackgroundJobs::ShouldStop&) { work(); });
}

void VersionHistory::waitForThread() const
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Its jobs tell the message thread what they did with callAsync and never wait for it, so it can wait for them,
    // however long they take: a folder copied while a job still writes into it wouldn't be whole.
    thread.waitForAll();
}

void VersionHistory::open()
{
    ++generation;
    folder = getVersionsFolder (song);
    store = folder != juce::File() ? std::make_shared<VersionStore> (folder, getThisComputer()) : nullptr;
    branches.clear();
    versions.clear();
    versionChanges.clear();
    messageEdits.clear();
    snapshots.clear();
    selectedSnapshot = 0;
    editedSinceSnapshot = false;
    currentBranch.clear();
    setBaseline ({}, {}, {});
    hasRead = store == nullptr;

    if (store == nullptr)
        return;

    // Its branches, every version their last ones follow, and the current branch's last version's song; and the merge the
    // song holds, if any, found again here rather than on the message thread.
    run (
        [shared = store, number = generation, weak = juce::WeakReference<VersionHistory> (this),
         mergingElement = song.getState().getChildWithName (schema::MERGING).createCopy()]
        {
            // Its packs joined into one once there are many, as each is read as the store opens.
            if (shared->getPieces().getNumPacks() > maxPacks)
                shared->getPieces().joinPacks();

            // Every computer's record of each branch, and every version their last ones follow; then each branch as this
            // computer takes them together.
            const auto records = shared->readBranches();
            auto current = shared->readCurrentBranch();
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

                if (auto version = shared->readVersion (id))
                {
                    toRead.addArray (version->parents);
                    versionsRead.emplace (id, std::move (*version));
                }
            }

            auto branchesRead = joinBranchRecords (records, versionsRead, shared->getComputer());
            keepReachable (versionsRead, branchesRead); // not what deleted branches alone held
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
                        head = readCommitSong (*shared, found->second.song, &parameters);

            std::optional<std::pair<juce::String, Merge>> mergeRead;

            if (const auto state = readMergeState (mergingElement))
                mergeRead.emplace (getMergeKey (mergingElement), findMergeAgain (*shared, versionsRead, *state));

            shared->getPieces().closeFiles();
            auto messages = shared->readMessages();
            auto snapshotsRead = shared->readSnapshots();

            juce::MessageManager::callAsync (
                [weak, number, branchesRead, current, versionsRead, head, parameters, messages, snapshotsRead, mergeRead]() mutable
                {
                    if (auto* self = weak.get())
                        self->opened (number, std::move (branchesRead), current, std::move (versionsRead), head, std::move (parameters),
                                      std::move (messages), std::move (snapshotsRead), std::move (mergeRead));
                });
        });
}

void VersionHistory::opened (int number, std::vector<BranchRecord> branchesRead, const juce::String& current,
                             std::map<juce::String, VersionRecord> versionsRead, const juce::ValueTree& head,
                             PluginParameterValues parameters, std::map<juce::String, MessageEdit> messages,
                             std::vector<Snapshot> snapshotsRead, std::optional<std::pair<juce::String, Merge>> mergeRead)
{
    --pendingJobs;

    if (number != generation)
        return;

    branches = std::move (branchesRead);
    versions = std::move (versionsRead);
    messageEdits = std::move (messages);
    applyMessageEdits();
    snapshots.assign (snapshotsRead.rbegin(), snapshotsRead.rend());
    lastSnapshotTime = juce::Time::currentTimeMillis();
    currentBranch = current;
    setBaseline (head, std::move (parameters), song.getDeviceChangesOutsideHistory());

    // The merge the song holds, as found on the history's thread, its plug-ins' clashes named here.
    if (mergeRead.has_value())
        keepFound (mergeRead->first, std::move (mergeRead->second));

    mergeDirty = true;
    hasRead = true;
    getMerging();  // the merge the song holds, as read already, or read now
    readBounces(); // and tells

    // A project keeps versions from its start, as a repository from its first commit: the song as it is now, on main.
    if (versions.empty())
    {
        saveVersion ("Initial commit", {});
        return;
    }

    // Local History: the song as it opened, where it differs from its last commit (it was left with changes).
    if (hasChanges())
        takeSnapshot ("open");
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
    // Not this computer's record of a commit checked out, which is no branch.
    std::vector<BranchRecord> real;
    std::copy_if (branches.begin(), branches.end(), std::back_inserter (real),
                  [] (const auto& branch) { return branch.detachedFrom.isEmpty(); });
    return sortByFirstVersion (real, versions);
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

    auto read = readCommitSong (*store, version->song, parameters, missing);
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
        [shared = store, number = generation, known = versions, weak = juce::WeakReference<VersionHistory> (this), done]
        {
            // Every pack the folder holds now, those written since the history opened too.
            shared->getPieces().readNewPacks();

            Check check;
            const auto names = shared->getPieces().getNames();
            check.numPieces = names.size();
            check.numVersions = (int) known.size();

            // Every piece read, which checks it against its name, and the commits among them.
            std::vector<VersionRecord> found;

            for (const auto& name : names)
            {
                const auto bytes = shared->getPieces().read (name);

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
                shared->readSong (version.song, nullptr, &missing);

                if (! missing.empty())
                    check.damaged[id] = std::move (missing);
            }

            for (auto& record : found)
                if (known.count (record.id) == 0)
                    check.lost.push_back (std::move (record));

            std::sort (check.lost.begin(), check.lost.end(), [] (const auto& a, const auto& b) { return a.saved > b.saved; });
            shared->getPieces().closeFiles();

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

    applyMessageEdits(); // as their messages were edited, before they were lost

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

    if (isDetached())
        return refuse ("The song is on no branch while a commit is checked out: create a branch here to commit what changed.");

    // The staged song; or, with nothing staged or a merge being made, the song as a save stores it, its plug-ins' own
    // settings included, and their knobs' values.
    const auto commitsStaged = ! getMerging().has_value() && ! getStagedChanges().empty();
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

    // A merge waiting, finished, or of none (Schema.h: MERGING) is never part of a commit's song, whichever song it is from:
    // the staged one (Stage All takes the song whole) or the song as it is.
    copy.removeChild (copy.getChildWithName (schema::MERGING), nullptr);

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
    const auto& waiting = getMerging();
    const auto mergesAnother = waiting.has_value() && waiting->version.isNotEmpty();

    VersionRecord version;
    version.name = name.trim();
    version.note = note.trim();
    version.summary = branch.head.isEmpty() ? juce::String ("Initial commit") : summariseNextCommit();
    version.saved = juce::Time::currentTimeMillis();
    version.branch = branch.id;
    version.phazite = getVersionString();

    if (branch.head.isNotEmpty())
        version.parents.add (branch.head);

    // A merge's commit follows both lines (summariseNextCommit() names it as git does).
    if (waiting.has_value() && waiting->version.isNotEmpty() && ! version.parents.contains (waiting->version))
    {
        version.parents.add (waiting->version);
        version.renumbered = waiting->renumbered;
    }

    saving = true;
    tellNow(); // "Saving..."

    run (
        [shared = store, number = generation, copy, parameters, committed, version, branch, mergesAnother,
         projectFolder = song.getFile().getParentDirectory(), weak = juce::WeakReference<VersionHistory> (this), done]() mutable
        {
            const auto pieces = splitSong (copy, parameters);
            version.files = shared->hashFiles (findAudioFilePaths (copy), projectFolder);
            version.id = shared->writeVersion (version, pieces);
            version.song = pieces.song;
            auto ok = version.id.isNotEmpty();

            if (ok)
            {
                const auto before = branch.head;
                branch.head = version.id;
                branch.working = {};
                branch.divergent.clear(); // a merge's commit follows the other line; any other leaves it as it was
                ok = shared->writeBranch (branch) && shared->writeCurrentBranch (branch.id);
                shared->logBranchMove (branch.id, before, version.id, mergesAnother ? "merge" : "commit");
            }

            shared->getPieces().closeFiles();

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

juce::String VersionHistory::summariseNextCommit()
{
    // A merge's commit is named as git names one: "Merge branch 'idea-2'" for a branch's last commit, else "Merge commit
    // '1a2b3c4'". It records the song as it is, whatever is staged.
    if (getMerging().has_value())
    {
        const auto* last = findLastVersion();
        return getMerging()->version.isNotEmpty() && (last == nullptr || last->id != getMerging()->version)
                   ? "Merge " + describeCommit (getMerging()->version)
                   : summarise (getChanges());
    }

    const auto& stagedLines = getStagedChanges();
    return summarise (! stagedLines.empty() ? stagedLines : getChanges());
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

    mergeDirty = true;
    comparedVersion.clear(); // the changes since the new version
    comparedSong = {};

    // A/B hears the new commit as B from now on, unless another commit is being heard (getCompareVersion()).
    if (! hearingBounce)
        bounceCompared.clear();

    if (auto found = std::find_if (branches.begin(), branches.end(), [&branch] (const auto& b) { return b.id == branch.id; });
        found != branches.end())
        *found = branch;
    else
        branches.push_back (branch);

    setBaseline (copy, std::move (parameters), std::move (committed));
    tellNow();
}

const std::optional<VersionHistory::Merge>& VersionHistory::getMerging() const
{
    if (mergeDirty)
        findMerging();

    return merging;
}

const juce::String& VersionHistory::getMergeProblem() const
{
    if (mergeFailure.isNotEmpty())
        return mergeFailure;

    if (mergeDirty)
        findMerging();

    return mergeProblem;
}

const VersionRecord* VersionHistory::findOwnCommit (const MergeState& said) const
{
    // A commit of the branch's line that merged the commit in as its second parent, on top of where the merge was taken,
    // and recorded every ID it gave (and maybe more: those another computer's merge gave, as they were here).
    const auto* last = findLastVersion();

    if (last == nullptr)
        return nullptr;

    for (const auto& [id, version] : versions)
    {
        if (version.parents.size() < 2 || version.parents[1] != said.version || ! follows (versions, last->id, id)
            || ! follows (versions, version.parents[0], said.head))
            continue;

        const auto recorded = [&version] (const RenumberedID& given)
        { return std::find (version.renumbered.begin(), version.renumbered.end(), given) != version.renumbered.end(); };

        if (std::all_of (said.renumbered.begin(), said.renumbered.end(), recorded))
            return &version;
    }

    return nullptr;
}

void VersionHistory::keepFound (const juce::String& key, Merge merge) const
{
    // Its plug-ins' clashes named here, on the message thread; and a few kept.
    if (merge.readWhole)
        nameKnobs (merge.clashes, merge.before, merge.theirs, readPluginParameters(), merge.theirParameters);

    constexpr size_t mostKept = 4;

    if (mergesFound.size() >= mostKept && mergesFound.count (key) == 0)
        mergesFound.erase (mergesFound.begin());

    mergesFound.insert_or_assign (key, std::move (merge));
}

void VersionHistory::mergeReadAgain (int number, const juce::String& key, std::optional<Merge> merge)
{
    --pendingJobs;
    mergesBeingRead.erase (key);

    if (number != generation || ! merge.has_value())
        return;

    keepFound (key, std::move (*merge));
    mergeDirty = true;
    tellNow();
}

void VersionHistory::findMerging() const
{
    mergeDirty = false;
    merging.reset();
    mergeProblem = mergeFailure;
    const auto element = song.getState().getChildWithName (schema::MERGING);

    if (! element.isValid() || store == nullptr || ! hasRead || isDetached())
        return;

    const auto say = [this] (const juce::String& problem) { mergeProblem = mergeFailure.isNotEmpty() ? mergeFailure : problem; };

    // One that isn't one, or names a commit the history hasn't (a project opened without its Versions folder, whose
    // history starts anew): no merge waits, and Abort Merge takes the element out.
    const auto said = readMergeState (element);
    const auto* merged = said.has_value() ? findVersion (said->version) : nullptr;

    if (merged == nullptr)
    {
        say ("This song holds a merge that can't be read, which no commit will finish: Abort Merge takes it out of the song.");
        return;
    }

    // Finished by this branch's own commit of it: silently.
    if (findOwnCommit (*said) != nullptr)
        return;

    // Held by the branch otherwise: through another computer's merge of it (or several), which gave what it added other
    // IDs than this song's, it waits still, so that its commit records this song's IDs beside those (each the other merge
    // gave and this one didn't, as it is here); held without any such merge, it waits no more.
    const auto* last = findLastVersion();
    const auto name = "\"" + getDisplayName (*merged) + "\"";
    auto renumbered = said->renumbered;
    juce::String problem;

    if (last != nullptr && follows (versions, last->id, said->version))
    {
        std::vector<const VersionRecord*> others;

        for (const auto& [id, version] : versions)
            if (version.parents.size() > 1 && version.parents[1] == said->version && follows (versions, last->id, id))
                others.push_back (&version);

        if (others.empty())
        {
            say ("This branch holds " + name
                 + " already (merged on another computer): the merge of it this song took waits no "
                   "more. Abort Merge takes it out of the song; Discard All Changes makes the song the branch's.");
            return;
        }

        for (const auto* other : others)
            for (const auto& given : other->renumbered)
                if (std::none_of (renumbered.begin(), renumbered.end(),
                                  [&given] (const RenumberedID& id) { return id.clip == given.clip && id.from == given.from; }))
                    renumbered.push_back ({ given.clip, given.from, given.from });

        problem = "This branch holds " + name
                  + " already, merged on another computer, which gave what it added other IDs than "
                    "this song has: commit to finish this merge on top of it, so that later merges of that line know this song's.";
    }

    // Found where the history has it; else read on the history's thread, waiting meanwhile, its conflicts not shown yet.
    const auto key = getMergeKey (element);
    Merge merge;

    if (const auto found = mergesFound.find (key); found != mergesFound.end())
    {
        merge = found->second;
    }
    else
    {
        merge.version = said->version;
        merge.head = said->head;
        merge.taken = said->taken;
        merge.translation = said->translation;
        merge.beforePiece = said->before;
        merge.reading = true;

        if (mergesBeingRead.insert (key).second)
        {
            auto* self = const_cast<VersionHistory*> (this);
            self->run (
                [shared = store, versionsNow = versions, number = generation, key, readState = *said,
                 weak = juce::WeakReference<VersionHistory> (self)]
                {
                    auto again = std::optional<Merge> (findMergeAgain (*shared, versionsNow, readState));
                    shared->getPieces().closeFiles();

                    juce::MessageManager::callAsync (
                        [weak, number, key, again]() mutable
                        {
                            if (auto* history = weak.get())
                                history->mergeReadAgain (number, key, std::move (again));
                        });
                });
        }
    }

    merge.renumbered = renumbered;
    merge.chosen = said->chosen;
    merge.chosen.resize (merge.clashes.size(), 1);

    for (const auto& branch : branches)
        if (branch.id == merged->branch)
            merge.otherBranch = branch.name;

    if (problem.isEmpty() && merge.reading)
        problem = "Reading the merge of " + name + " from the Versions folder...";
    else if (problem.isEmpty() && ! merge.readWhole)
        problem = "Phazite can't read all of the merge of " + name + " from the Versions folder: its conflicts aren't shown, and "
                  + (merge.before.isValid() ? juce::String ("Abort Merge makes the song what it was before it.")
                                            : juce::String ("Abort Merge makes the song the branch's last commit."));
    else if (problem.isEmpty() && (last == nullptr || last->id != said->head))
        problem = "This branch moved on since the merge of " + name
                  + " was taken (a commit made since, here or on another "
                    "computer): its commit will follow the branch's last commit and the one merged in.";

    say (problem);
    merging = std::move (merge);
}

juce::ValueTree VersionHistory::keepMerge (const Merge& merge, std::vector<int> chosen)
{
    followFolder();

    if (store == nullptr)
        return {};

    // The song before it, in the store now, so that the song (and every file it is saved into) names a piece the store has.
    auto kept = merge;

    if (kept.beforePiece.isEmpty())
    {
        const auto pieces = splitSong (merge.before);

        if (! store->writeSong (pieces))
            return {};

        kept.beforePiece = pieces.song;
    }

    kept.readWhole = true;
    const auto element =
        writeMergeState ({ kept.version, kept.head, kept.beforePiece, kept.taken, std::move (chosen), kept.renumbered, kept.translation });
    mergesFound.insert_or_assign (getMergeKey (element), std::move (kept));
    mergeDirty = true;
    return element;
}

bool VersionHistory::holdsMergeElement() const
{
    return song.getState().getChildWithName (schema::MERGING).isValid();
}

bool VersionHistory::holdsCommitMergeWaits (const BranchRecord& branch) const
{
    // The commits merges waiting took in: here, and in the songs left on the other branches.
    juce::StringArray taken;

    if (const auto& waiting = getMerging(); waiting.has_value())
        taken.add (waiting->version);

    for (const auto& other : branches)
        if (other.id != branch.id && other.id != currentBranch)
            if (const auto version = findMergeLeftOn (other); version.isNotEmpty())
                taken.add (version);

    for (const auto& version : taken)
    {
        if (! follows (versions, branch.head, version))
            continue;

        const auto heldElsewhere = std::any_of (branches.begin(), branches.end(), [&] (const BranchRecord& other)
                                                { return other.id != branch.id && follows (versions, other.head, version); });

        if (! heldElsewhere)
            return true;
    }

    return false;
}

juce::String VersionHistory::findMergeLeftOn (const BranchRecord& branch) const
{
    // In the song's own piece, which a merge's element is part of.
    if (store == nullptr || branch.working.isEmpty())
        return {};

    // A piece never changes: each read once.
    if (const auto known = mergesLeft.find (branch.working); known != mergesLeft.end())
        return known->second;

    juce::String version;

    if (const auto block = store->getPieces().read (branch.working))
        if (const auto left = readMergeState (readElement (*block).getChildWithName (schema::MERGING)))
            version = left->version;

    store->getPieces().closeFiles();
    mergesLeft.emplace (branch.working, version);
    return version;
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
    // A merge taken or undone, redone, aborted (Schema.h: MERGING): found again as it is asked for.
    if (child.hasType (schema::MERGING))
    {
        mergeDirty = true;
        changed();
    }

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
    if (tree.hasType (schema::MERGING)) // a conflict switched
    {
        mergeDirty = true;
        changed();
    }

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
    editedSinceSnapshot = true; // Local History takes the song again in a while

    // What is heard is what is shown: an edit is heard as it is made, in the song, and A/B stops waiting to hear a commit.
    if (hearWhenBounced.isNotEmpty() || compareProblem.isNotEmpty())
    {
        hearWhenBounced.clear();
        compareProblem.clear();
        changed();
    }

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

    // Local History: the song every few minutes while it changes, as long as it differs from its last commit; not while it
    // records (the take isn't in it yet), nor while a commit is being made.
    if (editedSinceSnapshot && hasRead && store != nullptr && ! saving && ! isRecording (song)
        && juce::Time::currentTimeMillis() - lastSnapshotTime >= (juce::int64) snapshotMinutes * 60 * 1000 && hasChanges())
        takeSnapshot ("auto");

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
    const auto fromCheckout = makeCurrent && current != nullptr && current->detachedFrom.isNotEmpty() ? current->id : juce::String();

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

    // Made from a commit checked out: the song goes on there, on a branch again, and the record of the checkout goes.
    if (fromCheckout.isNotEmpty())
    {
        if (const auto left =
                std::find_if (branches.begin(), branches.end(), [&fromCheckout] (const auto& b) { return b.id == fromCheckout; });
            left != branches.end())
        {
            auto gone = *left;
            gone.deleted = true;
            store->writeBranch (gone);
            branches.erase (left);
            checkoutRecord = fromCheckout;
        }
    }

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

    // The song as it is now, its view state and unsaved changes too, its plug-ins' settings stored, on the branch it leaves; or,
    // leaving a commit checked out, what changed there into Local History, and the record of the checkout deleted.
    const auto leavesCheckout = current->detachedFrom.isNotEmpty();
    auto left = *current;

    if (leavesCheckout)
    {
        if (hasChanges())
            takeSnapshot ("checkout");

        left.working = {};
        left.deleted = true;
    }
    else
    {
        song.storeDeviceStates();
        const auto pieces = splitSong (song.getState().createCopy(), readPluginParameters());

        if (! store->writeSong (pieces))
            return {};

        left.working = pieces.song;
    }

    // The other's, as it was left there, or as its last version had it.
    const auto* head = findVersion (found->head);
    auto other = found->working.isNotEmpty() ? store->readSong (found->working)
                 : head != nullptr           ? readCommitSong (*store, head->song)
                                             : juce::ValueTree();
    store->getPieces().closeFiles();

    if (! other.isValid() || ! store->writeBranch (left))
        return {};

    auto entered = *found;
    entered.working = {};

    if (! store->writeBranch (entered) || ! store->writeCurrentBranch (entered.id))
        return {};

    *found = entered;
    currentBranch = entered.id;

    if (leavesCheckout)
    {
        checkoutRecord = left.id; // made again for the next checkout
        branches.erase (std::find_if (branches.begin(), branches.end(), [&left] (const auto& branch) { return branch.id == left.id; }));
    }
    else
    {
        *std::find_if (branches.begin(), branches.end(), [&left] (const auto& branch) { return branch.id == left.id; }) = left;
    }

    mergeDirty = true;
    preview.reset();
    return other;
}

bool VersionHistory::isDetached() const
{
    return getDetachedFrom().isNotEmpty();
}

juce::String VersionHistory::getDetachedFrom() const
{
    const auto* current = findCurrentBranch();
    return current != nullptr ? current->detachedFrom : juce::String();
}

bool VersionHistory::checkout (const juce::String& versionID)
{
    followFolder();
    const auto current =
        std::find_if (branches.begin(), branches.end(), [this] (const auto& branch) { return branch.id == currentBranch; });

    if (store == nullptr || ! hasRead || saving || findVersion (versionID) == nullptr || current == branches.end())
        return false;

    // Checked out already: on to the other commit, in place, what changed here kept in Local History first.
    if (current->detachedFrom.isNotEmpty())
    {
        const auto then = readSong (versionID);
        auto moved = *current;
        moved.head = versionID;
        moved.changed = juce::Time::currentTimeMillis();

        if (! then.isValid() || ! store->writeBranch (moved))
            return false;

        if (hasChanges())
            takeSnapshot ("checkout");

        store->logBranchMove (moved.id, current->head, versionID, "checkout");
        *current = moved;
        mergeDirty = true;
        preview.reset();
        makeSongTheSame (song, then);
        PluginParameterValues parameters;
        auto baselineSong = readSong (versionID, &parameters);
        setBaseline (std::move (baselineSong), std::move (parameters), song.getDeviceChangesOutsideHistory());
        comparedVersion.clear();
        comparedSong = {};
        selectedChange.reset();

        if (! hearingBounce)
            bounceCompared.clear();

        tellNow();
        return true;
    }

    // A record of the checkout, from the branch the song is on (this computer's last one made again), switched to as a branch
    // is: the branch's song kept there, the commit's made the song's.
    BranchRecord record;
    record.id = checkoutRecord.isNotEmpty() ? checkoutRecord : juce::Uuid().toString();
    record.colour = current->colour;
    record.head = versionID;
    record.computer = getThisComputer();
    record.changed = juce::Time::currentTimeMillis();
    record.detachedFrom = currentBranch;

    if (! store->writeBranch (record))
        return false;

    branches.push_back (record);

    if (! switchTo (record.id))
    {
        // As it was: the record deleted again.
        record.deleted = true;
        store->writeBranch (record);
        branches.erase (std::find_if (branches.begin(), branches.end(), [&record] (const auto& branch) { return branch.id == record.id; }));
        checkoutRecord = record.id;
        return false;
    }

    store->logBranchMove (record.id, {}, versionID, "checkout");
    return true;
}

bool VersionHistory::backFromDetached()
{
    // To the branch it came from; or, if that went meanwhile (deleted on another computer), the first branch.
    const auto from = getDetachedFrom();
    const auto branchesNow = getBranches();

    if (from.isEmpty() || branchesNow.empty())
        return false;

    const auto there = std::any_of (branchesNow.begin(), branchesNow.end(), [&from] (const auto& branch) { return branch.id == from; });
    return switchTo (there ? from : branchesNow.front().id);
}

bool VersionHistory::switchTo (const juce::String& branchID)
{
    // A merge waiting stays with the branch it was taken on, in the song left there (Schema.h: MERGING).
    const auto other = leaveFor (branchID);

    if (! other.isValid())
        return false;

    // The other branch's song, in place; its changes since its last version from here on, which A/B hears as B.
    makeSongTheSame (song, other);
    const auto* last = findLastVersion();
    PluginParameterValues parameters;
    auto lastSong = last != nullptr ? readSong (last->id, &parameters) : juce::ValueTree();
    setBaseline (std::move (lastSong), std::move (parameters), song.getDeviceChangesOutsideHistory());
    mergeDirty = true;
    getMerging(); // the merge the song left there holds, read on the history's thread now if it must be
    comparedVersion.clear();
    comparedSong = {};
    selectedChange.reset();

    if (! hearingBounce)
        bounceCompared.clear();

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

    // Not a commit checked out, which is no branch: Create Branch Here names one.
    if (store == nullptr || found == branches.end() || newName.isEmpty() || taken || found->detachedFrom.isNotEmpty())
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

bool VersionHistory::deleteBranch (const juce::String& branchID)
{
    followFolder();
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });

    if (store == nullptr || ! hasRead || saving || found == branches.end() || branchID == currentBranch || branchID == getDetachedFrom()
        || found->detachedFrom.isNotEmpty())
        return false;

    // A merge of its line under way, or a merge waiting (here, or on another branch) that took in a commit only its line
    // holds, which would leave the graph with it.
    if ((preview.has_value() && preview->version == found->head) || holdsCommitMergeWaits (*found))
        return false;

    // This computer's record says it is deleted, the newest word on it; the song left on it goes.
    auto gone = *found;
    gone.deleted = true;
    gone.changed = juce::Time::currentTimeMillis();
    gone.computer = getThisComputer();
    gone.working = {};
    gone.divergent.clear();

    if (! store->writeBranch (gone))
        return false;

    store->logBranchMove (branchID, found->head, {}, "delete");
    branches.erase (found);

    if (selectedBranch == branchID)
        selectedBranch.clear();

    forgetUnreachable();
    readBounces(); // the bounces of the commits that went no longer count; and tells
    return true;
}

int VersionHistory::countVersionsOnlyOn (const juce::String& branchID) const
{
    // Those its line holds that no other branch's does.
    std::vector<BranchRecord> it, others;

    for (const auto& branch : branches)
        (branch.id == branchID ? it : others).push_back (branch);

    auto line = versions, held = versions;
    keepReachable (line, it);
    keepReachable (held, others);
    return (int) std::count_if (line.begin(), line.end(), [&held] (const auto& entry) { return held.count (entry.first) == 0; });
}

bool VersionHistory::hasChangesLeftOn (const juce::String& branchID) const
{
    // The song left on it, as this computer left it, against its last commit's, as the changes count them.
    for (const auto& branch : branches)
    {
        if (branch.id != branchID || branch.working.isEmpty() || store == nullptr)
            continue;

        const auto* head = findVersion (branch.head);

        if (head != nullptr && head->song == branch.working)
            return false; // the same pieces: the same song

        const auto left = store->readSong (branch.working);
        const auto last = head != nullptr ? readCommitSong (*store, head->song) : juce::ValueTree (schema::PHAZITE);
        store->getPieces().closeFiles();
        return left.isValid() && last.isValid() && ! findChanges (last, left).empty();
    }

    return false;
}

bool VersionHistory::canUndoLastCommit() const
{
    const auto* last = findLastVersion();
    return store != nullptr && hasRead && ! saving && ! getMerging().has_value() && ! preview.has_value() && last != nullptr
           && ! isDetached() && last->parents.size() == 1 && findVersion (last->parents[0]) != nullptr;
}

bool VersionHistory::undoLastCommit()
{
    followFolder();

    if (! canUndoLastCommit())
        return false;

    const auto undone = *findLastVersion();
    const auto& before = undone.parents[0];
    PluginParameterValues beforeParameters;
    auto beforeSong = readSong (before, &beforeParameters);

    if (! beforeSong.isValid())
        return false;

    // The branch at the commit before, on this computer's record, and its log says why.
    const auto current =
        std::find_if (branches.begin(), branches.end(), [this] (const auto& branch) { return branch.id == currentBranch; });

    if (current == branches.end())
        return false;

    auto moved = *current;
    moved.head = before;
    moved.computer = getThisComputer();
    moved.divergent.clear();

    if (! store->writeBranch (moved))
        return false;

    store->logBranchMove (moved.id, undone.id, before, "undo");
    *current = moved;

    // As git's soft reset: the index stays as it was, the commit's song with what was staged since, and the changes are
    // from the commit before. A plug-in whose stored settings differ between the two commits counts as changed in what is
    // staged, as it was in the commit (its count of the changes it said it made is taken back one).
    const auto statesThen = getPluginStates (beforeSong), statesUndone = getPluginStates (baseline);
    auto reported = reportedAtBaseline;

    for (const auto& [device, undoneState] : statesUndone)
        if (const auto then = statesThen.find (device); then == statesThen.end() || then->second != undoneState)
            --reported[device];

    baseline = std::move (beforeSong);
    baselineParameters = std::move (beforeParameters);
    reportedAtBaseline = std::move (reported);
    stagedChanged();

    // Its message the draft again, for a commit made again, unless one is being typed.
    if (draftName.isEmpty() && draftNote.isEmpty())
    {
        draftName = undone.name;
        draftNote = undone.note;
    }

    if (bouncingVersion == undone.id)
    {
        backgroundBounce = nullptr; // its bounce stops
        bouncingVersion.clear();
    }

    forgetUnreachable();
    readBounces(); // and tells
    return true;
}

const std::vector<SongChange>& VersionHistory::getVersionChanges (const juce::String& versionID)
{
    if (const auto found = versionChanges.find (versionID); found != versionChanges.end())
        return found->second;

    // From the commit it follows, or from nothing for the first; every plug-in whose settings differ counts.
    std::vector<SongChange> changed;

    if (const auto* version = findVersion (versionID))
    {
        PluginParameterValues before, after;
        const auto then = version->parents.isEmpty() ? juce::ValueTree (schema::PHAZITE) : readSong (version->parents[0], &before);
        const auto now = readSong (versionID, &after);

        if (then.isValid() && now.isValid())
            changed = findChanges (then, now, { &before, &after, nullptr });
    }

    return versionChanges[versionID] = std::move (changed);
}

void VersionHistory::takeSnapshot (const juce::String& reason)
{
    followFolder();

    if (store == nullptr || ! hasRead)
        return;

    // The song as a save stores it, its plug-ins' own settings included (as the project opens, as it was read: its plug-ins
    // may still be loading), and their knobs' values; split and written on the history's thread.
    if (reason != "open")
        song.storeDeviceStates();

    editedSinceSnapshot = false;
    lastSnapshotTime = juce::Time::currentTimeMillis();

    run (
        [shared = store, number = generation, copy = song.getState().createCopy(), parameters = readPluginParameters(),
         branch = currentBranch, time = lastSnapshotTime, reason, weak = juce::WeakReference<VersionHistory> (this)]() mutable
        {
            // Only when the song differs from the snapshot before it; thinned as it grows.
            const auto pieces = splitSong (copy, parameters);
            auto all = shared->readSnapshots();

            if ((all.empty() || all.back().song != pieces.song) && shared->writeSong (pieces))
            {
                all.push_back ({ time, branch, pieces.song, reason });
                all = thinSnapshots (all, time);
                shared->writeSnapshots (all);
            }

            shared->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, all]() mutable
                {
                    if (auto* self = weak.get())
                        self->snapshotTaken (number, std::move (all));
                });
        });
}

void VersionHistory::snapshotTaken (int number, std::vector<Snapshot> oldestFirst)
{
    --pendingJobs;

    if (number != generation)
        return;

    snapshots.assign (oldestFirst.rbegin(), oldestFirst.rend());
    tellNow();
}

juce::ValueTree VersionHistory::readSnapshot (juce::int64 time) const
{
    if (store == nullptr)
        return {};

    for (const auto& snapshot : snapshots)
    {
        if (snapshot.time == time)
        {
            auto read = store->readSong (snapshot.song);
            store->getPieces().closeFiles();
            return read;
        }
    }

    return {};
}

std::vector<Snapshot> thinSnapshots (const std::vector<Snapshot>& oldestFirst, juce::int64 now)
{
    // Newest first, the newest of each hour's or day's bucket kept, as Time Machine thins its backups.
    constexpr juce::int64 hour = 60 * 60 * 1000, day = 24 * hour;
    std::vector<Snapshot> kept;
    std::set<juce::int64> buckets; // an hour's by its number, a day's by its number below zero

    for (auto snapshot = oldestFirst.rbegin(); snapshot != oldestFirst.rend(); ++snapshot)
    {
        const auto age = now - snapshot->time;

        if (age > 30 * day)
            continue;

        if (age <= 2 * hour || buckets.insert (age <= day ? snapshot->time / hour : -1 - snapshot->time / day).second)
            kept.push_back (*snapshot);
    }

    std::reverse (kept.begin(), kept.end());
    return kept;
}

bool VersionHistory::editMessage (const juce::String& versionID, const juce::String& name, const juce::String& note)
{
    followFolder();
    const auto found = versions.find (versionID);

    if (store == nullptr || ! hasRead || found == versions.end())
        return false;

    const MessageEdit edit { name.trim(), note.trim(), juce::Time::currentTimeMillis() };

    if (! store->writeMessage (versionID, edit))
        return false;

    messageEdits[versionID] = edit;
    applyMessageEdits();
    tellNow();
    return true;
}

void VersionHistory::applyMessageEdits()
{
    for (const auto& [id, edit] : messageEdits)
    {
        if (const auto found = versions.find (id); found != versions.end())
        {
            found->second.name = edit.name;
            found->second.note = edit.note;
        }
    }
}

void VersionHistory::forgetUnreachable()
{
    mergeDirty = true; // the commit a merge waiting merged in may be among those that went
    keepReachable (versions, branches);

    if (findVersion (selectedVersion) == nullptr)
    {
        selectedVersion.clear();
        selectedVersionChange.reset();
    }

    if (comparedVersion.isNotEmpty() && findVersion (comparedVersion) == nullptr)
    {
        comparedVersion.clear();
        comparedSong = {};
    }
}

juce::String VersionHistory::findCommonVersion (const juce::String& a, const juce::String& b) const
{
    return findMergeBase (versions, a, b);
}

int VersionHistory::countVersionsNotIn (const juce::String& version, const juce::String& other) const
{
    BranchRecord from, to;
    from.head = version;
    to.head = other;
    auto line = versions, otherLine = versions;
    keepReachable (line, { from });
    keepReachable (otherLine, { to });
    return (int) std::count_if (line.begin(), line.end(), [&otherLine] (const auto& entry) { return otherLine.count (entry.first) == 0; });
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

    // Oldest first by the graph, so that a later merge's word on an ID wins (one merged on top of another computer's merge
    // of the same commit, with the IDs this song gave), whatever the computers' clocks said; by when they were saved where
    // neither follows the other.
    std::sort (merges.begin(), merges.end(), [] (const auto* a, const auto* b) { return a->saved < b->saved; });
    std::vector<const VersionRecord*> ordered;

    while (! merges.empty())
    {
        const auto first =
            std::find_if (merges.begin(), merges.end(),
                          [&] (const VersionRecord* candidate)
                          {
                              return std::none_of (merges.begin(), merges.end(), [&] (const VersionRecord* other)
                                                   { return other != candidate && follows (versions, candidate->id, other->id); });
                          });

        const auto next = first != merges.end() ? first : merges.begin();
        ordered.push_back (*next);
        merges.erase (next);
    }

    std::vector<RenumberedID> renumbered;

    for (const auto* merge : ordered)
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
    {
        if (branch.id == current)
        {
            if (const auto found = versions.find (branch.head); found != versions.end())
            {
                // Named as its message was edited, if it was.
                auto version = found->second;
                const auto messages = store.readMessages();

                if (const auto edited = messages.find (version.id); edited != messages.end())
                    version.name = edited->second.name;

                return { readCommitSong (store, version.song), getDisplayName (version) };
            }
        }
    }

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
    const auto legalComputer = juce::File::createLegalFileName (computer);

    // A commit checked out on its own is that computer's alone.
    for (const auto& record : records)
        if (record.detachedFrom.isEmpty() || record.computer == legalComputer)
            byBranch[record.id].push_back (&record);

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
        branch.deleted = false;

        // The last version that follows every computer's, else this computer's own.
        juce::StringArray heads;

        for (const auto* record : group)
            if (versions.count (record->head) > 0)
                heads.addIfNotAlreadyThere (record->head);

        // Deleted, where the newest record says so and its last version follows every other's: commits made on it elsewhere
        // since bring it back.
        if (newest->deleted
            && std::all_of (heads.begin(), heads.end(),
                            [&versions, newest] (const juce::String& other) { return follows (versions, newest->head, other); }))
            continue;

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
