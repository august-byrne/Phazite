// SPDX-License-Identifier: AGPL-3.0-or-later
// The benchmark gates: costs measured as ratios within one run on one machine, compared with stored bounds, so that they
// hold on a slower or a busier machine as long as it is the same kind of machine. Each cost is measured beside what it
// is compared with, the two taking turns, and each is the least of several tries (the try a busy machine interrupted
// least):
// - each of Phazite's devices, as a multiple of a fixed reference workload written here (biquad filters in a row), which
//   no change to Phazite touches: a device that becomes much costlier fails, whatever the machine's speed;
// - the engine playing the benchmark song (BenchmarkSong.h) of 100 tracks against the same song of 10, stopped and
//   playing, the plan's compile and a save: work that grows as the square of the tracks fails.
//
// The bounds are twice what a windows-profile build measured on the PC docs/performance.md describes (rounded up): a
// generous margin for another processor (each kind of processor favours some code over other), so that they catch a
// device that has become two or three times as costly, not one that is 30 % slower. They are for MSVC's optimised builds
// (windows-profile, windows-release); a Debug build or another compiler measures other ratios.
//
// What they don't see: a cost that grows in both of what is compared (the engine's per-track work doubled shows in the
// devices' ratios only if it is in a device), or anything under the margin. The counts in PerformanceGateTests.cpp,
// which ctest runs, see doubled work exactly.
//
// ctest skips them; the nightly workflow (.github/workflows/nightly.yml) runs them from windows-profile with
//   phazite_tests --test-suite=benchmark-gate --no-skip
// After a change that makes a device costlier on purpose, measure again and set its bound to twice the new ratio, saying
// why in the commit message.
#include "BenchmarkSong.h"
#include "EnginePlayback.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "model/devices/internal/Devices.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <string>

