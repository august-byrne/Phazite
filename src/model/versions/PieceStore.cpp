// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PieceStore.h"
#include "internal/SongPieces.h"
#include "model/base/WholeFile.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <vector>

namespace phazite
{
namespace
{
    constexpr const char* mark = "PZPK";
    constexpr int markSize = 4;
    constexpr int nameSize = 32;                           // a SHA-256
    constexpr int entrySize = nameSize + 8 + 4 + 4 + 8;    // its name, where it starts, its sizes, when it was written
    constexpr int writerSize = 16;                         // who wrote the pack
    constexpr int endSize = 4 + 8 + writerSize + markSize; // how many, where the index starts, who wrote it, and the mark

    std::atomic<juce::int64> timeForTests { 0 };

    juce::MemoryBlock makeWriter (const juce::String& computer)
    {
        const auto hash = juce::SHA256 (computer.toUTF8()).getRawData();
        return { hash.getData(), (size_t) writerSize };
    }

    juce::MemoryBlock compress (const juce::MemoryBlock& piece)
    {
        juce::MemoryOutputStream out;

        {
            juce::GZIPCompressorOutputStream zlib (out);
            zlib.write (piece.getData(), piece.getSize());
        }

        return out.getMemoryBlock();
    }

    // The most bytes zlib makes of one byte it compressed, and a little more for its header: a size past that is a damaged
    // index's, and so is one larger than a read can take.
    constexpr juce::uint64 maxExpansion = 1032, expansionSlack = 1024;

    bool isPossibleSize (juce::uint32 size, size_t compressedSize)
    {
        return (juce::uint64) size <= (juce::uint64) compressedSize * maxExpansion + expansionSlack
               && size <= (juce::uint32) std::numeric_limits<int>::max();
    }

    std::optional<juce::MemoryBlock> decompress (const juce::MemoryBlock& compressed, juce::uint32 size)
    {
        if (! isPossibleSize (size, compressed.getSize()))
            return std::nullopt;

        juce::MemoryInputStream in (compressed, false);
        juce::GZIPDecompressorInputStream zlib (&in, false, juce::GZIPDecompressorInputStream::zlibFormat);
        juce::MemoryBlock piece;

        try
        {
            piece.setSize (size);
        }
        catch (const std::bad_alloc&)
        {
            return std::nullopt;
        }

        if (size > 0 && zlib.read (piece.getData(), (int) size) != (int) size)
            return std::nullopt;

        return piece;
    }

    bool hasMark (const char* bytes)
    {
        return std::memcmp (bytes, mark, (size_t) markSize) == 0;
    }

    // A pack's name: the SHA-256 of its index and its writer's 16 bytes, so that two computers' packs never share a name.
    juce::String makePackName (const void* entries, size_t entriesSize, const void* wroteIt)
    {
        juce::MemoryBlock named (entries, entriesSize);
        named.append (wroteIt, (size_t) writerSize);
        return juce::SHA256 (named.getData(), named.getSize()).toHexString();
    }

    void writeEntry (juce::OutputStream& entries, const juce::String& name, juce::int64 start, juce::uint32 compressedSize,
                     juce::uint32 size, juce::int64 written)
    {
        juce::MemoryBlock nameBytes;
        nameBytes.loadFromHexString (name);
        jassert (nameBytes.getSize() == (size_t) nameSize);
        nameBytes.setSize ((size_t) nameSize, true);
        entries.write (nameBytes.getData(), (size_t) nameSize);
        entries.writeInt64 (start);
        entries.writeInt ((int) compressedSize);
        entries.writeInt ((int) size);
        entries.writeInt64 (written);
    }

    // A checksum of every byte written (64-bit FNV-1a), to read a joined pack back against: JUCE's SHA-256 takes its bytes
    // only at once, and a pack is too big to hold whole.
    struct Checksum
    {
        std::uint64_t value = 14695981039346656037ULL;

        void add (const void* data, size_t numBytes)
        {
            for (const auto* byte = static_cast<const unsigned char*> (data); numBytes-- > 0; ++byte)
                value = (value ^ *byte) * 1099511628211ULL;
        }
    };

