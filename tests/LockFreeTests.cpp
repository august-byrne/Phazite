// SPDX-License-Identifier: AGPL-3.0-or-later
// The engine's lock-free parts raced on real threads: the queue that live notes come through from any thread
// (EventQueue.h), the handover of what the audio thread plays (Handover.h), the playhead's stamps (PlayheadClock.h),
// the transport the message thread asks for (Engine.h), and a device's sample tap made as a display listens (SampleTap.h).
// Each race runs a fixed count of operations, never for a time, and waits only for what the other threads do, so that it
// says the same on one core as on many; ThreadSanitizer (linux-tsan) sees each access the threads share. What a thread
// finds wrong it counts, and the test thread checks the counts once the threads have ended (doctest's checks are the test
// thread's).
#include "TestHelpers.h"
#include "devices/core/SampleTap.h"
#include "engine/core/Engine.h"
#include "engine/core/EventQueue.h"
#include "engine/core/Handover.h"
#include "engine/core/PlayheadClock.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace
{
namespace engine = phazite::engine;

// What a producer pushes: which producer, its how-manyth push, and a sum of both, so that a value torn between two
// pushes shows.
struct Pushed
{
    std::uint32_t producer = 0, index = 0;
    std::uint64_t sum = 0;

    static std::uint64_t sumOf (std::uint32_t producer, std::uint32_t index) noexcept
    {
        return (((std::uint64_t) producer << 32) | index) ^ 0x9e3779b97f4a7c15ull;
    }
};
} // namespace

TEST_CASE ("The event queue holds as many as its size, drops what comes when it is full, and gives them back in order round and round")
{
    engine::EventQueue<Pushed, 8> queue;
    Pushed taken;
    CHECK_FALSE (queue.pop (taken));

    std::uint32_t pushed = 0, popped = 0;

    // Filled, emptied by a few, filled again: each cell taken round many times.
    for (int round = 0; round < 100; ++round)
    {
        while (queue.push ({ 0, pushed, Pushed::sumOf (0, pushed) }))
            ++pushed;

        CHECK (pushed - popped == 8u);

        for (int i = 0; i < 3 + round % 5; ++i)
        {
            REQUIRE (queue.pop (taken));
            CHECK (taken.index == popped);
            CHECK (taken.sum == Pushed::sumOf (0, popped));
            ++popped;
        }
    }

    while (queue.pop (taken))
    {
        CHECK (taken.index == popped);
        ++popped;
    }

    CHECK (popped == pushed);
}

TEST_CASE (
    "The event queue raced by four producers and a taker loses nothing, doubles nothing, tears nothing, and keeps each producer's order")
{
    static constexpr std::uint32_t producers = 4, pushesEach = 50000;
    engine::EventQueue<Pushed, 1024> queue;
    std::atomic<int> started { 0 };
    std::vector<std::thread> threads;

    for (std::uint32_t p = 0; p < producers; ++p)
    {
        threads.emplace_back (
            [&queue, &started, p]
            {
                ++started;

                while (started.load() < (int) producers + 1)
                    std::this_thread::yield();

                // A full queue drops what comes: pushed again until it is taken.
                for (std::uint32_t i = 0; i < pushesEach; ++i)
                    while (! queue.push ({ p, i, Pushed::sumOf (p, i) }))
                        std::this_thread::yield();
            });
    }

    // The taker, as the audio thread takes: each producer's next, and nothing more once all are taken.
    std::array<std::uint32_t, producers> next {};
    int wrongOrder = 0, torn = 0, unknown = 0;
    std::uint32_t takenCount = 0;
    ++started;

    while (takenCount < producers * pushesEach)
    {
        Pushed taken;

        if (! queue.pop (taken))
        {
            std::this_thread::yield();
            continue;
        }

        ++takenCount;

        if (taken.producer >= producers)
        {
            ++unknown;
            continue;
        }

        if (taken.sum != Pushed::sumOf (taken.producer, taken.index))
            ++torn;

        if (taken.index != next[taken.producer])
            ++wrongOrder;

        next[taken.producer] = taken.index + 1;
    }

    for (auto& thread : threads)
        thread.join();

    Pushed extra;
    CHECK_FALSE (queue.pop (extra)); // nothing doubled after the last
    CHECK (unknown == 0);
    CHECK (torn == 0);
    CHECK (wrongOrder == 0); // so none lost or doubled either: each producer's came 0, 1, 2... to the last

    for (const auto n : next)
        CHECK (n == pushesEach);
}