namespace
{
namespace dev = phazite::devices;

constexpr double rate = 48000.0;
constexpr int blockSize = 512;
constexpr int blocksPerTry = 188; // two seconds of audio
constexpr int tries = 7;

double secondsOf (const std::function<void()>& work)
{
    const auto start = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
}

// The least of several tries of two pieces of work, taking turns.
std::pair<double, double> leastOfTries (const std::function<void()>& first, const std::function<void()>& second, int count = tries)
{
    auto a = 1.0e9, b = 1.0e9;

    for (int i = 0; i < count; ++i)
    {
        a = std::min (a, secondsOf (first));
        b = std::min (b, secondsOf (second));
    }

    return { a, b };
}

//==============================================================================
// The reference: noise through sixteen biquad low-passes in a row on each of two sides, in blocks, written here once and
// never changed. Its cost is a moderate device's (about as much as an EQ of many bands).
class Reference
{
public:
    Reference()
    {
        juce::Random random (3);

        for (auto& sample : noise)
            sample = random.nextFloat() * 2.0f - 1.0f;

        // A low-pass at 2 kHz, Q 0.7, at 48 kHz (RBJ's cookbook), every stage the same.
        const auto w = juce::MathConstants<double>::twoPi * 2000.0 / rate;
        const auto alpha = std::sin (w) / (2.0 * 0.7);
        const auto a0 = 1.0 + alpha;
        b0 = (float) ((1.0 - std::cos (w)) / 2.0 / a0);
        b1 = (float) ((1.0 - std::cos (w)) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * std::cos (w) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    void playBlocks (int count)
    {
        for (int block = 0; block < count; ++block)
        {
            for (int side = 0; side < 2; ++side)
            {
                std::copy (noise.begin(), noise.end(), buffer.begin());

                for (int stage = 0; stage < stages; ++stage)
                {
                    auto& state = states[(size_t) (side * stages + stage)];

                    for (auto& sample : buffer)
                    {
                        const auto out = b0 * sample + state[0];
                        state[0] = b1 * sample - a1 * out + state[1];
                        state[1] = b2 * sample - a2 * out;
                        sample = out;
                    }
                }

                sink += buffer[(size_t) block % buffer.size()];
            }
        }
    }

    float getSink() const noexcept { return sink; }

private:
    static constexpr int stages = 16;
    std::array<float, blockSize> noise {}, buffer {};
    std::array<std::array<float, 2>, 2 * stages> states {};
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float sink = 0.0f; // so that the compiler keeps the work
};

//==============================================================================
// A device playing as Phazite's engine plays it, at its defaults: an instrument holding eight notes (a drum sampler
// hitting four pads every 12 blocks), an effect taking in noise.
class DevicePlaying
{
public:
    explicit DevicePlaying (const dev::DeviceInfo& deviceInfo)
        : info (deviceInfo),
          processor (phazite::createOwnDevice (info.typeID))
    {
        REQUIRE (processor != nullptr);
        processor->prepare (rate, blockSize);

        for (const auto& parameter : info.parameters)
            values.push_back (parameter.defaultValue);

        juce::Random random (7);

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < blockSize; ++i)
                noise.setSample (channel, i, (random.nextFloat() * 2.0f - 1.0f) * 0.25f);

        const auto drums = &info == &dev::DrumSampler::info;

        for (int n = 0; n < (drums ? 4 : 8); ++n)
            notes.push_back ({ .type = dev::NoteEvent::Type::noteOn, .sample = 0, .number = drums ? 36 + n : 48 + n * 3, .value = 0.8f });

        playBlocks (20); // warmed up
    }

    void playBlocks (int count)
    {
        const auto instrument = info.kind == dev::DeviceKind::instrument;
        const auto drums = &info == &dev::DrumSampler::info;

        for (int b = 0; b < count; ++b, ++block)
        {
            if (instrument)
                buffer.clear();
            else
                for (int channel = 0; channel < 2; ++channel)
                    buffer.copyFrom (channel, 0, noise, channel, 0, blockSize);

            const auto hit = instrument && (block == 0 || (drums && block % 12 == 0));
            const dev::ProcessContext context { .channels = buffer.getArrayOfWritePointers(),
                                                .numChannels = 2,
                                                .numSamples = blockSize,
                                                .events = hit ? std::span<const dev::NoteEvent> (notes) : std::span<const dev::NoteEvent>(),
                                                .parameters = values,
                                                .beat = block * blockSize * 2.0 / rate,
                                                .bars = block * blockSize * 2.0 / rate / 4.0,
                                                .isPlaying = true };
            processor->process (context);
        }
    }

private:
    const dev::DeviceInfo& info;
    std::unique_ptr<dev::Processor> processor;
    std::vector<float> values;
    std::vector<dev::NoteEvent> notes;
    juce::AudioBuffer<float> buffer { 2, blockSize }, noise { 2, blockSize };
    int block = 0;
};

// The bounds: each device's cost as a multiple of the reference's, at most: twice what windows-profile measured on
// 2026-11-14 (the more of two runs, which differed by under 5 %, in the comment), rounded up, and at least 0.1, under which
// a device at its defaults does next to nothing (the Sampler without a sound, the EQ with its bands off).
// clang-format off
const std::map<std::string, double> deviceBounds {
    { "phaziteSynth",          2.7  }, // 1.304
    { "phaziteWavetable",      3.9  }, // 1.905
    { "phaziteFM",            15.0  }, // 7.489
    { "phaziteSampler",        0.1  }, // 0.042
    { "phaziteDrums",          0.8  }, // 0.397
    { "phaziteEq",             0.1  }, // 0.039
    { "phaziteFilter",         0.27 }, // 0.133
    { "phaziteCompressor",     1.1  }, // 0.502
    { "phaziteMultiband",      5.0  }, // 2.458
    { "phaziteLimiter",        1.0  }, // 0.484
    { "phaziteSaturator",      2.4  }, // 1.155
    { "phaziteExciter",        7.3  }, // 3.620
    { "phaziteVinyl",          4.1  }, // 2.040
    { "phaziteDelay",          0.43 }, // 0.213
    { "phaziteReverb",         2.6  }, // 1.282
    { "phazitePhaser",         0.81 }, // 0.402
    { "phaziteFlanger",        0.6  }, // 0.296
    { "phaziteChorus",         0.44 }, // 0.218
    { "phaziteVocoder",        4.1  }, // 2.003
    { "phaziteStereoExpander", 0.43 }, // 0.210
};
// clang-format on

bool isOptimisedBuild()
{
#if defined(NDEBUG) && ! defined(_DEBUG)
    return true;
#else
    return false;
#endif
}
} // namespace

//==============================================================================
TEST_CASE ("Benchmark gate: each device's cost against a fixed reference" * doctest::test_suite ("benchmark-gate") * doctest::skip())
{
    if (! isOptimisedBuild())
        MESSAGE ("Not an optimised build: the ratios are reported but not checked.");

    juce::String report ("\n| Device | Its cost against the reference's | Bound |\n|---|---|---|\n");
    Reference reference;

    for (const auto* info : phazite::getOwnDevices())
    {
        const auto typeID = std::string (info->typeID);
        INFO (typeID);
        DevicePlaying device (*info);
        const auto [referenceSeconds, deviceSeconds] =
            leastOfTries ([&] { reference.playBlocks (blocksPerTry); }, [&] { device.playBlocks (blocksPerTry); });
        const auto ratio = deviceSeconds / referenceSeconds;
        const auto bound = deviceBounds.find (typeID);
        report << "| " << typeID << " | " << juce::String (ratio, 3) << " | "
               << (bound != deviceBounds.end() ? juce::String (bound->second, 2) : juce::String ("none")) << " |\n";

        if (isOptimisedBuild())
        {
            CHECK_MESSAGE (bound != deviceBounds.end(), "a device without a bound: measure it and add one");

            if (bound != deviceBounds.end())
                CHECK (ratio <= bound->second);
        }
    }

    CHECK (std::isfinite (reference.getSink()));
    MESSAGE (report);
}

//==============================================================================
namespace
{
struct SongCosts
{
    double stopped = 0.0, playing = 0.0, compile = 0.0, edit = 0.0, save = 0.0; // in seconds
};

// The benchmark song of so many tracks, on one thread: a block stopped and playing (the least of several tries' means), a
// plan's compile and an edit's work on the message thread (a note added, with what follows), and a save.
SongCosts measureSong (int numTracks)
{
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, numTracks, 64.0);
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    REQUIRE (s.getProject().save().wasOk());

