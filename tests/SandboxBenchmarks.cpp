// SPDX-License-Identifier: AGPL-3.0-or-later
// The measurement step 0 of T3 asks for (docs/tracktion-migration.md; roadmap step 3.3): a plug-in played in a helper,
// a block at a time at 128 samples, one every block's length as an audio device asks, against the same plug-in in
// Phazite's own process: each block's time from handing it over to having it back (median, 99th percentile, worst), the
// CPU of the thread that waits (as Phazite's audio thread would) and of the helper, and the helper's memory. With the
// tests' stand-in effect, and Surge XT's effects and synth where they are installed. Run by hand
// (--test-suite=benchmark --no-skip); ctest skips it.
#include "TestHelpers.h"
#include "engine/core/Workers.h"
#include "host/TestPlugins.h"
#include "host/ThreadCpu.h"
#include "sandbox/client/Helper.h"
#include "sandbox/client/RemoteInstance.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
namespace sandbox = phazite::sandbox;

constexpr double rate = 48000.0;
constexpr int blockSize = 128;
constexpr int numBlocks = 4000; // 10.7 s of blocks

// Each block's time in microseconds, and the CPU the playing thread took.
struct Measured
{
    std::vector<double> microseconds;
    double cpuSeconds = 0.0;
};

// Plays the blocks one every block's length, timing each.
template <typename Play>
Measured measure (Play&& play)
{
    Measured measured;
    measured.microseconds.reserve (numBlocks);
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (blockSize / rate));
    auto next = std::chrono::steady_clock::now();
    const auto cpuBefore = phazite::test::getThreadCpuSeconds();

    for (int block = 0; block < numBlocks; ++block)
    {
        next += period;
        const auto start = std::chrono::steady_clock::now();
        play (block);
        const auto end = std::chrono::steady_clock::now();
        measured.microseconds.push_back (std::chrono::duration<double, std::micro> (end - start).count());
        std::this_thread::sleep_until (next);
    }

    measured.cpuSeconds = phazite::test::getThreadCpuSeconds() - cpuBefore;
    return measured;
}

double percentile (std::vector<double> values, double fraction)
{
    std::sort (values.begin(), values.end());
    return values[(size_t) std::min ((double) values.size() - 1, fraction * (double) values.size())];
}

double percentOfCore (double cpuSeconds)
{
    return 100.0 * cpuSeconds / (numBlocks * blockSize / rate);
}

constexpr int chord[] = { 48, 55, 60, 64, 67 }; // held from the first block, for a synth

// A plug-in played in Phazite's process, on the thread that plays it.
struct Here
{
    explicit Here (std::unique_ptr<juce::AudioPluginInstance> pluginToPlay)
        : plugin (std::move (pluginToPlay))
    {
        REQUIRE (plugin != nullptr);
        plugin->disableNonMainBuses();
        plugin->setRateAndBufferSizeDetails (rate, blockSize);
        plugin->prepareToPlay (rate, blockSize);
        buffer.setSize (std::max (2, std::max (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels())), blockSize);
        midi.ensureSize (256);
    }

    ~Here() { plugin->releaseResources(); }

    void play (int block, bool isSynth)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), 0.1f, blockSize);

        midi.clear();

        if (block == 0 && isSynth)
            for (const auto note : chord)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

        plugin->processBlock (buffer, midi);
    }

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};

// A block for an instance's channel: 0.1 on both sides, and the chord on a synth's first.
void fillBlock (sandbox::BlockChannel& channel, int block, bool isSynth)
{
    auto& info = channel.getInfo();
    info.numSamples = blockSize;
    info.numChannels = 2;
    info.numEvents = 0;

    for (int i = 0; i < 2; ++i)
        juce::FloatVectorOperations::fill (channel.getChannel (i), 0.1f, blockSize);

    if (block == 0 && isSynth)
    {
        for (const auto note : chord)
        {
            auto& event = channel.getEvents()[info.numEvents++];
            event.sample = 0;
            event.size = 3;
            event.bytes[0] = 0x90;
            event.bytes[1] = (std::uint8_t) note;
            event.bytes[2] = 100;
        }
    }
}

struct Stats
{
    double cpuSeconds = 0.0;
    std::int64_t memory = 0;
};

Stats getStats (sandbox::Helper& helper)
{
    auto message = helper.makeRequest (sandbox::MessageType::stats);
    const auto answer = helper.request (message, 5000);
    REQUIRE (answer.has_value());
    sandbox::MessageReader reader (*answer);
    Stats stats;
    stats.cpuSeconds = reader.readDouble();
    stats.memory = reader.readInt64();
    return stats;
}