namespace
{
// What the handover hands: its number, and, apart from it, whether each number is alive, so that the audio thread can
// see that what it holds wasn't freed under it.
struct Given
{
    [[maybe_unused]] static constexpr std::uint8_t notMade = 0, alive = 1, freed = 2, freedTwice = 3;

    explicit Given (std::uint64_t n, std::vector<std::atomic<std::uint8_t>>& s)
        : number (n),
          states (s)
    {
        states[(size_t) number].store (alive);
    }

    ~Given()
    {
        auto& state = states[(size_t) number];
        state.store (state.load() == alive ? freed : freedTwice);
    }

    const std::uint64_t number;
    std::vector<std::atomic<std::uint8_t>>& states;

    Given (const Given&) = delete;
    Given& operator= (const Given&) = delete;
};
} // namespace

TEST_CASE ("The handover raced: the audio thread's blocks never hold what was freed, and take what was given in order, the last at the end")
{
    static constexpr std::uint64_t gives = 20000;
    std::vector<std::atomic<std::uint8_t>> states (gives + 1);
    engine::Handover<Given> handover;
    std::atomic<bool> allGiven { false };
    std::atomic<int> started { 0 };

    // The message thread's part: gives each number in turn.
    std::thread giver (
        [&]
        {
            ++started;

            while (started.load() < 2)
                std::this_thread::yield();

            for (std::uint64_t n = 1; n <= gives; ++n)
                handover.give (std::make_unique<Given> (n, states));

            allGiven = true;
        });

    // The audio thread's: a block after another until the last is given and taken, each looking at what it holds as it
    // starts and as it ends.
    int freedWhileHeld = 0, wentBack = 0;
    std::uint64_t latest = 0, blocks = 0;
    ++started;

    for (;;)
    {
        const auto lastGiven = allGiven.load();

        {
            const engine::Handover<Given>::Block block { handover };
            ++blocks;

            if (const auto* held = block.get())
            {
                const auto number = held->number;

                if (number < latest)
                    ++wentBack;

                latest = number;

                if (states[(size_t) number].load() != Given::alive)
                    ++freedWhileHeld;

                // A while in the block, as a block takes, during which the giver goes on.
                for (int i = 0; i < 50; ++i)
                    if (held->number != number)
                        ++freedWhileHeld;

                if (states[(size_t) number].load() != Given::alive)
                    ++freedWhileHeld;
            }
        }

        if (lastGiven && latest == gives)
            break;
    }

    giver.join();
    CHECK (freedWhileHeld == 0);
    CHECK (wentBack == 0);
    CHECK (latest == gives);
    CHECK (blocks > 0);

    // The last stays given; the others were freed once each (some may wait for a next give), and nothing twice.
    handover.give (nullptr);
    handover.freeReplaced();

    int notFreed = 0, twice = 0;

    for (std::uint64_t n = 1; n <= gives; ++n)
    {
        const auto state = states[(size_t) n].load();
        notFreed += state != Given::freed ? 1 : 0;
        twice += state == Given::freedTwice ? 1 : 0;
    }

    CHECK (notFreed == 0);
    CHECK (twice == 0);
}

