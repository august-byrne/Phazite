// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/VersionHistory.h"
#include "internal/ApplySong.h"
#include "internal/StoreSweep.h"
#include "model/arrangement/TrackColours.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/KeptByAge.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AppInfo.h"
#include "model/mixer/FreezeModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/VersionsModel.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace phazite
{
namespace
{
    constexpr int tickMilliseconds = 250; // how soon what changed is told, at most
    constexpr int maxPacks = 50;          // packs past which they are joined into one as the history opens

    // Why Save Version saved nothing, where the song held nothing a version would record.
    constexpr const char* nothingChanged = "Nothing changed since the last version.";

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
    void keepReachable (std::map<juce::String, VersionRecord>& versions, const std::vector<BranchRecord>& branches,
                        const juce::StringArray& alsoHeld = {})
    {
        std::set<juce::String> reachable;
        juce::StringArray toVisit (alsoHeld); // the commits offered for branches whose last can't be read

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

    // Whether a piece is a commit's record, by its start (PieceStore::readStart()), without reading a large piece whole:
    // none where its start can't be read.
    std::optional<bool> isCommitRecord (const PieceStore& pieces, const juce::String& name)
    {
        constexpr const char* versionStart = "<VERSION ";
        const auto size = std::strlen (versionStart);
        const auto start = pieces.readStart (name, (int) size);

        if (! start.has_value())
            return std::nullopt;

        return start->getSize() == size && std::memcmp (start->getData(), versionStart, size) == 0;
    }

    // Every commit's record the packs hold that reads, newest first; as many as were read when told to stop.
    std::vector<VersionRecord> findCommitsInPacks (const PieceStore& pieces, const std::function<bool()>& shouldStop)
    {
        std::vector<VersionRecord> found;

        for (const auto& name : pieces.getNames())
        {
            if (shouldStop())
                break;

            if (isCommitRecord (pieces, name).value_or (false))
                if (const auto bytes = pieces.read (name))
                    if (auto record = toVersionRecord (readElement (*bytes), name))
                        found.push_back (std::move (*record));
        }

        std::sort (found.begin(), found.end(), [] (const auto& a, const auto& b) { return a.saved > b.saved; });
        return found;
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
} // namespace

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

namespace
{
    // A file read for its hash in parts, as juce::SHA256 reads it: none more once stop says so (the hash then of what was
    // read), and each part told as it is read.
    class StoppableStream final : public juce::InputStream
    {
    public:
        StoppableStream (juce::InputStream& source, std::function<bool()> stopped, std::function<void (int)> partRead)
            : in (source),
              stop (std::move (stopped)),
              told (std::move (partRead))
        {
        }

        juce::int64 getTotalLength() override { return in.getTotalLength(); }
        bool isExhausted() override { return in.isExhausted() || stop(); }
        juce::int64 getPosition() override { return in.getPosition(); }
        bool setPosition (juce::int64 position) override { return in.setPosition (position); }

        int read (void* destination, int maxBytes) override
        {
            if (stop())
                return 0;

            const auto numRead = in.read (destination, juce::jmin (maxBytes, 1 << 20));

            if (numRead > 0)
                told (numRead);

            return numRead;
        }

    private:
        juce::InputStream& in;
        std::function<bool()> stop;
        std::function<void (int)> told;
    };
} // namespace

juce::ValueTree readCommitSong (const VersionStore& store, const juce::String& songPiece, PluginParameterValues* parameters,
                                std::vector<MissingPiece>* missing, bool* idsGivenAnew)
{
    auto read = store.readSong (songPiece, parameters, missing, idsGivenAnew);
    read.removeChild (read.getChildWithName (schema::MERGING), nullptr);
    read.removeProperty (schema::versionsBranch, nullptr); // where a song stands is no version's (Schema.h)
    read.removeProperty (schema::versionsBase, nullptr);
    return read;
}

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

    for (const auto chosen : element[schema::mergingChosen].toString())
    {
        if (chosen < '0' || chosen > '2')
            return std::nullopt;

        state.chosen.push_back ((int) (chosen - '0'));
    }

    const auto renumbered = readMergeIDs (element[schema::mergingRenumbered].toString());
    const auto translation = readMergeIDs (element[schema::mergingTranslated].toString());
    const auto crossed = readMergeIDs (element[schema::mergingCrossed].toString());

    if (! renumbered.has_value() || ! translation.has_value() || ! crossed.has_value())
        return std::nullopt;

    state.renumbered = *renumbered;
    state.translation = *translation;
    state.crossed = *crossed;
    return state;
}

juce::ValueTree writeMergeState (const MergeState& state)
{
    juce::ValueTree element (
        schema::MERGING,
        { { schema::mergingVersion, state.version }, { schema::mergingHead, state.head }, { schema::mergingBefore, state.before } });
    juce::StringArray chosen;

    for (const auto value : state.chosen)
        chosen.add (juce::String (value));

    if (! chosen.isEmpty())
        element.setProperty (schema::mergingChosen, chosen.joinIntoString ({}), nullptr);

    if (! state.renumbered.empty())
        element.setProperty (schema::mergingRenumbered, writeMergeIDs (state.renumbered), nullptr);

    if (! state.translation.empty())
        element.setProperty (schema::mergingTranslated, writeMergeIDs (state.translation), nullptr);

    if (! state.crossed.empty())
        element.setProperty (schema::mergingCrossed, writeMergeIDs (state.crossed), nullptr);

    return element;
}

VersionHistory::Merge findMergeAgain (const VersionStore& store, const std::map<juce::String, VersionRecord>& versions,
                                      const MergeState& state)
{
    VersionHistory::Merge merge;
    merge.version = state.version;
    merge.head = state.head;
    merge.translation = state.translation;
    merge.crossed = state.crossed;
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

    // The other line's own merges of what this line holds too, taken into the parting's numbering first, then both songs into
    // this line's, as the merge was (crossRenumbering(), findTranslation()).
    translateIDs (merge.theirs, state.crossed);
    merge.theirParameters = translateKnobs (std::move (merge.theirParameters), state.crossed);

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
    const auto result = mergeSongs (merge.base, merge.before, merge.theirs, [&next] { return next++; });
    merge.clashes = result.clashes;
    merge.notes = result.notes;
    merge.readWhole = true;
    return merge;
}

//==============================================================================
// What each piece names of audio, as a look of the versions' audio found it (VersionHistory::makeAudioPathsFinder()): a
// commit's record its files, a song kept without a commit (a branch's song left there, a snapshot's) the files it plays and
// the song before a merge it holds. A piece never changes, so what was read whole is kept; what wasn't (a pack that couldn't
// be opened then, a song whose pieces haven't all arrived from another computer) is read again at the next look, and leaves
// that look unsure. One look at a time, on any thread.
struct VersionsAudio
{
    struct Song
    {
        juce::StringArray paths;
        juce::String before; // the song before a merge it holds, none for none
    };

    std::mutex lock;
    std::map<juce::String, juce::StringArray> records; // by piece: a commit's files, none for a piece that isn't a commit
    std::map<juce::String, Song> songs;                // by piece: each read whole

    // A song kept, by its piece, as the look says it: "the song computer abc left on a branch", "Local History of ...".
    struct Named
    {
        juce::String piece, what;
    };

    VersionsAudioFound find (VersionStore& store, std::vector<Named> songsToo, const std::function<bool()>& shouldStop)
    {
        const std::scoped_lock sl (lock);
        VersionsAudioFound look;
        auto& pieces = store.getPieces();
        pieces.readNewPacks(); // those written since the history opened, here or on another computer
        look.key = findVersionsAudioKey (store);
        std::set<juce::String> found;

        // A pack that couldn't be read (held by another program, damaged, or of a layout this build doesn't read) might
        // hold any version or song: the look is unsure while one is there.
        for (const auto& pack : pieces.findUnreadPacks())
            look.unreadable.add ("a pack of the Versions folder (" + pack.getFileName().substring (0, 7)
                                 + ") can't be read: another program holds it, it is damaged, or another build of Phazite wrote it");

        // Every commit the packs hold, on a branch or not, by its record: a small piece, which the others' starts tell from
        // the rest without reading them whole.
        for (const auto& name : pieces.getNames())
        {
            if (shouldStop())
            {
                look.stopped = true;
                break;
            }

            auto known = records.find (name);

            if (known == records.end())
            {
                const auto isRecord = isCommitRecord (pieces, name);
                std::optional<juce::StringArray> files;

                if (isRecord.has_value() && ! *isRecord)
                    files.emplace(); // not a commit: whole as far as this look needs
                else if (isRecord.has_value())
                    if (const auto bytes = pieces.read (name))
                        if (const auto record = toVersionRecord (readElement (*bytes), name))
                        {
                            files.emplace();

                            for (const auto& file : record->files)
                                files->add (file.path);
                        }

                if (! files.has_value())
                {
                    look.unreadable.add ("a version's record in the Versions folder (" + name.substring (0, 7)
                                         + ") can't be read: its pack is damaged, or another program holds it");
                    continue;
                }

                known = records.emplace (name, *files).first;
            }

            found.insert (known->second.begin(), known->second.end());
        }

        // The last commit of every computer's record of every branch: one that hasn't arrived yet (its pack still on its
        // way from another computer) plays what isn't known.
        for (const auto& branch : store.readBranches())
            if (branch.head.isNotEmpty() && ! look.stopped && records.count (branch.head) == 0)
                look.unreadable.add ("the last version of a branch on computer " + branch.computer + " (" + branch.head.substring (0, 7)
                                     + ") isn't in the Versions folder yet");

        // And so for a version that left a branch within the last 30 days, by every computer's logs, which Recover Lost
        // Versions may bring back once its pack can be read (one held as the store opened, or not arrived yet).
        const auto now = getVersionsTime();

        for (const auto& move : store.readEveryLog())
            for (const auto& version : { move.from, move.to })
                if (version.isNotEmpty() && ! look.stopped && now - move.time <= keptByAge::dailyFor && records.count (version) == 0)
                    look.unreadable.addIfNotAlreadyThere ("a version a branch's log on computer " + move.computer + " names ("
                                                          + version.substring (0, 7) + ") can't be read from the Versions folder");

        // The songs kept without a commit, every piece of each read, and the songs before the merges they hold: every one the
        // files list whose piece the store still holds, whatever the clock says.
        for (const auto& kept : store.findKeptSongs (0))
            songsToo.push_back (
                { kept.piece,
                  kept.kind == KeptSong::Kind::snapshot
                      ? "the automatic save of " + juce::Time (kept.time).toString (true, true, false) + " (computer " + kept.computer + ")"
                  : kept.kind == KeptSong::Kind::deleted ? "the song computer " + kept.computer + " left on a branch deleted since"
                                                         : "the song computer " + kept.computer + " left on a branch" });

        for (size_t i = 0; i < songsToo.size() && ! look.stopped; ++i)
        {
            if (shouldStop())
            {
                look.stopped = true;
                break;
            }

            const auto named = songsToo[i];
            auto known = songs.find (named.piece);

            if (known == songs.end())
            {
                std::vector<MissingPiece> missing;
                const auto kept = store.readSong (named.piece, nullptr, &missing);

                if (! kept.hasType (schema::PHAZITE) || ! missing.empty())
                {
                    look.unreadable.add (named.what
                                         + " can't be read whole: its pieces haven't all reached this computer, or one is damaged");
                    continue;
                }

                Song song { findAudioFilePaths (kept), {} };

                if (const auto waiting = readMergeState (kept.getChildWithName (schema::MERGING)))
                    song.before = waiting->before;

                known = songs.emplace (named.piece, song).first;
            }

            found.insert (known->second.paths.begin(), known->second.paths.end());

            if (const auto& before = known->second.before;
                before.isNotEmpty()
                && std::none_of (songsToo.begin(), songsToo.end(), [&before] (const Named& other) { return other.piece == before; }))
                songsToo.push_back ({ before, "the song before a merge that waits in " + named.what });
        }

        pieces.closeFiles();

        for (const auto& path : found)
            look.paths.add (path);

        return look;
    }
};

juce::String findVersionsAudioKey (VersionStore& store)
{
    // What names a version's audio, cheaply: the packs by their names, read from the folder (so that a join under way keeps
    // none of this waiting), every kept song's piece, and every branch's last version. A look for the audio found under
    // another key may have missed what came since.
    juce::StringArray parts;

    for (const auto& pack : store.getPieces().getFolder().findChildFiles (juce::File::findFiles, false, "*.pack"))
        parts.add (pack.getFileName());

    for (const auto& kept : store.findKeptSongs (0))
        parts.add (kept.piece);

    for (const auto& branch : store.readBranches())
        parts.add (branch.computer + ":" + branch.head);

    parts.sort (false);
    return juce::SHA256 (parts.joinIntoString (" ").toUTF8()).toHexString();
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
namespace
{
    // The threads of the histories that went with writes still to do (a commit, a snapshot), each kept until they are
    // done, so that a close neither waits for them on the message thread nor loses them; a history opened on the same
    // Versions folder reads it only after them, waiting on its own thread. Its jobs hold their store themselves. keep(),
    // reap() and the counts on the message thread; waitFor() on any thread but those it keeps.
    class FinishingHistories
    {
    public:
        static FinishingHistories& get()
        {
            static FinishingHistories finishing;
            return finishing;
        }

        // The writes a program that quits without waiting for them still finishes as it ends, however long that takes.
        ~FinishingHistories() { waitFor ({}); }

        void keep (const juce::File& folder, std::shared_ptr<BackgroundJobs> jobs, std::shared_ptr<std::atomic<int>> writesLeft)
        {
            {
                const std::scoped_lock sl (lock);
                kept.push_back ({ folder, jobs, std::move (writesLeft) });
            }

            // Let go of once its last job has run (on the message thread, which destroys what it made).
            jobs->add ([] (const BackgroundJobs::ShouldStop&) { juce::MessageManager::callAsync ([] { get().reap(); }); });
        }

        // The jobs kept for a folder now (for every folder, for none): those of its histories that have gone. What a
        // history's own job waits for is those kept as the job was given, on the message thread, and no others: a job that
        // waited for whatever was kept by the time it ran waited for its own jobs too, kept meanwhile as its history went,
        // so for itself, without end (known bug 190: a first read, and whatever waited for the histories that went with
        // it); or for those of a later history of the folder, whose job waited for it in turn.
        std::vector<std::shared_ptr<BackgroundJobs>> getKept (const juce::File& folder)
        {
            const std::scoped_lock sl (lock);
            std::vector<std::shared_ptr<BackgroundJobs>> jobs;

            for (const auto& each : kept)
                if (folder == juce::File() || each.folder == folder)
                    jobs.push_back (each.jobs);

            return jobs;
        }

        // Waits until the writes into a folder are done (into every folder, for none). Not on a thread it keeps.
        void waitFor (const juce::File& folder)
        {
            for (const auto& jobs : getKept (folder))
                jobs->waitForAll();
        }

        // The writes still to do into a folder or one inside it (a project's, whose Versions folder they write into).
        int countWritesInside (const juce::File& folder)
        {
            const std::scoped_lock sl (lock);
            auto count = 0;

            for (const auto& each : kept)
                if (each.folder == folder || each.folder.isAChildOf (folder))
                    count += each.writesLeft->load();

            return count;
        }

        // The writes still to do into a folder, or into every one.
        int countWrites (const juce::File& folder = {})
        {
            const std::scoped_lock sl (lock);
            auto count = 0;

            for (const auto& each : kept)
                if (folder == juce::File() || each.folder == folder)
                    count += each.writesLeft->load();

            return count;
        }

        juce::StringArray getFolders()
        {
            const std::scoped_lock sl (lock);
            juce::StringArray folders;

            for (const auto& each : kept)
                if (each.writesLeft->load() > 0)
                    folders.addIfNotAlreadyThere (each.folder.getParentDirectory().getFileName());

            return folders;
        }

    private:
        struct Kept
        {
            juce::File folder;
            std::shared_ptr<BackgroundJobs> jobs;
            std::shared_ptr<std::atomic<int>> writesLeft;
        };

        std::mutex lock;
        std::vector<Kept> kept;

        // Those whose jobs are all done go; one whose last job runs still is looked at again in a moment.
        void reap()
        {
            std::vector<std::shared_ptr<BackgroundJobs>> done;
            auto again = false;

            {
                const std::scoped_lock sl (lock);

                for (auto each = kept.begin(); each != kept.end();)
                {
                    if (each->jobs->getNumJobs() == 0)
                    {
                        done.push_back (each->jobs);
                        each = kept.erase (each);
                    }
                    else
                    {
                        again = again || each->writesLeft->load() == 0;
                        ++each;
                    }
                }
            }

            done.clear(); // their threads end, each waited for, outside the lock

            if (again)
                juce::Timer::callAfterDelay (20, [] { get().reap(); });
        }
    };
} // namespace

std::atomic<bool> VersionHistory::holdWritesForTests { false }, VersionHistory::holdReadsForTests { false };
std::atomic<bool> VersionHistory::noAdoptionAtOpenForTests { false };
std::atomic<bool> VersionHistory::holdFirstReadsForTests { false };
std::atomic<int> VersionHistory::firstReadsHeldForTests { 0 };
std::atomic<int> VersionHistory::readsToStartForTests { -1 };

int countVersionsBeingFinished()
{
    return FinishingHistories::get().countWrites();
}

juce::StringArray getProjectsBeingFinished()
{
    return FinishingHistories::get().getFolders();
}

void waitForVersionsFinished()
{
    FinishingHistories::get().waitFor ({});
}

void whenNoVersionsWriteInto (const juce::File& folder, std::function<void()> action)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Looked at again in a moment while a history that went still writes there: its write's file put in place out of a
    // folder deleted under it found no file to put (JUCE's TemporaryFile asserts so), and its commit was lost with it.
    if (FinishingHistories::get().countWritesInside (folder) == 0)
    {
        action();
        return;
    }

    juce::Timer::callAfterDelay (20,
                                 [folder, action = std::move (action)]() mutable { whenNoVersionsWriteInto (folder, std::move (action)); });
}

VersionHistory::VersionHistory (Song& s, std::function<void()> changeCallback)
    : song (s),
      state (s.getState()),
      onChange (std::move (changeCallback)),
      thread (std::make_shared<BackgroundJobs> ("Phazite versions", 1, juce::Thread::Priority::normal)),
      audioPaths (std::make_shared<VersionsAudio>())
{
    changesOutsideHistory = song.getChangesOutsideHistory();
    outsideSeen = song.getDeviceChangesOutsideHistory();
    pluginsLoadingSeen = getPluginsLoading (song).size();
    state.addListener (this);

    if (auto* values = song.getValues())
        values->addListener (this);

    open();
    startTimer (tickMilliseconds);
}

VersionHistory::~VersionHistory()
{
    // A first save's copy of the folder is over: one that hasn't begun won't, and one under way stops at its next piece;
    // either way its job, a write, which runs even as the history's thread is handed on below, tells it so (copyFolder()).
    const auto copying = std::exchange (folderCopy, nullptr);

    if (copying != nullptr)
        endCopy (*copying, "The project closed before its versions were copied.");

    stopTimer();
    backgroundBounce = nullptr; // cancelled, before what it renders goes
    if (auto* values = song.getValues())
        values->removeListener (this);

    state.removeListener (this);

    // A version open: the song as it was again, so that whatever holds it on (a test's song, which outlives its project) holds
    // its own. Its owner's work as it goes back (VersionsModel.cpp) needs a history, which goes: the trial alone is put back.
    // One with an edit on top of it, whose branch waited for Now's piece, is kept instead, edit and all: never thrown away.
    if (shown.has_value())
    {
        auto gone = std::exchange (shown, std::nullopt);

        if (gone->branchWaits)
            gone->trial->keep ({});
        else
            gone->trial->putBack();
    }

    // What it was given to write (a commit, a snapshot) is written, never dropped nor cut short, however long that takes:
    // it is the user's work. Its thread finishes it after the history has gone, the message thread waiting for none of it,
    // and the project's next history reads the folder only after it (open()). What it was given to read is let go, the
    // read under way at its next step: nothing waits for it any more.
    closing->store (true);

    if (thread->getNumJobs() > 0)
        FinishingHistories::get().keep (folder, std::move (thread), writesLeft);
}

void VersionHistory::endCopy (FolderCopyState& copy, const juce::String& why)
{
    const std::scoped_lock lock (copy.lock);
    copy.over = true;
    copy.why = why;
    holdFolder (copy.folder, false);
}

bool VersionHistory::keepsVersions() const
{
    return store != nullptr;
}

bool VersionHistory::isWriting() const
{
    return writesLeft->load() > 0 || (folder != juce::File() && FinishingHistories::get().countWrites (folder) > 0);
}

void VersionHistory::run (std::function<void()> job, Work work)
{
    ++pendingJobs;
    const auto writes = work == Work::writes;

    if (writes)
        ++*writesLeft;

    thread->add (
        [job = std::move (job), writes, closingNow = closing, left = writesLeft] (const BackgroundJobs::ShouldStop&)
        {
            if (! writes)
            {
                // Tests' seam: a read held where it starts until let go (holdReadsForTests), or once so many have started
                // (readsToStartForTests; a history's reads start one after another, on its one thread).
                while ((holdReadsForTests.load() || readsToStartForTests.load() == 0) && ! closingNow->load())
                    juce::Thread::sleep (1);

                if (readsToStartForTests.load() > 0)
                    --readsToStartForTests;

                if (! closingNow->load())
                    job();

                return;
            }

            // A test's seam: a write made to take as long as the test wants (holdWritesForTests).
            while (holdWritesForTests.load())
                juce::Thread::sleep (1);

            job();
            --*left;
        });
}

void VersionHistory::waitForThread() const
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Its jobs tell the message thread what they did with callAsync and never wait for it, so it can wait for them,
    // however long they take: a folder copied while a job still writes into it wouldn't be whole. And so for a history of
    // the same folder that went before it. Nothing waits for housekeeping: a sweep under way stops, changing nothing (a later
    // open sweeps), and so does Verify History.
    stopHousekeeping();
    FinishingHistories::get().waitFor (folder);
    thread->waitForAll();
}

void VersionHistory::stopHousekeeping() const
{
    sweepStopped->store (true);
    checkCancelled->store (true);
}

void VersionHistory::copyFolder (std::function<juce::Result (const BackgroundJobs::ShouldStop&)> copy,
                                 std::function<void (const juce::Result&)> copied, std::function<void (const juce::String&)> bounceAgain)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Nothing more is written meanwhile: a bounce under way, which renders on a thread of its own, stops, to be made again
    // once the copy is over (bounceAgain); and the history gives its thread no write until endFolderCopy().
    if (backgroundBounce != nullptr)
    {
        bounceWaiting = bouncingVersion;
        backgroundBounce = nullptr;
        bouncingVersion.clear();
        bounceProblem.clear();
    }

    restartBounce = std::move (bounceAgain);
    copyingFolder = true;
    stopHousekeeping(); // the copy doesn't wait behind a sweep or a check
    tellNow();

    // What the copy job and the message thread share: whether the copy is over (its hold let go), whether the job has run,
    // and what to call back, which a history that goes with the job still waiting calls with a failure (~VersionHistory()).
    folderCopy = std::make_shared<FolderCopyState>();
    folderCopy->folder = folder;
    folderCopy->copied = std::move (copied);

    // A write, which the history's thread runs even once it is handed on as the history goes (~VersionHistory()), so that its
    // callback always comes: copied, or told why not.
    run (
        [copy = std::move (copy), state = folderCopy, weak = juce::WeakReference<VersionHistory> (this),
         earlier = FinishingHistories::get().getKept (folder)]() mutable
        {
            // After what the histories of the same folder that went before still write into it (FinishingHistories), as
            // open()'s job reads it: those writes, queued before this copy on their own threads, are done before the folder
            // is held.
            for (const auto& jobs : earlier)
                jobs->waitForAll();

            // The folder held from here, after every write given before this job (WholeFile.h: holdFolder()), until the copy is
            // over; none, if it is over before it began (the project closed, or the save was cancelled).
            juce::String over;

            {
                const std::scoped_lock lock (state->lock);

                if (state->over)
                    over = state->why;
                else
                    holdFolder (state->folder, true);
            }

            // Stopped between pieces once it is over: the project gone, its copy would be for nothing.
            const auto result = over.isNotEmpty() ? juce::Result::fail (over) : copy ([&state] { return state->over.load(); });
            auto outcome = result;

            {
                const std::scoped_lock lock (state->lock);

                if (result.wasOk() && state->over.load())
                    outcome = juce::Result::fail (state->why);
            }

            juce::MessageManager::callAsync (
                [weak, state, outcome]
                {
                    if (auto* self = weak.get())
                        --self->pendingJobs;

                    if (const auto onCopied = std::exchange (state->copied, nullptr); onCopied != nullptr)
                        onCopied (outcome);
                });
        },
        Work::writes);
}