    class ChecksummedStream final : public juce::OutputStream
    {
    public:
        explicit ChecksummedStream (juce::OutputStream& destination)
            : out (destination)
        {
        }

        bool write (const void* data, size_t numBytes) override
        {
            sum.add (data, numBytes);
            return numBytes == 0 || out.write (data, numBytes);
        }

        void flush() override { out.flush(); }
        bool setPosition (juce::int64) override { return false; }
        juce::int64 getPosition() override { return out.getPosition(); }

        Checksum sum;

    private:
        juce::OutputStream& out;
    };

    // The checksum of a file's bytes, read in parts; none if it can't be read whole.
    std::optional<std::uint64_t> checksumOf (const juce::File& file)
    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return std::nullopt;

        Checksum sum;
        juce::HeapBlock<char> part (1 << 20);

        for (;;)
        {
            const auto read = in.read (part.get(), 1 << 20);

            if (read < 0 || in.getStatus().failed())
                return std::nullopt;

            if (read == 0)
                break;

            sum.add (part.get(), (size_t) read);
        }

        return sum.value;
    }
} // namespace

std::atomic<bool> PieceStore::holdJoinsForTests { false };
std::atomic<juce::int64> PieceStore::smallPackForTests { 0 };
std::atomic<int> PieceStore::joinsHeldForTests { 0 };
std::function<void (const juce::File&)> PieceStore::joinWrittenForTests;

juce::int64 getVersionsTime()
{
    const auto set = timeForTests.load();
    return set != 0 ? set : juce::Time::currentTimeMillis();
}

void setVersionsTimeForTests (juce::int64 now)
{
    timeForTests = now;
}

PieceStore::PieceStore (juce::File packFolder, const juce::String& writerID)
    : folder (std::move (packFolder)),
      writer (makeWriter (writerID))
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.pack");
    files.sort();

    for (const auto& file : files)
        readIndex (file);

    countAgain();
}

PieceStore::~PieceStore()
{
    closeFiles();
}

int PieceStore::findPack (const juce::File& file) const
{
    for (size_t i = 0; i < packs.size(); ++i)
        if (packs[i].file == file)
            return (int) i;

    return -1;
}

void PieceStore::countAgain() const
{
    juce::int64 size = 0;

    for (const auto& pack : packs)
        size += pack.size;

    numPieces = (int) index.size();
    numPacks = (int) packs.size();
    totalSize = size;
}

void PieceStore::readIndex (const juce::File& file)
{
    // A pack that isn't whole, or isn't one, is left out: a write cut off before its end, which never got its name.
    juce::FileInputStream in (file);

    if (! in.openedOk() || in.getTotalLength() < markSize + endSize)
        return; // another program holds it, with no sharing: read the next time

    const auto length = in.getTotalLength();
    char start[markSize] = {}, end[markSize] = {}, wroteIt[writerSize] = {};
    in.read (start, markSize);
    in.setPosition (length - endSize);
    const auto count = (juce::uint32) in.readInt();
    const auto indexStart = in.readInt64();
    in.read (wroteIt, writerSize);
    in.read (end, markSize);

    if (! hasMark (start) || ! hasMark (end) || indexStart < markSize || indexStart + (juce::int64) count * entrySize != length - endSize)
        return;

    juce::MemoryBlock entries;
    const auto indexSize = (size_t) count * (size_t) entrySize;
    in.setPosition (indexStart);

    if (in.readIntoMemoryBlock (entries, (juce::pointer_sized_int) indexSize) != indexSize)
        return;

    // A pack is named by the SHA-256 of its index and its writer: one whose name doesn't match is damaged (or is a
    // temporary file a write left), and none of its entries can be trusted.
    if (makePackName (entries.getData(), entries.getSize(), wroteIt) != file.getFileNameWithoutExtension())
        return;

    // One read before again (written anew in place of its damaged copy, which had the same index) keeps its number. Its
    // writer is this computer only where the trailer says so: a pack whose writer can't be told is nobody's, never joined.
    const auto known = findPack (file);
    const auto packNumber = known >= 0 ? known : (int) packs.size();

    if (known < 0)
    {
        packs.push_back ({ file, std::memcmp (wroteIt, writer.getData(), (size_t) writerSize) == 0, length });
    }

    for (juce::uint32 i = 0; i < count; ++i)
    {
        const auto* entry = static_cast<const char*> (entries.getData()) + (size_t) i * entrySize;
        Location location;
        location.pack = packNumber;
        location.start = (juce::int64) juce::ByteOrder::littleEndianInt64 (entry + nameSize);
        location.compressedSize = juce::ByteOrder::littleEndianInt (entry + nameSize + 8);
        location.size = juce::ByteOrder::littleEndianInt (entry + nameSize + 12);
        location.written = (juce::int64) juce::ByteOrder::littleEndianInt64 (entry + nameSize + 16);

        // Within the pack's pieces, counted so that a start no file has can't wrap round; and not empty, which nothing
        // compressed is. Every pack's copy of it, the first read first.
        if (location.start >= markSize && location.start <= indexStart && location.compressedSize > 0
            && (juce::int64) location.compressedSize <= indexStart - location.start)
        {
            auto& copies = index[juce::String::toHexString (entry, nameSize, 0)];

            if (std::none_of (copies.begin(), copies.end(), [packNumber] (const Location& copy) { return copy.pack == packNumber; }))
                copies.push_back (location);
        }
    }
}

