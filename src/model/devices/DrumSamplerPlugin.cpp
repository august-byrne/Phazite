// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DrumSamplerPlugin.h"
#include "devices/drums/FactoryDrums.h"
#include "internal/DeviceFiles.h"
#include "model/base/internal/Schema.h"

namespace phazite
{
namespace
{
    using devices::DrumSampler;

    constexpr double longestSound = 30.0; // seconds of a file a pad plays at most

    // A sound file's first seconds, in mono or stereo, or nullptr if it can't be read.
    std::unique_ptr<DrumSampler::Sound> readSound (te::Engine& engine, const juce::File& file)
    {
        const std::unique_ptr<juce::AudioFormatReader> reader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return {};

        auto sound = std::make_unique<DrumSampler::Sound>();
        const auto length = (int) std::min (reader->lengthInSamples, (juce::int64) (longestSound * reader->sampleRate));
        sound->audio.setSize (juce::jlimit (1, 2, (int) reader->numChannels), length);
        sound->sampleRate = reader->sampleRate;

        if (! reader->read (&sound->audio, 0, length, 0, true, true))
            return {};

        return sound;
    }
} // namespace

DrumSamplerPlugin::DrumSamplerPlugin (te::PluginCreationInfo creation)
    : DevicePlugin (creation, DrumSampler::info, std::make_unique<DrumSampler>())
{
    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
        loadPad (pad);
}

DrumSampler& DrumSamplerPlugin::getSampler() noexcept
{
    return static_cast<DrumSampler&> (getProcessor());
}

juce::File DrumSamplerPlugin::getPadFile (int pad) const
{
    return findProjectFile (edit, state[schema::getPadSoundFile (pad)].toString());
}

int DrumSamplerPlugin::getFactorySound (int pad) const
{
    const auto index = devices::findFactoryDrum (state[schema::getPadSound (pad)].toString().toStdString());
    return index >= 0 ? index : pad;
}

juce::String DrumSamplerPlugin::getPadName (int pad) const
{
    if (const auto file = getPadFile (pad); file != juce::File())
        return file.getFileNameWithoutExtension();

    const auto name = devices::getFactoryDrumNames()[(size_t) getFactorySound (pad)];
    return juce::String (name.data(), name.size());
}

void DrumSamplerPlugin::loadPad (int pad)
{
    auto& sampler = getSampler();

    // A file that is gone or can't be read leaves the pad with its factory sound.
    if (const auto file = getPadFile (pad); file.existsAsFile())
    {
        if (auto sound = readSound (edit.engine, file))
        {
            sampler.setSound (pad, std::move (sound));
            return;
        }
    }

    const auto index = getFactorySound (pad);
    sampler.setSound (pad, index != pad ? devices::getFactoryDrum (index) : nullptr);
}

void DrumSamplerPlugin::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    DevicePlugin::valueTreePropertyChanged (tree, property);

    if (tree == state)
        for (int pad = 0; pad < DrumSampler::numPads; ++pad)
            if (property == schema::getPadSound (pad) || property == schema::getPadSoundFile (pad))
                loadPad (pad);
}

void DrumSamplerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& source)
{
    DevicePlugin::restorePluginStateFromValueTree (source);

    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
    {
        for (const auto& property : { schema::getPadSound (pad), schema::getPadSoundFile (pad) })
        {
            if (source.hasProperty (property))
                state.setProperty (property, source[property], nullptr);
            else
                state.removeProperty (property, nullptr);
        }
    }
}
} // namespace phazite