void VersionHistory::endFolderCopy()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! std::exchange (copyingFolder, false))
        return;

    if (const auto copy = std::exchange (folderCopy, nullptr))
        endCopy (*copy, "The copy was cancelled.");

    // Into the folder the project is in now; then, at the next turn of the message loop, what waited for the copy: unless the
    // history goes first, as the project closes (a quit, another project opening), when it would write for nothing.
    followFolder();
    readBounces();
    tellNow();

    juce::MessageManager::callAsync (
        [weak = juce::WeakReference<VersionHistory> (this)]
        {
            if (auto* self = weak.get())
                self->startWhatWaitedForTheCopy();
        });
}

void VersionHistory::whenNotCopying (std::function<void()> then)
{
    if (copyingFolder)
        waitingForCopy.push_back (std::move (then));
    else
        then();
}

void VersionHistory::startWhatWaitedForTheCopy()
{
    if (copyingFolder)
        return; // another copy began meanwhile: once it is over

    for (auto& then : std::exchange (waitingForCopy, {}))
        then();

    if (std::exchange (initialCommitWaiting, false) && branches.empty())
        saveVersion ("First version", {});

    if (std::exchange (openSnapshotWaiting, false) && ! versions.empty())
    {
        // Once its changes are known to be some (changesWorked()), if they aren't yet.
        markNotesHeard();

        if (sinceBase.isUpToDate())
        {
            if (hasChanges())
                takeSnapshot ("open");
        }
        else
        {
            snapshotOnFirstChanges = true;
        }
    }

    if (const auto bounce = std::exchange (bounceWaiting, {}); bounce.isNotEmpty() && restartBounce != nullptr)
        restartBounce (bounce);
}

struct VersionHistory::Opened
{
    std::vector<BranchRecord> branches;
    juce::String current;
    std::map<juce::String, VersionRecord> versions, departed;
    juce::ValueTree head;
    PluginParameterValues parameters;
    std::map<juce::String, MessageEdit> messages;
    std::vector<Snapshot> snapshots;
    std::optional<std::pair<juce::String, Merge>> mergeRead;
    std::map<juce::String, UnreadableHead> unreadable;
    std::vector<BranchMove> recentMoves;
    juce::String sweepProblem;
};

void VersionHistory::open()
{
    ++generation;
    folder = getVersionsFolder (song);
    store = folder != juce::File() ? std::make_shared<VersionStore> (folder, getThisComputer()) : nullptr;
    branches.clear();
    versions.clear();
    versionChanges.clear();
    versionChangesBeingRead.clear();
    messageEdits.clear();
    snapshots.clear();
    unreadableHeads.clear();
    departed.clear();
    recentMoves.clear();
    lastCheck.reset(); // another folder's (Save As), or this one's before its sweep
    sweepProblem.clear();
    adoptionProblem.clear();
    adoptionFailed.clear();
    adoptionQueued = adoptAgain = false;                        // a round of the store before ends unheard (its generation)
    sweepStopped = std::make_shared<std::atomic<bool>> (false); // this store's sweep, as it opens
    selectedSnapshot = 0;
    editedSinceSnapshot = false;
    currentBranch.clear();
    baseVersion.clear();
    missingBranch.clear();
    baseProblem.clear();
    previousBaseline = {};
    setBaseline ({}, {}, {});
    hasRead = store == nullptr;

    if (store == nullptr)
        return;

    // Its branches, every version their last ones follow, and the current branch's last version's song; and the merge the
    // song holds, if any, found again here rather than on the message thread.
    run (
        [shared = store, number = generation, weak = juce::WeakReference<VersionHistory> (this),
         mergingElement = song.getState().getChildWithName (schema::MERGING).createCopy(), closingNow = closing,
         earlier = FinishingHistories::get().getKept (folder), sweepStop = sweepStopped, names = readNames (song.getState()),
         songNow = readNames (song.getState()).base.isNotEmpty() ? copyWholeSong (song) : juce::ValueTree()]
        {
            // Tests' seam: the read held here, begun, and counted while it is (holdFirstReadsForTests).
            if (holdFirstReadsForTests.load())
            {
                ++firstReadsHeldForTests;

                while (holdFirstReadsForTests.load())
                    juce::Thread::sleep (1);

                --firstReadsHeldForTests;
            }

            // After what the histories of the same folder that went before this one opened were still writing (a commit
            // given as its project closed), and what that wrote.
            for (const auto& jobs : earlier)
                jobs->waitForAll();
            shared->getPieces().readNewPacks();

            // Swept (StoreSweep.h): its packs joined into one once there are many, as each is read as the store opens, or
            // once a week while something may have aged out, leaving out what nothing names that is older than 30 days;
            // before anything else here reads or writes it. Given up, the packs as they were, if the history goes meanwhile.
            const auto swept = sweepStore (*shared, mergingElement, maxPacks,
                                           [&closingNow, &sweepStop] { return closingNow->load() || sweepStop->load(); });

            // Every computer's record of each branch, and every version their last ones follow; then each branch as this
            // computer takes them together.
            const auto records = shared->readBranches();
            auto current = shared->readCurrentBranch();
            std::map<juce::String, VersionRecord> versionsRead;

            // A commit and every one it follows, those that can be read; whether it could be.
            const auto readLine = [&shared, &versionsRead, &closingNow] (const juce::String& from)
            {
                juce::StringArray toRead (from);

                while (! toRead.isEmpty() && ! closingNow->load())
                {
                    const auto id = toRead[toRead.size() - 1];
                    toRead.remove (toRead.size() - 1);

                    if (id.isEmpty() || versionsRead.count (id) > 0)
                        continue;

                    if (auto version = shared->readVersion (id))
                    {
                        toRead.addArray (version->parents);
                        versionsRead.emplace (id, std::move (*version));
                    }
                }

                return versionsRead.count (from) > 0;
            };

            for (const auto& record : records)
                readLine (record.head);

            if (closingNow->load())
                return;

            auto branchesRead = joinBranchRecords (records, versionsRead, shared->getComputer());

            // The branch the song says it is on, where the folder has it, and the version it grew from with its line, whether
            // a branch holds it or not (Schema.h: versionsBranch, versionsBase).
            const auto songsBranchThere = std::any_of (branchesRead.begin(), branchesRead.end(),
                                                       [&names] (const BranchRecord& branch) { return branch.id == names.branch; });

            if (songsBranchThere)
                current = names.branch;

            // One the folder hasn't (another computer's record of it not synced yet, or deleted): the song keeps growing from its
            // version where that reads, and no version is saved on the branch the folder names meanwhile (canSaveOnBranch()).
            const auto branchMissing = ! branchesRead.empty() && names.branch.isNotEmpty() && ! songsBranchThere;

            if (names.base.isNotEmpty())
                readLine (names.base);

            // One the branch moved back from on this computer (Undo Last Version, a reset), which follows its last version, and
            // which the song's file wasn't saved after: the song grows from the branch's last version, the work of the one it
            // moved back from its change (the review of S4.5: growing from that one, the next version put it back under itself).
            auto namedBase = names.base;

            for (const auto& branch : branchesRead)
                if (songsBranchThere && branch.id == names.branch && namedBase.isNotEmpty() && namedBase != branch.head
                    && versionsRead.count (branch.head) > 0 && follows (versionsRead, namedBase, branch.head))
                    for (const auto& move : shared->readBranchLog (branch.id))
                        if (move.from == names.base && (move.why == "undo" || move.why == "reset"))
                            namedBase.clear();

            // Each branch's last versions on the other computers' records, which stay on it whatever this one's says.
            std::map<juce::String, juce::StringArray> elsewhere;

            for (const auto& record : records)
                if (record.computer != makeUsableFileName (shared->getComputer()) && versionsRead.count (record.head) > 0)
                    elsewhere[record.id].addIfNotAlreadyThere (record.head);

            // A branch whose last commit can't be read (none of its computers' records' can): the newest commit that reads of
            // its logs (every computer's, as a project synced or copied from another computer has no log of this one's), or
            // else of the commits the packs hold that were made on it, offered in its place, its line kept in the graph
            // meanwhile.
            std::map<juce::String, UnreadableHead> unreadable;
            juce::StringArray offered;
            std::optional<std::vector<VersionRecord>> inPacks; // the packs' commits, newest first, read once if needed

            for (const auto& branch : branchesRead)
            {
                if (branch.head.isEmpty() || versionsRead.count (branch.head) > 0)
                    continue;

                auto& entry = unreadable[branch.id];
                entry.head = branch.head;
                const auto moves = shared->readEveryBranchLog (branch.id);

                for (auto move = moves.rbegin(); move != moves.rend() && entry.readable.isEmpty(); ++move)
                    for (const auto& candidate : { move->to, move->from })
                        if (entry.readable.isEmpty() && candidate.isNotEmpty() && candidate != branch.head && readLine (candidate))
                            entry.readable = candidate;

                if (entry.readable.isEmpty())
                {
                    if (! inPacks.has_value())
                        inPacks = findCommitsInPacks (shared->getPieces(), [&closingNow] { return closingNow->load(); });

                    for (const auto& record : *inPacks)
                        if (entry.readable.isEmpty() && record.branch == branch.id && record.id != branch.head && readLine (record.id))
                            entry.readable = record.id;
                }

                offered.addIfNotAlreadyThere (entry.readable);
            }

            offered.removeEmptyStrings();
            auto held = offered;

            if ((songsBranchThere || branchMissing) && versionsRead.count (namedBase) > 0)
                held.add (namedBase); // the version the song grew from, on a branch or not

            keepReachable (versionsRead, branchesRead, held); // not what deleted branches alone held
            juce::ValueTree head;
            PluginParameterValues parameters;

            // A current branch that isn't there any more: the first one made.
            if (! branchesRead.empty()
                && std::none_of (branchesRead.begin(), branchesRead.end(),
                                 [&current] (const auto& branch) { return branch.id == current; }))
                current = sortByFirstVersion (branchesRead, versionsRead).front().id;

            // The song's base: the version its names say it grew from, on this branch, where its song reads; else the branch's
            // last commit, and what couldn't be read of the one named said (none for a song that named none). Its song: none
            // while the last commit can't be read and a commit that reads is offered; an empty song where none is, as a commit
            // then starts its line again.
            juce::String base, baseSaid;

            const auto missingSaid = [&branchesRead, &current] (bool reads)
            {
                juce::String name;

                for (const auto& branch : branchesRead)
                    if (branch.id == current)
                        name = branch.name;

                return juce::String ("This song is on a branch this Versions folder doesn't have (yet: another computer's record of it "
                                     "may still be syncing; or it was deleted). ")
                       + (reads ? "Its changes are worked out against the version it grew from. "
                                : "The version it grew from can't be read either: its changes are worked out against '" + name
                                      + "''s last version. ")
                       + "No version is saved on '" + name + "', nor any branch switched to, nor '" + name
                       + "' moved back (Undo Last Version, Reset Branch), meanwhile, which would take that branch's work out or "
                         "leave the song on it: Create Branch first, which keeps working from the song on a branch of its own.";
            };

            if (branchMissing)
                baseSaid = missingSaid (false);

            for (const auto& branch : branchesRead)
            {
                if (branch.id != current)
                    continue;

                base = branch.head;

                if ((songsBranchThere || branchMissing) && namedBase.isNotEmpty() && namedBase != branch.head)
                {
                    if (const auto found = versionsRead.find (namedBase); found != versionsRead.end())
                        head = readCommitSong (*shared, found->second.song, &parameters);

                    if (branchMissing && head.isValid())
                    {
                        base = namedBase;
                        baseSaid = missingSaid (true);
                        continue;
                    }

                    // Where the branch has newer versions and the song is its last one's as it is (a version written as
                    // the project closed, or one whose names the project file couldn't take): it grew from that one.
                    const auto last = versionsRead.find (branch.head);
                    PluginParameterValues lastParameters;
                    const auto lastSong =
                        head.isValid() && songNow.isValid() && last != versionsRead.end() && follows (versionsRead, branch.head, namedBase)
                            ? readCommitSong (*shared, last->second.song, &lastParameters)
                            : juce::ValueTree();

                    if (lastSong.isValid() && findChanges (lastSong, songNow).empty())
                    {
                        head = lastSong;
                        parameters = std::move (lastParameters);
                        continue;
                    }

                    if (head.isValid())
                    {
                        base = namedBase;
                        continue;
                    }

                    if (! branchMissing)
                        baseSaid = "The version this song grew from (" + namedBase.substring (0, 7)
                                   + ") can't be read from the Versions folder: it is damaged, or its pieces haven't all reached this "
                                     "computer. The changes are worked out against the branch's last version instead, which the song "
                                     "grows from now. Nothing of the song was lost.";
                    parameters.clear();
                }

                if (const auto found = versionsRead.find (branch.head); found != versionsRead.end())
                    head = readCommitSong (*shared, found->second.song, &parameters);
                else if (const auto broken = unreadable.find (branch.id); broken != unreadable.end() && broken->second.readable.isEmpty())
                    head = juce::ValueTree (schema::PHAZITE);
            }

            auto read = std::make_shared<Opened>();
            read->sweepProblem = swept.sure ? juce::String() : swept.why;

            if (const auto mergeState = readMergeState (mergingElement))
                read->mergeRead.emplace (getMergeKey (mergingElement), findMergeAgain (*shared, versionsRead, *mergeState));

            // What left every branch within the last 30 days, by every computer's logs: the lines it left, read, which
            // Recover Lost Versions lists.
            const auto now = getVersionsTime();

            for (auto& move : shared->readEveryLog())
                if (now - move.time <= keptByAge::dailyFor)
                    read->recentMoves.push_back (std::move (move));

            for (const auto& move : read->recentMoves)
            {
                juce::StringArray toRead (move.from);

                while (! toRead.isEmpty() && ! closingNow->load())
                {
                    const auto id = toRead[toRead.size() - 1];
                    toRead.remove (toRead.size() - 1);

                    if (id.isEmpty() || versionsRead.count (id) > 0 || read->departed.count (id) > 0)
                        continue;

                    if (auto version = shared->readVersion (id))
                    {
                        toRead.addArray (version->parents);
                        read->departed.emplace (id, std::move (*version));
                    }
                }
            }

            shared->getPieces().closeFiles();
            read->branches = std::move (branchesRead);
            read->current = current;
            read->versions = std::move (versionsRead);
            read->head = head;
            read->parameters = std::move (parameters);
            read->messages = shared->readMessages();
            read->snapshots = shared->readSnapshots (shared->getKeepingTime());
            read->unreadable = std::move (unreadable);

            juce::MessageManager::callAsync (
                [weak, number, read, base, baseSaid, elsewhere, missing = branchMissing ? names.branch : juce::String()]() mutable
                {
                    if (auto* self = weak.get())
                    {
                        self->baseRead (number, base, baseSaid, std::move (elsewhere), missing);
                        self->opened (number, std::move (*read));

                        if (number == self->generation)
                            self->writeNames(); // where they named nothing it has, or something it couldn't read
                    }
                });
        },
        Work::reads);
}