void PieceStore::readAgain (const std::vector<juce::File>& files)
{
    openPacks.clear();
    packs.clear();
    index.clear();

    for (const auto& file : files)
        readIndex (file);

    countAgain();
}

void PieceStore::removeLeftovers()
{
    // What a join a crash cut off left: a day old, it is no join's under way. What is left of the younger is counted in
    // the store's room until it goes.
    juce::int64 left = 0;

    for (const auto& stale : folder.findChildFiles (juce::File::findFiles, false, "joining-*"))
        if (! (juce::Time::getCurrentTime() - stale.getLastModificationTime() > juce::RelativeTime::days (1.0) && stale.deleteFile()))
            left += stale.getSize();

    leftoverSize = left;
}

bool PieceStore::hasOwnCopy (const juce::String& name) const
{
    const juce::ScopedLock sl (lock);
    const auto found = index.find (name);
    return found != index.end()
           && std::any_of (found->second.begin(), found->second.end(),
                           [this] (const Location& copy) { return packs[(size_t) copy.pack].own; });
}

juce::int64 PieceStore::stamp() const
{
    // The latest of the clock, the floor and the last stamp and a millisecond, given once.
    auto last = lastStamp.load();

    for (;;)
    {
        const auto after = last < std::numeric_limits<juce::int64>::max() ? last + 1 : last; // never overflowing
        const auto next = std::max ({ getVersionsTime(), stampFloor.load(), after });

        if (lastStamp.compare_exchange_weak (last, next))
            return next;
    }
}

void PieceStore::raiseStampFloor (juce::int64 time)
{
    auto current = stampFloor.load();

    while (time > current && ! stampFloor.compare_exchange_weak (current, time))
    {
    }
}

juce::int64 PieceStore::findNewestOwnWrite() const
{
    const juce::ScopedLock sl (lock);
    juce::int64 newest = 0;

    for (const auto& entry : index)
        for (const auto& copy : entry.second)
            if (packs[(size_t) copy.pack].own)
                newest = std::max (newest, copy.written);

    return newest;
}

bool PieceStore::has (const juce::String& name) const
{
    const juce::ScopedLock sl (lock);
    return index.find (name) != index.end();
}

PieceStore::Copy PieceStore::readCopy (const Location& location, const juce::String& name, juce::MemoryBlock& piece) const
{
    auto& stream = openPacks[location.pack];

    if (stream == nullptr)
    {
        stream = std::make_unique<juce::FileInputStream> (packs[(size_t) location.pack].file);

        if (! stream->openedOk())
        {
            stream = nullptr;
            return Copy::unread;
        }
    }

    juce::MemoryBlock compressed (location.compressedSize);

    if (! stream->setPosition (location.start)
        || stream->read (compressed.getData(), (int) location.compressedSize) != (int) location.compressedSize)
    {
        // A pack that couldn't be read is opened again for the next piece: its stream reads nothing more. One read whole
        // that ends before the piece does was cut short: its copy is damaged.
        if (stream->getStatus().failed())
        {
            stream = nullptr;
            return Copy::unread;
        }

        return Copy::damaged;
    }

    // What it holds, if it is what its name says.
    auto read = decompress (compressed, location.size);

    if (! read.has_value() || getPieceName (*read) != name)
        return Copy::damaged;

    piece = std::move (*read);
    return Copy::whole;
}