juce::String row (const juce::String& plugin, const juce::String& where, const Measured& measured, const juce::String& helperCpu,
                  const juce::String& memory)
{
    return "| " + plugin + " | " + where + " | " + juce::String (percentile (measured.microseconds, 0.5), 1) + " | "
           + juce::String (percentile (measured.microseconds, 0.99), 1) + " | "
           + juce::String (*std::max_element (measured.microseconds.begin(), measured.microseconds.end()), 0) + " | "
           + juce::String (percentOfCore (measured.cpuSeconds), 2) + " | " + helperCpu + " | " + memory + " |\n";
}

juce::String megabytes (std::int64_t bytes)
{
    return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
}

// A plug-in to measure: how to make it here, and which it is for the helper.
struct Case
{
    juce::String name;
    std::function<std::unique_ptr<juce::AudioPluginInstance>()> makeHere;
    sandbox::RemoteInstance::Plugin plugin;
    bool isSynth = false;
};

std::vector<Case> getCases (juce::VST3PluginFormat& vst3)
{
    std::vector<Case> cases;
    cases.push_back ({ "Test Effect (a gain)",
                       [] { return phazite::test::TestPlugins::createEffect(); },
                       { phazite::test::TestPlugins::formatName, phazite::test::TestPlugins::effectID, "Test Effect" },
                       false });

    const juce::File folder ("C:\\Program Files\\Common Files\\VST3\\Surge Synth Team");

    for (const auto& [file, isSynth] :
         { std::pair (juce::String ("Surge XT Effects.vst3"), false), std::pair (juce::String ("Surge XT.vst3"), true) })
    {
        const auto path = folder.getChildFile (file);

        if (! path.exists())
            continue;

        juce::OwnedArray<juce::PluginDescription> types;
        vst3.findAllTypesForFile (types, path.getFullPathName());

        if (types.isEmpty())
            continue;

        const auto description = *types[0];
        cases.push_back ({ description.name,
                           [&vst3, description]
                           {
                               juce::String error;
                               return vst3.createInstanceFromDescription (description, rate, blockSize, error);
                           },
                           { "VST3", path.getFullPathName(), description.name },
                           isSynth });
    }

    return cases;
}
} // namespace

