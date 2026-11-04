// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DrumKit.h"
#include "devices/drums/FactoryDrums.h"
#include "internal/DeviceFiles.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"

namespace phazite
{
namespace
{
    using devices::DrumSampler;

    constexpr double longestSound = 30.0; // seconds of a file a pad plays at most

    // A sound file's first seconds, in mono or stereo, or nullptr if it can't be read.
    std::unique_ptr<DrumSampler::Sound> readSound (const juce::File& file)
    {
        const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

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

    // The factory sound a pad's sounds name, or the pad's own in the default kit.
    int getFactorySound (const juce::ValueTree& sounds, int pad)
    {
        const auto index = devices::findFactoryDrum (sounds[schema::getPadSound (pad)].toString().toStdString());
        return index >= 0 ? index : pad;
    }

    // A drum sampler's sounds in the song, if the device is one and the pad is one of its pads.
    juce::ValueTree findPadSounds (const Song& song, DeviceID id, int pad)
    {
        const auto device = findDeviceState (song, id);
        return isDrumSampler (device) && juce::isPositiveAndBelow (pad, DrumSampler::numPads) ? device.getChildWithName (schema::SOUNDS)
                                                                                              : juce::ValueTree();
    }

    juce::File getPadFile (const Song& song, const juce::ValueTree& sounds, int pad)
    {
        return findProjectFile (song, sounds[schema::getPadSoundFile (pad)].toString());
    }
} // namespace

bool isDrumSampler (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::DEVICE) && deviceState[schema::deviceType].toString() == DrumSampler::typeID;
}

bool isDrumSampler (const Song& song, DeviceID id)
{
    return isDrumSampler (findDeviceState (song, id));
}

juce::String getPadName (const Song& song, const juce::ValueTree& deviceState, int pad)
{
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);

    if (const auto file = getPadFile (song, sounds, pad); file != juce::File())
        return file.getFileNameWithoutExtension();

    // Its factory sound, or else the default kit's for the pad.
    const auto name = devices::getFactoryDrumNames()[(size_t) getFactorySound (sounds, pad)];
    return juce::String (name.data(), name.size());
}

juce::String getPadName (const Song& song, DeviceID id, int pad)
{
    const auto device = findDeviceState (song, id);
    return isDrumSampler (device) && juce::isPositiveAndBelow (pad, DrumSampler::numPads) ? getPadName (song, device, pad) : juce::String();
}

bool hasPadSound (const Song& song, DeviceID id, int pad)
{
    const auto sounds = findPadSounds (song, id, pad);
    return sounds.isValid() && getPadFile (song, sounds, pad) != juce::File();
}

bool setPadSound (Song& song, DeviceID id, int pad, const juce::File& file)
{
    if (! isDrumSampler (song, id) || ! juce::isPositiveAndBelow (pad, DrumSampler::numPads) || ! file.existsAsFile()
        || getFileKind (file) != FileKind::audio)
        return false;

    const auto copy = copyIntoProject (song, file);
    const UndoStep step (song, "Put " + copy.getFileNameWithoutExtension() + " on Pad " + juce::String (pad + 1));
    setDeviceSound (song, id, schema::getPadSoundFile (pad), getProjectPath (song, copy));
    return true;
}

void clearPadSound (Song& song, DeviceID id, int pad)
{
    if (! hasPadSound (song, id, pad))
        return;

    const UndoStep step (song, "Reset Pad " + juce::String (pad + 1));
    setDeviceSound (song, id, schema::getPadSoundFile (pad), {});
}

void loadPadSound (DrumSampler& sampler, const Song& song, const juce::ValueTree& sounds, int pad)
{
    // A file that is gone or can't be read leaves the pad with its factory sound.
    if (const auto file = getPadFile (song, sounds, pad); file.existsAsFile())
    {
        if (auto sound = readSound (file))
        {
            sampler.setSound (pad, std::move (sound));
            return;
        }
    }

    const auto index = getFactorySound (sounds, pad);
    sampler.setSound (pad, index != pad ? devices::getFactoryDrum (index) : nullptr);
}
} // namespace phazite