std::optional<juce::MemoryBlock> PieceStore::read (const juce::String& name) const
{
    const juce::ScopedLock sl (lock);
    const auto found = index.find (name);

    if (found == index.end())
        return std::nullopt;

    // Each pack's copy in turn; one that isn't what its name says forgotten, so that a write holding the piece writes it
    // again once no copy is left.
    auto& copies = found->second;
    juce::MemoryBlock piece;

    for (size_t i = 0; i < copies.size();)
    {
        const auto copy = readCopy (copies[i], name, piece);

        if (copy == Copy::whole)
            return piece;

        if (copy == Copy::damaged)
            copies.erase (copies.begin() + (std::ptrdiff_t) i);
        else
            ++i;
    }

    if (copies.empty())
    {
        index.erase (found);
        countAgain();
    }

    return std::nullopt;
}

std::optional<juce::MemoryBlock> PieceStore::readStart (const juce::String& name, int numBytes) const
{
    const juce::ScopedLock sl (lock);
    const auto found = index.find (name);

    if (found == index.end() || found->second.empty() || numBytes <= 0)
        return std::nullopt;

    // Enough of its compressed bytes for the start of it: zlib's header and the first block's tables come first.
    const auto& location = found->second.front();
    const auto wanted = (int) juce::jmin ((juce::uint32) numBytes, location.size);
    const auto compressedWanted = (int) juce::jmin (location.compressedSize, (juce::uint32) (wanted + 4096));
    auto& stream = openPacks[location.pack];

    if (stream == nullptr)
    {
        stream = std::make_unique<juce::FileInputStream> (packs[(size_t) location.pack].file);

        if (! stream->openedOk())
        {
            stream = nullptr;
            return std::nullopt;
        }
    }

    juce::MemoryBlock compressed ((size_t) compressedWanted);

    if (! stream->setPosition (location.start) || stream->read (compressed.getData(), compressedWanted) != compressedWanted)
    {
        if (stream->getStatus().failed())
            stream = nullptr;

        return std::nullopt;
    }

    juce::MemoryInputStream in (compressed, false);
    juce::GZIPDecompressorInputStream zlib (&in, false, juce::GZIPDecompressorInputStream::zlibFormat);
    juce::MemoryBlock start ((size_t) wanted);
    const auto read = zlib.read (start.getData(), wanted);

    if (read <= 0)
        return std::nullopt;

    start.setSize ((size_t) read);
    return start;
}

bool PieceStore::isOthersPack (const juce::File& file) const
{
    // Its writer as its trailer says: of one read already, or read from the file. One that can't be read (held, or cut
    // short) says nothing, and is replaced or not as a damaged one of this computer's is.
    {
        const juce::ScopedLock sl (lock);

        if (const auto known = findPack (file); known >= 0)
            return ! packs[(size_t) known].own;
    }

    juce::FileInputStream in (file);

    if (! in.openedOk() || in.getTotalLength() < markSize + endSize)
        return false;

    char wroteIt[writerSize] = {}, end[markSize] = {};
    in.setPosition (in.getTotalLength() - writerSize - markSize);
    in.read (wroteIt, writerSize);
    in.read (end, markSize);
    return hasMark (end) && std::memcmp (wroteIt, writer.getData(), (size_t) writerSize) != 0;
}

