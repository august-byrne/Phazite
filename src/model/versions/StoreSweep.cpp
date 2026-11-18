// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/StoreSweep.h"
#include "internal/VersionHistory.h"
#include "model/base/KeptByAge.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    std::atomic<juce::int64> floorForTests { -1 };
    std::atomic<bool> letsGoForTests { false };

    // A piece's or a version's name: 64 lowercase hexadecimal digits. Anything else a file says is no name.
    bool isPieceName (const juce::String& text)
    {
        return text.length() == 64 && text.containsOnly ("0123456789abcdef");
    }

    bool startsWith (const juce::MemoryBlock& bytes, const char* start)
    {
        const auto size = std::strlen (start);
        return bytes.getSize() >= size && std::memcmp (bytes.getData(), start, size) == 0;
    }

    // What a sweep left out and couldn't finish: the bounce and this computer's edited message of each of those versions
    // that a sure walk names no more and whose record is gone from the store. One named again (brought back meanwhile), or
    // whose record is still here (the join didn't finish, or a pack can't be opened now), isn't gone, and is no longer
    // noted. Those whose bounces another program holds stay noted.
    void finishWhatWent (VersionStore& store, VersionStore::Swept& swept, const Named& named)
    {
        if (! doesSweepLetGo())
            return; // nothing goes, a bounce or a message neither

        juce::StringArray gone, pending;

        for (const auto& version : swept.bounces)
        {
            if (! named.sure || named.pieces.count (version) > 0 || store.getPieces().has (version))
                continue;

            gone.add (version);

            if (const auto bounce = store.getBounceFile (version); bounce.existsAsFile() && ! bounce.deleteFile())
                pending.add (version);
        }

        store.forgetMessages (gone);
        swept.bounces = pending;
    }
} // namespace

void setSweepFloorForTests (juce::int64 bytes)
{
    floorForTests = bytes;
}

bool doesSweepLetGo()
{
    return sweepLetsGo || letsGoForTests.load();
}

void setSweepLetsGoForTests (bool letsGo)
{
    letsGoForTests = letsGo;
}