TEST_CASE ("Benchmark: a sandboxed plug-in's round trip at 128 samples" * doctest::test_suite ("benchmark") * doctest::skip())
{
    juce::VST3PluginFormat vst3;
    juce::String report (
        "\n| Plug-in | Where | Median (us) | 99th % (us) | Worst (us) | CPU here (% of a core) | CPU in helper | Helper's memory |\n"
        "|---|---|---|---|---|---|---|---|\n");

    for (const auto& c : getCases (vst3))
    {
        // In Phazite's process: the plug-in played on this thread.
        {
            Here here (c.makeHere());
            const auto measured = measure ([&] (int block) { here.play (block, c.isSynth); });
            report << row (c.name, "Phazite's process", measured, "-", "-");
        }

        // In a helper, waiting asleep at once, spinning 30 microseconds first, and spinning until it answers.
        for (const auto spin : { (std::int64_t) 0, (std::int64_t) 30'000, (std::int64_t) 100'000'000 })
        {
            sandbox::Helper helper (juce::File (PHAZITE_TEST_HOST));
            REQUIRE (helper.start (20000));
            const auto empty = getStats (helper);

            sandbox::BlockChannel::Limits limits;
            limits.maxSamples = blockSize;
            juce::String error;
            auto instance = sandbox::RemoteInstance::open (helper, c.plugin, rate, limits, error, 60000);
            REQUIRE_MESSAGE (instance != nullptr, error);
            const auto before = getStats (helper);
            auto& channel = instance->getChannel();
            auto late = 0;
            auto longestLate = 0.0; // ms

            const auto measured = measure (
                [&] (int block)
                {
                    fillBlock (channel, block, c.isSynth);

                    // A block answered late (past 100 ms) is counted, and waited for.
                    if (channel.play (spin, 100'000'000) != sandbox::BlockChannel::Played::done)
                    {
                        ++late;
                        const auto start = sandbox::getNanoseconds();

                        while (channel.isBusy() && helper.isRunning())
                            std::this_thread::yield();

                        REQUIRE (helper.isRunning());

                        longestLate = std::max (longestLate, (double) (sandbox::getNanoseconds() - start) * 1.0e-6 + 100.0);
                    }
                });

            const auto after = getStats (helper);
            auto where = spin == 0 ? juce::String ("A helper, asleep at once")
                                   : (spin < 1'000'000 ? juce::String ("A helper, spinning 30 us")
                                                       : juce::String ("A helper, spinning until it answers"));

            if (late > 0)
                where << " (" << late << " late, the longest " << juce::String (longestLate, 0) << " ms)";

            report << row (c.name, where, measured, juce::String (percentOfCore (after.cpuSeconds - before.cpuSeconds), 2) + " %",
                           megabytes (empty.memory) + " empty, " + megabytes (after.memory) + " with it");
        }
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: eight sandboxed synths in a block at 128 samples" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Surge XT where it is installed (the last case), else the stand-in.
    juce::VST3PluginFormat vst3;
    const auto cases = getCases (vst3);
    const auto& c = cases.back();
    constexpr int count = 8;

    juce::String report ("\n| Eight of " + c.name
                         + " | Median (us) | 99th % (us) | Worst (us) | CPU here (% of a core) | CPU in helpers | Helpers' memory |\n"
                           "|---|---|---|---|---|---|---|\n");
    const auto addRow =
        [&report] (const juce::String& where, const Measured& measured, const juce::String& helperCpu, const juce::String& memory)
    {
        report << "| " << where << " | " << juce::String (percentile (measured.microseconds, 0.5), 1) << " | "
               << juce::String (percentile (measured.microseconds, 0.99), 1) << " | "
               << juce::String (*std::max_element (measured.microseconds.begin(), measured.microseconds.end()), 0) << " | "
               << juce::String (percentOfCore (measured.cpuSeconds), 2) << " | " << helperCpu << " | " << memory << " |\n";
    };

    // In Phazite's process, one after another on this thread.
    {
        std::vector<std::unique_ptr<Here>> plugins;

        for (int i = 0; i < count; ++i)
            plugins.push_back (std::make_unique<Here> (c.makeHere()));

        const auto measured = measure (
            [&] (int block)
            {
                for (auto& plugin : plugins)
                    plugin->play (block, c.isSynth);
            });

        addRow ("Phazite's process, one after another", measured, "-", "-");

        // On the engine's workers, side by side as its strips play: the whole process's CPU, in the column of the helpers'.
        phazite::engine::Workers workers;
        workers.setNumThreads (count);
        struct Job
        {
            std::vector<std::unique_ptr<Here>>* plugins;
            int block;
            bool isSynth;
        } job { &plugins, 0, c.isSynth };

        const auto processBefore = phazite::test::getProcessCpuSeconds();
        const auto onWorkers = measure (
            [&] (int block)
            {
                job.block = block;
                workers.run (
                    count,
                    [] (void* context, int index) noexcept
                    {
                        auto& running = *static_cast<Job*> (context);
                        (*running.plugins)[(size_t) index]->play (running.block, running.isSynth);
                    },
                    &job);
            });

        addRow ("Phazite's process, on the engine's workers", onWorkers,
                juce::String (percentOfCore (phazite::test::getProcessCpuSeconds() - processBefore), 2) + " % (all its threads)", "-");
    }

    // In helpers: one each, or all in one; each waited for in turn, or all handed over first and then waited for.
    for (const auto shared : { false, true })
    {
        for (const auto together : { false, true })
        {
            std::vector<std::unique_ptr<sandbox::Helper>> helpers;

            for (int i = 0; i < (shared ? 1 : count); ++i)
            {
                helpers.push_back (std::make_unique<sandbox::Helper> (juce::File (PHAZITE_TEST_HOST)));
                REQUIRE (helpers.back()->start (20000));
            }

            std::vector<std::unique_ptr<sandbox::RemoteInstance>> instances;
            sandbox::BlockChannel::Limits limits;
            limits.maxSamples = blockSize;

            for (int i = 0; i < count; ++i)
            {
                juce::String error;
                instances.push_back (
                    sandbox::RemoteInstance::open (*helpers[shared ? 0 : (size_t) i], c.plugin, rate, limits, error, 60000));
                REQUIRE_MESSAGE (instances.back() != nullptr, error);
            }

            const auto sum = [&helpers]
            {
                Stats total;

                for (auto& helper : helpers)
                {
                    const auto stats = getStats (*helper);
                    total.cpuSeconds += stats.cpuSeconds;
                    total.memory += stats.memory;
                }

                return total;
            };

            const auto before = sum();
            const auto measured = measure (
                [&] (int block)
                {
                    for (auto& instance : instances)
                        fillBlock (instance->getChannel(), block, c.isSynth);

                    if (together)
                    {
                        for (auto& instance : instances)
                            REQUIRE (instance->getChannel().handOver());

                        for (auto& instance : instances)
                            REQUIRE (instance->getChannel().waitForAnswer (0, 1'000'000'000) == sandbox::BlockChannel::Played::done);
                    }
                    else
                    {
                        for (auto& instance : instances)
                            REQUIRE (instance->getChannel().play (0, 1'000'000'000) == sandbox::BlockChannel::Played::done);
                    }
                });

            const auto after = sum();
            const auto where = juce::String (shared ? "One helper with all eight" : "A helper each")
                               + (together ? ", handed over first, then waited for" : ", each waited for in turn");
            addRow (where, measured, juce::String (percentOfCore (after.cpuSeconds - before.cpuSeconds), 2) + " %",
                    megabytes (after.memory));
            instances.clear();
        }
    }

    MESSAGE (report);
}