bool PieceStore::write (const std::map<juce::String, juce::MemoryBlock>& pieces, juce::String* problem)
{
    // A piece counts as held only where this computer's own packs hold it: one only another computer's pack holds is
    // written again, so that this computer's copy stays whatever that computer's sweep does with its own (joinPacks()).
    std::vector<std::pair<juce::String, const juce::MemoryBlock*>> fresh;
    const auto now = stamp();

    {
        const juce::ScopedLock sl (lock);

        for (const auto& [name, piece] : pieces)
        {
            const auto found = index.find (name);

            if (found == index.end()
                || std::none_of (found->second.begin(), found->second.end(),
                                 [this] (const Location& copy) { return packs[(size_t) copy.pack].own; }))
                fresh.emplace_back (name, &piece);
        }
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
        writeEntry (entries, name, start, (juce::uint32) compressed.getSize(), (juce::uint32) piece->getSize(), now);
    }

    const auto indexStart = (juce::int64) pack.getDataSize();
    pack.write (entries.getData(), entries.getDataSize());
    pack.writeInt ((int) fresh.size());
    pack.writeInt64 (indexStart);
    pack.write (writer.getData(), writer.getSize());
    pack.write (mark, markSize);

    const auto fail = [problem] (const juce::String& why)
    {
        if (problem != nullptr)
            *problem = why;

        return false;
    };

    // Named by its index and its writer: another computer's pack of the same pieces, written the same millisecond, has
    // another name, so that no file has two writers.
    const auto file = folder.getChildFile (makePackName (entries.getData(), entries.getDataSize(), writer.getData()) + ".pack");

    if (const auto made = folder.createDirectory(); made.failed())
        return fail ("Phazite couldn't make the Versions folder's pieces folder: " + made.getErrorMessage());

    // Written whole beside it, flushed to the disk, then given its name (model/base/WholeFile.h): a pack is never seen half
    // written. Written only if every piece is found in it as it is read back.
    const auto holdsThem = [this, &fresh, &file] // with the lock held: each piece has a copy in the pack
    {
        const auto number = findPack (file);

        for (const auto& [name, piece] : fresh)
            if (const auto found = index.find (name); found == index.end()
                                                      || std::none_of (found->second.begin(), found->second.end(),
                                                                       [number] (const Location& copy) { return copy.pack == number; }))
                return false;

        return true;
    };

    {
        // One of its name that is there already (another store's) is read, and is enough if it holds them whole; one that
        // doesn't read, cut short or damaged (a copy of it this store found damaged, which had the same index), is written
        // again, let go of first so that it can be replaced.
        const juce::ScopedLock sl (lock);

        if (file.existsAsFile() && findPack (file) < 0)
        {
            readIndex (file);
            countAgain();

            if (holdsThem()
                && std::all_of (fresh.begin(), fresh.end(), [this] (const auto& piece) { return read (piece.first).has_value(); }))
                return true;
        }

        if (const auto known = findPack (file); known >= 0)
            openPacks.erase (known);
    }

    // One of its name there already, damaged, is replaced; another program that holds it (a sync program, a virus scanner)
    // keeps that from happening, which is said.
    if (const auto written = writeWholeFile (file, pack.getData(), pack.getDataSize()); written.failed())
        return fail ("Phazite couldn't write the pack " + file.getFileName() + " into the Versions folder: " + written.getErrorMessage()
                     + (file.existsAsFile() ? " Another program (a sync program or a virus scanner) may hold the damaged pack of that "
                                              "name, which it replaces: try again once it lets go of it."
                                            : juce::String()));

    const juce::ScopedLock sl (lock);
    readIndex (file);
    countAgain();
    return holdsThem() || fail ("Phazite wrote the pack " + file.getFileName() + " but couldn't read it back whole.");
}

juce::StringArray PieceStore::getNames() const
{
    const juce::ScopedLock sl (lock);
    juce::StringArray names;
    names.ensureStorageAllocated ((int) index.size());

    for (const auto& [name, location] : index)
        names.add (name);

    return names;
}

int PieceStore::getNumOwnPacks() const
{
    const juce::ScopedLock sl (lock);
    return (int) std::count_if (packs.begin(), packs.end(), [] (const Pack& pack) { return pack.own; });
}

juce::int64 PieceStore::getOwnSize() const
{
    const juce::ScopedLock sl (lock);
    juce::int64 size = 0;

    for (const auto& pack : packs)
        if (pack.own)
            size += pack.size;

    return size;
}

