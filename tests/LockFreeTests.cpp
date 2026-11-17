// SPDX-License-Identifier: AGPL-3.0-or-later
// The engine's lock-free parts raced on real threads: the queue that live notes come through from any thread
// (EventQueue.h), the handover of what the audio thread plays (Handover.h), the playhead's stamps (PlayheadClock.h),
// the transport the message thread asks for (Engine.h), a device's sample tap made as a display listens (SampleTap.h),
// and the sounds the wavetable synth and the sampler let go of once the audio thread can't be reading them.
// Each race runs a fixed count of operations, never for a time, and waits only for what the other threads do, so that it
// says the same on one core as on many; ThreadSanitizer (linux-tsan) sees each access the threads share. What a thread
// finds wrong it counts, and the test thread checks the counts once the threads have ended (doctest's checks are the test
// thread's).
#include "TestHelpers.h"
#include "devices/core/SampleTap.h"
#include "devices/sampler/Sampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/core/Engine.h"
#include "engine/core/EventQueue.h"
#include "engine/core/Handover.h"
#include "engine/core/PlayheadClock.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
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

TEST_CASE ("The handover raced with keeps: what the audio thread keeps playing past what was given since is never freed under it")
{
    // As the engine keeps a plan while what goes out fades out across a plan whose delay compensation changed: every so
    // many blocks, the audio thread keeps what it took for the next few blocks, saying so in each, while the giver goes on.
    static constexpr std::uint64_t gives = 20000;
    std::vector<std::atomic<std::uint8_t>> states (gives + 1);
    engine::Handover<Given> handover;
    std::atomic<bool> allGiven { false };
    std::atomic<int> started { 0 };

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

    int freedWhileKept = 0, keeps = 0;
    std::uint64_t latest = 0, blocks = 0, keptNumber = 0;
    const Given* kept = nullptr;
    int keeping = 0; // blocks left to keep it for
    ++started;

    for (;;)
    {
        const auto lastGiven = allGiven.load();

        {
            const engine::Handover<Given>::Block block { handover };
            ++blocks;

            // As the engine does, what it kept before is looked at before this block says what it keeps (choosePlan()
            // reads the plan it played, then keep()): it must be there still.
            if (keeping > 0 && states[(size_t) keptNumber].load() != Given::alive)
                ++freedWhileKept;

            const auto* playing = keeping > 0 ? kept : block.get();
            block.keep (playing);

            if (playing != nullptr)
            {
                const auto number = keeping > 0 ? keptNumber : playing->number;
                latest = std::max (latest, block.get() != nullptr ? block.get()->number : 0);

                for (int i = 0; i < 50; ++i)
                    if (states[(size_t) number].load() != Given::alive)
                        ++freedWhileKept;

                if (keeping > 0)
                {
                    --keeping;
                }
                else if (blocks % 7 == 0)
                {
                    kept = playing;
                    keptNumber = number;
                    keeping = 5;
                    ++keeps;
                }
            }
        }

        if (lastGiven && latest == gives && keeping == 0)
            break;
    }

    giver.join();
    CHECK (freedWhileKept == 0);
    CHECK (keeps > 0);

    // Nothing kept any more: the others were freed once each, and nothing twice.
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

namespace
{
// A plan at 48 kHz of a track and the master, its delay compensation numbered, for the engine raced below.
std::unique_ptr<engine::Plan> makeRacedPlan (std::uint64_t alignment, const std::shared_ptr<engine::StripState>& track,
                                             const std::shared_ptr<engine::StripState>& master)
{
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, 120.0 } }, { { 0.0, 4, 4 } }, 48000.0);
    plan->maximumBlockSize = 64;
    plan->alignment = alignment;
    engine::Plan::Strip first, last;
    first.id = 1;
    first.state = track;
    last.state = master;
    last.inputs.push_back ({ 0, std::make_shared<engine::Delay> ((int) alignment % 300) });
    plan->strips.push_back (std::move (first));
    plan->strips.push_back (std::move (last));
    return plan;
}
} // namespace