void VersionHistory::opened (int number, Opened read)
{
    --pendingJobs;

    if (number != generation)
        return;

    branches = std::move (read.branches);
    versions = std::move (read.versions);
    departed = std::move (read.departed);
    sweepProblem = read.sweepProblem;
    recentMoves = std::move (read.recentMoves);
    unreadableHeads = std::move (read.unreadable);
    messageEdits = std::move (read.messages);
    applyMessageEdits();
    snapshots.assign (read.snapshots.rbegin(), read.snapshots.rend());
    lastSnapshotTime = getVersionsTime();
    currentBranch = read.current;
    setBaseline (read.head, std::move (read.parameters), song.getDeviceChangesOutsideHistory());

    // The merge the song holds, as found on the history's thread, its plug-ins' clashes named here.
    if (read.mergeRead.has_value())
        keepFound (read.mergeRead->first, std::move (read.mergeRead->second));

    mergeDirty = true;
    hasRead = true;
    getMerging();  // the merge the song holds, as read already, or read now
    readBounces(); // and tells
    if (! noAdoptionAtOpenForTests.load())
        adoptLater(); // what the history shows that another computer wrote, made this computer's own too

    // A project keeps versions from its start, as a repository from its first commit: the song as it is now, on main; once a
    // first save's copy of the folder is over, if one is under way (copyFolder()). Only where the Versions folder has no
    // branch: one whose commits can't be read says so instead (getHeadProblem()).
    if (branches.empty())
    {
        if (copyingFolder)
            initialCommitWaiting = true;
        else
            saveVersion ("First version", {});

        return;
    }

    // Local History: the song as it opened, where it differs from the version it grew from (it was left with changes), once
    // its changes are worked out (changesWorked()), which starts now.
    snapshotOnFirstChanges = true;
    askForChanges();
}

void VersionHistory::followFolder()
{
    // The project's folder came with it (Project::moveTo()), its versions too: the same history, in the new folder.
    if (const auto moved = getVersionsFolder (song); moved != folder && store != nullptr && moved != juce::File())
    {
        folder = moved;
        const auto newest = store->getPieces().getNewestStamp(); // the stamps go on from where they were
        store = std::make_shared<VersionStore> (folder, getThisComputer());
        store->getPieces().raiseStampFloor (newest < std::numeric_limits<juce::int64>::max() ? newest + 1 : newest);
        readBounces(); // those the copy took: a bounce that wasn't copied is none here (known bug 169)
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

    auto read = readCommitSong (*store, version->song, parameters, missing);
    store->getPieces().closeFiles();
    return read;
}

void VersionHistory::readCommitInBackground (const juce::String& versionID, std::function<void (const juce::ValueTree&)> then)
{
    const auto* version = findVersion (versionID);

    if (version == nullptr || store == nullptr)
    {
        then ({});
        return;
    }

    run (
        [shared = store, piece = version->song, then = std::move (then), weak = juce::WeakReference<VersionHistory> (this)]
        {
            auto read = readCommitSong (*shared, piece);
            shared->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, then, read]
                {
                    if (auto* self = weak.get())
                    {
                        --self->pendingJobs;
                        then (read);
                    }
                });
        },
        Work::reads);
}

juce::ValueTree VersionHistory::readSongOwn (const juce::String& versionID) const
{
    const auto* version = findVersion (versionID);

    if (version == nullptr || store == nullptr)
        return {};

    std::vector<MissingPiece> others; // what it doesn't read
    auto read = joinSong (
        version->song, [this, own = version->song] (const juce::String& name)
        { return name == own ? store->getPieces().read (name) : std::nullopt; }, nullptr, &others, folder.getParentDirectory());
    store->getPieces().closeFiles();
    return read;
}

bool VersionHistory::checkHistory (std::function<void()> done)
{
    if (store == nullptr || ! hasRead || isSaving() || checking)
        return false;

    checking = true;
    checkProgress->store (0.0f);
    checkCancelled->store (false);
    checkProgressTold = 0.0f;
    tellNow(); // "Verifying..."

    run (
        [shared = store, number = generation, known = versions, weak = juce::WeakReference<VersionHistory> (this), done,
         closingNow = closing, progress = checkProgress, cancelled = checkCancelled, projectFolder = song.getFile().getParentDirectory()]
        {
            // Every pack the folder holds now, those written since the history opened too.
            shared->getPieces().readNewPacks();

            Check check;
            const auto names = shared->getPieces().getNames();
            check.numPieces = names.size();

            // Every pack file as it is on the disk now: one that doesn't read whole as a pack (held, cut off, a byte of its
            // index or its trailer changed, or of a layout this build doesn't read) might hold anything.
            for (const auto& pack : shared->getPieces().findDamagedPacks())
                check.unreadPacks.add (pack.getFileName());
            check.numVersions = (int) known.size();

            // Let go of, at its next step, as the history goes; stopped there, as it is cancelled.
            const auto stop = [&]
            {
                check.stopped = check.stopped || cancelled->load();
                return closingNow->load() || check.stopped;
            };

            // How far it has come: the pieces a tenth of it, the songs kept another, the audio, by its bytes, the rest.
            const auto tell = [&progress] (float from, float to, double part, double all)
            { progress->store (from + (to - from) * (all > 0.0 ? (float) juce::jlimit (0.0, 1.0, part / all) : 1.0f)); };

            // Every piece read, which checks it against its name, and the versions among them. A damaged piece's copies are
            // forgotten as they are read, so that the next version that holds it writes it again (PieceStore.h).
            std::vector<VersionRecord> found;

            for (int i = 0; i < names.size() && ! stop(); ++i)
            {
                tell (0.0f, 0.1f, i, names.size());
                const auto bytes = shared->getPieces().read (names[i]);

                if (! bytes.has_value())
                {
                    check.damagedPieces.add (names[i]);
                    continue;
                }

                constexpr const char* versionStart = "<VERSION ";

                if (bytes->getSize() > std::strlen (versionStart)
                    && std::memcmp (bytes->getData(), versionStart, std::strlen (versionStart)) == 0)
                    if (auto record = toVersionRecord (readElement (*bytes), names[i]))
                        found.push_back (std::move (*record));
            }

            // Each version's song walked piece by piece, and each song kept without a version (Local History's, those
            // left on branches); the versions no branch's line holds.
            for (const auto& [id, version] : known)
            {
                if (stop())
                    break;

                std::vector<MissingPiece> missing;
                shared->readSong (version.song, nullptr, &missing);

                if (! missing.empty())
                    check.damaged[id] = std::move (missing);

                // The versions it follows, which the history and Restore need: one that can't be read is said.
                for (const auto& parent : version.parents)
                    if (known.count (parent) == 0 && ! shared->readVersion (parent).has_value())
                        check.unreadableParents[id].add (parent);
            }

            const auto kept = shared->findKeptSongs (getVersionsTime());
            check.numSongs = (int) kept.size();

            for (size_t i = 0; i < kept.size() && ! stop(); ++i)
            {
                tell (0.1f, 0.2f, (double) i, (double) kept.size());
                std::vector<MissingPiece> missing;

                if (const auto read = shared->readSong (kept[i].piece, nullptr, &missing); ! read.isValid() && missing.empty())
                    missing.push_back ({ {}, kept[i].piece });

                if (! missing.empty())
                    check.damagedSongs.push_back ({ kept[i], std::move (missing) });
            }

            shared->getPieces().closeFiles();

            for (auto& record : found)
                if (known.count (record.id) == 0)
                    check.lost.push_back (std::move (record));

            std::sort (check.lost.begin(), check.lost.end(), [] (const auto& a, const auto& b) { return a.saved > b.saved; });

            // Every version's audio files, each file read once however many versions name it, against what each version
            // recorded of it: there, of that size, and of that SHA-256.
            struct Recorded
            {
                juce::String version;
                VersionRecord::File file;
            };

            std::map<juce::String, std::vector<Recorded>> byPath;

            for (const auto& [id, version] : known)
                for (const auto& file : version.files)
                    byPath[file.path].push_back ({ id, file });

            check.numAudioFiles = (int) byPath.size();
            const auto resolve = [&projectFolder] (const juce::String& path)
            { return juce::File::isAbsolutePath (path) ? makeFileOfFullPath (path) : projectFolder.getChildFile (path); };

            double bytesToRead = 0.0, bytesRead = 0.0;

            for (const auto& [path, recorded] : byPath)
                bytesToRead += (double) juce::jmax ((juce::int64) 0, resolve (path).getSize());

            for (const auto& [path, recorded] : byPath)
            {
                if (stop())
                    break;

                const auto file = resolve (path);
                const auto exists = file.existsAsFile();
                const auto size = exists ? file.getSize() : 0;
                juce::String hash;

                // Hashed in parts, Stop (and the history going) looked at between them, the bytes counted as they go.
                if (exists
                    && std::any_of (recorded.begin(), recorded.end(), [size] (const Recorded& each) { return each.file.size == size; }))
                {
                    // Read a megabyte at a time (JUCE's SHA-256 asks for 64 bytes a call, and a file's stream doesn't buffer);
                    // nothing made on the stream before it opened (a buffer asks its source's length as it is made, which
                    // a stream that didn't open asserts on: a file another program holds fails to open on Windows, one
                    // without read permission on Linux and macOS).
                    juce::FileInputStream in (file);

                    if (in.openedOk())
                    {
                        juce::BufferedInputStream buffered (in, 1 << 20);
                        StoppableStream parts (buffered, stop,
                                               [&] (int numRead)
                                               {
                                                   bytesRead += numRead;
                                                   tell (0.2f, 1.0f, bytesRead, bytesToRead);
                                               });
                        hash = juce::SHA256 (parts).toHexString();
                    }

                    if (stop())
                        break; // its hash only of what was read before

                    if (! in.openedOk() || in.getStatus().failed() || in.getPosition() != size)
                        hash = {}; // it can't be read: said so, not as changed
                }
                else
                {
                    bytesRead += (double) size;
                }

                tell (0.2f, 1.0f, bytesRead, bytesToRead);

                for (const auto& each : recorded)
                {
                    using Kind = AudioProblem::Kind;

                    if (! exists)
                        check.audio[each.version].push_back ({ path, Kind::missing });
                    else if (size != each.file.size)
                        check.audio[each.version].push_back ({ path, Kind::otherSize });
                    else if (hash.isEmpty())
                        check.audio[each.version].push_back ({ path, Kind::unreadable });
                    else if (hash != each.file.sha256)
                        check.audio[each.version].push_back ({ path, Kind::otherContent });
                }
            }

            if (closingNow->load())
                return;

            progress->store (1.0f);

            juce::MessageManager::callAsync (
                [weak, number, check, done]() mutable
                {
                    if (auto* self = weak.get())
                        self->checked (number, std::move (check));

                    if (done != nullptr)
                        done();
                });
        },
        Work::reads);

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

std::vector<VersionHistory::LostLine> VersionHistory::getLostLines() const
{
    const auto now = getVersionsTime();

    // The versions no branch holds that can come back: those that left within 30 days, and those the last check found.
    std::map<juce::String, const VersionRecord*> lost;

    for (const auto& [id, record] : departed)
        if (versions.count (id) == 0)
            lost.emplace (id, &record);

    if (lastCheck.has_value())
        for (const auto& record : lastCheck->lost)
            if (versions.count (record.id) == 0)
                lost.emplace (record.id, &record);

    // A line's last version is one no other lost one follows; its versions, those of the lost it follows, newest first.
    std::set<juce::String> followed;

    for (const auto& [id, record] : lost)
        for (const auto& parent : record->parents)
            if (lost.count (parent) > 0)
                followed.insert (parent);

    const auto findLine = [&lost] (const juce::String& head)
    {
        std::vector<const VersionRecord*> line;
        std::set<juce::String> seen;
        juce::StringArray toVisit (head);

        while (! toVisit.isEmpty())
        {
            const auto id = toVisit[toVisit.size() - 1];
            toVisit.remove (toVisit.size() - 1);

            if (const auto found = lost.find (id); found != lost.end() && seen.insert (id).second)
            {
                line.push_back (found->second);
                toVisit.addArray (found->second->parents);
            }
        }

        std::sort (line.begin(), line.end(), [] (const auto* a, const auto* b) { return a->saved > b->saved; });
        juce::StringArray ids;

        for (const auto* record : line)
            ids.add (record->id);

        return ids;
    };

    // A deleted branch's song brought back already, by a recovery since.
    const auto wasRecovered = [this] (const BranchMove& deleted)
    {
        return std::any_of (
            recentMoves.begin(), recentMoves.end(), [&deleted] (const BranchMove& move)
            { return move.why == "recover" && move.to == deleted.from && move.song == deleted.song && move.time >= deleted.time; });
    };

    // Each by the newest move that left it, within 30 days; a deleted branch's song by its deletion.
    std::vector<LostLine> lines;
    std::set<juce::String> keys;

    for (auto move = recentMoves.rbegin(); move != recentMoves.rend(); ++move)
    {
        if (move->from.isEmpty() || now - move->time > keptByAge::dailyFor || move->why == "cancel")
            continue;

        // A song left there that is its last version's, the same pieces, holds nothing to bring back.
        const auto isLine = lost.count (move->from) > 0 && followed.count (move->from) == 0;
        const auto* last = lost.count (move->from) > 0 ? lost.at (move->from) : findVersion (move->from);
        const auto leftSong =
            move->why == "delete" && ! wasRecovered (*move) && (last == nullptr || last->song != move->song) ? move->song : juce::String();

        if (! isLine && leftSong.isEmpty())
            continue;

        // One row for a head: the newest move's, but for a deleted branch's song no newer row brings back (another branch
        // deleted at the same version, with changes of its own).
        const auto sameHead = std::any_of (lines.begin(), lines.end(), [&move] (const LostLine& line) { return line.head == move->from; });

        if (sameHead
            && (leftSong.isEmpty()
                || std::any_of (lines.begin(), lines.end(), [&leftSong] (const LostLine& line) { return line.song == leftSong; })))
            continue;

        if (const auto key = move->from + "/" + leftSong; keys.insert (key).second)
            lines.push_back (
                { key, move->from, move->name, move->why, move->time, leftSong, isLine ? findLine (move->from) : juce::StringArray(), {} });
    }

    // The lines the check found that no move names, by their last versions' branches.
    for (const auto& [id, record] : lost)
    {
        if (followed.count (id) > 0 || std::any_of (lines.begin(), lines.end(), [&id] (const LostLine& line) { return line.head == id; }))
            continue;

        juce::String branchName;

        for (const auto& branch : branches)
            if (branch.id == record->branch)
                branchName = branch.name;

        lines.push_back ({ id + "/", id, branchName, "found", record->saved, {}, findLine (id), {} });
    }

    // What each one's last version is called, as its message was edited.
    for (auto& line : lines)
    {
        const auto* last = lost.count (line.head) > 0 ? lost.at (line.head) : findVersion (line.head);
        const auto edit = messageEdits.find (line.head);

        if (last != nullptr)
            line.name =
                edit != messageEdits.end() ? (edit->second.name.isNotEmpty() ? edit->second.name : last->summary) : getDisplayName (*last);
    }

    std::stable_sort (lines.begin(), lines.end(), [] (const LostLine& a, const LostLine& b) { return a.time > b.time; });
    return lines;
}

juce::String VersionHistory::recoverLine (const juce::String& key)
{
    followFolder();

    if (store == nullptr || ! hasRead || isSaving())
        return {};

    const auto lines = getLostLines();
    const auto line = std::find_if (lines.begin(), lines.end(), [&key] (const LostLine& each) { return each.key == key; });

    if (line == lines.end())
        return {};

    // Still in the store: one the sweep let go of since it was listed (a check of another folder's, a list read before a
    // sweep) comes back as nothing, and the view says so.
    if (! store->getPieces().has (line->head) || (line->song.isNotEmpty() && ! store->getPieces().has (line->song)))
    {
        readingProblem = "That line of versions is no longer kept in the Versions folder: more than 30 days went by since it left "
                         "its branch, and it was let go of.";
        tellNow();
        return {};
    }

    // Its versions taken into the history again, as their messages were edited before they were lost.
    std::map<juce::String, VersionRecord> records;

    for (const auto& id : line->versions)
    {
        if (const auto found = departed.find (id); found != departed.end())
            records.emplace (id, found->second);
        else if (lastCheck.has_value())
            for (const auto& record : lastCheck->lost)
                if (record.id == id)
                    records.emplace (id, record);
    }

    if (records.count (line->head) == 0 && findVersion (line->head) == nullptr)
        return {};

    // "main (recovered)", "main (recovered 2)"; "recovered" for a branch no log names.
    const auto isTaken = [this] (const juce::String& name)
    {
        return std::any_of (branches.begin(), branches.end(), [&name] (const auto& branch) { return branch.name.equalsIgnoreCase (name); });
    };

    const auto of = line->branch.isNotEmpty() ? line->branch.substring (0, 80).trimEnd() + " " : juce::String();
    const auto named = [&of] (int number)
    {
        const auto word = "recovered" + (number > 1 ? " " + juce::String (number) : juce::String());
        return of.isEmpty() ? word : of + "(" + word + ")";
    };

    auto name = named (1);

    for (int number = 2; isTaken (name); ++number)
        name = named (number);

    BranchRecord branch;
    branch.id = juce::Uuid().toString();
    branch.name = toBranchName (name);
    branch.colour = getTrackColours()[(branches.size() + 1) % getTrackColours().size()].colour;
    branch.head = line->head;
    branch.computer = getThisComputer();
    branch.changed = stampNow();
    branch.working = line->song; // the song as it was left there, to switch to

    if (! store->writeBranch (branch))
        return {};

    logMove (branch.id, {}, line->head, "recover", branch.name, line->song);
    branches.push_back (branch);

    for (auto& [id, record] : records)
        versions.emplace (id, std::move (record));

    applyMessageEdits();
    adoptLater();
    readBounces(); // theirs count again; and tells
    return branch.id;
}

juce::int64 VersionHistory::stampNow() const
{
    return store != nullptr ? store->stamp() : getVersionsTime();
}

void VersionHistory::adoptLater()
{
    // Only while the sweep lets go: with nothing deleted, nothing another computer wrote needs a copy of this one's.
    if (store == nullptr || ! hasRead || ! doesSweepLetGo())
        return;

    if (adoptionQueued)
    {
        adoptAgain = true; // once the round under way is done, with what the history names then
        return;
    }

    // Every version the history shows and those Recover Lost Versions offers, the merge waiting in the song, and the songs
    // the branches name as left on them (a recovered branch's, which another computer may have left); not what couldn't be
    // copied before, until the history opens again. What already has an own copy is passed over at once.
    Adoption work;

    const auto add = [this] (juce::StringArray& into, const juce::String& id)
    {
        if (id.isNotEmpty() && adoptionFailed.count (id) == 0)
            into.addIfNotAlreadyThere (id);
    };

    for (const auto* named : { &versions, &departed })
        for (const auto& [id, version] : *named)
            add (work.versions, id);

    if (const auto waiting = song.getState().getChildWithName (schema::MERGING); waiting.isValid())
    {
        add (work.versions, waiting[schema::mergingVersion].toString());
        add (work.versions, waiting[schema::mergingHead].toString());
        add (work.songs, waiting[schema::mergingBefore].toString());
    }

    for (const auto& branch : branches)
        add (work.songs, branch.working);

    adoptionQueued = true;
    adoptAgain = false;
    adoptRound (std::move (work));
}

void VersionHistory::adoptRound (Adoption work)
{
    run (
        [shared = store, work, closingNow = closing, stopped = sweepStopped, number = generation,
         weak = juce::WeakReference<VersionHistory> (this)]() mutable
        {
            const auto adopted = adoptVersions (*shared, work, [&closingNow, &stopped] { return closingNow->load() || stopped->load(); });
            shared->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, left = std::move (work), adopted]() mutable
                {
                    if (auto* self = weak.get())
                    {
                        --self->pendingJobs;

                        if (number == self->generation)
                            self->adoptionDone (std::move (left), adopted);
                    }
                });
        },
        Work::reads);
}

