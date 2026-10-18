// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/WavetablePlugin.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "internal/DeviceFiles.h"
#include "model/base/internal/Schema.h"

namespace phazite
{
namespace
{
    using devices::WavetableSynth;

    // At most this much of a sound becomes a table: 256 frames of 8192 samples.
    constexpr juce::int64 longestSound = (juce::int64) devices::Wavetable::maxFrames * 8192;

    // The frame length a WAV file's 'clm ' chunk gives, as Serum writes it ("<!>2048" and more), or 0 if it has none.
    int findSerumFrameLength (const juce::File& file)
    {
        if (! file.hasFileExtension ("wav"))
            return 0;

        juce::FileInputStream in (file);
        char id[4] = {};

        if (! in.openedOk() || in.read (id, 4) != 4 || juce::String (id, 4) != "RIFF")
            return 0;

        in.readInt(); // the file's length

        if (in.read (id, 4) != 4 || juce::String (id, 4) != "WAVE")
            return 0;

        while (! in.isExhausted())
        {
            if (in.read (id, 4) != 4)
                return 0;

            const auto size = (juce::int64) (juce::uint32) in.readInt();

            if (juce::String (id, 4) != "clm ")
            {
                in.skipNextBytes (size + (size & 1)); // chunks are padded to even lengths
                continue;
            }

            juce::MemoryBlock text;
            in.readIntoMemoryBlock (text, std::min (size, (juce::int64) 64));
            const auto value = text.toString().fromFirstOccurrenceOf ("<!>", false, false).initialSectionContainingOnly ("0123456789");
            return value.isEmpty() ? 0 : value.getIntValue();
        }

        return 0;
    }
} // namespace

std::shared_ptr<const devices::Wavetable> readWavetable (te::Engine& engine, const juce::File& file)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return {};

    const auto length = (int) std::min (reader->lengthInSamples, longestSound);
    const auto channels = (int) std::min (reader->numChannels, 2u);
    juce::AudioBuffer<float> audio (channels, length);

    if (! reader->read (&audio, 0, length, 0, true, channels > 1))
        return {};

    std::vector<float> mono ((size_t) length, 0.0f);

    for (int channel = 0; channel < channels; ++channel)
        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += audio.getSample (channel, i) / (float) channels;

    return devices::Wavetable::fromSound (file.getFileNameWithoutExtension(), mono, findSerumFrameLength (file));
}

WavetablePlugin::WavetablePlugin (te::PluginCreationInfo creation)
    : DevicePlugin (creation, WavetableSynth::info, std::make_unique<WavetableSynth>())
{
    for (int osc = 0; osc < WavetableSynth::numOscillators; ++osc)
        loadTable (osc);
}

devices::WavetableSynth& WavetablePlugin::getSynth() noexcept
{
    return static_cast<WavetableSynth&> (getProcessor());
}

juce::File WavetablePlugin::getTableFile (int osc) const
{
    return findProjectFile (edit, state[schema::getOscillatorWavetableFile (osc)].toString());
}

juce::String WavetablePlugin::getTableName (int osc) const
{
    if (const auto file = getTableFile (osc); file != juce::File())
        return file.getFileNameWithoutExtension();

    const auto names = devices::getFactoryWavetableNames();
    const auto index = devices::findFactoryWavetable (state[schema::getOscillatorWavetable (osc)].toString().toStdString());
    const auto name = names[(size_t) std::max (0, index)];
    return juce::String (name.data(), name.size());
}

void WavetablePlugin::loadTable (int osc)
{
    auto& synth = getSynth();

    // A file that is gone or can't be read leaves the oscillator on its factory table.
    if (const auto file = getTableFile (osc); file.existsAsFile())
    {
        if (auto table = readWavetable (edit.engine, file))
        {
            synth.setTable (osc, std::move (table));
            return;
        }
    }

    const auto index = devices::findFactoryWavetable (state[schema::getOscillatorWavetable (osc)].toString().toStdString());
    synth.setTable (osc, devices::getFactoryWavetable (std::max (0, index)));
}

void WavetablePlugin::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    DevicePlugin::valueTreePropertyChanged (tree, property);

    if (tree == state)
        for (int osc = 0; osc < WavetableSynth::numOscillators; ++osc)
            if (property == schema::getOscillatorWavetable (osc) || property == schema::getOscillatorWavetableFile (osc))
                loadTable (osc);
}

void WavetablePlugin::restorePluginStateFromValueTree (const juce::ValueTree& source)
{
    DevicePlugin::restorePluginStateFromValueTree (source);

    for (int osc = 0; osc < WavetableSynth::numOscillators; ++osc)
    {
        for (const auto* property : { &schema::getOscillatorWavetable (osc), &schema::getOscillatorWavetableFile (osc) })
        {
            if (source.hasProperty (*property))
                state.setProperty (*property, source[*property], nullptr);
            else
                state.removeProperty (*property, nullptr);
        }
    }
}
} // namespace phazite