TEST_CASE (
    "The playhead's stamps raced: a reader gets each stamp whole, never one older than it got before, and the beat at a moment from its own block")
{
    // Stamp n: at beat n, n * 8 ms, an eighth of a beat each millisecond, 8 ms long, its run moved on each thousand, its round
    // each seventh, going round in each third; so
    // that 4 ms into it, the playhead is at n + 0.5, exactly.
    static constexpr std::uint64_t stamps = 200000;
    const auto makeStamp = [] (std::uint64_t n)
    {
        return engine::PlayheadStamps::Stamp { (double) n, 8.0 * (double) n,           0.125,
                                               8.0,        (std::uint32_t) (n / 1000), (std::uint32_t) (n / 7),
                                               n % 3 == 0 };
    };

    engine::PlayheadStamps playhead;
    std::atomic<bool> allPublished { false };
    std::atomic<int> started { 0 };

    std::thread writer (
        [&]
        {
            ++started;

            while (started.load() < 2)
                std::this_thread::yield();

            for (std::uint64_t n = 0; n < stamps; ++n)
                playhead.publish (makeStamp (n));

            allPublished = true;
        });

    int torn = 0, wentBack = 0, wrongBeat = 0, reads = 0;
    double latest = -1.0;
    ++started;

    for (bool last = false; ! last;)
    {
        last = allPublished.load();
        const auto stamp = playhead.getLatest();

        if (! stamp.has_value())
            continue; // none yet, or the writer went round the slots meanwhile

        ++reads;
        const auto n = (std::uint64_t) stamp->beat;
        const auto expected = makeStamp (n);

        if (! juce::exactlyEqual (stamp->beat, expected.beat) || ! juce::exactlyEqual (stamp->milliseconds, expected.milliseconds)
            || ! juce::exactlyEqual (stamp->beatsPerMillisecond, expected.beatsPerMillisecond)
            || ! juce::exactlyEqual (stamp->length, expected.length) || stamp->run != expected.run || stamp->round != expected.round
            || stamp->wentRound != expected.wentRound)
            ++torn;

        if (stamp->beat < latest)
            ++wentBack;

        latest = stamp->beat;

        // 4 ms into stamp n: n + 0.5 from that stamp; or, if the writer has gone round its slot since (or a run starts
        // after it), the beat of the oldest later stamp it could read whole; or nothing, if it could read none.
        if (const auto beat = playhead.getBeatAt (expected.milliseconds + 4.0))
        {
            const auto fromItsStamp = juce::exactlyEqual (*beat, stamp->beat + 0.5);
            const auto fromALaterOne = juce::exactlyEqual (*beat, std::floor (*beat)) && *beat > stamp->beat;

            if (! fromItsStamp && ! fromALaterOne)
                ++wrongBeat;
        }
    }

    writer.join();
    CHECK (torn == 0);
    CHECK (wentBack == 0);
    CHECK (wrongBeat == 0);
    CHECK (reads > 0);
    CHECK (juce::exactlyEqual (latest, (double) (stamps - 1))); // the last read after the last was published
}

TEST_CASE (
    "The transport raced: asked to play, stop and jump on one thread while the audio thread plays, it ends as last asked, and the playhead never shows a beat that wasn't asked for")
{
    // Jumps to whole thousands of beats, from which the blocks played in the race can't take the playhead 60 beats on.
    static constexpr double rate = 48000.0, spacing = 1000.0, reach = 60.0;
    static constexpr int blockSize = 64, blocks = 20000, asks = 20000, reads = 20000;
    static_assert (blocks * blockSize / rate * 2.0 < reach); // 120 BPM: two beats a second

    engine::Engine player;
    player.prepare (rate, blockSize);
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, 120.0 } }, { { 0.0, 4, 4 } }, rate);
    player.setPlan (std::move (plan));

    const auto isAsked = [] (double beat)
    { return beat >= 0.0 && beat <= 1002.0 * spacing && beat - spacing * std::floor (beat / spacing) <= reach; };

    std::atomic<int> started { 0 };
    std::atomic<bool> audioDone { false };
    const auto startTogether = [&started]
    {
        ++started;

        while (started.load() < 3)
            std::this_thread::yield();
    };

    // The message thread's part: play, stop and jump, drawn from a seed, and last a jump and play.
    std::thread asker (
        [&]
        {
            startTogether();
            juce::Random random (154);

            for (int i = 0; i < asks; ++i)
            {
                switch (random.nextInt (3))
                {
                    case 0:
                        player.locate (spacing * (1 + random.nextInt (1000)));
                        break;
                    case 1:
                        player.play();
                        break;
                    default:
                        player.stop();
                        break;
                }
            }

            player.locate (1001.0 * spacing);
            player.play();
        });

    // The audio thread's: its blocks.
    std::thread audio (
        [&]
        {
            startTogether();
            juce::AudioBuffer<float> buffer (2, blockSize);

            for (int i = 0; i < blocks; ++i)
                player.process (buffer.getArrayOfWritePointers(), 2, blockSize);

            audioDone = true;
        });

    // The window's: where the playhead is, now and at a moment.
    int notAsked = 0, readsDone = 0;
    double firstNotAsked = 0.0;
    startTogether();

    while (readsDone < reads || ! audioDone.load())
    {
        ++readsDone;

        for (const auto beat : { player.getPlayheadBeat(), player.getPlayheadBeatAt (juce::Time::getMillisecondCounterHiRes()) })
        {
            if (! isAsked (beat))
            {
                if (notAsked++ == 0)
                    firstNotAsked = beat;
            }
        }
    }

    asker.join();
    audio.join();
    CAPTURE (firstNotAsked);
    CHECK (notAsked == 0);

    // As last asked: a jump while it played fades out what sounds and is taken then (in the race, or in the blocks that
    // follow), and it plays on from there.
    REQUIRE (player.isPlaying());
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int i = 0; i < 8; ++i)
        player.process (buffer.getArrayOfWritePointers(), 2, blockSize);

    // Where the latest block started, a block and a half on (as far as the stamps reach after it).
    const auto latest = [&player] { return player.getPlayheadBeatAt (1.0e12); };
    const auto landed = latest();
    CHECK (landed >= 1001.0 * spacing);
    CHECK (landed <= 1001.0 * spacing + reach);

    for (int i = 0; i < 8; ++i)
        player.process (buffer.getArrayOfWritePointers(), 2, blockSize);

    CHECK (latest() - landed == doctest::Approx (8.0 * blockSize / rate * 2.0));

    // Stopped and put elsewhere, it stays there.
    player.stop();
    player.locate (7.0);
    player.process (buffer.getArrayOfWritePointers(), 2, blockSize);
    player.process (buffer.getArrayOfWritePointers(), 2, blockSize);
    CHECK_FALSE (player.isPlaying());
    CHECK (juce::exactlyEqual (player.getPlayheadBeat(), 7.0));
}