void VersionHistory::adoptionDone (Adoption left, const Adopted& adopted)
{
    // What couldn't be copied: said, and not tried again until the history opens again.
    for (const auto& id : adopted.unreadable)
        adoptionFailed.insert (id);

    if (! adopted.unreadable.isEmpty() || ! adopted.written)
    {
        adoptionProblem = adopted.written ? "Phazite couldn't copy " + juce::String (adoptionFailed.size())
                                                + (adoptionFailed.size() == 1 ? " version or song" : " versions or songs")
                                                + " another computer saved into this computer's own packs: a piece of it can't be "
                                                  "read (damaged, or not arrived). It tries again as the project opens"
                                          : "Phazite couldn't write into the Versions folder what another computer saved that this "
                                            "computer's history uses. It tries again as the project opens";
        tellNow();
    }

    // The rest as a further job, a batch a job, so that nothing waits long behind it; none once the sweep was stopped
    // (a save into a new folder): the next open copies it.
    if (! left.isEmpty() && adopted.written && ! sweepStopped->load())
    {
        adoptRound (std::move (left));
        return;
    }

    adoptionQueued = false;

    if (adoptAgain)
        adoptLater();
}

void VersionHistory::logMove (const juce::String& branchID, const juce::String& from, const juce::String& to, const juce::String& why,
                              const juce::String& name, const juce::String& leftSong)
{
    if (store == nullptr)
        return;

    store->logBranchMove (branchID, from, to, why, name, leftSong);
    recentMoves.push_back ({ stampNow(), from, to, why, name, leftSong, branchID, makeUsableFileName (getThisComputer()) });
}

juce::StringArray VersionHistory::getOfferedCommits() const
{
    juce::StringArray offered;

    for (const auto& [id, entry] : unreadableHeads)
        offered.addIfNotAlreadyThere (entry.readable);

    offered.removeEmptyStrings();
    return offered;
}

juce::String VersionHistory::getHeadProblem() const
{
    const auto* branch = findCurrentBranch();
    const auto found = branch != nullptr ? unreadableHeads.find (branch->id) : unreadableHeads.end();

    if (found == unreadableHeads.end())
        return {};

    const auto said = "The last version of branch '" + branch->name + "' (" + found->second.head.substring (0, 7)
                      + ") can't be read from the Versions folder: a piece of it is damaged or missing.";

    if (const auto* readable = findVersion (found->second.readable))
        return said + " The newest version of the branch that reads is \"" + getDisplayName (*readable) + "\" ("
               + juce::Time (readable->saved).toString (true, true, false) + "): Reset Branch to Last Readable Version takes the "
               + "branch back to it, the song as it is. No version is saved meanwhile. After the reset, the version that can't be "
               + "read is on no branch: should its pieces become readable (a sync finishing), Recover Lost Versions brings it back.";

    return said
           + " Neither its logs nor the packs hold a version of it that reads: Verify History finds what can be recovered, and "
             "a version saved starts the branch again from the song as it is.";
}

bool VersionHistory::canResetToReadable() const
{
    // Not while the song's branch isn't in the folder: the branch this would move is the folder's, which the song isn't on
    // (missingBranch: Create Branch first).
    const auto* branch = findCurrentBranch();
    const auto found = branch != nullptr ? unreadableHeads.find (branch->id) : unreadableHeads.end();
    return store != nullptr && hasRead && ! isSaving() && missingBranch.isEmpty() && found != unreadableHeads.end()
           && findVersion (found->second.readable) != nullptr;
}

juce::String VersionHistory::getReadableCommit() const
{
    const auto found = unreadableHeads.find (currentBranch);
    return found != unreadableHeads.end() ? found->second.readable : juce::String();
}

bool VersionHistory::resetToReadable (juce::ValueTree then, PluginParameterValues parameters)
{
    followFolder();

    if (! canResetToReadable())
        return false;

    // As git's soft reset: the branch at that commit on this computer's record, its log saying why; the song as it is, its
    // changes since that commit.
    const auto current =
        std::find_if (branches.begin(), branches.end(), [this] (const auto& branch) { return branch.id == currentBranch; });
    const auto entry = unreadableHeads.at (currentBranch);

    if (! then.isValid())
        return false;

    auto moved = *current;
    moved.head = entry.readable;
    moved.computer = getThisComputer();
    moved.divergent.clear();

    if (! store->writeBranch (moved))
        return false;

    logMove (moved.id, entry.head, entry.readable, "reset", moved.name);
    *current = moved;
    unreadableHeads.erase (currentBranch);
    moveBase (entry.readable, std::move (then), std::move (parameters), song.getDeviceChangesOutsideHistory());
    writeNames();
    forgetUnreachable();
    adoptLater();
    readBounces(); // and tells
    return true;
}

juce::int64 VersionHistory::getSize() const
{
    return store != nullptr ? store->getPieces().getSize() : 0;
}

VersionHistory::AudioPathsFinder VersionHistory::makeAudioPathsFinder() const
{
    if (store == nullptr)
        return {};

    // The song before the merge the song holds, which Abort Merge brings back, beside what the store keeps.
    std::vector<VersionsAudio::Named> songsToo;

    if (const auto waiting = readMergeState (song.getState().getChildWithName (schema::MERGING)))
        songsToo.push_back ({ waiting->before, "the song before the merge that waits in the song" });

    return [shared = store, cache = audioPaths, songsToo] (const std::function<bool()>& shouldStop)
    { return cache->find (*shared, songsToo, shouldStop); };
}

juce::String VersionHistory::findVersionsAudioKey() const
{
    return store != nullptr ? phazite::findVersionsAudioKey (*store) : juce::String();
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

    if (copyingFolder)
        return refuse ("Phazite is saving the project into its folder.");

    if (saving)
        return refuse ("A version is being saved.");

    if (commandRead.has_value())
        return refuse ("Phazite is still at work: " + commandRead->title + ".");

    // Never on top of a last commit that can't be read while one that reads is offered (getHeadProblem()); where none is,
    // the commit starts the branch's line again.
    if (const auto* current = findCurrentBranch(); current != nullptr)
        if (const auto found = unreadableHeads.find (current->id); found != unreadableHeads.end() && found->second.readable.isNotEmpty())
            return refuse ("The branch's last version can't be read: Reset Branch to Last Readable Version takes it back to the newest "
                           "version that reads, to save a version on top of.");

    // Never over versions of the branch the song didn't grow from (another computer's, or newer than the project file that
    // came back from a backup), which a version on top of its base alone would take off the branch.
    if (const auto why = whyNotSaveOnBranch(); why.isNotEmpty())
        return refuse (why);

    // Never of a version open in the song's place (docs/design/versions.md: A version open; Save Version goes back to Now first).
    if (song.isShowingOther())
    {
        jassertfalse;
        return refuse ("Go Back to Now first: a version is open in the song's place.");
    }

    // The song as a save stores it, its plug-ins' own settings included, and their knobs' values.
    song.storeDeviceStates();
    auto copy = copyWholeSong (song);
    auto parameters = readPluginParameters();
    auto committed = song.getDeviceChangesHeld(); // the changes the plug-ins said they made, as far as the version holds them

    // A merge waiting, finished, or of none (Schema.h: MERGING) is never part of a version's song, nor where the song stands
    // in the versions.
    copy.removeChild (copy.getChildWithName (schema::MERGING), nullptr);
    copy.removeProperty (schema::versionsBranch, nullptr);
    copy.removeProperty (schema::versionsBase, nullptr);

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
        branch.changed = stampNow();
    }

    branch.computer = getThisComputer();
    const auto& waiting = getMerging();
    const auto mergesAnother = waiting.has_value() && waiting->version.isNotEmpty();

    VersionRecord version;
    version.name = name.trim();
    version.note = note.trim();
    // Named for what it merges, as git does ("Merge idea-2"), or the first; else for what changed, worked out from the copy on
    // the history's thread (below), which acts on the song as it is, never on a list of its changes that may be older.
    const auto* base = findBaseVersion();
    version.summary = base == nullptr                                 ? juce::String ("First version")
                      : mergesAnother && base->id != waiting->version ? "Merge " + describeCommit (waiting->version)
                                                                      : juce::String();
    version.saved = stampNow();
    version.branch = branch.id;
    version.phazite = getVersionString();

    // On the version the song grew from, which is the branch's last or one that follows it (whyNotSaveOnBranch()): none for
    // the first, or for a branch whose last commit can't be read and has none offered.
    if (findBaseVersion() != nullptr)
        version.parents.add (baseVersion);

    // A merge's commit follows both lines (summariseNextCommit() names it as git does): one parent, the merged version, where
    // that follows the base already and the merge gave nothing anew (Merge the Newer Versions: the graph needs no other).
    if (waiting.has_value() && waiting->version.isNotEmpty() && ! version.parents.contains (waiting->version))
    {
        if (waiting->renumbered.empty() && findVersion (waiting->version) != nullptr && follows (versions, waiting->version, baseVersion))
            version.parents.clear();

        version.parents.add (waiting->version);
        version.renumbered = waiting->renumbered;
    }

    // Verify History under way stops: saving goes ahead rather than waiting behind its hashing (a check stopped says so).
    if (checking)
        cancelCheck();

    saving = true;
    tellNow(); // "Saving..."

    run (
        [shared = store, number = generation, copy, parameters, committed, version, branch, mergesAnother,
         projectFolder = song.getFile().getParentDirectory(), weak = juce::WeakReference<VersionHistory> (this), done, reference = baseline,
         referenceParameters = baselineParameters, changesAtCopy = countSongChanges(),
         reported = findReported (reportedAtBaseline, song.getDeviceChangesOutsideHistory())]() mutable
        {
            // What it changed, from the copy, as its name while it has none of its own: the song as it is, never a list of its
            // changes that may be older than it (Save Version is there while the song may have changes: an edit undone, a
            // discard, the base restored all leave none). Nothing written for no change at all, but where a merge waits.
            juce::String problem;
            auto ok = true;

            if (version.summary.isEmpty())
            {
                const auto changed = findChanges (reference, copy, { &referenceParameters, &parameters, &reported });
                version.summary = summarise (changed);

                if (changed.empty() && ! mergesAnother)
                {
                    problem = nothingChanged;
                    ok = false;
                }
            }

            if (ok)
            {
                const auto pieces = splitSong (copy, parameters);
                version.files = shared->hashFiles (findAudioFilePaths (copy), projectFolder);
                version.id = shared->writeVersion (version, pieces, &problem);
                version.song = pieces.song;
                ok = version.id.isNotEmpty();
            }

            if (ok)
            {
                const auto before = branch.head;
                branch.head = version.id;
                branch.working = {};
                branch.divergent.clear(); // a merge's commit follows the other line; any other leaves it as it was
                ok = shared->writeBranch (branch) && shared->writeCurrentBranch (branch.id);
                shared->logBranchMove (branch.id, before, version.id, mergesAnother ? "merge" : "commit", branch.name);
            }

            shared->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, ok, version, branch, copy, parameters, committed, done, problem, changesAtCopy]() mutable
                {
                    if (auto* self = weak.get())
                    {
                        // Why not, said where the Versions view says its problems, until the next version or edit.
                        self->saveProblem = ok ? juce::String()
                                            : problem.isNotEmpty()
                                                ? problem
                                                : juce::String ("Phazite couldn't write the version into the project's Versions folder.");
                        self->saved (number, ok, version, branch, copy, std::move (parameters), std::move (committed), changesAtCopy);
                    }

                    if (done != nullptr)
                        done (ok ? juce::Result::ok()
                                 : juce::Result::fail (
                                       problem.isNotEmpty()
                                           ? problem
                                           : juce::String ("Phazite couldn't write the version into the project's Versions folder.")));
                });
        },
        Work::writes);

    return true;
}

juce::String VersionHistory::summariseNextCommit()
{
    // A merge's version is named for what it merged: "Merge idea-2" for a branch's last version, else "Merge "Verse 2"".
    if (getMerging().has_value())
    {
        const auto* last = findBaseVersion();
        return getMerging()->version.isNotEmpty() && (last == nullptr || last->id != getMerging()->version)
                   ? "Merge " + describeCommit (getMerging()->version)
                   : summarise (getChanges());
    }

    return summarise (getChanges());
}

