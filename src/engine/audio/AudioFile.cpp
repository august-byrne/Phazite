// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioFile.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
namespace
{
    constexpr double aheadSeconds = 10.0; // of a longer file, read ahead of where it plays
    constexpr int behindChunks = 2;       // and kept behind it
} // namespace

AudioFile::AudioFile (std::unique_ptr<SampleReader> r, const std::atomic<std::uint64_t>* engineBlocks)
    : reader (std::move (r)),
      blocks (engineBlocks)
{
    numChannels = std::max (1, reader->getNumChannels());
    sampleRate = reader->getSampleRate() > 0.0 ? reader->getSampleRate() : 44100.0;
    length = std::max<std::int64_t> (0, reader->getLength());
    numChunks = (int) ((length + chunkSamples - 1) / chunkSamples);
    whole = blocks == nullptr || (double) length <= shortSeconds * sampleRate;
    chunks = std::make_unique<std::atomic<const Chunk*>[]> ((size_t) std::max (1, numChunks));

    for (int i = 0; i < numChunks; ++i)
        chunks[(size_t) i].store (nullptr, std::memory_order_relaxed);

    read.resize ((size_t) numChunks);
}

AudioFile::~AudioFile() = default;

const float* AudioFile::getSamples (int channel, std::int64_t start, int& numSamples) noexcept
{
    if (start < 0 || start >= length)
    {
        numSamples = 0;
        return nullptr;
    }

    const auto index = (int) (start / chunkSamples);
    const auto offset = (int) (start - (std::int64_t) index * chunkSamples);
    playing.store (index, std::memory_order_relaxed);

    if (blocks != nullptr)
        playedAt.store (blocks->load (std::memory_order_relaxed), std::memory_order_relaxed);

    const auto* chunk = chunks[(size_t) index].load (std::memory_order_acquire);
    const auto inChunk = (int) std::min<std::int64_t> (chunkSamples - offset, length - start);
    numSamples = std::min (numSamples, inChunk);

    if (chunk == nullptr)
        return nullptr;

    return chunk->samples.getReadPointer (std::min (channel, numChannels - 1), offset);
}

void AudioFile::keep (std::int64_t sample)
{
    const std::lock_guard lock (mutex);

    if (sample >= 0 && sample < length)
        kept.insert ((int) (sample / chunkSamples));
}

bool AudioFile::readChunk (int index)
{
    auto chunk = std::make_unique<Chunk>();
    const auto start = (std::int64_t) index * chunkSamples;
    const auto size = (int) std::min<std::int64_t> (chunkSamples, length - start);
    chunk->samples.setSize (numChannels, size);

    // A part that can't be read plays as silence rather than stopping what is around it.
    if (! reader->read (chunk->samples, start, size))
        chunk->samples.clear();

    chunks[(size_t) index].store (chunk.get(), std::memory_order_release);
    read[(size_t) index] = std::move (chunk);
    return true;
}

bool AudioFile::readNext()
{
    const std::lock_guard lock (mutex);

    // What was let go of goes, once the engine has finished a block since.
    if (blocks != nullptr)
    {
        const auto now = blocks->load (std::memory_order_acquire);
        std::erase_if (letGo, [now] (const auto& chunk) { return now >= chunk.second; });
    }

    // A short file is read whole, from its start.
    if (whole)
    {
        for (int i = 0; i < numChunks; ++i)
            if (read[(size_t) i] == nullptr)
                return readChunk (i);

        return false;
    }

    // A longer one: where each clip that plays it starts, then ahead of where it plays.
    for (const auto index : kept)
        if (index < numChunks && read[(size_t) index] == nullptr)
            return readChunk (index);

    const auto at = playing.load (std::memory_order_relaxed);
    const auto ahead = (int) std::ceil (aheadSeconds * sampleRate / chunkSamples);

    for (int i = at; i < std::min (numChunks, at + ahead); ++i)
        if (read[(size_t) i] == nullptr)
            return readChunk (i);

    // What it played long ago, or is far ahead of, it lets go of; freed only once the audio thread can't be reading it.
    for (int i = 0; i < numChunks; ++i)
    {
        if (read[(size_t) i] != nullptr && kept.count (i) == 0 && (i < at - behindChunks || i >= at + ahead + 1))
        {
            chunks[(size_t) i].store (nullptr, std::memory_order_release);
            letGo.emplace_back (std::move (read[(size_t) i]), blocks->load (std::memory_order_acquire) + 2);
        }
    }

    return false;
}

void AudioFile::readNow (std::int64_t start, std::int64_t numSamples)
{
    const std::lock_guard lock (mutex);
    const auto first = (int) std::clamp<std::int64_t> (start / chunkSamples, 0, numChunks);
    const auto last = (int) std::clamp<std::int64_t> ((start + numSamples + chunkSamples - 1) / chunkSamples, 0, numChunks);

    for (int i = first; i < last; ++i)
        if (read[(size_t) i] == nullptr)
            readChunk (i);
}
} // namespace phazite::engine