std::map<juce::String, juce::int64> PieceStore::findOwnPiecesWrittenBefore (juce::int64 time) const
{
    const juce::ScopedLock sl (lock);
    std::map<juce::String, juce::int64> old;

    for (const auto& [name, copies] : index)
    {
        auto newest = std::numeric_limits<juce::int64>::min();
        juce::int64 bytes = 0;

        for (const auto& copy : copies)
        {
            if (packs[(size_t) copy.pack].own)
            {
                newest = std::max (newest, copy.written);
                bytes += copy.compressedSize;
            }
        }

        if (bytes > 0 && newest < time)
            old.emplace (name, bytes);
    }

    return old;
}

juce::StringArray PieceStore::findPiecesWrittenSince (juce::int64 time) const
{
    const juce::ScopedLock sl (lock);
    juce::StringArray young;

    for (const auto& [name, copies] : index)
        if (std::any_of (copies.begin(), copies.end(), [time] (const Location& copy) { return copy.written >= time; }))
            young.add (name);

    return young;
}

void PieceStore::readNewPacks()
{
    const juce::ScopedLock sl (lock);
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.pack");
    files.sort();

    for (const auto& file : files)
        if (findPack (file) < 0)
            readIndex (file);

    countAgain();
}

juce::Array<juce::File> PieceStore::findUnreadPacks() const
{
    const juce::ScopedLock sl (lock);
    juce::Array<juce::File> unread;

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false, "*.pack"))
        if (findPack (file) < 0)
            unread.add (file);

    return unread;
}

std::set<int> PieceStore::chooseRewritten (const std::vector<Pack>& before, const std::map<juce::String, std::vector<Location>>& copies,
                                           bool everyOwnPack, std::set<int>& duplicated)
{
    // Every own pack, or the small ones, those whose every piece another own pack holds too (what a join a crash cut off
    // before its deletes left twice), and those of a size class (from smallPack up, each four times the one before) that
    // holds gatherFrom or more: joined geometrically, as a log-structured store merges, so that the count of packs stays
    // near gatherFrom for each class, a few classes for any size, and a sync program uploads again only what is joined.
    std::set<int> rewritten;
    std::map<int, std::vector<int>> classes;
    const auto small = smallPackForTests.load() > 0 ? smallPackForTests.load() : smallPack;

    for (size_t i = 0; i < before.size(); ++i)
    {
        if (! before[i].own)
            continue;

        if (everyOwnPack || before[i].size < small)
        {
            rewritten.insert ((int) i);
            continue;
        }

        auto sizeClass = 0;

        for (auto bound = small * 4; before[i].size >= bound && sizeClass < 30; bound *= 4)
            ++sizeClass;

        classes[sizeClass].push_back ((int) i);
    }

    for (const auto& [sizeClass, members] : classes)
        if ((int) members.size() >= gatherFrom)
            rewritten.insert (members.begin(), members.end());

    std::map<int, bool> allElsewhere; // by own pack: every piece of it has a copy in another own pack

    for (const auto& [name, its] : copies)
    {
        auto ownCopies = 0;

        for (const auto& copy : its)
            ownCopies += before[(size_t) copy.pack].own ? 1 : 0;

        for (const auto& copy : its)
            if (before[(size_t) copy.pack].own)
                allElsewhere.try_emplace (copy.pack, true).first->second &= ownCopies > 1;
    }

    for (const auto& [pack, twice] : allElsewhere)
    {
        if (twice)
        {
            rewritten.insert (pack);
            duplicated.insert (pack);
        }
    }

    // Small ones alone, one of them, are nothing to gather.
    if (rewritten.size() < 2 && duplicated.empty())
        rewritten.clear();

    return rewritten;
}

bool PieceStore::needsGathering() const
{
    std::vector<Pack> now;
    std::map<juce::String, std::vector<Location>> copies;

    {
        const juce::ScopedLock sl (lock);
        now = packs;
        copies = index;
    }

    std::set<int> duplicated;
    return ! chooseRewritten (now, copies, false, duplicated).empty();
}