void VersionHistory::saved (int number, bool ok, const VersionRecord& version, const BranchRecord& branch, const juce::ValueTree& copy,
                            PluginParameterValues parameters, ReportCounts committed, juce::int64 changesAtCopy)
{
    --pendingJobs;
    saving = false;

    // Not saved (nothing changed, a write that failed): its name and note back in the field, as typed, unless another is typed.
    if (! ok && number == generation && draftName.isEmpty() && draftNote.isEmpty())
    {
        draftName = version.name;
        draftNote = version.note;
    }

    if (! ok || number != generation)
    {
        tellNow();
        return;
    }

    versions[version.id] = version;
    unreadableHeads.erase (branch.id); // its line goes on from this commit
    currentBranch = branch.id;

    mergeDirty = true;
    comparedVersion.clear(); // the changes since the new version
    comparedSong = {};

    // A/B hears the new commit as B from now on, unless another commit is being heard (getCompareVersion()).
    if (! hearingBounce)
        bounceCompared.clear();

    if (auto found = std::find_if (branches.begin(), branches.end(), [&branch] (const auto& b) { return b.id == branch.id; });
        found != branches.end())
    {
        // The branch's last versions this one doesn't follow (another computer's, saved over as a line of its own:
        // canSaveOnBranch()) went their own way, which the view offers to merge.
        auto joined = branch;
        auto before = found->divergent;
        before.add (found->head);

        for (const auto& other : before)
            if (other.isNotEmpty() && ! follows (versions, version.id, other))
                joined.divergent.addIfNotAlreadyThere (other);

        *found = joined;
    }
    else
    {
        branches.push_back (branch);
    }

    // The song grows from the version saved now, on its branch, as it says from here on (into its file as well); its changes
    // since are none, known at once, where it didn't change since it was copied.
    moveBase (version.id, copy, std::move (parameters), std::move (committed), countSongChanges() == changesAtCopy);
    previousBaseline = {};
    snapshotOnFirstChanges = false; // the song as it opened is the version saved now, or changed since: no "As opened" of it
    writeNames();
    adoptLater(); // the versions it follows, another computer's too
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

    if (! element.isValid() || store == nullptr || ! hasRead)
        return;

    const auto say = [this] (const juce::String& problem) { mergeProblem = mergeFailure.isNotEmpty() ? mergeFailure : problem; };

    // One that isn't one, or names a commit the history hasn't (a project opened without its Versions folder, whose
    // history starts anew): no merge waits, and Abort Merge takes the element out.
    const auto said = readMergeState (element);
    const auto* merged = said.has_value() ? findVersion (said->version) : nullptr;

    if (merged == nullptr)
    {
        say ("This song holds a merge that can't be read, which no version will finish: Cancel Merge takes it out of the song.");
        return;
    }

    // Finished by this branch's own commit of it: silently.
    if (findOwnCommit (*said) != nullptr)
        return;

    // Or by a version of the song's line on top of the one merged in (Merge the Newer Versions' version, which follows only the
    // newer one, as the graph needs no other): silently too.
    if (baseVersion != said->head && follows (versions, baseVersion, said->version))
        return;

    // Held by the branch otherwise: through another computer's merge of it (or several), which gave what it added other
    // IDs than this song's, it waits still, so that its commit records this song's IDs beside those (each the other merge
    // gave and this one didn't, as it is here); held without any such merge, it waits no more.
    const auto* last = findLastVersion();
    const auto name = "\"" + getDisplayName (*merged) + "\"";
    auto renumbered = said->renumbered;
    juce::String problem;

    // Not the branch's last version itself, which the song didn't grow from (Take the Newer Versions' merge of them): that
    // merge waits for its version.
    if (last != nullptr && follows (versions, last->id, said->version)
        && (last->id != said->version || follows (versions, baseVersion, said->version)))
    {
        std::vector<const VersionRecord*> others;

        for (const auto& [id, version] : versions)
            if (version.parents.size() > 1 && version.parents[1] == said->version && follows (versions, last->id, id))
                others.push_back (&version);

        if (others.empty())
        {
            say ("This branch holds " + name
                 + " already (merged on another computer): the merge of it this song took waits no "
                   "more. Cancel Merge takes it out of the song; Discard All Changes makes the song the branch's.");
            return;
        }

        for (const auto* other : others)
            for (const auto& given : other->renumbered)
                if (std::none_of (renumbered.begin(), renumbered.end(),
                                  [&given] (const RenumberedID& id) { return id.clip == given.clip && id.from == given.from; }))
                    renumbered.push_back ({ given.clip, given.from, given.from });

        problem = "This branch holds " + name
                  + " already, merged on another computer, which gave what it added other IDs than "
                    "this song has: save a version to finish this merge beside it, so that later merges of that line know this song's.";
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
        merge.translation = said->translation;
        merge.crossed = said->crossed;
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
                },
                Work::reads);
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
    else if (problem.isEmpty() && ! merge.before.isValid())
        problem = "Phazite can't read the song as it was before the merge of " + name
                  + " from the Versions folder: it is no longer kept, or it is damaged. Cancel Merge makes the song the branch's "
                    "last version instead.";
    else if (problem.isEmpty() && ! merge.readWhole)
        problem = "Phazite can't read all of the merge of " + name + " from the Versions folder: its conflicts aren't shown, and "
                  + (merge.before.isValid() ? juce::String ("Cancel Merge makes the song what it was before it.")
                                            : juce::String ("Cancel Merge makes the song the branch's last version."));
    else if (problem.isEmpty() && baseVersion != said->head)
        problem = "This song moved on since the merge of " + name
                  + " was taken (a version saved since, after the merge's Undo): its version will follow the song's last "
                    "version and the one merged in.";

    say (problem);
    merging = std::move (merge);
}

juce::ValueTree VersionHistory::keepMerge (const Merge& merge, std::vector<int> chosen)
{
    followFolder();

    // Never while a version is open in the song's place, whose song isn't the one before the merge (startMerge() goes back to
    // Now first).
    jassert (! song.isShowingOther());

    if (store == nullptr || song.isShowingOther())
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
    const auto element = writeMergeState (
        { kept.version, kept.head, kept.beforePiece, std::move (chosen), kept.renumbered, kept.translation, kept.crossed });
    mergesFound.insert_or_assign (getMergeKey (element), std::move (kept));
    mergeDirty = true; // its copy made this computer's own once the merge is in the song (VersionsModel.cpp: takeMergeIn())
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

PluginParameterValues VersionHistory::readKnobs (const std::set<std::uint64_t>& devices) const
{
    PluginParameterValues values;

    for (const auto device : devices)
    {
        if (auto* instance = findPluginInstance (song, DeviceID::fromRaw (device)))
        {
            auto& parameters = values[device];
            const auto& infos = instance->getParameterInfos();

            for (int i = 0; i < (int) infos.size(); ++i)
                parameters.push_back ({ infos[(size_t) i].id, infos[(size_t) i].name, instance->getParameterValue (i) });
        }
    }

    return values;
}

PluginParameterValues VersionHistory::readBaseKnobs (const std::set<std::uint64_t>& devices)
{
    PluginParameterValues values;

    for (const auto device : devices)
    {
        // A plug-in that was still loading, in the background, as the base was saved or read had no values then: those it
        // opened with are its values then (the state it opened from is the one the base holds), so that a knob turned after,
        // in Phazite, shows as a change (the review of known bug 166). Kept from the first round that copies it loaded.
        if (! baselineParameters.contains (device))
        {
            if (auto* instance = findPluginInstance (song, DeviceID::fromRaw (device));
                instance != nullptr && ! instance->getParameterInfos().empty())
            {
                std::vector<PluginParameterValue> opened;

                for (const auto& info : instance->getParameterInfos())
                    opened.push_back ({ info.id, info.name, info.value });

                baselineParameters[device] = std::move (opened);
            }
        }

        if (const auto found = baselineParameters.find (device); found != baselineParameters.end())
            values.insert (*found);
    }

    return values;
}

//==============================================================================
void VersionHistory::setBaseline (juce::ValueTree baselineSong, PluginParameterValues parameters, ReportCounts reported, bool songIsIt)
{
    baseline = std::move (baselineSong);
    baselineParameters = std::move (parameters);
    reportedAtBaseline = std::move (reported);
    baselineChanged (songIsIt);
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

void VersionHistory::baselineChanged (bool songIsIt)
{
    // The clips whose notes were heard to change marked first, as the tree's changes mark at once: the comparison with the
    // version compared keeps them, and the one with the baseline starts again below, so that none is left waiting for the
    // next ask (which kept mayHaveChanges() true with no changes since a version saved at once).
    markNotesHeard();

    // Compared again with the song from here on, in the background: at once known to be none, where the song is it.
    if (baseline.isValid())
        sinceBase.setReference (baseline, baselineParameters, true, songIsIt);
    else
        sinceBase.clear();

    changesMayDiffer();
}

juce::String VersionHistory::describeCommit (const juce::String& versionID) const
{
    for (const auto& branch : branches)
        if (branch.head == versionID && branch.id != currentBranch)
            return branch.name;

    if (const auto* version = findVersion (versionID))
        return "\"" + getDisplayName (*version) + "\"";

    return "\"" + versionID.substring (0, 7) + "\"";
}

const std::vector<SongChange>& VersionHistory::getChanges()
{
    static const std::vector<SongChange> none;
    takeChangesOutsideHistory();
    takePluginsJoined();

    if (! hasRead || ! baseline.isValid())
        return none;

    // The last list worked out; a round asked for if the song changed since, which tells the listeners once it is in.
    askForChanges();
    return sinceBase.getChanges();
}

bool VersionHistory::hasChanges()
{
    return ! getChanges().empty();
}

bool VersionHistory::areChangesWorkedOut() const
{
    return notesHeard.empty() && sinceBase.isUpToDate() && (comparedVersion.isEmpty() || againstCompared.isUpToDate());
}

bool VersionHistory::mayHaveChanges()
{
    // Now's, while a version is open in its place.
    if (shown.has_value())
        return shown->mayHaveChanges;

    return hasChanges() || (hasRead && baseline.isValid() && (! notesHeard.empty() || ! sinceBase.isUpToDate()));
}

void VersionHistory::askForChanges()
{
    markNotesHeard();

    if (! hasRead || store == nullptr)
        return;

    // Not while a version is open in the song's place: the changes listed are Now's, and what the version changed is compared
    // again once Now is back, which comes out as before.
    if (song.isShowingOther())
        return;

    // Not during a drag, a gesture of many changes that is one step (a round each time one came back copied the track it
    // drags in, many times a second): asked once it is committed or rolled back. And at most one round a tick otherwise,
    // the next asked by the tick (timerCallback()).
    if (Gesture::isInProgress (song))
        return;

    // Nor while a version is being saved: its song becomes the one compared with (saved()), so a round taken meanwhile would
    // run after its write, against the version before, and be dropped (a gate counted it, late: the review of S4.5).
    if (saving)
        return;

    if (const auto now = juce::Time::getMillisecondCounter();
        lastRoundAt != 0 && now - lastRoundAt < (juce::uint32) (tickMilliseconds * 3 / 4))
        return;

    // A round of each that has something to work out and none under way: copies of what changed, taken here, worked out on
    // the history's thread, and told back here (changesWorked()). One that comes back after the history went is dropped.
    const auto reported = findReported (reportedAtBaseline, song.getDeviceChangesOutsideHistory());

    for (auto* worker : { &sinceBase, &againstCompared })
    {
        const auto compared = worker == &againstCompared;
        auto request = worker->take (
            song, [this] (const std::set<std::uint64_t>& devices) { return readKnobs (devices); },
            [this, compared] (const std::set<std::uint64_t>& devices)
            { return compared ? PluginParameterValues() : readBaseKnobs (devices); }, reported);

        if (request == nullptr)
            continue;

        lastRoundAt = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter());
        thread->add (
            [request, compared, weak = juce::WeakReference<VersionHistory> (this), closingNow = closing] (const BackgroundJobs::ShouldStop&)
            {
                // Nothing waits for it as the history goes.
                if (closingNow->load())
                    return;

                auto result = ChangesWorker::work (*request);

                juce::MessageManager::callAsync (
                    [weak, compared, result = std::move (result)]
                    {
                        if (auto* self = weak.get())
                            self->changesWorked (compared, result);
                    });
            });
    }
}

void VersionHistory::changesWorked (bool compared, const ChangesWorker::Result& result)
{
    auto& worker = compared ? againstCompared : sinceBase;

    // Of another reference (the song grew from another version since it was asked for, or another is compared): dropped,
    // as a list of another song's is never shown.
    if (worker.takeResult (result))
    {
        // Local History's snapshot as the project opened, once its changes are known to be some.
        if (! compared && std::exchange (snapshotOnFirstChanges, false) && ! worker.getChanges().empty())
            takeSnapshot ("open");

        tellNow();
    }

    askForChanges(); // what changed meanwhile
}

//==============================================================================
void VersionHistory::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! schema::isCounted (tree, property))
        return;

    edited();

    // The automation curve, the track (or the master) it is in, nearest first, or else the song's own.
    for (auto element = tree; element.isValid(); element = element.getParent())
    {
        if (element.hasType (schema::CURVE) && element.getParent().hasType (schema::AUTOMATION))
        {
            // One whose device or parameter changed is another curve: every one compared again.
            if (element == tree && (property == schema::curveDevice || property == schema::curveParameter))
                markAllChanged();
            else
                markCurve (schema::getElementKey (element));

            return;
        }

        if (element.hasType (schema::AUTOMATION))
        {
            markAllChanged();
            return;
        }

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
        {
            markTrack (element, false);
            return;
        }
    }

    markSongOwn();
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

    // A track added or taken away, with the tracks in it (a folder's): its line, and the others' names ("Track 3") and the
    // song's order of them.
    if (child.hasType (schema::TRACK))
    {
        markTrack (child, true);
        markOrder();
        return;
    }

    if (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER) || child.hasType (schema::AUTOMATION))
    {
        markAllChanged();
        return;
    }

    if (child.hasType (schema::CURVE) && parent.hasType (schema::AUTOMATION))
    {
        markCurve (schema::getElementKey (child));
        return;
    }

    markChanged (parent, {});
}

void VersionHistory::markTrack (const juce::ValueTree& track, bool withTracksInIt)
{
    const auto id = readID (track[schema::trackID]);
    sinceBase.markTrack (id);
    againstCompared.markTrack (id);

    if (withTracksInIt)
        for (const auto& child : track)
            if (child.hasType (schema::TRACK))
                markTrack (child, true);

    changesMayDiffer();
}

void VersionHistory::clipNotesChanged (const std::vector<ClipID>& clips)
{
    // A change of the song as its tree's are (valueTreePropertyChanged()), in a clip: its track's line, once its track is
    // known (markNotesHeard()).
    ++songChanges;
    edited();
    notesHeard.insert (notesHeard.end(), clips.begin(), clips.end());
    changesMayDiffer();
}

void VersionHistory::clipsChanged (const std::vector<SongValues::ClipChange>& clips, const std::vector<TrackID>& reordered)
{
    // A change of the song as its tree's are, in clips: the lines of the tracks they were on and are on, and of those whose
    // clips came in another order, by their IDs (a listener of the song's values reads nothing of the song).
    ++songChanges;
    edited();

    const auto mark = [this] (TrackID track)
    {
        sinceBase.markTrack (track.getRaw());
        againstCompared.markTrack (track.getRaw());
    };

    for (const auto& clip : clips)
    {
        if (clip.before.isValid())
            mark (clip.before);

        if (clip.after.isValid() && clip.after != clip.before)
            mark (clip.after);
    }

    for (const auto track : reordered)
        mark (track);

    changesMayDiffer();
}

void VersionHistory::tempoMapChanged()
{
    // A change of the song as its tree's are, in its tempo map: the song's own line (as markChanged() marks it).
    ++songChanges;
    edited();
    markSongOwn();
}

void VersionHistory::chordLaneChanged()
{
    // A change of the song as its tree's are, in its chord lane: the song's own line (as markChanged() marks it).
    ++songChanges;
    edited();
    markSongOwn();
}

void VersionHistory::markNotesHeard()
{
    // A clip gone since was taken off its track, which marked it.
    for (const auto clip : std::exchange (notesHeard, {}))
        if (const auto track = getClipTrack (song, clip); track.isValid())
            markTrack (findTrackState (song, track), false);
}

void VersionHistory::markCurve (const juce::String& key)
{
    sinceBase.markCurve (key);
    againstCompared.markCurve (key);
    changesMayDiffer();
}

void VersionHistory::markSongOwn()
{
    sinceBase.markSong();
    againstCompared.markSong();
    changesMayDiffer();
}

void VersionHistory::markOrder()
{
    sinceBase.markOrder();
    againstCompared.markOrder();
    changesMayDiffer();
}

void VersionHistory::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (isHeardAsKept (tree))
        return; // the areas': tempoMapChanged(), chordLaneChanged()

    ++songChanges;

    if (tree.hasType (schema::MERGING)) // a conflict switched
    {
        mergeDirty = true;
        changed();
    }

    // Where the song stands, moved by an Undo or a Redo: followed once the step is whole (its other changes, undone after this
    // one or redone before it, in the song too), at the next turn of the message loop.
    if (schema::isVersionsProperty (tree, property) && ! movingNames)
        juce::MessageManager::callAsync (
            [weak = juce::WeakReference<VersionHistory> (this)]
            {
                if (auto* self = weak.get())
                    self->followNamesOfSong();
            });

    // A track's ID, name or kind changes what the lines are called; its ID, which one it is.
    if (tree.hasType (schema::TRACK) && property == schema::trackID)
        markAllChanged();
    else if (tree.hasType (schema::TRACK) && (property == schema::trackName || property == schema::trackKind))
        markOrder();

    markChanged (tree, property);
}

void VersionHistory::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex)
{
    if (isHeardAsKept (parent))
        return; // the areas': tempoMapChanged(), chordLaneChanged()

    ++songChanges;

    // Tracks in another order: the song's line, and their numbers.
    if (parent.getChild (newIndex).hasType (schema::TRACK))
    {
        edited();
        markOrder();
    }
    else
    {
        markChanged (parent, {});
    }
}

void VersionHistory::markAllChanged()
{
    sinceBase.markAll();
    againstCompared.markAll();
    changesMayDiffer();
}

void VersionHistory::changed()
{
    notifyPending = true;
}

void VersionHistory::changesMayDiffer()
{
    // Only what changed is compared again, in the next round (ChangesWorker.h): asked for as the changes are asked for, or
    // at the next tick, whose listeners ask.
    changed();
}

