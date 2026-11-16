// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio file as Phazite's engine plays it: its samples read in chunks by a reading thread (FileReader.h), each
// handed to the audio thread through an atomic pointer once read, so that the audio thread never waits for the disk; a
// chunk not read yet plays as silence, and a render waits for it. A short file is read whole and kept; a longer one is
// read ahead of everywhere it plays (two clips can play it at once, in two places) and of where the engine says it will
// play next (where playback will start, a loop's start), with the start of each clip that plays it kept, and what it
// played long ago let go of. What reads the file (its format) is the model's, behind SampleReader.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
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

    // A number no other file made in this run has had, nor will: what it is, where its address could be another's later
    // (a clip's render made again, at the same address as the one let go of).
    std::uint64_t getSerial() const noexcept { return serial; }

    // What it was opened as, as whoever opened it names it (the song's player: the file's path and what was known of it
    // then), for the tests' comparisons of plans (tests/PlanCheck.h). Message thread, before a plan plays it.
    void setOrigin (juce::String text) { origin = std::move (text); }
    const juce::String& getOrigin() const noexcept { return origin; }

    // Audio thread: the samples of a channel from a sample on, as far as their chunk goes (at most numSamples, which it
    // sets to how many there are), or nullptr if the chunk isn't read yet. Says where it plays, for what is read ahead.
    const float* getSamples (int channel, std::int64_t start, int& numSamples) noexcept;

    // Any thread: whether the chunk a sample is in is read now, without saying that the file plays there.
    bool isRead (std::int64_t sample) const noexcept
    {
        return sample >= 0 && sample < length && chunks[(size_t) (sample / chunkSamples)].load (std::memory_order_acquire) != nullptr;
    }

    // Audio thread: a sample it will play from next though it doesn't play there now (where playback will start, the
    // start of a loop that plays further on): read ahead of as where it plays is, for as long as it is said in each block.
    void expect (std::int64_t sample) noexcept;

    // Message thread: a sample it plays from at once, though nothing has read it there yet (where a jump lands): read
    // before anything else of this file or another (known bug 36), its reading thread woken for it. The audio thread asks
    // for a chunk so itself as it finds it not read, without waking anyone (which it mustn't).
    void want (std::int64_t sample);

    // Message thread: what wakes its reading thread (FileReader.h), which want() signals.
    void setWake (std::shared_ptr<juce::WaitableEvent> event) { wake = std::move (event); }

    // Message thread: a sample to keep read whatever plays, as a clip's start in the file is.
    void keep (std::int64_t sample);

    // The reading thread: reads the chunk wanted first, if it isn't read yet; whether it read it.
    bool readWanted();

    // The reading thread: reads the next chunk wanted (the one asked for first), and lets go of those long played; whether
    // there was one to read.
    bool readNext();

    // A render's thread: reads every chunk of a range now, if not read yet.
    void readNow (std::int64_t start, std::int64_t numSamples);

private:
    juce::String origin; // setOrigin()'s
    struct Chunk
    {
        juce::AudioBuffer<float> samples;
    };

    std::unique_ptr<SampleReader> reader;
    const std::atomic<std::uint64_t>* blocks;
    const std::uint64_t serial;
    static inline std::atomic<std::uint64_t> filesMade { 0 };
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

    // The chunk asked for first (want()), or -1; and what wakes the reading thread for it.
    std::atomic<int> wanted { -1 };
    std::shared_ptr<juce::WaitableEvent> wake;

    // What reads the file and what it has read (read, the chunks, letGo, playingAt): the reading thread's and a render's,
    // one at a time, held across a read from the disk. The message thread never takes it, so that a new plan never waits
    // for the disk (known bug 85: keep() took it, and waited for the read under way, 25 ms at a cold read of 20 ms and as
    // long as 1.2 s while one read followed another). What the message thread keeps (kept) has a lock of its own, held for
    // as long as it takes to add to it or to look at it; never the audio thread either.
    std::mutex mutex;
    std::mutex keptMutex;
    std::set<int> kept;
    std::vector<std::pair<std::unique_ptr<Chunk>, std::uint64_t>> letGo; // each with the block count it can go after
    std::vector<int> playingAt;                                          // the reading thread's: where it plays now

    bool readChunk (int index); // under the mutex
    bool readWantedChunk();     // under the mutex
    void findWherePlaying();    // into playingAt, under the mutex
    bool isKept (int index);    // under the mutex: takes keptMutex
};
} // namespace phazite::engine