bool PieceStore::joinPacks (const std::function<bool()>& shouldStop, const Pruning& pruning, juce::StringArray* leftOut)
{
    jassert (! isInHeldFolder (folder)); // a first save copies the Versions folder (WholeFile.h: holdFolder())
    const auto stopping = [&shouldStop] { return shouldStop != nullptr && shouldStop(); };

    // What there is now, copied under the lock; the rest of the join works on the copy, the lock free, so that reading the
    // store waits for nothing but the swap at the end.
    std::vector<Pack> before;
    std::map<juce::String, std::vector<Location>> copies;

    {
        const juce::ScopedLock sl (lock);
        before = packs;
        copies = index;
    }

    // What goes: a piece of this computer's own packs, all its own copies written before the cutoff, that isn't kept.
    std::set<juce::String> dropped;

    if (pruning.keep != nullptr)
    {
        for (const auto& [name, its] : copies)
        {
            auto newest = std::numeric_limits<juce::int64>::min();
            auto own = false;

            for (const auto& copy : its)
            {
                if (before[(size_t) copy.pack].own)
                {
                    own = true;
                    newest = std::max (newest, copy.written);
                }
            }

            if (own && newest < pruning.cutoff && ! pruning.keep (name))
                dropped.insert (name);
        }
    }

    // Which of its own packs are rewritten (chooseRewritten()).
    std::set<int> duplicated;
    auto rewritten = chooseRewritten (before, copies, pruning.everyOwnPack, duplicated);

    for (const auto& name : dropped)
        for (const auto& copy : copies[name])
            if (before[(size_t) copy.pack].own)
                rewritten.insert (copy.pack);

    if (dropped.empty() && rewritten.size() < 2 && duplicated.empty())
        return true;

    // Into the new pack: every piece one of them holds that is kept, a copy of it read and checked, with the time of its
    // newest own copy; never left to a copy in another pack that isn't read here (it may be damaged where theirs is whole).
    std::vector<std::pair<juce::String, juce::int64>> joining;

    for (const auto& [name, its] : copies)
    {
        if (dropped.count (name) > 0)
            continue;

        auto inRewritten = false;
        auto newest = std::numeric_limits<juce::int64>::min();

        for (const auto& copy : its)
        {
            if (! before[(size_t) copy.pack].own)
                continue;

            newest = std::max (newest, copy.written);
            inRewritten = inRewritten || rewritten.count (copy.pack) > 0;
        }

        if (inRewritten)
            joining.emplace_back (name, newest);
    }

    // Each piece's compressed bytes, from the first copy that checks out (another computer's pack's too), copied as they are
    // into a pack written whole beside the others (WholeFile.h: a write that fails leaves nothing), under a name of its own
    // until its index names it; the bytes checksummed as they go. None where nothing is joined (every piece of the packs
    // rewritten left out, or held in another own pack): those packs just go.
    std::set<juce::String> unreadable; // kept, but no copy of it reads whole: what holds it stays
    juce::File joined;
    auto wasThere = false;

    if (! joining.empty() && ! writeJoined (joining, copies, before, stopping, unreadable, joined, wasThere))
        return false;

    // What goes: each pack rewritten, every piece of it left out or in the new pack, read back whole; one holding a kept
    // piece no copy of which reads whole stays; and never a pack another computer wrote, whatever its number says (its
    // trailer read again).
    std::vector<juce::File> going;

    for (const auto number : rewritten)
    {
        const auto keepsSomething =
            std::any_of (unreadable.begin(), unreadable.end(),
                         [&copies, number] (const juce::String& name)
                         {
                             const auto& its = copies[name];
                             return std::any_of (its.begin(), its.end(), [number] (const Location& copy) { return copy.pack == number; });
                         });

        if (! keepsSomething && before[(size_t) number].file != joined && ! isOthersPack (before[(size_t) number].file))
            going.push_back (before[(size_t) number].file);
    }

    if (stopping())
    {
        if (joined != juce::File() && ! wasThere)
            joined.deleteFile();

        return false; // nothing deleted yet: the packs as they were
    }

    // The swap, under the lock: the store read anew of the new pack and every pack but those going (those that arrived
    // meanwhile too); then those deleted, the lock free, one that can't be (another program holds it) read again.
    {
        const juce::ScopedLock sl (lock);
        std::vector<juce::File> staying;

        if (joined != juce::File())
            staying.push_back (joined);

        for (const auto& pack : packs)
            if (pack.file != joined && std::find (going.begin(), going.end(), pack.file) == going.end())
                staying.push_back (pack.file);

        readAgain (staying);

        if (joined != juce::File() && findPack (joined) < 0)
        {
            // It didn't read as a pack after all: nothing goes.
            for (const auto& pack : going)
                staying.push_back (pack);

            staying.erase (staying.begin());
            readAgain (staying);

            if (! wasThere)
                joined.deleteFile();

            return false;
        }
    }

    for (const auto& pack : going)
    {
        if (! pack.deleteFile())
        {
            const juce::ScopedLock sl (lock);
            readIndex (pack);
            countAgain();
        }
    }

    if (leftOut != nullptr)
        for (const auto& name : dropped)
            if (! has (name))
                leftOut->add (name);

    return true;
}