TEST_CASE ("A device's sample tap raced: made as a display first listens while the audio thread pushes, each pair whole and in order")
{
    // The audio thread pushes pairs, a count up the left and its negative on the right, in blocks, a fixed count of them; the
    // message thread starts listening once it has pushed some (the tap made then, and published to the audio thread through its
    // atomic pointer) and pulls until the audio thread has ended and nothing is left. What the audio thread pushed while
    // nobody listened, and what didn't fit, are dropped; whatever is pulled is whole and in order.
    constexpr int blocks = 20000, blockSize = 64;
    phazite::devices::SampleTap tap (4096);
    std::atomic<int> pushedBlocks { 0 };
    std::atomic<bool> audioDone { false };

    std::thread audio (
        [&]
        {
            std::array<float, blockSize> lefts {}, rights {};
            auto next = 0;

            for (int block = 0; block < blocks; ++block)
            {
                for (int i = 0; i < blockSize; ++i, ++next)
                {
                    lefts[(size_t) i] = (float) next;
                    rights[(size_t) i] = (float) -next;
                }

                tap.push (lefts.data(), rights.data(), blockSize);
                pushedBlocks.store (block + 1, std::memory_order_release);
            }

            audioDone.store (true, std::memory_order_release);
        });

    // Listening only once the audio thread is pushing, as a display opens while the song plays.
    while (pushedBlocks.load (std::memory_order_acquire) < 100 && ! audioDone.load (std::memory_order_acquire))
        std::this_thread::yield();

    tap.setListening (true);
    std::vector<float> lefts (1024), rights (1024);
    auto pulled = 0, torn = 0, outOfOrder = 0;
    auto last = -1.0f;

    for (;;)
    {
        const auto done = audioDone.load (std::memory_order_acquire);
        const auto n = tap.pull (lefts.data(), rights.data(), (int) lefts.size());

        for (int i = 0; i < n; ++i)
        {
            if (! juce::exactlyEqual (rights[(size_t) i], -lefts[(size_t) i]))
                ++torn;

            if (lefts[(size_t) i] <= last)
                ++outOfOrder;

            last = lefts[(size_t) i];
        }

        pulled += n;

        if (done && n == 0)
            break;

        if (n == 0)
            std::this_thread::yield();
    }

    audio.join();
    CHECK (pulled > 0);
    CHECK (torn == 0);
    CHECK (outOfOrder == 0);
    CHECK (last <= (float) (blocks * blockSize - 1));
}
