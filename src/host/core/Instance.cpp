// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Instance.h"
#include "devices/core/AudioThreads.h"

#include <array>

namespace phazite::host
{
namespace
{
    constexpr std::int64_t checkForExitNanoseconds = 100'000'000; // how long the thread sleeps before it checks it should end
    constexpr int maximumChannels = 32;                           // a plug-in's channels the thread plays at most
} // namespace

Instance::Instance (std::unique_ptr<juce::AudioPluginInstance> pluginToHost, std::unique_ptr<sandbox::BlockChannel> blockChannel,
                    double sampleRate)
    : juce::Thread ("Phazite plug-in"),
      plugin (std::move (pluginToHost)),
      channel (std::move (blockChannel))
{
    const auto& limits = channel->getLimits();

    // Its main buses alone (a synth's other outputs aren't played), at the channel's longest block.
    plugin->disableNonMainBuses();
    plugin->setRateAndBufferSizeDetails (sampleRate, limits.maxSamples);
    plugin->prepareToPlay (sampleRate, limits.maxSamples);

    numPluginChannels = std::min (maximumChannels, std::max (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()));
    extra.setSize (std::max (1, numPluginChannels - limits.maxChannels), limits.maxSamples);
    midi.ensureSize ((size_t) limits.maxEvents * 8);

    devices::startAudioThread (*this);
}

Instance::~Instance()
{
    signalThreadShouldExit();
    stopThread (2000);
    plugin->releaseResources();
}

void Instance::run()
{
    const devices::ProAudioTask proAudio;

    while (! threadShouldExit())
    {
        if (! channel->waitForBlock (checkForExitNanoseconds))
            continue;

        play();
        channel->finish();
    }
}

void Instance::play() noexcept
{
    const auto& info = channel->getInfo();
    const auto& limits = channel->getLimits();
    const auto numSamples = juce::jlimit (0, limits.maxSamples, info.numSamples);

    // The channel's audio in place; any channels of the plug-in's beyond what came, cleared.
    std::array<float*, maximumChannels> channels {};
    const auto numChannels = juce::jlimit (0, limits.maxChannels, info.numChannels);

    for (int i = 0; i < numPluginChannels; ++i)
    {
        channels[(size_t) i] =
            i < limits.maxChannels ? channel->getChannel (i) : extra.getWritePointer ((i - limits.maxChannels) % extra.getNumChannels());

        if (i >= numChannels)
            juce::FloatVectorOperations::clear (channels[(size_t) i], numSamples);
    }

    // Its MIDI, at their samples.
    midi.clear();
    const auto* events = channel->getEvents();

    for (int i = 0; i < juce::jlimit (0, limits.maxEvents, info.numEvents); ++i)
        if (events[i].size > 0 && events[i].size <= 3)
            midi.addEvent (events[i].bytes, events[i].size, juce::jlimit (0, std::max (0, numSamples - 1), events[i].sample));

    juce::AudioBuffer<float> buffer (channels.data(), numPluginChannels, numSamples);
    plugin->processBlock (buffer, midi);
}
} // namespace phazite::host
