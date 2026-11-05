// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PieceStore.h"
#include "internal/SongPieces.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstring>
#include <vector>

namespace phazite
{
namespace
{
    constexpr const char* mark = "PZPK";
    constexpr int markSize = 4;
    constexpr int nameSize = 32;                    // a SHA-256
    constexpr int entrySize = nameSize + 8 + 4 + 4; // its name, where it starts, its sizes
    constexpr int endSize = 4 + 8 + markSize;       // how many, where the index starts, and the mark

    juce::MemoryBlock compress (const juce::MemoryBlock& piece)
    {
        juce::MemoryOutputStream out;

        {
            juce::GZIPCompressorOutputStream zlib (out);
            zlib.write (piece.getData(), piece.getSize());
        }

        return out.getMemoryBlock();
    }

    std::optional<juce::MemoryBlock> decompress (const juce::MemoryBlock& compressed, juce::uint32 size)
    {
        juce::MemoryInputStream in (compressed, false);
        juce::GZIPDecompressorInputStream zlib (&in, false, juce::GZIPDecompressorInputStream::zlibFormat);
        juce::MemoryBlock piece (size);

        if (size > 0 && zlib.read (piece.getData(), (int) size) != (int) size)
            return std::nullopt;

        return piece;
    }

    bool hasMark (const char* bytes)
    {
        return std::memcmp (bytes, mark, (size_t) markSize) == 0;
    }
} // namespace

PieceStore::PieceStore (juce::File packFolder)
    : folder (std::move (packFolder))
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.pack");
    files.sort();

    for (const auto& file : files)
        readIndex (file);
}

PieceStore::~PieceStore()
{
    closeFiles();
}

void PieceStore::readIndex (const juce::File& file)
{
    // A pack that isn't whole, or isn't one, is left out: a write cut off before its end, which never got its name.
    juce::FileInputStream in (file);
    const auto length = in.getTotalLength();

    if (! in.openedOk() || length < markSize + endSize)
        return;

    char start[markSize] = {}, end[markSize] = {};
    in.read (start, markSize);
    in.setPosition (length - endSize);
    const auto count = (juce::uint32) in.readInt();
    const auto indexStart = in.readInt64();
    in.read (end, markSize);

    if (! hasMark (start) || ! hasMark (end) || indexStart < markSize || indexStart + (juce::int64) count * entrySize != length - endSize)
        return;

    juce::MemoryBlock entries;
    const auto indexSize = (size_t) count * (size_t) entrySize;
    in.setPosition (indexStart);

    if (in.readIntoMemoryBlock (entries, (juce::pointer_sized_int) indexSize) != indexSize)
        return;

    const auto packNumber = packs.size();
    packs.add (file);
    totalSize += length;

    for (juce::uint32 i = 0; i < count; ++i)
    {
        const auto* entry = static_cast<const char*> (entries.getData()) + (size_t) i * entrySize;
        Location location;
        location.pack = packNumber;
        location.start = (juce::int64) juce::ByteOrder::littleEndianInt64 (entry + nameSize);
        location.compressedSize = juce::ByteOrder::littleEndianInt (entry + nameSize + 8);
        location.size = juce::ByteOrder::littleEndianInt (entry + nameSize + 12);

        if (location.start >= markSize && location.start + location.compressedSize <= indexStart)
            index.emplace (juce::String::toHexString (entry, nameSize, 0), location);
    }
}

bool PieceStore::has (const juce::String& name) const
{
    const juce::ScopedLock sl (lock);
    return index.find (name) != index.end();
}

std::optional<juce::MemoryBlock> PieceStore::read (const juce::String& name) const
{
    const juce::ScopedLock sl (lock);
    const auto found = index.find (name);

    if (found == index.end())
        return std::nullopt;

    const auto& location = found->second;
    auto& stream = openPacks[location.pack];

    if (stream == nullptr)
    {
        stream = std::make_unique<juce::FileInputStream> (packs[location.pack]);

        if (! stream->openedOk())
        {
            stream = nullptr;
            return std::nullopt;
        }
    }

    juce::MemoryBlock compressed (location.compressedSize);

    if (! stream->setPosition (location.start)
        || stream->read (compressed.getData(), (int) location.compressedSize) != (int) location.compressedSize)
        return std::nullopt;

    // What it holds, if it is what its name says.
    auto piece = decompress (compressed, location.size);

    if (! piece.has_value() || getPieceName (*piece) != name)
        return std::nullopt;

    return piece;
}

bool PieceStore::write (const std::map<juce::String, juce::MemoryBlock>& pieces)
{
    std::vector<std::pair<juce::String, const juce::MemoryBlock*>> fresh;

    {
        const juce::ScopedLock sl (lock);

        for (const auto& [name, piece] : pieces)
            if (index.find (name) == index.end())
                fresh.emplace_back (name, &piece);
    }

    if (fresh.empty())
        return true;

    juce::MemoryOutputStream pack, entries;
    pack.write (mark, markSize);

    for (const auto& [name, piece] : fresh)
    {
        const auto compressed = compress (*piece);
        const auto start = (juce::int64) pack.getDataSize();
        pack.write (compressed.getData(), compressed.getSize());

        juce::MemoryBlock nameBytes;
        nameBytes.loadFromHexString (name);
        jassert (nameBytes.getSize() == (size_t) nameSize);
        nameBytes.setSize ((size_t) nameSize, true);

        entries.write (nameBytes.getData(), (size_t) nameSize);
        entries.writeInt64 (start);
        entries.writeInt ((int) compressed.getSize());
        entries.writeInt ((int) piece->getSize());
    }

    const auto indexStart = (juce::int64) pack.getDataSize();
    pack.write (entries.getData(), entries.getDataSize());
    pack.writeInt ((int) fresh.size());
    pack.writeInt64 (indexStart);
    pack.write (mark, markSize);

    if (! folder.createDirectory())
        return false;

    // Written whole beside it, then given its name: a pack is never seen half written.
    const auto file = folder.getChildFile (juce::SHA256 (entries.getData(), entries.getDataSize()).toHexString() + ".pack");

    if (! file.existsAsFile())
    {
        juce::TemporaryFile temporary (file);

        if (! temporary.getFile().replaceWithData (pack.getData(), pack.getDataSize()) || ! temporary.overwriteTargetFileWithTemporary())
            return false;
    }

    const juce::ScopedLock sl (lock);

    if (! packs.contains (file))
        readIndex (file);

    return true;
}

juce::int64 PieceStore::getSize() const
{
    const juce::ScopedLock sl (lock);
    return totalSize;
}

int PieceStore::getNumPieces() const
{
    const juce::ScopedLock sl (lock);
    return (int) index.size();
}

void PieceStore::closeFiles() const
{
    const juce::ScopedLock sl (lock);
    openPacks.clear();
}
} // namespace phazite