    phazite::test::EnginePlayback device (phazite::test::EnginePlayback::Input::silence, phazite::test::EnginePlayback::Timing::realTime,
                                          rate, blockSize);
    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    const auto threads = engine.getNumThreads();
    engine.setNumThreads (1);

    const auto blockCost = [] (int count)
    {
        auto least = 1.0e9;

        for (int i = 0; i < tries; ++i)
            least = std::min (least, phazite::test::meanOf (phazite::test::timeEngineBlocks (count, blockSize)));

        return least;
    };

    SongCosts costs;
    phazite::test::timeEngineBlocks (20, blockSize);
    costs.stopped = blockCost (50);
    phazite::playOrStop (*song);
    phazite::test::timeEngineBlocks (20, blockSize);
    costs.playing = blockCost (50);
    phazite::stopAndReturn (*song);

    auto* session = phazite::findPlayingSession (*song);
    REQUIRE (session != nullptr);
    costs.compile = 1.0e9;
    costs.edit = 1.0e9;
    costs.save = 1.0e9;

    for (int i = 0; i < tries; ++i)
    {
        costs.compile = std::min (costs.compile, secondsOf ([session] { session->update(); }));
        costs.edit =
            std::min (costs.edit, secondsOf (
                                      [&]
                                      {
                                          phazite::addNote (s.context, built.clips[(size_t) i], 72, phazite::BeatPosition::fromBeats (2.5),
                                                            phazite::BeatDuration::fromBeats (0.25), 100);
                                          phazite::test::dispatchPendingMessages();
                                      }));
        costs.save = std::min (costs.save, secondsOf ([&] { REQUIRE (s.getProject().save().wasOk()); }));
        phazite::test::timeEngineBlocks (4, blockSize); // so that the plans replaced are let go of
    }

    engine.setNumThreads (threads);
    return costs;
}
} // namespace

TEST_CASE ("Benchmark gate: the engine and the model with 100 tracks against 10" * doctest::test_suite ("benchmark-gate") * doctest::skip())
{
    const auto small = measureSong (10), big = measureSong (100);

    struct Row
    {
        const char* what;
        double SongCosts::* cost;
        double bound; // the ratio's, ten times the tracks: about ten times the work, and far below the 100 of a square
    };

    const Row rows[] = { { "A block of 512 samples, stopped", &SongCosts::stopped, 15.0 },
                         { "A block of 512 samples, playing", &SongCosts::playing, 15.0 },
                         { "The plan's compile", &SongCosts::compile, 20.0 },
                         { "A note added, with what follows on the message thread", &SongCosts::edit, 20.0 },
                         { "A save", &SongCosts::save, 20.0 } };

    juce::String report ("\n| On one thread | 10 tracks | 100 tracks | Ratio | Bound |\n|---|---|---|---|---|\n");

    for (const auto& row : rows)
    {
        const auto ratio = big.*row.cost / small.*row.cost;
        report << "| " << row.what << " | " << juce::String (small.*row.cost * 1000.0, 3) << " ms | "
               << juce::String (big.*row.cost * 1000.0, 3) << " ms | " << juce::String (ratio, 2) << " | " << row.bound << " |\n";

        if (isOptimisedBuild())
        {
            INFO (std::string (row.what));
            CHECK (ratio <= row.bound);
        }
    }

    MESSAGE (report);
}
