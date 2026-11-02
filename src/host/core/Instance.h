// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper (docs/tracktion-migration.md, T3): the instance JUCE's format loaded, prepared for its
// channel's blocks, and the thread that plays them, which waits on the channel's bell, plays the block in place and rings
// Phazite back. The thread is in MMCSS's Pro Audio task, as Phazite's own audio threads are (devices/core/AudioThreads.h).
#pragma once

#include "sandbox/core/BlockChannel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace phazite::host
{
class Instance final : private juce::Thread
{
public:
    // The plug-in, prepared at the sample rate for the channel's longest block; its thread starts at once.
    Instance (std::unique_ptr<juce::AudioPluginInstance>, std::unique_ptr<sandbox::BlockChannel>, double sampleRate);
    ~Instance() override;

    juce::AudioPluginInstance& getPlugin() noexcept { return *plugin; }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<sandbox::BlockChannel> channel;
    juce::AudioBuffer<float> extra; // channels the plug-in has beyond the channel's: made once, in prepare
    juce::MidiBuffer midi;          // a block's MIDI, its room made once
    int numPluginChannels = 0;

    void run() override;
    void play() noexcept;
};
} // namespace phazite::host
