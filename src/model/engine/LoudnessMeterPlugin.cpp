// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LoudnessMeterPlugin.h"
#include "model/base/Schema.h"

#include <array>

namespace phazite
{
const char* LoudnessMeterPlugin::xmlTypeName = schema::phaziteLoudnessMeter;

LoudnessMeterPlugin::LoudnessMeterPlugin (te::PluginCreationInfo info)
    : te::Plugin (info)
{
}

LoudnessMeterPlugin::~LoudnessMeterPlugin()
{
    notifyListenersOfDeletion();
}

juce::ValueTree LoudnessMeterPlugin::create()
{
    juce::ValueTree state (te::IDs::PLUGIN);
    state.setProperty (te::IDs::type, xmlTypeName, nullptr);
    return state;
}

void LoudnessMeterPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    // All the meter's allocation happens here, before the audio thread uses it: Tracktion calls this on the
    // message thread, when the sample rate or block size changes, not while processing.
    meter.prepare (info.sampleRate, 2, info.blockSizeSamples);
}

void LoudnessMeterPlugin::applyToBuffer (const te::PluginRenderContext& context)
{
    if (context.destBuffer == nullptr)
        return;

    // The block's part of each channel, on the stack.
    auto& buffer = *context.destBuffer;
    const auto numChannels = std::min (buffer.getNumChannels(), maxChannels);
    std::array<const float*, maxChannels> channels {};

    for (int channel = 0; channel < numChannels; ++channel)
        channels[(size_t) channel] = buffer.getReadPointer (channel, context.bufferStartSample);

    meter.process (channels.data(), numChannels, context.bufferNumSamples);
}

//==============================================================================
LoudnessMeterPlugin* findLoudnessMeter (const te::Edit& edit)
{
    return edit.getMasterPluginList().findFirstPluginOfType<LoudnessMeterPlugin>();
}

LoudnessMeterPlugin* ensureLoudnessMeter (te::Edit& edit)
{
    if (auto* existing = findLoudnessMeter (edit))
        return existing;

    auto plugin = edit.getPluginCache().createNewPlugin (LoudnessMeterPlugin::create());
    edit.getMasterPluginList().insertPlugin (plugin, -1, nullptr);
    return findLoudnessMeter (edit);
}
} // namespace phazite
