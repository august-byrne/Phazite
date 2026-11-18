// SPDX-License-Identifier: AGPL-3.0-or-later
// The pieces of a project's versions (SongPieces.h), in packs in Versions/pieces: a pack is written whole, once, as a
// version is saved, with the pieces this computer's own packs don't hold yet, and never changed. A piece is found by its
// name through the packs' indexes, read as the store opens, in any pack, whoever wrote it. Reading a piece checks what it
// holds against its name, so a damaged piece is never read as another: it is read from another pack that holds it (two
// computers may each have written it), and a copy found damaged is forgotten, so that the next write that holds the piece
// writes it again (until then, the versions that need it can't be read; the others can).
//
// Each computer writes the pieces its own packs don't hold into packs of its own, and joins, prunes and deletes those packs
// alone (joinPacks()): the store keeps its rule that each computer writes, and deletes, only its own files. Any thread, a
// write or a join at a time; what the message thread asks of its size and count never waits for one.
//
// A pack, Versions/pieces/<name>.pack, named by the SHA-256 of its index and its writer's 16 bytes (a pack whose name
// doesn't match, or
// whose marks or index don't fit, is left out, and so is an entry whose size no zlib stream of its length could have):
//   "PZPK"                     what the file is
//   the pieces                 each compressed by zlib, one after another
//   the index                  for each piece: its name (32 bytes), where it starts (8), its size compressed (4) and not (4),
//                              and when it was written into the store (8: milliseconds since 1970)
//   how many pieces it holds   4 bytes
//   where its index starts     8 bytes
//   who wrote it               16 bytes: the first half of the SHA-256 of the computer's id (VersionStore.h)
//   "PZPK"                     the file is whole
// Numbers are little-endian. A pack of one writer holding the same pieces, written at the same time, has the same name: one
// there already that doesn't read whole is written again in its place. Two writers' packs never share a name.
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace phazite
{
// The time the versions keep by, in milliseconds since 1970: the computer's clock, or the time a test set (0: the clock
// again), so that a test lets days pass without waiting (as Project::setBackupTime() does for backups). Any thread.
juce::int64 getVersionsTime();
void setVersionsTimeForTests (juce::int64 now);

class PieceStore
{
public:
    // Versions/pieces, which needn't exist yet, as a computer writes it (its id: VersionStore.h); none for a store that only
    // reads.
    explicit PieceStore (juce::File folder, const juce::String& writer = {});
    ~PieceStore();

    const juce::File& getFolder() const noexcept { return folder; }

    bool has (const juce::String& name) const;
    std::optional<juce::MemoryBlock> read (const juce::String& name) const;

    // The first bytes of a piece, as many as it has of those asked for, unchecked (only the whole can be checked against its
    // name): to tell what kind of piece it is without reading a large one whole. Nothing if it can't be read.
    std::optional<juce::MemoryBlock> readStart (const juce::String& name, int numBytes) const;

    // Writes those of the pieces that this computer's own packs don't hold yet into a new pack, whole or not at all (nothing
    // for none): a piece only another computer's pack holds is written again, as this computer's own copy. True only once
    // it holds every one of them: the pack is on the disk and was read back. False, with nothing changed, if it couldn't be
    // written, saying why in problem if given (a damaged pack of the same name that another program holds, which it would
    // replace).
    bool write (const std::map<juce::String, juce::MemoryBlock>& pieces, juce::String* problem = nullptr);

    // How many bytes its packs take, how many pieces and packs it has: read without waiting, from any thread, while a join
    // or a read goes on. And the pieces' names (in no particular order).
    juce::int64 getSize() const noexcept { return totalSize.load() + leftoverSize.load(); } // what a cut-off join left too
    int getNumPieces() const noexcept { return numPieces.load(); }
    int getNumPacks() const noexcept { return numPacks.load(); }
    juce::StringArray getNames() const;

    // Whether this computer's own packs hold a piece (PieceStore.h: each computer keeps its own copy of what it uses).
    bool hasOwnCopy (const juce::String& name) const;

    // The time what is written now is stamped with: the clock, but never before the floor its store sets
    // (VersionStore::settleStampFloor()), so that a clock set back doesn't make what is written now look old; and always
    // later than the stamp it gave before, so that no two things written are stamped the same instant (snapshots are found
    // by their times). Any thread. The floor is only ever raised; the newest of it and the stamps given, for a store that
    // takes over from this one (VersionHistory::followFolder()). And the newest time this computer's own packs say a piece
    // was written.
    juce::int64 stamp() const;
    void raiseStampFloor (juce::int64 time);
    juce::int64 getNewestStamp() const noexcept { return std::max (stampFloor.load(), lastStamp.load()); }
    juce::int64 findNewestOwnWrite() const;

    // What a join a crash cut off left beside the packs: deleted once a day old, counted in getSize() until then. As a
    // history opens.
    void removeLeftovers();

    // This computer's own packs: how many, and how many bytes they take.
    int getNumOwnPacks() const;
    juce::int64 getOwnSize() const;

    // The pieces this computer's own packs hold that were written before a time (by their newest own copy), each with the
    // bytes its own copies take: those a sweep could leave out. And every piece with a copy, whoever wrote it, written at
    // that time or later.
    std::map<juce::String, juce::int64> findOwnPiecesWrittenBefore (juce::int64 time) const;
    juce::StringArray findPiecesWrittenSince (juce::int64 time) const;

    // This computer's own packs joined (git's repack, with its prune): a new pack written whole beside them (model/base/
    // WholeFile.h), its bytes read back and checked before any of them goes, with every piece of theirs that is kept, each
    // with the time its newest own copy was written (read from any whole copy, another computer's too). Another computer's
    // packs are never touched. A pack whose pieces aren't all kept elsewhere stays (one holding a piece no pack holds whole),
    // and so does one another program holds open, for the next time.
    //
    // Which: every own pack (everyOwnPack), else those holding a piece left out, those smaller than smallPack, those whose
    // every piece another own pack holds too, and those of a size class holding gatherFrom or more (each class four times
    // the size of the one before): joined geometrically, so that the count of packs stays small for any size of store. A piece written before the cutoff that keep says isn't kept is left out (none: every piece is kept);
    // those left out, if asked. False, changing nothing, if the new pack couldn't be written or read back as it was written;
    // gives up so between pieces once shouldStop says so (a waiter for the history's thread sets it). The store stays
    // readable meanwhile: the copying holds no lock, only the swap at its end.
    struct Pruning
    {
        std::function<bool (const juce::String& name)> keep;
        juce::int64 cutoff = 0;
        bool everyOwnPack = false;
    };

    static constexpr juce::int64 smallPack = 1024 * 1024;
    static constexpr int gatherFrom = 4;
    static std::atomic<juce::int64> smallPackForTests; // another smallPack while above 0
    bool joinPacks (const std::function<bool()>& shouldStop = {}, const Pruning& = {}, juce::StringArray* leftOut = nullptr);

    // Whether a join would gather anything with nothing left out: packs of a class to join, small ones, or packs twice.
    bool needsGathering() const;

    // For tests: while set, a join waits as it begins to copy (the lock free), looking at its shouldStop, counted while it
    // waits; and what a join calls once its new pack is in place, before it reads it back (a test damages
    // it there).
    static std::atomic<bool> holdJoinsForTests;
    static std::atomic<int> joinsHeldForTests;
    static std::function<void (const juce::File&)> joinWrittenForTests;

    // The packs its folder holds now that it hasn't read (written since it opened, by another computer whose folder this
    // one syncs, or by another store), read.
    void readNewPacks();

    // The packs its folder holds that it couldn't read (another program held one as it was read, one is damaged, or it is
    // of a layout this build doesn't read), whose pieces might be anything: those it hasn't read, as of now.
    juce::Array<juce::File> findUnreadPacks() const;

    // Lets go of the packs it has open for reading, which it opens again as needed.
    void closeFiles() const;

private:
    struct Location
    {
        int pack = 0;
        juce::int64 start = 0;
        juce::uint32 compressedSize = 0, size = 0;
        juce::int64 written = 0;
    };

    struct Pack
    {
        juce::File file;
        bool own = false; // this store's computer wrote it
        juce::int64 size = 0;
    };

    const juce::File folder;
    const juce::MemoryBlock writer; // who writes its packs: 16 bytes
    std::vector<Pack> packs;
    mutable std::map<juce::String, std::vector<Location>>
        index; // each piece's copies, in the order their packs were read; a read forgets one found damaged
    mutable std::atomic<juce::int64> totalSize { 0 };
    std::atomic<juce::int64> leftoverSize { 0 };
    std::atomic<juce::int64> stampFloor { 0 };
    mutable std::atomic<juce::int64> lastStamp { 0 };
    mutable std::atomic<int> numPieces { 0 }, numPacks { 0 };
    mutable std::map<int, std::unique_ptr<juce::FileInputStream>> openPacks;
    juce::CriticalSection lock;

    void readIndex (const juce::File& pack);
    int findPack (const juce::File&) const;
    bool isOthersPack (const juce::File&) const;     // a pack there whose trailer names another writer
    void countAgain() const;                         // the counts the message thread reads, with the lock held
    void readAgain (const std::vector<juce::File>&); // the index made anew of these packs, with the lock held
    static std::set<int> chooseRewritten (const std::vector<Pack>&, const std::map<juce::String, std::vector<Location>>&, bool everyOwnPack,
                                          std::set<int>& duplicated);

    // A copy of a piece read where it is, with the lock held: nothing if it can't be, and whether it was read and isn't what
    // its name says (rather than its pack failing to open or read, which may pass).
    enum class Copy
    {
        whole,
        unread,
        damaged
    };

    Copy readCopy (const Location&, const juce::String& name, juce::MemoryBlock& piece) const;

    // joinPacks()' new pack written beside the others and read back against what was written, the lock free: false,
    // changing nothing, if it couldn't be. The pieces no copy of which reads whole are said in unreadable.
    bool writeJoined (const std::vector<std::pair<juce::String, juce::int64>>& joining,
                      std::map<juce::String, std::vector<Location>>& copies, const std::vector<Pack>& before,
                      const std::function<bool()>& stopping, std::set<juce::String>& unreadable, juce::File& joined, bool& wasThere);

    JUCE_DECLARE_NON_COPYABLE (PieceStore)
};
} // namespace phazite
