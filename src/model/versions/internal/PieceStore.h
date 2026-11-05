// SPDX-License-Identifier: AGPL-3.0-or-later
// The pieces of a project's versions (SongPieces.h), in packs in Versions/pieces: a pack is written whole, once, as a
// version is saved, with the pieces no pack before it holds, and never changed. A piece is found by its name through the
// packs' indexes, read as the store opens. Reading a piece checks what it holds against its name, so a damaged piece is
// never read as another (the versions that need it can't be read; the others can). Any thread, a write at a time.
//
// A pack, Versions/pieces/<name>.pack, named by the SHA-256 of its index:
//   "PZPK"                     what the file is
//   the pieces                 each compressed by zlib, one after another
//   the index                  for each piece: its name (32 bytes), where it starts (8), and its size compressed (4) and not (4)
//   how many pieces it holds   4 bytes
//   where its index starts     8 bytes
//   "PZPK"                     the file is whole
// Numbers are little-endian.
#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <optional>

namespace phazite
{
class PieceStore
{
public:
    explicit PieceStore (juce::File folder); // Versions/pieces, which needn't exist yet
    ~PieceStore();

    bool has (const juce::String& name) const;
    std::optional<juce::MemoryBlock> read (const juce::String& name) const;

    // Writes those of the pieces that it doesn't hold yet into a new pack, whole or not at all (nothing for none).
    bool write (const std::map<juce::String, juce::MemoryBlock>& pieces);

    // How many bytes its packs take, and how many pieces they hold.
    juce::int64 getSize() const;
    int getNumPieces() const;

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
    std::map<juce::String, Location> index;
    juce::int64 totalSize = 0;
    mutable std::map<int, std::unique_ptr<juce::FileInputStream>> openPacks;
    juce::CriticalSection lock;

    void readIndex (const juce::File& pack);

    JUCE_DECLARE_NON_COPYABLE (PieceStore)
};
} // namespace phazite