void VersionHistory::edited()
{
    editedSinceSnapshot = true; // Local History takes the song again in a while
    saveProblem.clear();

    if (song.getUndoManager().isPerformingUndoRedo() && isBehind())
        healPending = true;

    // What is heard is what is shown: an edit is heard as it is made, in the song.
    if (compareProblem.isNotEmpty())
    {
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

void VersionHistory::compareWith (const juce::String& versionID, juce::ValueTree read, PluginParameterValues readParameters)
{
    // The version the song grew from is the changes' own; another is read once, to list what differs from it while the song
    // changes.
    const auto* last = findBaseVersion();
    const auto other = versionID.isNotEmpty() && (last == nullptr || last->id != versionID) ? versionID : juce::String();

    if (other == comparedVersion)
        return;

    comparedVersion = other;
    comparedParameters = other.isNotEmpty() ? std::move (readParameters) : PluginParameterValues();
    comparedSong = other.isNotEmpty() ? read : juce::ValueTree();

    if (other.isNotEmpty() && ! comparedSong.isValid())
        comparedVersion.clear(); // it can't be read

    // Worked out on the history's thread, every plug-in whose settings differ counting (its version knows no report).
    if (comparedVersion.isNotEmpty())
        againstCompared.setReference (comparedSong, comparedParameters, false);
    else
        againstCompared.clear();

    selectedChange.reset();
    tellNow();
}

const std::vector<SongChange>& VersionHistory::getListedChanges()
{
    if (comparedVersion.isEmpty())
        return getChanges();

    takeChangesOutsideHistory();
    askForChanges();
    return againstCompared.getChanges();
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
    // A plug-in's own change (in its window): its knobs, which the song doesn't hold. Its track compared again, or every
    // one for a change no device's count says.
    if (const auto outside = song.getChangesOutsideHistory(); outside != changesOutsideHistory)
    {
        changesOutsideHistory = outside;
        edited();
        const auto& now = song.getDeviceChangesOutsideHistory();
        const auto moved = findReported (outsideSeen, now);
        outsideSeen = now;

        if (moved.empty() || moved.count (0) > 0)
        {
            markAllChanged();
            return;
        }

        std::vector<juce::ValueTree> plugins;
        findPlugins (song.getState(), plugins);
        std::set<std::uint64_t> placed;

        for (const auto& plugin : plugins)
        {
            if (const auto device = readID (plugin[schema::deviceID]); moved.count (device) > 0)
            {
                for (auto element = plugin.getParent(); element.isValid(); element = element.getParent())
                {
                    if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
                    {
                        markTrack (element, false);
                        placed.insert (device);
                        break;
                    }
                }
            }
        }

        if (placed.size() < moved.size())
            markAllChanged(); // one that is gone, or isn't on a track
    }
}

void VersionHistory::takePluginsJoined()
{
    // A plug-in joining the song (one loaded in the background as the project opened): its knobs' values can be read from
    // now on, which say whether a state that differs from the last commit's counts (SongDiff.h: PluginValues), and those it
    // opened with are the base's where the base came without them. Taken in as the changes are asked for too, not only at a
    // tick: what asks just after one joined compares with its values.
    if (const auto loading = getPluginsLoading (song).size(); loading != pluginsLoadingSeen)
    {
        if (loading < pluginsLoadingSeen)
            markAllChanged();

        pluginsLoadingSeen = loading;
    }
}

void VersionHistory::timerCallback()
{
    followFolder();
    takeChangesOutsideHistory();
    takePluginsJoined();

    // Verify History's progress, as it moves.
    if (checking && std::abs (checkProgress->load() - checkProgressTold) >= 0.01f)
    {
        checkProgressTold = checkProgress->load();
        changed();
    }

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
    // records (the take isn't in it yet), nor while a commit is being made, nor while a version is open in its place.
    if (editedSinceSnapshot && hasRead && store != nullptr && ! isSaving() && ! isRecording (song) && ! song.isShowingOther()
        && getVersionsTime() - lastSnapshotTime >= (juce::int64) snapshotMinutes * 60 * 1000 && mayHaveChanges())
        takeSnapshot ("auto");

    if (healPending && ! Gesture::isInProgress (song) && ! isReadingForCommand() && ! song.isShowingOther())
        healToLast();

    // A round of the changes for what changed since the last, whoever asks for them.
    askForChanges();

    if (std::exchange (notifyPending, false) && onChange != nullptr)
        onChange();
}

//==============================================================================
juce::String VersionHistory::makeBranch (const juce::String& fromVersion, bool makeCurrent)
{
    followFolder();

    // Never while a version is open in the song's place, which a branch from Now would take with it as its song, and a branch
    // made beside it would leave named for the wrong one (its commands go back to Now first; an edit's branch is
    // branchFromShown()).
    jassert (! song.isShowingOther());

    if (store == nullptr || ! hasRead || isSaving() || song.isShowingOther())
        return {};

    // From Now: from the version the song grew from, which the song's changes are from.
    const auto head = fromVersion.isNotEmpty() ? fromVersion : baseVersion;

    if (findVersion (head) == nullptr)
        return {};

    const auto branch = makeBranchRecord (head);

    if (! store->writeBranch (branch) || (makeCurrent && ! store->writeCurrentBranch (branch.id)))
        return {};

    logMove (branch.id, {}, head, "branch", branch.name);

    branches.push_back (branch);

    // The same last version: the changes since stay as they are, the song on the new branch (into its file as well).
    if (makeCurrent)
    {
        currentBranch = branch.id;
        missingBranch.clear(); // the song is on a branch the folder has now
        baseProblem.clear();
        writeNames();
    }

    adoptLater(); // a branch from another computer's version
    tellNow();
    return branch.id;
}

BranchRecord VersionHistory::makeBranchRecord (const juce::String& head) const
{
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
    branch.changed = stampNow();
    return branch;
}

const VersionHistory::ShownSong* VersionHistory::findShownSong (const juce::String& versionID) const
{
    const auto found =
        std::find_if (shownSongs.begin(), shownSongs.end(), [&versionID] (const auto& each) { return each.version == versionID; });
    return found != shownSongs.end() ? &*found : nullptr;
}

void VersionHistory::keepShownSong (ShownSong read)
{
    // The newest last, the oldest let go past the few kept.
    std::erase_if (shownSongs, [&read] (const auto& each) { return each.version == read.version; });
    shownSongs.push_back (std::move (read));

    if (shownSongs.size() > shownSongsKept)
        shownSongs.erase (shownSongs.begin());
}

void VersionHistory::keepShownNow()
{
    if (! shown.has_value() || store == nullptr || ! hasRead)
        return;

    shown->nowPieceComing = true;

    whenNotCopying (
        [weak = juce::WeakReference<VersionHistory> (this), number = shown->number]
        {
            auto* self = weak.get();

            if (self == nullptr || ! self->shown.has_value() || self->shown->number != number)
                return;

            // No store any more (the history closing): it never comes, which what waits for it hears.
            if (self->store == nullptr)
            {
                self->shown->nowPieceComing = false;

                if (const auto then = std::exchange (self->shown->whenNowIn, {}); then != nullptr)
                    then (false);

                return;
            }

            self->run (
                [shared = self->store, weak, number, generationThen = self->generation, now = self->shown->now,
                 parameters = self->shown->nowParameters]
                {
                    const auto pieces = splitSong (now, parameters);
                    const auto written = shared->writeSong (pieces);
                    shared->getPieces().closeFiles();

                    juce::MessageManager::callAsync (
                        [weak, number, generationThen, written, piece = pieces.song]
                        {
                            auto* back = weak.get();

                            if (back == nullptr)
                                return;

                            --back->pendingJobs;

                            if (! back->shown.has_value() || back->shown->number != number)
                                return;

                            // Kept, and told to what waits for it (an edit's branch), which may end what is shown: taken out first.
                            const auto in = written && back->generation == generationThen;
                            back->shown->nowPieceComing = false;

                            if (in)
                                back->shown->nowPiece = piece;

                            if (const auto then = std::exchange (back->shown->whenNowIn, {}); then != nullptr)
                                then (in);
                        });
                },
                Work::writes);
        });
}

juce::String VersionHistory::branchFromShown()
{
    followFolder();
    branchProblem = "busy";

    // Not while the song shows the version still (the edit's step is a step of the branch's song, which isn't Now).
    jassert (! song.isShowingOther());

    if (! shown.has_value() || song.isShowingOther() || store == nullptr || ! hasRead || isSaving() || commandRead.has_value()
        || missingBranch.isNotEmpty() || shown->nowPiece.isEmpty())
        return {};

    const auto* version = findVersion (shown->version);
    const auto* then = findShownSong (shown->version);
    const auto current =
        std::find_if (branches.begin(), branches.end(), [this] (const auto& branch) { return branch.id == currentBranch; });

    if (version == nullptr || then == nullptr || current == branches.end())
        return {};

    // Now, by its piece, on the branch it leaves, as finishSwitch() keeps the song it leaves; then the branch, from the
    // version, the current one. Each record as it was if a write fails, as a switch's.
    branchProblem = "write";
    const auto before = *current;
    auto left = before;
    left.working = shown->nowPiece;
    const auto branch = makeBranchRecord (version->id);

    if (! store->writeBranch (left))
        return {};

    if (! store->writeBranch (branch) || ! store->writeCurrentBranch (branch.id))
    {
        store->writeBranch (before);
        return {};
    }

    branchProblem.clear();

    *current = left;
    logMove (branch.id, {}, branch.head, "branch", branch.name);
    branches.push_back (branch);
    currentBranch = branch.id;

    // The song grows from the version on the new branch, its changes none (it is that version's), as a switch to the branch
    // leaves it; a merge waiting stays with the song left on the branch it was taken on.
    preview.reset();
    previousBaseline = {};
    moveBase (version->id, then->song.createCopy(), then->parameters, song.getDeviceChangesOutsideHistory(), true);
    previousBaseline = {};
    writeNames();
    mergeDirty = true;
    getMerging();
    comparedVersion.clear();
    comparedSong = {};
    selectedChange.reset();
    adoptLater();
    tellNow();
    return branch.id;
}

juce::String VersionHistory::makeBranchWithSong (const juce::String& fromVersion, const juce::String& songPiece)
{
    // Never while a version is open in the song's place (makeBranch() refuses it).
    const auto id = makeBranch (fromVersion, false);

    if (id.isEmpty())
        return {};

    for (auto& branch : branches)
    {
        if (branch.id == id)
        {
            auto withSong = branch;
            withSong.working = songPiece;

            if (! store->writeBranch (withSong))
                return {};

            branch = withSong;
        }
    }

    return id;
}

//==============================================================================
bool VersionHistory::startReading (const juce::String& title, std::function<TakeIn()> read, std::shared_ptr<void> hold,
                                   std::function<void (bool)> done)
{
    if (store == nullptr || ! hasRead || commandRead.has_value() || isSaving())
        return false;

    // Verify History under way stops: the command reads at once rather than behind its hashing, the window held.
    if (checking)
        cancelCheck();

    const auto number = ++readingsStarted;
    commandRead = CommandRead { title, std::move (hold), std::move (done), number, folder };
    readingProblem.clear();
    readOnThread (number, std::move (read));
    tellNow();
    return true;
}

void VersionHistory::readOnThread (int number, std::function<TakeIn()> read)
{
    // What it read taken in once it is read, on the message thread, unless it was cancelled meanwhile.
    run (
        [read = std::move (read), weak = juce::WeakReference<VersionHistory> (this), number, generationNow = generation]
        {
            auto takeIn = read();

            juce::MessageManager::callAsync (
                [weak, number, generationNow, takeIn = std::move (takeIn)]
                {
                    if (auto* self = weak.get())
                        self->readingDone (generationNow, number, takeIn);
                });
        },
        Work::reads);
}

void VersionHistory::readingDone (int generationThen, int number, const TakeIn& takeIn)
{
    --pendingJobs;

    if (! commandRead.has_value() || commandRead->number != number)
        return; // cancelled meanwhile

    // Not into another folder than the one it read from (a first save moved the project meanwhile), nor while the folder is
    // being copied, which no write may land in (copyFolder()).
    if (isCopyingFolder() || folder != commandRead->folder)
    {
        endReading (Taken::refused ("The project was saved into a new folder while its versions were read: nothing was changed. "
                                    "Try once more."));
        return;
    }

    if (generationThen != generation || takeIn == nullptr)
    {
        endReading (Taken::refused ("The project's versions were read again meanwhile: nothing was changed. Try once more."));
        return;
    }

    auto taken = takeIn();

    // Out of date: read once more, from the song as it is, the window held still.
    if (taken.again != nullptr)
    {
        readOnThread (number, std::move (taken.again));
        return;
    }

    endReading (std::move (taken));
}

void VersionHistory::cancelReading (const juce::String& why)
{
    if (commandRead.has_value())
        endReading (why.isNotEmpty() ? Taken::refused (why) : Taken (false));
}

void VersionHistory::endReading (Taken taken)
{
    auto ended = std::move (*commandRead);
    commandRead.reset();
    lastReadingDid = taken.did;

    // Why it did nothing, said until the next: what couldn't be read, or what refused it.
    if (! taken.did && taken.problem.isNotEmpty())
        readingProblem = taken.problem;

    ended.hold = nullptr; // the window takes input again
    tellNow();

    if (ended.done != nullptr)
        ended.done (taken.did);
}

//==============================================================================
void VersionHistory::keepLeft (Switch& plan)
{
    // With where it stands in the versions (Schema.h: versionsBranch, versionsBase), kept, unlike a version's song: the branch
    // is implied by where it is left, but its base isn't (the branch's last version may move on, on another computer, while
    // the song waits there, and the record of a left song holds only its piece), and a song back from a branch must grow
    // from its own version, not the branch's newer one. So its piece is never the version's, and whether it has changes is
    // found by reading both (hasChangesLeftOn()).
    song.storeDeviceStates();
    plan.left = copyWholeSong (song);
    plan.leftParameters = readPluginParameters();
    plan.changesAtCopy = countSongChanges();
}

std::optional<VersionHistory::Switch> VersionHistory::prepareSwitch (const juce::String& branchID, bool withinRead)
{
    followFolder();
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });
    const auto current =
        std::find_if (branches.begin(), branches.end(), [this] (const auto& branch) { return branch.id == currentBranch; });

    // Never while a version is open in the song's place, which would be left on the branch as its song (keepLeft(); a switch
    // goes back to Now first).
    jassert (! song.isShowingOther());

    if (store == nullptr || ! hasRead || isSaving() || (commandRead.has_value() && ! withinRead) || found == branches.end()
        || current == branches.end() || branchID == currentBranch || missingBranch.isNotEmpty() || song.isShowingOther())
        return std::nullopt;

    // The other's song, as it was left there, or as its last version had it; and its last version's.
    Switch plan;
    plan.branchID = branchID;
    const auto* head = findVersion (found->head);
    plan.headPiece = head != nullptr ? head->song : juce::String();
    plan.head = found->head;
    plan.otherIsHead = found->working.isEmpty();
    plan.otherPiece = plan.otherIsHead ? plan.headPiece : found->working;

    if (plan.otherPiece.isEmpty())
        return std::nullopt;

    keepLeft (plan);
    return plan;
}

void VersionHistory::keepLeftIn (VersionStore& into, const Switch& plan, SwitchRead& read)
{
    // The song left, in pieces, kept in the store (as a commit keeps its own, on this thread).
    if (plan.left.isValid())
    {
        const auto pieces = splitSong (plan.left, plan.leftParameters);
        read.leftKept = into.writeSong (pieces);
        read.leftPiece = pieces.song;
        read.leftChanges = plan.changesAtCopy;
    }
}

VersionHistory::SwitchRead VersionHistory::readSwitch (VersionStore& from, const Switch& plan)
{
    SwitchRead read;
    keepLeftIn (from, plan, read);

    // The song left there, and the version it grew from as its names say (Schema.h: versionsBranch, versionsBase), where
    // they say that branch and a version that reads: another computer may have saved versions on the branch since it was
    // left, which it didn't grow from.
    if (! plan.otherIsHead)
    {
        read.other = from.readSong (plan.otherPiece);

        if (const auto names = readNames (read.other); names.branch == plan.branchID && names.base.isNotEmpty() && names.base != plan.head)
        {
            if (auto record = from.readVersion (names.base))
            {
                read.last = readCommitSong (from, record->song, &read.lastParameters);

                if (read.last.isValid())
                {
                    read.base = names.base;
                    read.baseRecord = std::move (record);
                }
                else
                {
                    read.lastParameters.clear();
                }
            }
        }
    }

    // Else its last commit's song, read once where the two are the same.
    if (! read.last.isValid() && plan.headPiece.isNotEmpty())
        read.last = readCommitSong (from, plan.headPiece, &read.lastParameters);

    if (plan.otherIsHead)
        read.other = read.last.isValid() ? read.last.createCopy() : juce::ValueTree();

    from.getPieces().closeFiles();
    return read;
}

bool VersionHistory::copyLeftAgain (Switch& plan)
{
    // The song left changed since it was copied (a plug-in's own change, the automation written as it plays): copied again.
    // Never a version open in its place (finishSwitch() refuses it).
    if (! plan.left.isValid() || countSongChanges() == plan.changesAtCopy || song.isShowingOther())
        return false;

    song.storeDeviceStates();
    plan.left = copyWholeSong (song);
    plan.leftParameters = readPluginParameters();
    plan.changesAtCopy = countSongChanges();
    return true;
}