TEST_CASE ("The engine raced: plans that realign given and dropped while blocks play, as the playback host lets go of them")
{
    // The message thread gives plan after plan, each lining its paths up otherwise (which the engine fades across, keeping
    // the plan before meanwhile), and now and then lets go of them all as the playback host does (no plan, two blocks, then
    // dropPlans()); the audio thread plays meanwhile. dropPlans() wrote what the audio thread reads and writes (the plan it
    // played, the fade): ThreadSanitizer saw the race (the review of known bug 167); now it only counts the drop.
    static constexpr int gives = 3000;
    engine::Engine player;
    player.prepare (48000.0, 64);
    auto track = std::make_shared<engine::StripState>(), master = std::make_shared<engine::StripState>();
    track->prepare (48000.0, 64);
    master->prepare (48000.0, 64);
    player.setPlan (makeRacedPlan (0, track, master));
    player.play();
    std::atomic<bool> done { false };

    std::thread audio (
        [&]
        {
            juce::AudioBuffer<float> block (2, 64);

            while (! done.load())
                player.process (block.getArrayOfWritePointers(), 2, 64);
        });

    for (int n = 1; n <= gives; ++n)
    {
        if (n % 100 == 0)
        {
            player.setPlan (nullptr);
            const auto from = player.getBlocksPlayed().load();

            while (player.getBlocksPlayed().load() < from + 2)
                std::this_thread::yield();

            player.dropPlans();
        }

        player.setPlan (makeRacedPlan ((std::uint64_t) n, track, master));

        // A few blocks now and then, so that some fades run out and some plans come during them.
        if (n % 7 == 0)
            for (const auto from = player.getBlocksPlayed().load(); player.getBlocksPlayed().load() < from + 3;)
                std::this_thread::yield();
    }

    done = true;
    audio.join();
    player.dropPlans();
    CHECK (player.getBlocksPlayed().load() > 0);
}

namespace
{
// What the audio thread plays below: a delay, as long as it is.
struct RacedDelay
{
    std::shared_ptr<engine::Delay> delay;
    int length = 0;
};
} // namespace