bool PieceStore::writeJoined (const std::vector<std::pair<juce::String, juce::int64>>& joining,
                              std::map<juce::String, std::vector<Location>>& copies, const std::vector<Pack>& before,
                              const std::function<bool()>& stopping, std::set<juce::String>& unreadable, juce::File& joined, bool& wasThere)
{
    const auto newPack = folder.getChildFile ("joining-" + juce::Uuid().toString() + ".tmp");
    std::map<int, std::unique_ptr<juce::FileInputStream>> streams;
    juce::MemoryOutputStream entries;
    std::uint64_t checksum = 0;
    int count = 0;

    const auto written = writeWholeFile (
        newPack,
        [&] (juce::OutputStream& file)
        {
            ChecksummedStream out (file);

            if (! out.write (mark, markSize))
                return false;

            // Tests' seam: held here, copying, the lock free, until let go or told to stop.
            if (holdJoinsForTests.load())
            {
                ++joinsHeldForTests;

                while (holdJoinsForTests.load() && ! stopping())
                    juce::Thread::sleep (1);

                --joinsHeldForTests;
            }

            for (const auto& [name, time] : joining)
            {
                if (stopping())
                    return false; // the packs as they were

                auto whole = false;

                for (const auto& location : copies[name])
                {
                    auto& stream = streams[location.pack];

                    if (stream == nullptr)
                        if (auto opened = std::make_unique<juce::FileInputStream> (before[(size_t) location.pack].file); opened->openedOk())
                            stream = std::move (opened);

                    juce::MemoryBlock compressed (location.compressedSize);

                    if (stream == nullptr || ! stream->setPosition (location.start)
                        || stream->read (compressed.getData(), (int) location.compressedSize) != (int) location.compressedSize)
                    {
                        stream = nullptr;
                        continue;
                    }

                    if (const auto piece = decompress (compressed, location.size); ! piece.has_value() || getPieceName (*piece) != name)
                        continue;

                    writeEntry (entries, name, out.getPosition(), location.compressedSize, location.size, time);

                    if (! out.write (compressed.getData(), compressed.getSize()))
                        return false;

                    ++count;
                    whole = true;
                    break;
                }

                if (! whole)
                    unreadable.insert (name);
            }

            const auto indexStart = out.getPosition();
            const auto done = (entries.getDataSize() == 0 || out.write (entries.getData(), entries.getDataSize())) && out.writeInt (count)
                              && out.writeInt64 (indexStart) && out.write (writer.getData(), writer.getSize())
                              && out.write (mark, markSize);
            checksum = out.sum.value;
            return done;
        });

    streams.clear();

    if (written.failed())
        return false; // nothing written, nothing changed

    // Named by its index and its writer, as any pack is; then read back, every byte against what was written, before
    // anything goes.
    joined = folder.getChildFile (makePackName (entries.getData(), entries.getDataSize(), writer.getData()) + ".pack");
    wasThere = joined.existsAsFile();

    if (wasThere)
    {
        newPack.deleteFile();
    }
    else if (! newPack.moveFileTo (joined))
    {
        newPack.deleteFile();
        return false;
    }

    if (joinWrittenForTests != nullptr)
        joinWrittenForTests (joined);

    // Stopped here, nothing deleted yet: the new pack goes, the packs as they were.
    if (stopping() || checksumOf (joined) != std::optional (checksum) || stopping())
    {
        if (! wasThere)
            joined.deleteFile();

        return false;
    }

    return true;
}

void PieceStore::closeFiles() const
{
    const juce::ScopedLock sl (lock);
    openPacks.clear();
}
} // namespace phazite