VersionHistory::Taken VersionHistory::finishSwitch (const Switch& plan, SwitchRead read)
{
    followFolder();
    const auto findBranch = [this] (const juce::String& id)
    { return std::find_if (branches.begin(), branches.end(), [&id] (const auto& branch) { return branch.id == id; }); };
    auto current = findBranch (currentBranch);
    const auto nothing = juce::String (" Nothing was switched: the song and the branch are as they were.");

    if (isCopyingFolder())
        return Taken::refused ("The project was being saved into a new folder while the song was read." + nothing
                               + " Try again once it is saved.");

    if (store == nullptr || ! hasRead || isSaving() || current == branches.end())
        return Taken::refused ("The project's versions changed while the song was read." + nothing);

    // Never while a version is open in the song's place, which would be left on the branch as its song.
    jassert (! song.isShowingOther());

    if (song.isShowingOther())
        return Taken::refused ("A version was opened while the song was read." + nothing);

    if (! read.other.isValid())
        return Taken::refused (
            (plan.otherIsHead ? "The version's song can't be read from the Versions folder: a piece of it is damaged or missing."
                              : "The song left on that branch can't be read from the Versions folder: a piece of it is damaged or missing.")
            + nothing + " Verify History says what is damaged.");

    const auto couldNotWrite = Taken::refused ("Phazite couldn't write into the project's Versions folder." + nothing
                                               + " Make room on the drive, or check that the folder can be written to.");

    // The song as it is now on the branch it leaves, as it was copied and split on the history's thread (again, if it changed
    // while the other was read: VersionsModel.cpp); still changed after that, split here, as the hold keeps anything else
    // from changing it.
    auto left = *current;

    if (! plan.left.isValid() || countSongChanges() != read.leftChanges)
    {
        song.storeDeviceStates();
        const auto pieces = splitSong (copyWholeSong (song), readPluginParameters());
        read.leftKept = store->writeSong (pieces);
        read.leftPiece = pieces.song;
    }

    if (! read.leftKept)
        return couldNotWrite;

    left.working = read.leftPiece;
    auto found = findBranch (plan.branchID);

    if (found == branches.end())
        return Taken::refused ("That branch isn't there any more." + nothing);

    // The entered branch's record still names the song left there: let go of once the project file holds the song
    // (switchSaved()), so that a save that fails loses neither branch's song.
    const auto entered = *found;
    const auto before = *current;
    const auto leftWritten = store->writeBranch (left);

    if (! leftWritten || ! store->writeCurrentBranch (entered.id))
    {
        // As it was: the record of the branch left written back as it was (it is still the current one, which no song left
        // on it names).
        if (leftWritten)
            store->writeBranch (before);

        return couldNotWrite;
    }

    currentBranch = entered.id;
    *findBranch (left.id) = left;
    missingBranch.clear();
    // The other's song, in place; its changes since the version it grew from (its names', else the branch's last) from here
    // on, which A/B hears as B; the merge the song left there holds read on the history's thread now if it must be.
    preview.reset();

    {
        const juce::ScopedValueSetter<bool> moving (movingNames, true); // the names it brings are taken below
        makeSongTheSame (song, read.other);
    }

    if (read.baseRecord.has_value())
        versions.emplace (read.base, *read.baseRecord);

    // Its changes are none, known at once, where the song is the branch's last version as it was read.
    previousBaseline = {};
    moveBase (read.base.isNotEmpty() ? read.base : entered.head, std::move (read.last), std::move (read.lastParameters),
              song.getDeviceChangesOutsideHistory(), plan.otherIsHead);
    previousBaseline = {};
    writeNames();
    mergeDirty = true;
    getMerging();
    comparedVersion.clear();
    comparedSong = {};
    selectedChange.reset();

    if (! hearingBounce)
        bounceCompared.clear();

    adoptLater(); // a branch whose last version another computer wrote
    tellNow();
    return true;
}

bool VersionHistory::renameBranch (const juce::String& branchID, const juce::String& name)
{
    // As typed, without what no name holds (toBranchName()).
    const auto newName = toBranchName (name);
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });
    const auto taken = std::any_of (branches.begin(), branches.end(), [&branchID, &newName] (const auto& branch)
                                    { return branch.id != branchID && branch.name.equalsIgnoreCase (newName); });

    // Not while a version is being written, which writes the branch's record with the name it had as it began, or a first
    // save copies the folder (copyFolder()).
    if (store == nullptr || ! hasRead || isSaving() || found == branches.end() || newName.isEmpty() || taken)
        return false;

    auto renamed = *found;
    renamed.name = newName;
    renamed.changed = stampNow(); // the newest name, on every computer

    if (! store->writeBranch (renamed))
        return false;

    *found = renamed;
    tellNow();
    return true;
}

bool VersionHistory::deleteBranch (const juce::String& branchID, bool takenBack)
{
    followFolder();
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });

    if (store == nullptr || ! hasRead || isSaving() || found == branches.end() || branchID == currentBranch)
        return false;

    // A merge of its line under way, or a merge waiting (here, or on another branch) that took in a commit only its line
    // holds, which would leave the graph with it.
    if ((preview.has_value() && preview->version == found->head) || holdsCommitMergeWaits (*found))
        return false;

    // This computer's record says it is deleted, the newest word on it; the song left on it goes from the record, and is
    // kept 30 days with the branch's versions by the log's line, which names it (but for a branch taken back).
    auto gone = *found;
    gone.deleted = true;
    gone.changed = stampNow();
    gone.computer = getThisComputer();
    gone.working = {};
    gone.divergent.clear();

    if (! store->writeBranch (gone))
        return false;

    logMove (branchID, found->head, {}, takenBack ? "cancel" : "delete", found->name, takenBack ? juce::String() : found->working);
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

std::optional<VersionHistory::LeftSong> VersionHistory::hasChangesLeftOn (const juce::String& branchID)
{
    // The song left on it, as this computer left it, against its last commit's, as the changes count them: compared on the
    // history's thread the first time it is asked for (known bug 126), and known from then on, as pieces never change.
    for (const auto& branch : branches)
    {
        if (branch.id != branchID || branch.working.isEmpty() || store == nullptr)
            continue;

        const auto* head = findVersion (branch.head);

        if (head != nullptr && head->song == branch.working)
            return LeftSong::same; // the same pieces: the same song

        const auto key = branch.working + "/" + (head != nullptr ? head->song : juce::String());

        if (const auto known = changesLeft.find (key); known != changesLeft.end())
            return known->second;

        if (changesLeftBeingRead.insert (key).second)
        {
            run (
                [shared = store, key, left = branch.working, last = head != nullptr ? head->song : juce::String(),
                 weak = juce::WeakReference<VersionHistory> (this)]
                {
                    const auto leftSong = shared->readSong (left);
                    const auto lastSong = last.isNotEmpty() ? readCommitSong (*shared, last) : juce::ValueTree (schema::PHAZITE);
                    shared->getPieces().closeFiles();
                    // One that can't be read is said so: what was left there may be anything (found in review).
                    const auto changed = ! leftSong.isValid() || ! lastSong.isValid() ? LeftSong::unreadable
                                         : findChanges (lastSong, leftSong).empty()   ? LeftSong::same
                                                                                      : LeftSong::changed;

                    juce::MessageManager::callAsync (
                        [weak, key, changed]
                        {
                            if (auto* history = weak.get())
                            {
                                --history->pendingJobs;
                                history->changesLeft[key] = changed;
                                history->changesLeftBeingRead.erase (key);
                                history->tellNow();
                            }
                        });
                },
                Work::reads);
        }

        return std::nullopt;
    }

    return LeftSong::same; // none left there
}

bool VersionHistory::canUndoLastCommit() const
{
    // Of the version the song grew from, which is the branch's last (not the versions it didn't grow from); not while the
    // song's branch isn't in the folder, whose branch this would move back (missingBranch: Create Branch first).
    const auto* last = findLastVersion();
    return store != nullptr && hasRead && ! isSaving() && missingBranch.isEmpty() && ! getMerging().has_value() && ! preview.has_value()
           && last != nullptr && last->id == baseVersion && last->parents.size() == 1 && findVersion (last->parents[0]) != nullptr;
}

bool VersionHistory::undoLastCommit (juce::ValueTree beforeSong, PluginParameterValues beforeParameters)
{
    followFolder();

    if (! canUndoLastCommit())
        return false;

    const auto undone = *findLastVersion();
    const auto& before = undone.parents[0];

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

    logMove (moved.id, undone.id, before, "undo", moved.name);
    *current = moved;

    // The song stays as it is, and the changes are from the version before, so that what the undone version recorded shows
    // in them again. A plug-in whose stored settings differ between the two versions counts as changed, as it did in the
    // version (its count of the changes it said it made is taken back one).
    const auto statesThen = getPluginStates (beforeSong), statesUndone = getPluginStates (baseline);
    auto reported = reportedAtBaseline;

    for (const auto& [device, undoneState] : statesUndone)
        if (const auto then = statesThen.find (device); then == statesThen.end() || then->second != undoneState)
            --reported[device];

    moveBase (before, std::move (beforeSong), std::move (beforeParameters), std::move (reported));
    previousBaseline = {};
    writeNames();

    // Its name and note the draft again, for the version saved again, unless one is being typed.
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
    adoptLater();
    readBounces(); // and tells
    return true;
}

const std::vector<SongChange>& VersionHistory::getVersionChanges (const juce::String& versionID)
{
    static const std::vector<SongChange> none;

    if (const auto found = versionChanges.find (versionID); found != versionChanges.end())
        return found->second;

    const auto* version = findVersion (versionID);

    if (version == nullptr || store == nullptr)
        return none;

    // From the commit it follows, or from nothing for the first; every plug-in whose settings differ counts. Both songs read
    // and compared on the history's thread, none listed meanwhile (isReadingVersionChanges()).
    if (versionChangesBeingRead.insert (versionID).second)
    {
        const auto* parent = version->parents.isEmpty() ? nullptr : findVersion (version->parents[0]);

        run (
            [shared = store, number = generation, versionID, first = version->parents.isEmpty(),
             thenPiece = parent != nullptr ? parent->song : juce::String(), nowPiece = version->song,
             weak = juce::WeakReference<VersionHistory> (this)]
            {
                PluginParameterValues before, after;
                const auto then = first                    ? juce::ValueTree (schema::PHAZITE)
                                  : thenPiece.isNotEmpty() ? readCommitSong (*shared, thenPiece, &before)
                                                           : juce::ValueTree();
                const auto now = readCommitSong (*shared, nowPiece, &after);
                shared->getPieces().closeFiles();
                std::vector<SongChange> changed;

                if (then.isValid() && now.isValid())
                    changed = findChanges (then, now, { &before, &after, nullptr });

                juce::MessageManager::callAsync (
                    [weak, number, versionID, changed]() mutable
                    {
                        if (auto* self = weak.get())
                            self->versionChangesRead (number, versionID, std::move (changed));
                    });
            },
            Work::reads);
    }

    return none;
}

bool VersionHistory::isReadingVersionChanges (const juce::String& versionID) const
{
    return versionChangesBeingRead.count (versionID) > 0;
}

void VersionHistory::versionChangesRead (int number, const juce::String& versionID, std::vector<SongChange> changed)
{
    --pendingJobs;
    versionChangesBeingRead.erase (versionID);

    if (number != generation || findVersion (versionID) == nullptr)
        return;

    versionChanges[versionID] = std::move (changed); // a commit never changes
    tellNow();
}

void VersionHistory::takeSnapshot (const juce::String& reason)
{
    followFolder();

    // Never of a version open in the song's place: the song as it was before is in Local History already (openVersion()).
    if (store == nullptr || ! hasRead || song.isShowingOther())
        return;

    // Not while a first save copies the folder (copyFolder()): once it is over, the song as it is then.
    if (copyingFolder)
    {
        openSnapshotWaiting = openSnapshotWaiting || reason == "open";
        editedSinceSnapshot = true;
        return;
    }

    // The song as a save stores it, its plug-ins' own settings included (as the project opens, as it was read: its plug-ins
    // may still be loading), and their knobs' values; split and written on the history's thread.
    if (reason != "open")
        song.storeDeviceStates();

    editedSinceSnapshot = false;
    lastSnapshotTime = getVersionsTime();

    run (
        [shared = store, number = generation, copy = copyWholeSong (song), parameters = readPluginParameters(), branch = currentBranch,
         time = stampNow(), reason, weak = juce::WeakReference<VersionHistory> (this), reference = baseline]() mutable
        {
            // Only while the song differs from the version it grew from (its changes, worked out in the background, may not
            // have been in as it was asked for), and from the snapshot before it; thinned as it grows.
            const auto differs = ! reference.isValid() || ! findChanges (reference, copy).empty();
            const auto pieces = splitSong (copy, parameters);
            // Thinned by the keeping time (VersionStore::getKeepingTime()), as the sweep keeps them: a clock set ahead
            // thins away no more than a week's worth.
            const auto keepingTime = shared->getKeepingTime();
            auto all = shared->readSnapshots (keepingTime);

            if (differs && (all.empty() || all.back().song != pieces.song) && shared->writeSong (pieces))
            {
                all.push_back ({ time, branch, pieces.song, reason });
                all = thinSnapshots (all, keepingTime);
                shared->writeSnapshots (all);
            }

            shared->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, all]() mutable
                {
                    if (auto* self = weak.get())
                        self->snapshotTaken (number, std::move (all));
                });
        },
        Work::writes);
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
    // As a project's backups are kept (model/base/KeptByAge.h).
    std::vector<juce::int64> times;

    for (const auto& snapshot : oldestFirst)
        times.push_back (snapshot.time);

    const auto keep = keepByAge (times, now);
    std::vector<Snapshot> kept;

    for (size_t i = 0; i < oldestFirst.size(); ++i)
        if (keep[i])
            kept.push_back (oldestFirst[i]);

    return kept;
}