Named findNamedPieces (VersionStore& store, const juce::ValueTree& openMerging, const juce::StringArray& alsoFrom, juce::int64 asOf,
                       const std::function<bool()>& shouldStop)
{
    Named named;

    // A file that names something and doesn't read whole might name anything.
    if (! store.areRecordsWhole())
    {
        named.sure = false;
        named.why = "a file of the Versions folder that says what it keeps doesn't read whole";
        return named;
    }

    auto& pieces = store.getPieces();
    juce::StringArray toVisit (alsoFrom);

    const auto name = [&toVisit] (const juce::String& id)
    {
        if (isPieceName (id))
            toVisit.add (id);
    };

    const auto nameMerge = [&name] (const juce::ValueTree& merging)
    {
        for (const auto* property : { &schema::mergingBefore, &schema::mergingVersion, &schema::mergingHead })
            name (merging[*property].toString());
    };

    // Every branch as every computer's records make it (a branch deleted everywhere is none), each line read for it: the
    // last versions and the lines that went their own way. A record's last version that can't be read is named, so that it
    // makes the walk unsure.
    const auto records = store.readBranches();
    std::map<juce::String, VersionRecord> versions;
    std::set<std::pair<juce::String, juce::String>> recorded; // each computer's record of each branch, by their files' names

    for (const auto& record : records)
    {
        recorded.insert ({ makeUsableFileName (record.id), record.computer });
        juce::StringArray toRead (record.head);

        while (! toRead.isEmpty() && ! shouldStop())
        {
            const auto id = toRead[toRead.size() - 1];
            toRead.remove (toRead.size() - 1);

            if (id.isNotEmpty() && versions.count (id) == 0)
                if (auto version = store.readVersion (id))
                {
                    toRead.addArray (version->parents);
                    versions.emplace (id, std::move (*version));
                }
        }

        if (! record.deleted && versions.count (record.head) == 0)
            name (record.head);
    }

    for (const auto& branch : joinBranchRecords (records, versions, store.getComputer()))
    {
        name (branch.head);

        for (const auto& line : branch.divergent)
            name (line);
    }

    // What left every branch within the 30 days before the keeping time, by every computer's logs: where a branch was and
    // where it went, and the song left on a branch deleted. And where a computer's record of a branch can't be found any
    // more, where its log last put the branch, by the log's own order (whatever its clock said), unless it deleted it.
    std::map<std::pair<juce::String, juce::String>, BranchMove> last;

    for (const auto& move : store.readEveryLog (false))
    {
        if (asOf - move.time <= keptByAge::dailyFor)
        {
            name (move.from);
            name (move.to);

            if (move.why == "delete")
                name (move.song);
        }

        last[{ move.branch, move.computer }] = move;
    }

    for (const auto& [where, move] : last)
        if (recorded.count (where) == 0 && move.why != "delete" && move.why != "cancel")
            name (move.to);

    // The songs kept without a version: those left on branches, Local History's (thinned as of the keeping time), and those
    // left on branches deleted within the 30 days before it. And the merge waiting in the open song. (Another root goes
    // here: a version the project file names, roadmap S4.5.)
    for (const auto& kept : store.findKeptSongs (asOf))
        name (kept.piece);

    if (openMerging.isValid())
        nameMerge (openMerging);

    // Each piece walked down to what it names: a version to its song and the versions it follows, a song's pieces to theirs
    // and, for a whole song, to the merge waiting in it. A plug-in's state and its parameters' values name nothing: only
    // whether the store holds them counts.
    while (! toVisit.isEmpty())
    {
        if (shouldStop())
        {
            named.sure = false;
            named.why = "it was stopped";
            break;
        }

        const auto id = toVisit[toVisit.size() - 1];
        toVisit.remove (toVisit.size() - 1);

        if (! named.pieces.insert (id).second)
            continue;

        const auto bytes = pieces.has (id) ? pieces.read (id) : std::nullopt;

        if (! bytes.has_value())
        {
            named.sure = false; // it might name anything

            if (named.why.isEmpty())
                named.why =
                    "a piece something keeps (" + id.substring (0, 7) + ") can't be read: damaged, or not arrived from another computer";

            continue;
        }

        if (startsWith (*bytes, "<VERSION "))
        {
            if (const auto record = toVersionRecord (readElement (*bytes), id))
            {
                name (record->song);

                for (const auto& parent : record->parents)
                    name (parent);
            }
            else
            {
                named.sure = false;
            }

            continue;
        }

        for (const auto& reference : findReferences (*bytes))
        {
            if (reference.what.startsWith (schema::versions::BYTES.toString()) || reference.what == schema::versions::PARAMETERS.toString())
            {
                named.pieces.insert (reference.piece);

                if (! pieces.has (reference.piece))
                {
                    named.sure = false;
                    named.why = named.why.isNotEmpty()
                                    ? named.why
                                    : "a plug-in's state something keeps (" + reference.piece.substring (0, 7) + ") isn't there";
                }
            }
            else
            {
                name (reference.piece);
            }
        }

        if (startsWith (*bytes, "<PHAZITE"))
            nameMerge (readElement (*bytes).getChildWithName (schema::MERGING));
    }

    pieces.closeFiles();
    return named;
}

