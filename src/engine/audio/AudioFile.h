// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio file as Phazite's engine plays it: its samples read in chunks by a reading thread (FileReader.h), each
// handed to the audio thread through an atomic pointer once read, so that the audio thread never waits for the disk; a
// chunk not read yet plays as silence, and a render waits for it. A short file is read whole and kept; a longer one is
// read ahead of everywhere it plays (two clips can play it at once, in two places), with the start of each clip that plays
// it kept, and what it played long ago let go of. What reads the file (its format) is the model's, behind SampleReader.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace phazite::engine
{
// Reads a file's samples, on the reading thread or a render's (never the audio thread).
class SampleReader
{
public:
    virtual ~SampleReader() = default;
    virtual int getNumChannels() const = 0;
    virtual double getSampleRate() const = 0;
    virtual std::int64_t getLength() const = 0;

    // Reads samples as floats, from a sample of the file, into a buffer of the file's channels; false if it can't.
    virtual bool read (juce::AudioBuffer<float>&, std::int64_t start, int numSamples) = 0;
};

class AudioFile
{
public:
    static constexpr int chunkSamples = 1 << 15; // about 0.7 s at 48 kHz
    static constexpr double shortSeconds = 30.0; // a file no longer is read whole and kept

    // Plays the file a reader reads. Blocks counts the blocks of the engine that plays it, after which a chunk let go of is
    // freed (nullptr for a render's, which lets go of nothing).
    AudioFile (std::unique_ptr<SampleReader>, const std::atomic<std::uint64_t>* blocks);
    ~AudioFile();

    int getNumChannels() const noexcept { return numChannels; }
    double getSampleRate() const noexcept { return sampleRate; }
    std::int64_t getLength() const noexcept { return length; }

    // Audio thread: the samples of a channel from a sample on, as far as their chunk goes (at most numSamples, which it
    // sets to how many there are), or nullptr if the chunk isn't read yet. Says where it plays, for what is read ahead.
    const float* getSamples (int channel, std::int64_t start, int& numSamples) noexcept;

    // Message thread: a sample to keep read whatever plays, as a clip's start in the file is.
    void keep (std::int64_t sample);

    // The reading thread: reads the next chunk wanted, and lets go of those long played; whether there was one to read.
    bool readNext();

    // A render's thread: reads every chunk of a range now, if not read yet.
    void readNow (std::int64_t start, std::int64_t numSamples);

private:
    struct Chunk
    {
        juce::AudioBuffer<float> samples;
    };

    std::unique_ptr<SampleReader> reader;
    const std::atomic<std::uint64_t>* blocks;
    int numChannels = 0;
    double sampleRate = 44100.0;
    std::int64_t length = 0;
    int numChunks = 0;
    bool whole = true;

    std::unique_ptr<std::atomic<const Chunk*>[]> chunks; // what the audio thread reads
    std::vector<std::unique_ptr<Chunk>> read;            // each chunk read, owned here

    // When the audio thread read each chunk last: the engine's block count then, plus one (0 for never). Those it read in
    // its last few blocks are where the file plays now, one place for each clip that plays it.
    std::unique_ptr<std::atomic<std::uint64_t>[]> playedIn;

    std::mutex mutex; // between the reading thread, a render's and the message thread; never the audio thread
    std::set<int> kept;
    std::vector<std::pair<std::unique_ptr<Chunk>, std::uint64_t>> letGo; // each with the block count it can go after
    std::vector<int> playingAt;                                          // the reading thread's: where it plays now

    bool readChunk (int index); // under the mutex
    void findWherePlaying();    // into playingAt, under the mutex
};
} // namespace phazite::engine