bool VersionHistory::editMessage (const juce::String& versionID, const juce::String& name, const juce::String& note)
{
    followFolder();
    const auto found = versions.find (versionID);

    if (store == nullptr || ! hasRead || copyingFolder || found == versions.end())
        return false;

    const MessageEdit edit { name.trim(), note.trim(), stampNow() };

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
    const auto before = versions;
    auto held = getOfferedCommits();
    held.add (baseVersion); // the version the song grew from, on a branch or not
    keepReachable (versions, branches, held);

    for (const auto& [id, record] : before)
        if (versions.count (id) == 0)
            departed[id] = record;

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

std::vector<RenumberedID> VersionHistory::findRenumbering (const juce::String& ours, const juce::String& theirs, const juce::String& parted,
                                                           bool onlyAtParting) const
{
    return phazite::findRenumbering (versions, ours, theirs, parted, onlyAtParting);
}

std::vector<RenumberedID> findRenumbering (const std::map<juce::String, VersionRecord>& versions, const juce::String& ours,
                                           const juce::String& theirs, const juce::String& parted, bool onlyAtParting)
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

        if (version->parents.size() > 1 && ! version->renumbered.empty() && follows (versions, theirs, version->parents[1])
            && (! onlyAtParting || parted.isEmpty() || follows (versions, parted, version->parents[1])))
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

//==============================================================================
// The song tied to its version (VersionHistory.h).
VersionHistory::Names VersionHistory::readNames (const juce::ValueTree& songState)
{
    // As anyone may have written them: an id of its form, or none.
    Names names;

    if (const auto branch = songState[schema::versionsBranch].toString(); schema::isVersionsID (branch, schema::versionsBranchDigits))
        names.branch = branch;

    if (const auto base = songState[schema::versionsBase].toString(); schema::isVersionsID (base, schema::versionsBaseDigits))
        names.base = base;

    return names;
}

void VersionHistory::baseRead (int number, const juce::String& base, const juce::String& unreadable,
                               std::map<juce::String, juce::StringArray> elsewhere, const juce::String& missing)
{
    if (number != generation)
        return;

    missingBranch = missing;
    baseVersion = base;
    baseProblem = unreadable;
    headsElsewhere = std::move (elsewhere);
    previousBaseline = {};
}

void VersionHistory::writeNames()
{
    // Without an undo step, as where the song stands is no change to it (Schema.h); the project writes them into its file
    // (Project.h: versionsMoved()). Not before the first version, which writes them as it is saved.
    if (store == nullptr || ! hasRead || branches.empty() || currentBranch.isEmpty() || missingBranch.isNotEmpty())
        return; // (where the song's branch is missing here, they stay what they are: it may sync yet)

    auto songState = song.getState();
    const juce::ScopedValueSetter<bool> moving (movingNames, true); // the history's own move, not one to follow

    // Each as an id of its form (a branch or a version another tool wrote under another id leaves the song's out: the
    // folder's current file names the branch then, and its last version the base).
    const auto set = [&songState] (const juce::Identifier& name, const juce::String& value, int digits)
    {
        if (! schema::isVersionsID (value, digits))
            songState.removeProperty (name, nullptr);
        else if (songState[name].toString() != value)
            songState.setProperty (name, value, nullptr);
    };

    set (schema::versionsBranch, currentBranch, schema::versionsBranchDigits);
    set (schema::versionsBase, baseVersion, schema::versionsBaseDigits);
}

const VersionRecord* VersionHistory::findBaseVersion() const
{
    return findVersion (baseVersion);
}

bool VersionHistory::isBehind() const
{
    if (missingBranch.isNotEmpty())
        return false; // the branch the folder names isn't the song's: none of its versions are offered

    // The branch's last version is neither the base nor one the base follows: newer versions on it, or a line elsewhere.
    const auto* last = findLastVersion();
    return last != nullptr && last->id != baseVersion && ! follows (versions, baseVersion, last->id);
}

int VersionHistory::countNewerVersions() const
{
    return isBehind() ? countVersionsNotIn (findLastVersion()->id, baseVersion) : 0;
}

bool VersionHistory::canSaveOnBranch() const
{
    if (missingBranch.isNotEmpty())
        return false; // a version saved now would land on a branch the song isn't on, taking that branch's work out

    // A version saved now would follow the base alone and become the branch's last, leaving the versions it doesn't follow
    // off the branch, out of sight: not until they are in the song, but by the merge that takes them in.
    if (! isBehind())
        return true;

    const auto last = findLastVersion()->id;

    if (const auto& waiting = getMerging(); waiting.has_value() && follows (versions, waiting->version, last))
        return true;

    // Held by another computer's record, which a version saved here leaves as it is: the lines go their own ways, to be
    // merged (describeDivergence()).
    const auto found = headsElsewhere.find (currentBranch);
    return found != headsElsewhere.end()
           && std::any_of (found->second.begin(), found->second.end(),
                           [this, &last] (const juce::String& head) { return follows (versions, head, last); });
}

juce::String VersionHistory::whyNotSaveOnBranch() const
{
    if (canSaveOnBranch())
        return {};

    if (missingBranch.isNotEmpty())
        return "This song's branch isn't in this Versions folder (yet): Create Branch keeps working from the song on a branch of "
               "its own, where versions can be saved.";

    const auto* branch = findCurrentBranch();
    const auto newer = countNewerVersions();
    return "'" + (branch != nullptr ? branch->name : juce::String()) + "' has " + juce::String (newer)
           + (newer == 1 ? " version" : " versions")
           + " the song didn't grow from, which a version saved now would leave on no branch: take or merge them in first "
             "(or Create Branch, which goes on from the version the song grew from).";
}

void VersionHistory::moveBase (const juce::String& version, juce::ValueTree songThen, PluginParameterValues parameters,
                               ReportCounts reported, bool songIsIt)
{
    // The one it was kept, for an Undo that brings it back (followNamesOfSong()).
    if (version != baseVersion && baseline.isValid())
        previousBaseline = { baseVersion, baseline, baselineParameters };

    baseVersion = version;
    baseProblem.clear();
    setBaseline (std::move (songThen), std::move (parameters), std::move (reported), songIsIt);
}

void VersionHistory::followNamesOfSong()
{
    // Moved by an undo step's Undo or Redo (Take the Newer Versions'): the base the song's names say now, on this branch, its
    // song the one kept before it if it is that one, else read on the history's thread, the changes none meanwhile. As at
    // open: where it can't be read, the branch's last version, said; where the branch has newer versions and the song is
    // its last one's as it is (an Undo past a Take and a version saved after it, then Redo), it grew from that one.
    const auto names = readNames (song.getState());

    if (movingNames || ! hasRead || store == nullptr || names.branch != currentBranch || names.base.isEmpty() || names.base == baseVersion)
        return;

    const auto* version = findVersion (names.base);

    if (version == nullptr)
        return;

    const auto* last = findLastVersion();
    const auto mayBeLast = last != nullptr && last->id != names.base && follows (versions, last->id, names.base);

    if (previousBaseline.version == names.base && previousBaseline.song.isValid() && ! mayBeLast)
    {
        auto kept = std::exchange (previousBaseline, {});
        moveBase (kept.version, kept.song, std::move (kept.parameters), song.getDeviceChangesOutsideHistory());
        tellNow();
        return;
    }

    moveBase (names.base, {}, {}, song.getDeviceChangesOutsideHistory());
    const auto read = ++baseReads;

    if (mayBeLast)
        song.storeDeviceStates();

    run (
        [shared = store, piece = version->song, number = generation, read, base = names.base,
         weak = juce::WeakReference<VersionHistory> (this), lastID = last != nullptr ? last->id : juce::String(),
         lastPiece = last != nullptr ? last->song : juce::String(), songNow = mayBeLast ? copyWholeSong (song) : juce::ValueTree()]
        {
            PluginParameterValues parameters, lastParameters;
            auto then = readCommitSong (*shared, piece, &parameters);
            auto took = base;
            const auto baseWhole = then.isValid();

            // The branch's last version instead: where the base can't be read, or where the song is that version's.
            if (lastPiece.isNotEmpty() && (! then.isValid() || songNow.isValid()))
            {
                auto lastSong = readCommitSong (*shared, lastPiece, &lastParameters);

                if (lastSong.isValid() && (! then.isValid() || findChanges (lastSong, songNow).empty()))
                {
                    then = std::move (lastSong);
                    parameters = std::move (lastParameters);
                    took = lastID;
                }
            }

            shared->getPieces().closeFiles();

            juce::MessageManager::callAsync (
                [weak, number, read, base, took, then, parameters, baseWhole]() mutable
                {
                    auto* self = weak.get();

                    if (self == nullptr)
                        return;

                    --self->pendingJobs;

                    if (number == self->generation && read == self->baseReads && base == self->baseVersion && then.isValid())
                    {
                        self->setBaseline (then, std::move (parameters), self->song.getDeviceChangesOutsideHistory());

                        if (took != base)
                        {
                            self->baseVersion = took;
                            self->writeNames();
                        }

                        if (! baseWhole)
                            self->baseProblem = "The version this song grew from (" + base.substring (0, 7)
                                                + ") can't be read from the Versions folder: the changes are worked out against "
                                                  "the branch's last version instead, which the song grows from now. Nothing of "
                                                  "the song was lost.";
                    }
                    else if (number == self->generation && read == self->baseReads && base == self->baseVersion)
                    {
                        self->baseProblem = "The version this song grew from (" + base.substring (0, 7)
                                            + ") can't be read from the Versions folder, nor the branch's last version: its changes "
                                              "aren't shown until one of them reads.";
                    }

                    self->tellNow();
                });
        },
        Work::reads);
}

void VersionHistory::healToLast()
{
    // Not of a version open in the song's place, which isn't the song: once it is back (timerCallback()).
    if (song.isShowingOther())
        return;

    healPending = false;
    const auto* last = findLastVersion();

    if (! isBehind() || last == nullptr || ! follows (versions, last->id, baseVersion))
        return;

    // The song as a save stores it, compared with the branch's last version's on the history's thread: the same, and unchanged
    // since, it grows from that one (its changes since are none, known at once).
    song.storeDeviceStates();

    run (
        [shared = store, piece = last->song, id = last->id, number = generation, copy = copyWholeSong (song), changes = countSongChanges(),
         weak = juce::WeakReference<VersionHistory> (this)]
        {
            PluginParameterValues parameters;
            auto lastSong = readCommitSong (*shared, piece, &parameters);
            shared->getPieces().closeFiles();
            const auto same = lastSong.isValid() && findChanges (lastSong, copy).empty();

            juce::MessageManager::callAsync (
                [weak, number, id, changes, same, lastSong, parameters]() mutable
                {
                    auto* self = weak.get();

                    if (self == nullptr)
                        return;

                    --self->pendingJobs;
                    const auto* lastNow = self->findLastVersion();

                    if (number == self->generation && same && self->countSongChanges() == changes && self->isBehind() && lastNow != nullptr
                        && lastNow->id == id)
                    {
                        self->moveBase (id, lastSong, std::move (parameters), self->song.getDeviceChangesOutsideHistory(), true);
                        self->writeNames();
                        self->tellNow();
                    }
                });
        },
        Work::reads);
}

bool VersionHistory::takeBaseFrom (const juce::String& versionID, const juce::ValueTree& versionSong, PluginParameterValues parameters)
{
    const auto* last = findLastVersion();

    if (! hasRead || last == nullptr || last->id != versionID || ! isBehind() || ! versionSong.isValid())
        return false;

    moveBase (versionID, versionSong, std::move (parameters), song.getDeviceChangesOutsideHistory());
    tellNow();
    return true;
}

void VersionHistory::switchSaved()
{
    // The song left on the branch entered goes from its record once the project file holds the song (finishSwitch() kept
    // it, so that a save that failed loses neither branch's song).
    whenNotCopying (
        [weak = juce::WeakReference<VersionHistory> (this)]
        {
            auto* self = weak.get();

            if (self == nullptr || self->store == nullptr)
                return;

            for (auto& branch : self->branches)
            {
                if (branch.id != self->currentBranch || branch.working.isEmpty())
                    continue;

                auto entered = branch;
                entered.working = {};

                if (self->store->writeBranch (entered))
                    branch = entered;
            }
        });
}

juce::StringArray findVersionsNamedByProject (const juce::File& projectFile)
{
    // The song's own element's start tag, which holds its properties (SongText.h writes its elements after them): the bytes
    // from "<PHAZITE" to the next "<", which no attribute's value holds, read a block at a time, parsed as an element alone.
    const auto readBase = [] (const juce::File& file) -> juce::String
    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return {};

        juce::MemoryOutputStream bytes;
        constexpr int blockSize = 16384;
        constexpr juce::int64 mostRead = 16 * 1024 * 1024;
        const juce::String opening ("<" + schema::PHAZITE.toString());

        while (! in.isExhausted() && bytes.getDataSize() < (size_t) mostRead)
        {
            if (bytes.writeFromInputStream (in, blockSize) <= 0)
                break;

            const juce::String text (juce::CharPointer_UTF8 (static_cast<const char*> (bytes.getData())),
                                     juce::CharPointer_UTF8 (static_cast<const char*> (bytes.getData()) + bytes.getDataSize()));
            const auto start = text.indexOf (opening);
            const auto end = start >= 0 ? text.indexOfChar (start + 1, '<') : -1;

            if (start < 0 || (end < 0 && ! in.isExhausted()))
                continue;

            auto tag = text.substring (start, end >= 0 ? end : text.length()).trim();

            if (tag.endsWith ("></" + schema::PHAZITE.toString() + ">"))
                tag = tag.dropLastCharacters (schema::PHAZITE.toString().length() + 3);

            if (! tag.endsWith ("/>") && tag.endsWithChar ('>'))
                tag = tag.dropLastCharacters (1) + "/>";

            if (const auto element = juce::XmlDocument::parse (tag))
                return element->getStringAttribute (schema::versionsBase.toString());

            return {};
        }

        return {};
    };

    juce::StringArray files { projectFile.getFullPathName() };

    for (const auto& copy : projectFile.getSiblingFile ("Backups").findChildFiles (
             juce::File::findFiles, false, projectFile.getFileNameWithoutExtension() + "*" + Project::fileExtension))
        files.add (copy.getFullPathName()); // its backups and its autosave

    juce::StringArray named;

    for (const auto& path : files)
    {
        juce::ValueTree names (schema::PHAZITE);
        names.setProperty (schema::versionsBase, readBase (juce::File (path)), nullptr);

        if (const auto base = VersionHistory::readNames (names).base; base.isNotEmpty())
            named.addIfNotAlreadyThere (base);
    }

    return named;
}

namespace
{
    // The IDs a song holds as a merge's records key them (SongMerge.h: RenumberedID): an item's by no group, a note's by its
    // clip's group ("link 12", "clip 66"), as the song is numbered.
    std::set<std::pair<juce::String, std::uint64_t>> collectRecordedIDs (const juce::ValueTree& song)
    {
        std::set<std::pair<juce::String, std::uint64_t>> held;
        const std::function<void (const juce::ValueTree&)> collect = [&held, &collect] (const juce::ValueTree& element)
        {
            for (int i = 0; i < element.getNumProperties(); ++i)
                if (const auto name = element.getPropertyName (i); ! element.hasType (schema::NOTE) && schema::holdsID (element, name))
                    held.insert ({ {}, readID (element[name]) });

            if (element.hasType (schema::CLIP))
            {
                const auto group = element.hasProperty (schema::clipLink)
                                       ? "link " + juce::String ((juce::uint64) readID (element[schema::clipLink]))
                                       : "clip " + juce::String ((juce::uint64) readID (element[schema::clipID]));

                for (const auto& note : element)
                    if (note.hasType (schema::NOTE))
                        held.insert ({ group, readID (note[schema::noteID]) });
            }

            for (const auto& child : element)
                collect (child);
        };

        collect (song);
        return held;
    }

    // A note's group as a merge's records key it ("link 12", "clip 66"), its alias group's or clip's ID translated through the
    // items' IDs given (from, to).
    juce::String translateGroup (const juce::String& group, const std::map<std::uint64_t, std::uint64_t>& items)
    {
        const auto space = group.lastIndexOfChar (' ');
        const auto to = space >= 0 ? items.find ((std::uint64_t) group.substring (space + 1).getLargeIntValue()) : items.end();
        return to != items.end() ? group.substring (0, space + 1) + juce::String ((juce::uint64) to->second) : group;
    }
} // namespace

std::vector<RenumberedID> crossRenumbering (const std::vector<RenumberedID>& theirMerges, const std::vector<RenumberedID>& ourMerges,
                                            const std::vector<RenumberedID>& theirMergesAtParting,
                                            const std::vector<RenumberedID>& ourMergesAtParting, const juce::ValueTree& base,
                                            const juce::ValueTree& theirSong, const std::function<std::uint64_t()>& makeID)
{
    using Key = std::pair<juce::String, std::uint64_t>;
    const auto held = collectRecordedIDs (theirSong), inBase = collectRecordedIDs (base);

    // Each line's latest word for an ID, oldest first as findRenumbering() gives them; the other line's only for what its
    // song holds as that word gave it (three lines' merges of the same version, or an item that line took out since).
    std::map<Key, std::uint64_t> theirs, ours;
    std::set<Key> atParting;

    for (const auto& id : theirMerges)
        theirs[{ id.clip, id.from }] = id.to;

    for (const auto& id : ourMerges)
        ours[{ id.clip, id.from }] = id.to;

    for (const auto& id : theirMergesAtParting)
        atParting.insert ({ id.clip, id.from });

    // What the translation after this one moves (findTranslation(), through this line's records at the parting, as it takes
    // them): an item the parting holds, by its ID there; a note by its clip as this line numbers it.
    std::map<std::uint64_t, std::uint64_t> partingItems;
    std::map<Key, std::uint64_t> partingNotes;

    for (const auto& id : findLatestWords (ourMergesAtParting))
    {
        if (id.clip.isNotEmpty())
            partingNotes[{ id.clip, id.from }] = id.to;
        else if (inBase.count ({ {}, id.from }) > 0)
            partingItems[id.from] = id.to;
    }

    // The new IDs: none either song or any record holds, so that one never meets another item.
    std::set<std::uint64_t> taken;

    for (const auto* ids : { &held, &inBase })
        for (const auto& key : *ids)
            taken.insert (key.second);

    for (const auto* records : { &theirMerges, &ourMerges, &theirMergesAtParting, &ourMergesAtParting })
    {
        for (const auto& id : *records)
        {
            taken.insert (id.from);
            taken.insert (id.to);
        }
    }

    const auto makeFreeID = [&taken, &makeID]
    {
        auto id = makeID();

        while (! taken.insert (id).second)
            id = makeID();

        return id;
    };

    // The items first, then the notes, each in their clip as the items' translation numbers it (translateIDs() looks them up
    // so). Each kept by its key in the other line's song. What crosses into the parting's numbering, one numbering a stage:
    // to the parting's ID (the record's from) where the translation after this one moves it on to this line's, else straight
    // to this line's ID (this line's latest word for the same from, else from itself), where both lines' merges gave the same
    // ID to itself, so that it counts as this line's (findTranslation()). And the other line's own items in the way, each
    // given a new ID first: one under an ID something crosses to, unless it crosses away itself or is the parting's own item
    // there; and one under an ID a merge of that line gave away at the parting, which holds that version's item under it
    // (theirMergesAtParting), whether this line has a word for it or not.
    std::vector<RenumberedID> crossed, givenAnew;
    std::map<std::uint64_t, std::uint64_t> items, partingOf; // the items' translation here; a crossed item's ID in the parting

    // Whether the parting holds an ID of the other line's song (a note's in its clip as the parting numbers it); and whether
    // that line's latest word gave the parting's item under it another ID (its own item holds that one then).
    const auto inParting = [&inBase, &partingOf] (const Key& key)
    { return inBase.count ({ translateGroup (key.first, partingOf), key.second }) > 0; };
    const auto givenAway = [&theirs] (const Key& key)
    {
        const auto word = theirs.find (key);
        return word != theirs.end() && word->second != key.second;
    };

    for (const auto notes : { false, true })
    {
        const auto isOfGroup = [notes] (const Key& key) { return key.first.isNotEmpty() == notes; };
        std::map<Key, std::uint64_t> crossings;

        for (const auto& [from, to] : theirs)
        {
            if (! isOfGroup (from) || held.count ({ from.first, to }) == 0)
                continue;

            // Its clip as this line numbers it after both translations, by which this line's records key a note.
            const auto ourGroup = translateGroup (translateGroup (from.first, items), partingItems);
            const auto movedOn =
                notes ? partingNotes.count ({ ourGroup, from.second }) > 0 && inParting (from) : partingItems.count (from.second) > 0;
            const auto our = ours.find ({ ourGroup, from.second });
            crossings[{ from.first, to }] = movedOn ? from.second : our != ours.end() ? our->second : from.second;

            if (! notes)
                partingOf[to] = from.second;
        }

        std::set<Key> own;

        for (const auto& [at, target] : crossings)
            if (const Key under { at.first, target };
                held.count (under) > 0 && crossings.count (under) == 0 && (! inParting (under) || givenAway (under)))
                own.insert (under);

        for (const auto& key : atParting)
            if (isOfGroup (key) && held.count (key) > 0 && crossings.count (key) == 0 && inParting (key))
                own.insert (key);

        for (const auto& [at, target] : crossings)
            crossed.push_back ({ at.first, at.second, target });

        for (const auto& key : own)
            givenAnew.push_back ({ key.first, key.second, makeFreeID() });

        if (! notes)
        {
            for (const auto* ids : { &crossed, &givenAnew })
                for (const auto& id : *ids)
                    items[id.from] = id.to;
        }
    }

    // Each note keyed by its clip as the items' translation numbers it.
    const auto keyNotes = [] (std::vector<RenumberedID> ids, const std::map<std::uint64_t, std::uint64_t>& translatedItems)
    {
        for (auto& id : ids)
            id.clip = translateGroup (id.clip, translatedItems);

        return ids;
    };

    auto all = crossed;
    all.insert (all.end(), givenAnew.begin(), givenAnew.end());
    all = keyNotes (std::move (all), items);

    if (areOneToOne (all))
        return all;

    // Where the crossings aren't one to one (records another history wrote, or damaged), they are left out; the new IDs stay,
    // as an ID no item has can never make two items one.
    std::map<std::uint64_t, std::uint64_t> itemsGivenAnew;

    for (const auto& id : givenAnew)
        if (id.clip.isEmpty())
            itemsGivenAnew[id.from] = id.to;

    return keyNotes (givenAnew, itemsGivenAnew);
}

std::vector<RenumberedID> recordCrossings (const std::vector<RenumberedID>& crossed, const std::vector<RenumberedID>& translation)
{
    std::map<std::uint64_t, std::uint64_t> items;
    std::map<std::pair<juce::String, std::uint64_t>, std::uint64_t> notes;

    for (const auto& id : translation)
    {
        if (id.clip.isEmpty())
            items[id.from] = id.to;
        else
            notes[{ id.clip, id.from }] = id.to;
    }

    // As translateIDs() takes the second after the first: the items, then each note by its clip as they number it.
    std::vector<RenumberedID> recorded;

    for (const auto& id : crossed)
    {
        if (id.clip.isEmpty())
        {
            const auto to = items.find (id.to);
            recorded.push_back ({ {}, id.from, to != items.end() ? to->second : id.to });
        }
        else
        {
            const auto group = translateGroup (id.clip, items);
            const auto to = notes.find ({ group, id.to });
            recorded.push_back ({ group, id.from, to != notes.end() ? to->second : id.to });
        }
    }

    return recorded;
}

std::pair<juce::ValueTree, juce::String> readLastCommitSong (const juce::File& projectFile, const juce::String& computer)
{
    const VersionStore store (projectFile.getSiblingFile ("Versions"), computer);
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
    const auto legalComputer = makeUsableFileName (computer);

    for (const auto& record : records)
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

        // Its name as anyone may have written it (another computer, a folder from anywhere), kept as a typed one is, never
        // empty.
        branch.name = toBranchName (branch.name);

        if (branch.name.isEmpty())
            branch.name = "branch";

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
