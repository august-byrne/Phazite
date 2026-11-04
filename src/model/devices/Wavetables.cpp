// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Wavetables.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "internal/DeviceFiles.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"

#include <map>
#include <optional>

namespace phazite
{
namespace
{
    using devices::WavetableSynth;

    juce::String getStepName (const juce::String& table, int osc)
    {
        return "Put " + table + " on Osc " + juce::String (osc + 1);
    }

    // A wavetable synth's sounds in the song (none while nothing was ever set: its defaults), if the device is one and the
    // oscillator is one of its oscillators.
    std::optional<juce::ValueTree> findOscillatorSounds (const Song& song, DeviceID id, int osc)
    {
        const auto device = findDeviceState (song, id);

        if (! device.hasType (schema::DEVICE) || device[schema::deviceType].toString() != WavetableSynth::typeID
            || ! juce::isPositiveAndBelow (osc, WavetableSynth::numOscillators))
            return {};

        return device.getChildWithName (schema::SOUNDS);
    }

    juce::File getTableFile (const Song& song, const juce::ValueTree& sounds, int osc)
    {
        return findProjectFile (song, sounds[schema::getOscillatorWavetableFile (osc)].toString());
    }

    int getFactoryTable (const juce::ValueTree& sounds, int osc)
    {
        return std::max (0, devices::findFactoryWavetable (sounds[schema::getOscillatorWavetable (osc)].toString().toStdString()));
    }

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

std::shared_ptr<const devices::Wavetable> readWavetable (const juce::File& file)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

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

bool isWavetableSynth (const Song& song, DeviceID id)
{
    return findOscillatorSounds (song, id, 0).has_value();
}

juce::String getWavetableName (const Song& song, DeviceID id, int osc)
{
    const auto sounds = findOscillatorSounds (song, id, osc);

    if (! sounds.has_value())
        return {};

    if (const auto file = getTableFile (song, *sounds, osc); file != juce::File())
        return file.getFileNameWithoutExtension();

    const auto name = devices::getFactoryWavetableNames()[(size_t) getFactoryTable (*sounds, osc)];
    return juce::String (name.data(), name.size());
}

bool hasWavetableFile (const Song& song, DeviceID id, int osc)
{
    const auto sounds = findOscillatorSounds (song, id, osc);
    return sounds.has_value() && getTableFile (song, *sounds, osc) != juce::File();
}

std::shared_ptr<const devices::Wavetable> getWavetable (const Song& song, DeviceID id, int osc)
{
    const auto sounds = findOscillatorSounds (song, id, osc);

    if (! sounds.has_value())
        return nullptr;

    // What the engine plays, while it plays the song.
    if (const auto* session = findPlayingSession (song))
        if (const auto* node = session->getPlayer().findDevice ((std::int64_t) id.getRaw()))
            if (const auto* synth = dynamic_cast<const WavetableSynth*> (&node->getRunner().getProcessor()))
                return synth->getTable (osc);

    // Else what its sounds name: a file read once while it stays the same, or a factory table.
    if (const auto file = getTableFile (song, *sounds, osc); file.existsAsFile())
    {
        static std::map<juce::String, std::pair<juce::Time, std::shared_ptr<const devices::Wavetable>>> read; // message thread
        auto& kept = read[file.getFullPathName()];

        if (kept.second == nullptr || kept.first != file.getLastModificationTime())
            kept = { file.getLastModificationTime(), readWavetable (file) };

        if (kept.second != nullptr)
            return kept.second;
    }

    return devices::getFactoryWavetable (getFactoryTable (*sounds, osc));
}

void setFactoryWavetable (Song& song, DeviceID id, int osc, int index)
{
    const auto names = devices::getFactoryWavetableNames();

    if (! isWavetableSynth (song, id) || ! juce::isPositiveAndBelow (osc, WavetableSynth::numOscillators)
        || ! juce::isPositiveAndBelow (index, (int) names.size()))
        return;

    const juce::String name (names[(size_t) index].data(), names[(size_t) index].size());

    if (! hasWavetableFile (song, id, osc) && getWavetableName (song, id, osc) == name)
        return;

    const UndoStep step (song, getStepName (name, osc));
    setDeviceSound (song, id, schema::getOscillatorWavetableFile (osc), {});
    setDeviceSound (song, id, schema::getOscillatorWavetable (osc), name);
}

bool setWavetableFile (Song& song, DeviceID id, int osc, const juce::File& file)
{
    if (! isWavetableSynth (song, id) || ! juce::isPositiveAndBelow (osc, WavetableSynth::numOscillators) || ! file.existsAsFile()
        || getFileKind (file) != FileKind::audio)
        return false;

    const auto copy = copyIntoProject (song, file);
    const UndoStep step (song, getStepName (copy.getFileNameWithoutExtension(), osc));
    setDeviceSound (song, id, schema::getOscillatorWavetableFile (osc), getProjectPath (song, copy));
    return true;
}

void loadOscillatorTable (WavetableSynth& synth, const Song& song, const juce::ValueTree& sounds, int osc)
{
    // A file that is gone or can't be read leaves the oscillator on its factory table.
    if (const auto file = getTableFile (song, sounds, osc); file.existsAsFile())
    {
        if (auto table = readWavetable (file))
        {
            synth.setTable (osc, std::move (table));
            return;
        }
    }

    synth.setTable (osc, devices::getFactoryWavetable (getFactoryTable (sounds, osc)));
}
} // namespace phazite
