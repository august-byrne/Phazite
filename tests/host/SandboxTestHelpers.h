// SPDX-License-Identifier: AGPL-3.0-or-later
// For tests of plug-ins in helpers (src/sandbox/, src/host/): the tests' helper program, the tests' stand-ins
// opened in one (TestPlugins.h), the helper's notices handed to them, and a block played through one. Included after
// TestHelpers.h, which brings doctest.
#pragma once

#include "TestPlugins.h"
#include "sandbox/client/Helper.h"
#include "sandbox/client/RemoteInstance.h"

#include <map>
#include <mutex>

namespace phazite::test
{
inline juce::File getTestHost()
{
    return juce::File (PHAZITE_TEST_HOST);
}

inline juce::String toXml (const juce::PluginDescription& description)
{
    return description.createXml()->toString (juce::XmlElement::TextFormat().withoutHeader().singleLine());
}

inline sandbox::BlockChannel::Limits makeLimits (int maxSamples = 512)
{
    sandbox::BlockChannel::Limits limits;
    limits.maxSamples = maxSamples;
    return limits;
}

// A helper started, whose notices go to the instances opened in it.
class TestHelper
{
public:
    TestHelper()
        : helper (getTestHost())
    {
        helper.onNotice = [this] (sandbox::MessageReader& notice)
        {
            const auto id = sandbox::RemoteInstance::readNoticeInstance (notice);
            const std::lock_guard held (lock);

            if (const auto found = instances.find (id); found != instances.end())
                found->second->handleNotice (notice.getType(), notice);
        };

        helper.onLost = [this] { ++losses; };
        REQUIRE (helper.start (20000));
    }

    sandbox::Helper& get() noexcept { return helper; }
    int getLosses() const noexcept { return losses.load(); }

    std::unique_ptr<sandbox::RemoteInstance> open (TestPlugins::Kind kind, const juce::MemoryBlock& state = {}, int maxSamples = 512,
                                                   double sampleRate = 48000.0)
    {
        juce::String error;
        auto instance = sandbox::RemoteInstance::open (helper, toXml (TestPlugins::getDescription (kind)), state, sampleRate,
                                                       makeLimits (maxSamples), error, 20000);
        REQUIRE_MESSAGE (instance != nullptr, error);
        const std::lock_guard held (lock);
        instances[instance->getID()] = instance.get();
        return instance;
    }

    // Before an instance goes.
    void forget (const sandbox::RemoteInstance& instance)
    {
        const std::lock_guard held (lock);
        instances.erase (instance.getID());
    }

private:
    sandbox::Helper helper;
    std::mutex lock;
    std::map<std::int64_t, sandbox::RemoteInstance*> instances;
    std::atomic<int> losses { 0 };
};

// A block of numSamples on both sides at a value, played through an instance's channel with a deadline.
inline sandbox::BlockChannel::Played playBlock (sandbox::RemoteInstance& instance, int numSamples, float value,
                                                std::int64_t timeoutNanoseconds = 5'000'000'000)
{
    auto& channel = instance.getChannel();
    auto& info = channel.getInfo();
    info.numSamples = numSamples;
    info.numChannels = 2;

    for (int i = 0; i < 2; ++i)
        juce::FloatVectorOperations::fill (channel.getChannel (i), value, numSamples);

    const auto played = channel.play (0, timeoutNanoseconds);
    info.numEvents = 0;
    info.numChanges = 0;
    info.numSidechainChannels = 0;
    return played;
}
} // namespace phazite::test
