// SPDX-License-Identifier: AGPL-3.0-or-later
// The sandbox's parts (src/sandbox/; docs/tracktion-migration.md, T3): a block channel's round trip, its deadline and a
// block the helper answers late, and the tests' helper program, started for real, playing a block through the tests'
// stand-in effect and lost when it is ended.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "host/TestPlugins.h"
#include "sandbox/client/Helper.h"
#include "sandbox/client/RemoteInstance.h"
#include "sandbox/core/BlockChannel.h"

#include <atomic>
#include <cmath>
#include <thread>

namespace
{
namespace sandbox = phazite::sandbox;

// A name for a channel no other test uses at the same time.
std::string makeChannelName()
{
    static std::atomic<int> count { 0 };
    return "t" + std::to_string (juce::Time::currentTimeMillis() % 1000000) + "-" + std::to_string (++count);
}

sandbox::BlockChannel::Limits makeLimits()
{
    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = 512;
    return limits;
}

juce::File getTestHost()
{
    return juce::File (PHAZITE_TEST_HOST);
}

sandbox::RemoteInstance::Plugin toPlugin (const juce::PluginDescription& description)
{
    return { description.pluginFormatName, description.fileOrIdentifier, description.name };
}
} // namespace

TEST_CASE ("A block channel hands a block over and gets it back played, one round trip")
{
    const auto name = makeChannelName();
    auto engineSide = sandbox::BlockChannel::create (name, makeLimits());
    REQUIRE (engineSide != nullptr);
    auto helperSide = sandbox::BlockChannel::open (name, makeLimits());
    REQUIRE (helperSide != nullptr);

    // The helper doubles what comes in.
    std::atomic<bool> stop { false };
    std::thread helper (
        [&]
        {
            while (! stop)
            {
                if (! helperSide->waitForBlock (10'000'000))
                    continue;

                const auto& info = helperSide->getInfo();

                for (int channel = 0; channel < info.numChannels; ++channel)
                    juce::FloatVectorOperations::multiply (helperSide->getChannel (channel), 2.0f, info.numSamples);

                helperSide->finish();
            }
        });

    for (int block = 0; block < 100; ++block)
    {
        auto& info = engineSide->getInfo();
        info.numSamples = 128;
        info.numChannels = 2;

        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (engineSide->getChannel (channel), (float) block, 128);

        REQUIRE (engineSide->play (20'000, 1'000'000'000) == sandbox::BlockChannel::Played::done);
        CHECK (engineSide->getChannel (1)[127] == doctest::Approx (2.0f * (float) block));
    }

    stop = true;
    helper.join();
}

TEST_CASE ("A block answered late stays the helper's: the next finds the channel busy until it is answered")
{
    const auto name = makeChannelName();
    auto engineSide = sandbox::BlockChannel::create (name, makeLimits());
    auto helperSide = sandbox::BlockChannel::open (name, makeLimits());
    REQUIRE (engineSide != nullptr);
    REQUIRE (helperSide != nullptr);

    std::atomic<bool> release { false };
    std::thread helper (
        [&]
        {
            REQUIRE (helperSide->waitForBlock (-1));

            while (! release)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));

            helperSide->finish();
        });

    engineSide->getInfo().numSamples = 128;
    CHECK (engineSide->play (0, 2'000'000) == sandbox::BlockChannel::Played::late); // 2 ms
    CHECK (engineSide->isBusy());
    CHECK (engineSide->play (0, 2'000'000) == sandbox::BlockChannel::Played::busy);

    release = true;
    helper.join();
    CHECK_FALSE (engineSide->isBusy());
}

TEST_CASE ("The helper program starts, plays a block through a plug-in it hosts, and is lost when it is ended")
{
    sandbox::Helper helper (getTestHost());
    std::atomic<int> losses { 0 };
    helper.onLost = [&losses] { ++losses; };
    REQUIRE (helper.start (20000));
    CHECK (helper.isRunning());

    // The tests' stand-in effect, through a channel of its own: at its starting gain, its sound as it came.
    const auto limits = makeLimits();
    juce::String error;
    auto instance = sandbox::RemoteInstance::open (helper, toPlugin (phazite::test::TestPlugins::getEffectDescription()), 48000.0, limits,
                                                   error, 10000);
    REQUIRE_MESSAGE (instance != nullptr, error);
    CHECK (instance->getLatencySamples() == 0);

    auto& channel = instance->getChannel();
    auto& info = channel.getInfo();
    info.numSamples = 128;
    info.numChannels = 2;

    for (int i = 0; i < 128; ++i)
        channel.getChannel (0)[i] = channel.getChannel (1)[i] = std::sin ((float) i * 0.1f);

    REQUIRE (channel.play (50'000, 5'000'000'000) == sandbox::BlockChannel::Played::done);
    CHECK (channel.getChannel (0)[50] == doctest::Approx (std::sin (5.0f)));

    // The pipe stays up while nothing goes through it, as while a song plays (a pipe's read that timed out once ended it).
    juce::Thread::sleep (6000);
    CHECK (helper.isRunning());
    REQUIRE (channel.play (50'000, 5'000'000'000) == sandbox::BlockChannel::Played::done);

    // What it uses, as the measurement reads it.
    auto stats = helper.makeRequest (sandbox::MessageType::stats);
    const auto answer = helper.request (stats, 5000);
    REQUIRE (answer.has_value());
    sandbox::MessageReader reader (*answer);
    CHECK (reader.readDouble() > 0.0);   // CPU seconds
    CHECK (reader.readInt64() > 100000); // bytes of its own

    // A plug-in it doesn't have.
    const auto refused = sandbox::RemoteInstance::open (helper, { phazite::test::TestPlugins::formatName, "NoSuchPlugin", "Nothing" },
                                                        48000.0, limits, error, 10000);
    CHECK (refused == nullptr);
    CHECK (error.isNotEmpty());

    // Ended, as a hang asks: lost once.
    instance = nullptr;
    helper.kill();
    CHECK_FALSE (helper.isRunning());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&losses] { return losses.load() > 0; }, 10000));
    CHECK (losses.load() == 1);
}
