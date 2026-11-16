// SPDX-License-Identifier: AGPL-3.0-or-later
// The pieces of a project's versions (SongPieces.h), in packs in Versions/pieces: a pack is written whole, once, as a
// version is saved, with the pieces no pack before it holds, and never changed but in one case: a pack whose every piece
// was found damaged and is written again makes a pack of the same name (its index is the same), which takes the damaged
// one's place, whole or not at all. A piece is found by its name through the packs' indexes, read as the store opens.
// Reading a piece checks what it holds against its name, so a damaged piece is never read as another: it is read from
// another pack that holds it (two computers may each have written it), and a copy found damaged is forgotten, so that the
// next write that holds the piece writes it again (until then, the versions that need it can't be read; the others can).
// Any thread, a write at a time.
//
// A pack, Versions/pieces/<name>.pack, named by the SHA-256 of its index (a pack whose index doesn't match its name, or
// whose marks or index don't fit, is left out, and so is an entry whose size no zlib stream of its length could have):
//   "PZPK"                     what the file is
//   the pieces                 each compressed by zlib, one after another
//   the index                  for each piece: its name (32 bytes), where it starts (8), and its size compressed (4) and not (4)
//   how many pieces it holds   4 bytes
//   where its index starts     8 bytes
//   "PZPK"                     the file is whole
// Numbers are little-endian.
#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class PieceStore
{
public:
    explicit PieceStore (juce::File folder); // Versions/pieces, which needn't exist yet
    ~PieceStore();

    bool has (const juce::String& name) const;
    std::optional<juce::MemoryBlock> read (const juce::String& name) const;

    // The first bytes of a piece, as many as it has of those asked for, unchecked (only the whole can be checked against its
    // name): to tell what kind of piece it is without reading a large one whole. Nothing if it can't be read.
    std::optional<juce::MemoryBlock> readStart (const juce::String& name, int numBytes) const;

    // Writes those of the pieces that it doesn't hold yet into a new pack, whole or not at all (nothing for none). True
    // only once it holds every one of them: the pack is on the disk and was read back. False, with nothing changed, if it
    // couldn't be written, saying why in problem if given (a damaged pack of the same name that another program holds, which
    // it would replace).
    bool write (const std::map<juce::String, juce::MemoryBlock>& pieces, juce::String* problem = nullptr);

    // How many bytes its packs take, how many pieces they hold, and their names (in no particular order).
    juce::int64 getSize() const;
    int getNumPieces() const;
    juce::StringArray getNames() const;

    // How many packs it has; and its packs joined into one (git's repack), written whole beside them before they go, with
    // every piece that reads as what its name says: a pack holding a piece that doesn't stays as it is. A store reads
    // every pack's index as it opens, so a history of thousands of commits is a few files again. False, changing nothing,
    // if the new pack couldn't be written.
    int getNumPacks() const;
    bool joinPacks (const std::function<bool()>& shouldStop = {}); // gives up between pieces, changing nothing, once it says so

    // The packs its folder holds now that it hasn't read (written since it opened, by another computer whose folder this
    // one syncs, or by another store), read.
    void readNewPacks();

    // Lets go of the packs it has open for reading, which it opens again as needed.
    void closeFiles() const;

private:
    struct Location
    {
        int pack = 0;
        juce::int64 start = 0;
        juce::uint32 compressedSize = 0, size = 0;
    };

    const juce::File folder;
    juce::Array<juce::File> packs;
    mutable std::map<juce::String, std::vector<Location>>
        index; // each piece's copies, in the order their packs were read; a read forgets one found damaged
    juce::int64 totalSize = 0;
    mutable std::map<int, std::unique_ptr<juce::FileInputStream>> openPacks;
    juce::CriticalSection lock;

    void readIndex (const juce::File& pack);

    // A copy of a piece read where it is, with the lock held: nothing if it can't be, and whether it was read and isn't what
    // its name says (rather than its pack failing to open or read, which may pass).
    enum class Copy
    {
        whole,
        unread,
        damaged
    };

    Copy readCopy (const Location&, const juce::String& name, juce::MemoryBlock& piece) const;

    JUCE_DECLARE_NON_COPYABLE (PieceStore)
};
} // namespace phazite