TEST_CASE (
    "A delay's ring raced: one made in place of a shorter one reads the past from that one until it holds its own, and is let go of meanwhile")
{
    // The message thread makes delay after delay, each on a ring of its own that takes the place of the one before (as a
    // delay compensation that grows makes one), half of them fading in, and lets go of the old rings once they are done
    // with; the audio thread plays a count through whichever it was given last, reading what predates each ring from the
    // rings before it, and now and then starts it from silence (a reset, as a jump makes, of a ring not yet done with the
    // one before). Each sample that comes out is the count so many samples before, or nothing (while a ring doesn't hold
    // that much of the past, or after a reset), or the count faded in: never a sample of another, or one torn; and a
    // block's work is no more than the block, whatever the rings hold. Each delay holds the one it takes the place of until
    // it has played, and the message thread lets go of it then: before a delay plays, how much further back it reads than
    // the one played last (the engine's hold across a leave) is exactly that, however many were given meanwhile.
    static constexpr int gives = 5000;
    engine::Handover<RacedDelay> handover;
    std::atomic<bool> allGiven { false };
    int wrong = 0, exact = 0, overworked = 0, wrongGrowth = 0;

    std::thread giver (
        [&]
        {
            std::shared_ptr<engine::DelayLine> line;
            std::shared_ptr<engine::Delay> delay;

            for (int n = 1; n <= gives; ++n)
            {
                if (line != nullptr)
                    line->letGoOfTheOld(); // the one before, once done with, as each plan compiled does

                if (delay != nullptr)
                    delay->letGoOfThePrevious();

                const auto length = 64 + (n % 9) * 40;
                line = std::make_shared<engine::DelayLine> (length + 16, std::move (line));
                auto next = std::make_shared<engine::Delay> (line, length, n % 2 == 0 ? 50 : 0);
                next->takePlaceOf (std::move (delay));
                next->markReadsLeaving(); // (every one: what it plays again is what it grew by)
                delay = next;
                handover.give (std::make_unique<RacedDelay> (RacedDelay { std::move (next), length }));
                line->letGoOfTheOld();
            }

            allGiven = true;
        });

    juce::AudioBuffer<float> in (2, 32), out (2, 32);
    std::int64_t count = 0, blocks = 0;
    const engine::Delay* last = nullptr;
    auto lastLength = -1;

    while (! allGiven.load() || count < 100000)
    {
        const engine::Handover<RacedDelay>::Block block { handover };
        const auto* played = block.get();

        if (played == nullptr)
            continue;

        if (played->delay.get() != last)
        {
            wrongGrowth += played->delay->getGrowth() != (lastLength < 0 ? 0 : std::max (0, played->length - lastLength)) ? 1 : 0;
            last = played->delay.get();
            lastLength = played->length;
        }

        for (int i = 0; i < 32; ++i)
            for (int channel = 0; channel < 2; ++channel)
                in.setSample (channel, i, (float) (count + i + 1));

        if (++blocks % 37 == 0)
            played->delay->reset();

        out.clear();
        played->delay->addDelayed (in.getArrayOfReadPointers(), out.getArrayOfWritePointers(), 32);
        overworked += played->delay->getSamplesTouched() > 2 * 32 ? 1 : 0;
        wrongGrowth += played->delay->getGrowth() != 0 ? 1 : 0; // (it played: none)

        for (int i = 0; i < 32; ++i)
        {
            const auto got = out.getSample (0, i), expected = (float) (count + i + 1 - played->length);
            exact += juce::exactlyEqual (got, expected) ? 1 : 0;
            wrong += got < 0.0f || got > std::max (expected, 0.0f) ? 1 : 0; // nothing before the count began
            wrong += juce::exactlyEqual (out.getSample (1, i), got) ? 0 : 1;
        }

        count += 32;
    }

    giver.join();
    CHECK (wrong == 0);
    CHECK (exact > 0);
    CHECK (overworked == 0);
    CHECK (wrongGrowth == 0);
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

        for (const auto beat : { player.getPlayheadBeat(), player.getPlayheadBeatAt (phazite::devices::Clock::getMilliseconds()) })
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
    // nobody listened, and what didn't fit, are dropped; whatever is pulled is whole and in order. The audio thread pushes on
    // past its count until something was pulled, however late a busy machine let the message thread run (a hang's time at
    // most): it once pushed them all before the message thread listened.
    constexpr int blocks = 20000, blockSize = 64;
    phazite::devices::SampleTap tap (4096);
    std::atomic<int> pushedBlocks { 0 };
    std::atomic<bool> audioDone { false }, pulledSome { false };
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (phazite::test::hangTimeout);

    std::thread audio (
        [&]
        {
            std::array<float, blockSize> lefts {}, rights {};
            auto next = 0;

            // Past its count, a block at a time, giving the core away between them, and no further than a float counts each
            // sample exactly.
            constexpr int mostBlocks = (1 << 24) / blockSize;

            for (int block = 0;
                 block < blocks
                 || (block < mostBlocks && ! pulledSome.load (std::memory_order_acquire) && std::chrono::steady_clock::now() < until);
                 ++block)
            {
                if (block >= blocks)
                    std::this_thread::yield();

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

        if (n > 0)
            pulledSome.store (true, std::memory_order_release);

        if (done && n == 0)
            break;

        if (n == 0)
            std::this_thread::yield();
    }

    audio.join();
    CHECK (pulled > 0);
    CHECK (torn == 0);
    CHECK (outOfOrder == 0);
    CHECK (last <= (float) (pushedBlocks.load() * blockSize - 1));
}

namespace
{
// A device's sounds swapped by another thread as it plays in blocks of one sample, the shortest the runner makes: each
// sound given once, in order, while notes play; and as soon as the device has let go of one, the giver fills it with what
// isn't a number (it still holds it, so the memory is there), so that a read of it after the device let go comes out in what
// it plays. The giver waits for a few blocks to a few hundred between its gives, so that the device takes many of them in the
// middle of a control step, and lets go of some before the next comes. What it finds: how many samples the device played
// that aren't numbers (none), and how many sounds it let go of while it played.
struct Raced
{
    int notNumbers = 0, letGo = 0;
};

template <typename Device, typename SoundPointer, typename Give, typename Poison>
Raced raceSoundSwaps (Device& device, std::vector<SoundPointer>& sounds, std::span<const float> values, Give give, Poison poison)
{
    std::atomic<std::uint64_t> blocksPlayed { 0 };
    std::atomic<bool> allGiven { false };
    std::atomic<int> letGo { 0 };

    std::thread giver (
        [&]
        {
            std::vector<bool> poisoned (sounds.size(), false);

            for (size_t n = 0; n < sounds.size(); ++n)
            {
                const auto from = blocksPlayed.load();

                while (blocksPlayed.load() < from + 3 + (n * 37) % 200)
                    std::this_thread::yield();

                give (sounds[n]);

                for (size_t each = 0; each < n; ++each)
                {
                    if (! poisoned[each] && sounds[each].use_count() == 1)
                    {
                        poison (*sounds[each]);
                        poisoned[each] = true;
                        ++letGo;
                    }
                }
            }

            allGiven = true;
        });

    juce::AudioBuffer<float> buffer (2, 1);
    Raced raced;

    for (std::uint64_t played = 0; ! allGiven.load() || played == 0; ++played)
    {
        // A note every so often, so that notes start on each sound, and voices are taken for them once all sound.
        const phazite::devices::NoteEvent note { phazite::devices::NoteEvent::Type::noteOn, 0, 48 + (int) (played / 64) % 24, 1.0f };

        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        phazite::devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = 1;
        block.parameters = values;
        block.isPlaying = true;
        block.events =
            played % 64 == 0 ? std::span<const phazite::devices::NoteEvent> (&note, 1) : std::span<const phazite::devices::NoteEvent>();
        device.process (block);
        raced.notNumbers += std::isfinite (buffer.getSample (0, 0)) && std::isfinite (buffer.getSample (1, 0)) ? 0 : 1;
        ++blocksPlayed;
    }

    giver.join();
    raced.letGo = letGo.load();
    return raced;
}

std::vector<float> getDefaultValues (const phazite::devices::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}
} // namespace

TEST_CASE ("The wavetable synth's tables swapped as it plays raced: none is let go of while a control step still reads it")
{
    namespace devices = phazite::devices;
    std::vector<std::shared_ptr<const devices::Wavetable>> tables;

    for (int n = 0; n < 200; ++n)
    {
        devices::Wavetable::Harmonics saw;

        for (int h = 1; h <= 64; ++h)
            saw.push_back (std::polar (1.0f / (float) h, (float) n * 0.01f * (float) h));

        tables.push_back (devices::Wavetable::fromHarmonics ("Saw", { saw }));
    }

    devices::WavetableSynth synth;
    synth.prepare (48000.0, 1);
    const auto values = getDefaultValues (devices::WavetableSynth::info);

    const auto raced = raceSoundSwaps (
        synth, tables, values, [&synth] (const auto& table) { synth.setTable (0, table); },
        [] (const devices::Wavetable& table)
        {
            for (int frame = 0; frame < table.getNumFrames(); ++frame)
            {
                for (int level = 0; level < devices::Wavetable::numLevels; ++level)
                {
                    const auto read = table.getLevel (frame, level);
                    auto* samples = const_cast<float*> (read.samples);

                    for (int i = -1; i < read.length + 2; ++i)
                        samples[i] = std::numeric_limits<float>::quiet_NaN();
                }
            }
        });

    CHECK (raced.notNumbers == 0);
    CHECK (raced.letGo > 100); // most of them, while it played
}

TEST_CASE ("The sampler's sounds swapped as it plays raced: none is let go of while a voice still reads it")
{
    namespace devices = phazite::devices;
    std::vector<std::shared_ptr<const devices::Sampler::Sound>> sounds;

    for (int n = 0; n < 200; ++n)
    {
        auto sound = std::make_shared<devices::Sampler::Sound>();
        sound->sampleRate = 48000.0;
        sound->audio.setSize (1, 48000);

        for (int i = 0; i < sound->audio.getNumSamples(); ++i)
            sound->audio.setSample (0, i, 0.5f * (float) std::sin (0.03 * i + n));

        devices::Sampler::makeOctaves (*sound);
        sounds.push_back (std::move (sound));
    }

    devices::Sampler sampler;
    sampler.prepare (48000.0, 1);
    const auto values = getDefaultValues (devices::Sampler::info);

    const auto raced = raceSoundSwaps (
        sampler, sounds, values, [&sampler] (const auto& sound) { sampler.setSound (sound); },
        [] (const devices::Sampler::Sound& sound)
        {
            auto& audio = const_cast<juce::AudioBuffer<float>&> (sound.audio);

            for (int i = 0; i < audio.getNumSamples(); ++i)
                audio.setSample (0, i, std::numeric_limits<float>::quiet_NaN());

            for (auto& octave : const_cast<devices::Sampler::Sound&> (sound).octaves)
                for (int i = 0; i < octave.getNumSamples(); ++i)
                    octave.setSample (0, i, std::numeric_limits<float>::quiet_NaN());
        });

    CHECK (raced.notNumbers == 0);
    CHECK (raced.letGo > 100); // most of them, while it played
}

TEST_CASE (
    "Songs read from the versions made consistent on another thread raced: none asks the message thread's sessions as they come and go")
{
    // Found in review (known bug 126's round): the loader's checks, run on the history's thread on a song joined from the
    // versions' pieces, asked a plug-in's curve's range of its instance, through the playback sessions the message thread
    // makes and destroys (findSession()). Now a plug-in's curve is kept within 0 to 1 from its element alone. A fixed count
    // of joins on another thread, while the message thread opens and closes songs, each with a session of its own.
    auto song = phazite::createSong();
    auto state = song->getState();
    auto track = phazite::getTrackElements (state).front();
    track.appendChild (juce::ValueTree (phazite::schema::DEVICE, { { phazite::schema::deviceID, 900 },
                                                                   { phazite::schema::deviceType, phazite::schema::deviceIsPlugin } }),
                       nullptr);
    juce::ValueTree curve (phazite::schema::CURVE, { { phazite::schema::curveDevice, 900 }, { phazite::schema::curveParameter, "p1" } });
    curve.appendChild (
        juce::ValueTree (phazite::schema::POINT, { { phazite::schema::pointBeat, 0.0 }, { phazite::schema::pointValue, 2.0 } }), nullptr);
    juce::ValueTree automation (phazite::schema::AUTOMATION);
    automation.appendChild (curve, nullptr);
    state.appendChild (automation, nullptr);
    const auto pieces = phazite::splitSong (state);

    constexpr int joins = 300;
    std::atomic<int> joined { 0 }, kept { 0 };
    std::thread reader (
        [&]
        {
            for (int i = 0; i < joins; ++i)
            {
                const auto read = phazite::joinSong (pieces.song,
                                                     [&pieces] (const juce::String& name) -> std::optional<juce::MemoryBlock>
                                                     {
                                                         const auto found = pieces.pieces.find (name);
                                                         return found != pieces.pieces.end() ? std::optional (found->second) : std::nullopt;
                                                     });
                const auto point = read.getChildWithName (phazite::schema::AUTOMATION).getChild (0).getChild (0);

                if (juce::exactlyEqual ((double) point[phazite::schema::pointValue], 1.0))
                    ++kept; // within the plug-in's 0 to 1

                ++joined;
            }
        });

    phazite::ProjectContext context (phazite::test::getEngineContext());

    while (joined.load() < joins)
    {
        auto other = phazite::createSong();
        phazite::setSong (context, other.get());
        phazite::setSong (context, nullptr);
    }

    reader.join();
    CHECK (kept.load() == joins);
}

TEST_CASE ("The tests' device types registered while another thread looks for them raced: each found whole once it is there")
{
    // The second review of known bug 126: registerDeviceType() grew the registry while a history's thread, one an earlier
    // test case left, looked a device up in it (the checks of a song read from the versions). Now under a lock that only a
    // look past Phazite's own devices takes, in a deque whose elements stay where they are.
    constexpr int types = 64;
    static std::array<std::string, types> names;
    static std::array<phazite::devices::DeviceInfo, types> infos;

    for (int i = 0; i < types; ++i)
    {
        names[(size_t) i] = "racedDevice" + std::to_string (i);
        infos[(size_t) i] = { names[(size_t) i], names[(size_t) i] };
    }

    std::atomic<int> registered { 0 }, wrong { 0 };
    std::thread looker (
        [&]
        {
            for (int round = 0; round < 2000; ++round)
            {
                const auto there = registered.load();

                for (int i = 0; i < there; ++i)
                    if (const auto* info = phazite::findOwnDevice (names[(size_t) i]); info == nullptr || info->typeID != names[(size_t) i])
                        ++wrong;
            }
        });

    for (int i = 0; i < types; ++i)
    {
        phazite::registerDeviceType (infos[(size_t) i], [] { return std::unique_ptr<phazite::devices::Processor>(); });
        ++registered;
    }

    looker.join();
    CHECK (wrong.load() == 0);
}

TEST_CASE ("The clock a test holds raced: moved on as it is let go, it runs afterwards, never held again, and never goes back")
{
    // Review of known bug 180: an advance just after a release added to the release's mark and left the clock held, near
    // the machine's start, for the rest of the run. An advance now adds only while the clock is held.
    using phazite::devices::Clock;

    for (int round = 0; round < 500; ++round)
    {
        Clock::hold();
        const auto heldAt = Clock::getTicks();
        std::atomic<bool> go { false };

        std::thread mover (
            [&go]
            {
                while (! go.load())
                    std::this_thread::yield();

                for (int i = 0; i < 200; ++i)
                    Clock::advance (0.001);
            });

        go = true;
        Clock::release();
        mover.join();
        REQUIRE_FALSE (Clock::isHeld());
        CHECK (Clock::getTicks() >= heldAt);
    }
}

TEST_CASE ("The clock a test held runs on from where it stood as it is let go, never back to where the machine's clock is")
{
    // Review of known bug 180: let go, it jumped back to JUCE's clock, by as far as a test had moved it on (two minutes, in
    // Auto Save's), under the engine's stamps and the counters that take differences of it.
    using phazite::devices::Clock;
    Clock::hold();
    const auto heldAt = Clock::getMilliseconds();
    REQUIRE (Clock::advance (60000.0));
    Clock::release();

    // To a hundredth of a millisecond: the clock counts whole ticks, so a minute moved on can come out a part of a tick
    // short in milliseconds (it did once, on CI's Clang build, 2026-11-17, the two numbers alike to six digits).
    CHECK (Clock::getMilliseconds() >= heldAt + 60000.0 - 0.01);
}