Sweep sweepStore (VersionStore& store, const juce::ValueTree& openMerging, int maxPacks, const std::function<bool()>& shouldStop,
                  bool force)
{
    Sweep sweep;
    auto& pieces = store.getPieces();

    // The stamps' floor, from what this computer stamped before (kept apart from the sweep's time).
    store.settleStampFloor();

    if (isInHeldFolder (pieces.getFolder()))
        return sweep; // a first save copies it: nothing is written into it meanwhile (WholeFile.h: holdFolder())

    const auto stopping = [&shouldStop] { return shouldStop != nullptr && shouldStop(); };
    pieces.readNewPacks();
    pieces.removeLeftovers();

    // A noted time after now (a clock set ahead once) written again as now, so that weekly sweeps go on once the clock is
    // put right.
    const auto now = getVersionsTime();
    auto swept = store.readSwept();

    if (swept.time > now)
    {
        swept.time = now;
        store.writeSwept (swept);
    }

    // Nothing of this computer's to sweep, nor a first sweep to note: a new project's opens write nothing.
    if (pieces.getNumOwnPacks() == 0)
        return sweep;

    // A first sweep notes when, and leaves nothing out: a week's grace, whatever the clock says. Its packs gathered, if
    // there are many.
    if (swept.time <= 0)
    {
        if (pieces.getNumOwnPacks() > maxPacks)
            pieces.joinPacks (shouldStop);

        swept.time = now;
        store.writeSwept (swept);
        return sweep;
    }

    const auto due = force || pieces.getNumOwnPacks() > maxPacks || now - swept.time >= VersionStore::sweepEvery;

    if (! due)
        return sweep;

    // What may go: this computer's own pieces written more than 30 days before the keeping time, and of those, what nothing
    // names, with the bounces of the versions among them; only where it frees enough to be worth rewriting packs. A walk
    // also where what a sweep before noted waits to be finished.
    const auto keepingTime = std::min (now, swept.time + VersionStore::sweepEvery);
    const auto cutoff = keepingTime - keptByAge::dailyFor;
    const auto old = pieces.findOwnPiecesWrittenBefore (cutoff);
    const auto worth = force ? 0 : floorForTests.load() >= 0 ? floorForTests.load() : std::max (minimumFreed, pieces.getOwnSize() / 10);
    std::map<juce::String, juce::int64> bounces; // of the versions whose records are old, by version

    for (const auto& bounce : store.getBouncesFolder().findChildFiles (juce::File::findFiles, false, "*.flac"))
        if (const auto version = bounce.getFileNameWithoutExtension(); old.count (version) > 0)
            bounces[version] = bounce.getSize();

    juce::int64 couldFree = 0;

    for (const auto& [piece, bytes] : old)
        couldFree += bytes + (bounces.count (piece) > 0 ? bounces[piece] : 0);

    std::set<juce::String> unnamed;
    Named named;
    auto walked = false;

    if ((! old.empty() && couldFree >= worth && couldFree > 0) || ! swept.bounces.isEmpty())
    {
        named = findNamedPieces (store, openMerging, pieces.findPiecesWrittenSince (cutoff), keepingTime, shouldStop);
        walked = true;
        sweep.sure = named.sure;
        sweep.why = named.why;
        juce::int64 frees = 0;

        if (named.sure)
        {
            for (const auto& [piece, bytes] : old)
            {
                if (named.pieces.count (piece) == 0)
                {
                    unnamed.insert (piece);
                    frees += bytes + (bounces.count (piece) > 0 ? bounces[piece] : 0);
                }
            }
        }

        // Letting go off (doesSweepLetGo()): nothing left out, the walk noted done.
        if (frees < worth || frees == 0 || ! doesSweepLetGo())
            unnamed.clear();
    }

    if (stopping())
        return sweep; // changing nothing: a later open sweeps

    if (walked && ! doesSweepLetGo())
    {
        swept.time = now;
        store.writeSwept (swept);
    }

    if (unnamed.empty() && pieces.getNumOwnPacks() <= maxPacks && ! pieces.needsGathering())
    {
        if (walked)
            finishWhatWent (store, swept, named);

        swept.time = now;
        store.writeSwept (swept);
        return sweep;
    }

    // The versions among what goes, known by how their pieces start: noted before the join, so that their bounces and
    // edited messages go even if it is cut off once their records are gone (finishWhatWent(), at the next sure walk).
    juce::StringArray records;

    for (const auto& piece : unnamed)
        if (const auto start = pieces.readStart (piece, 9); start.has_value() && startsWith (*start, "<VERSION "))
            records.add (piece);

    for (const auto& record : records)
        swept.bounces.addIfNotAlreadyThere (record);

    store.writeSwept (swept);

    PieceStore::Pruning pruning;
    pruning.keep = [&unnamed] (const juce::String& piece) { return unnamed.count (piece) == 0; };
    pruning.cutoff = cutoff;

    if (! pieces.joinPacks (shouldStop, pruning, &sweep.leftOut))
        return sweep; // nothing changed, what was noted finished at the next sure walk

    sweep.joined = true;

    for (const auto& record : records)
        if (sweep.leftOut.contains (record))
            sweep.versionsGone.add (record);

    if (walked)
        finishWhatWent (store, swept, named);

    swept.time = now;
    store.writeSwept (swept);
    return sweep;
}

Adopted adoptVersions (VersionStore& store, Adoption& left, const std::function<bool()>& shouldStop)
{
    auto& pieces = store.getPieces();
    Adopted adopted;

    if (isInHeldFolder (pieces.getFolder()))
        return adopted; // a first save copies it: the rest at a later round

    // What this computer's packs lack, batched. Taken: what is in the batch; a piece a tree that can't be copied whole met
    // is not, so that another tree that names it copies it itself.
    std::set<juce::String> taken;
    std::map<juce::String, juce::MemoryBlock> batch;
    size_t batchSize = 0;

    // Depth first, children first, with a stack of its own: a piece goes into the batch after all it names (a version's
    // song, a song's pieces, and the merge a whole song holds); and none of the tree where a piece of it can't be read, so
    // that no piece has an own copy while what it names hasn't (a write takes a batch whole or not at all).
    const auto copyTree = [&] (const juce::String& root)
    {
        std::vector<std::pair<juce::String, bool>> stack { { root, false } };
        std::set<juce::String> seen;
        std::map<juce::String, juce::MemoryBlock> read;
        std::vector<std::pair<juce::String, juce::MemoryBlock>> collected;
        auto treeWhole = true;

        while (! stack.empty())
        {
            const auto [name, expanded] = stack.back();
            stack.pop_back();

            if (expanded)
            {
                if (auto found = read.find (name); found != read.end())
                {
                    collected.emplace_back (name, std::move (found->second));
                    read.erase (found);
                }

                continue;
            }

            if (! isPieceName (name) || taken.count (name) > 0 || ! seen.insert (name).second || pieces.hasOwnCopy (name))
                continue;

            auto bytes = pieces.read (name);

            if (! bytes.has_value())
            {
                treeWhole = false; // not arrived yet, or damaged
                continue;
            }

            stack.push_back ({ name, true });

            if (startsWith (*bytes, "<VERSION "))
            {
                if (const auto record = toVersionRecord (readElement (*bytes), name))
                    stack.push_back ({ record->song, false });
            }
            else
            {
                for (const auto& reference : findReferences (*bytes))
                    stack.push_back ({ reference.piece, false });

                if (startsWith (*bytes, "<PHAZITE"))
                    if (const auto merging = readElement (*bytes).getChildWithName (schema::MERGING); merging.isValid())
                        for (const auto* property : { &schema::mergingBefore, &schema::mergingVersion, &schema::mergingHead })
                            stack.push_back ({ merging[*property].toString(), false });
            }

            read.emplace (name, std::move (*bytes));
        }

        if (! treeWhole)
            return false;

        for (auto& [name, bytes] : collected)
        {
            batchSize += bytes.getSize();
            taken.insert (name);
            batch.emplace (name, std::move (bytes));
        }

        return true;
    };

    // Songs first, then versions, from the front, until a batch is full or it is told to stop; what was batched written.
    while (! left.isEmpty() && batchSize <= ((size_t) 8 << 20))
    {
        if (shouldStop != nullptr && shouldStop())
            break;

        auto& from = left.songs.isEmpty() ? left.versions : left.songs;
        const auto root = from[0];
        from.remove (0);

        if (! copyTree (root))
            adopted.unreadable.add (root);
    }

    if (! batch.empty() && ! pieces.write (batch))
        adopted.written = false;

    return adopted;
}
} // namespace phazite
