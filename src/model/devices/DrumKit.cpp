// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DrumKit.h"
#include "devices/drums/FactoryDrums.h"
#include "internal/DeviceFiles.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"

namespace phazite
{
namespace
{
    using devices::DrumSampler;
    static_assert (schema::drumPads == DrumSampler::numPads && schema::drumLayers == DrumSampler::numLayers);

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

    // The factory sound a pad's layer's sounds name, or else the pad's own in the default kit (its first layer's): -1 for
    // none, off the kit's sixteen pads.
    int getFactorySound (const juce::ValueTree& sounds, int pad, int layer)
    {
        if (const auto named = devices::findFactoryDrum (sounds[schema::getPadSound (pad, layer)].toString().toStdString()); named >= 0)
            return named;

        const auto inKit = pad - DrumSampler::kitFirstNote;
        return layer == 0 && juce::isPositiveAndBelow (inKit, DrumSampler::kitPads) ? inKit : -1;
    }

    // The factory sound a pad's layer's sounds name, -1 for none: what the sampler is given rather than playing its default.
    int getNamedFactorySound (const juce::ValueTree& sounds, int pad, int layer)
    {
        return devices::findFactoryDrum (sounds[schema::getPadSound (pad, layer)].toString().toStdString());
    }

    bool isPadLayer (int pad, int layer)
    {
        return juce::isPositiveAndBelow (pad, DrumSampler::numPads) && juce::isPositiveAndBelow (layer, DrumSampler::numLayers);
    }

    // A drum sampler's sounds in the song, if the device is one and the pad's layer is one of its.
    juce::ValueTree findPadSounds (const Song& song, DeviceID id, int pad, int layer)
    {
        const auto device = findDeviceState (song, id);
        return isDrumSampler (device) && isPadLayer (pad, layer) ? device.getChildWithName (schema::SOUNDS) : juce::ValueTree();
    }

    juce::File getPadFile (const Song& song, const juce::ValueTree& sounds, int pad, int layer)
    {
        return findProjectFile (song, sounds[schema::getPadSoundFile (pad, layer)].toString());
    }

    // "Pad 36", or "Pad 36's Layer 2".
    juce::String describePadLayer (int pad, int layer)
    {
        return "Pad " + juce::String (pad) + (layer > 0 ? "'s Layer " + juce::String (layer + 1) : juce::String());
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

juce::String getPadName (const Song& song, const juce::ValueTree& deviceState, int pad, int layer)
{
    if (! isPadLayer (pad, layer))
        return {};

    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);

    if (const auto file = getPadFile (song, sounds, pad, layer); file != juce::File())
        return file.getFileNameWithoutExtension();

    // Its factory sound, or else the default kit's for the pad; nothing for a layer without one.
    const auto index = getFactorySound (sounds, pad, layer);

    if (index < 0)
        return {};

    const auto name = devices::getFactoryDrumNames()[(size_t) index];
    return juce::String (name.data(), name.size());
}

juce::String getPadName (const Song& song, DeviceID id, int pad, int layer)
{
    const auto device = findDeviceState (song, id);
    return isDrumSampler (device) ? getPadName (song, device, pad, layer) : juce::String();
}

bool hasPadSound (const Song& song, DeviceID id, int pad, int layer)
{
    const auto sounds = findPadSounds (song, id, pad, layer);
    return sounds.isValid() && getPadFile (song, sounds, pad, layer) != juce::File();
}

bool isPadEmpty (const Song& song, const juce::ValueTree& deviceState, int pad)
{
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);

    for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
        if (getPadFile (song, sounds, pad, layer) != juce::File() || getFactorySound (sounds, pad, layer) >= 0)
            return false;

    return true;
}

bool isPadEmpty (const Song& song, DeviceID id, int pad)
{
    const auto device = findDeviceState (song, id);
    return ! isDrumSampler (device) || ! juce::isPositiveAndBelow (pad, DrumSampler::numPads) || isPadEmpty (song, device, pad);
}

bool setPadSound (Song& song, DeviceID id, int pad, const juce::File& file, int layer)
{
    if (! isDrumSampler (song, id) || ! isPadLayer (pad, layer) || ! file.existsAsFile() || getFileKind (file) != FileKind::audio)
        return false;

    const auto copy = copyIntoProject (song, file);
    const UndoStep step (song, "Put " + copy.getFileNameWithoutExtension() + " on " + describePadLayer (pad, layer));
    setDeviceSound (song, id, schema::getPadSoundFile (pad, layer), getProjectPath (song, copy));
    return true;
}

void clearPadSound (Song& song, DeviceID id, int pad, int layer)
{
    // The first layer's file, which leaves the pad its kit's sound; another layer's file and factory sound, which leave it
    // empty.
    const auto sounds = findPadSounds (song, id, pad, layer);

    if (! sounds.isValid() || (layer == 0 ? ! hasPadSound (song, id, pad) : getPadName (song, id, pad, layer).isEmpty()))
        return;

    const UndoStep step (song, (layer == 0 ? "Reset " : "Clear ") + describePadLayer (pad, layer));
    setDeviceSound (song, id, schema::getPadSoundFile (pad, layer), {});

    if (layer > 0)
        setDeviceSound (song, id, schema::getPadSound (pad, layer), {});
}

void loadPadSound (DrumSampler& sampler, const Song& song, const juce::ValueTree& sounds, int pad)
{
    // Each layer's file, or a file that is gone or can't be read leaves it its factory sound.
    for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
    {
        if (const auto file = getPadFile (song, sounds, pad, layer); file.existsAsFile())
        {
            if (auto sound = readSound (file))
            {
                sampler.setSound (pad, layer, std::move (sound));
                continue;
            }
        }

        const auto named = getNamedFactorySound (sounds, pad, layer);
        sampler.setSound (pad, layer, named >= 0 ? devices::getFactoryDrum (named) : nullptr);
    }
}

TrackID findPadTrack (const Song& song, DeviceID id, int pad)
{
    for (const auto& state : getTrackStates (song))
        if (state.hasProperty (schema::trackPad) && (int) state[schema::trackPad] == pad
            && (juce::int64) state[schema::trackPadDevice] == (juce::int64) id.getRaw())
            return getTrackID (state);

    return {};
}

std::optional<std::pair<DeviceID, int>> getPlayedPad (const Song& song, TrackID track)
{
    const auto state = findTrackState (song, track);

    if (! state.hasProperty (schema::trackPad))
        return std::nullopt;

    return std::pair (DeviceID::fromRaw ((std::uint64_t) (juce::int64) state[schema::trackPadDevice]), (int) state[schema::trackPad]);
}

TrackID givePadItsOwnTrack (Song& song, DeviceID id, int pad)
{
    if (! isDrumSampler (song, id) || ! juce::isPositiveAndBelow (pad, DrumSampler::numPads))
        return {};

    if (const auto existing = findPadTrack (song, id, pad); existing.isValid())
        return existing;

    const auto drums = getDeviceChannel (song, id);
    const auto drumState = findTrackState (song, drums);

    if (! drumState.isValid() || isMaster (song, drums))
        return {};

    // After the sampler's track, named after what the pad plays, in that track's colour where it has one of its own.
    const UndoStep step (song, "Give Pad " + juce::String (pad) + " Its Own Track");
    const auto track = insertTrack (song, drums, nullptr);
    auto state = findTrackState (song, track);

    if (! state.isValid())
        return {};

    const auto sound = getPadName (song, id, pad);
    setTrackName (song, track, sound.isNotEmpty() ? sound : "Pad " + juce::String (pad));

    if (drumState.hasProperty (schema::trackColour))
        state.setProperty (schema::trackColour, drumState[schema::trackColour], &song.getUndoManager());

    state.setProperty (schema::trackPadDevice, (juce::int64) id.getRaw(), &song.getUndoManager());
    state.setProperty (schema::trackPad, pad, &song.getUndoManager());
    return track;
}

void bringPadBack (Song& song, DeviceID id, int pad)
{
    if (const auto track = findPadTrack (song, id, pad); track.isValid())
    {
        const UndoStep step (song, "Bring Pad " + juce::String (pad) + " Back");
        deleteTracks (song, { track }, nullptr);
    }
}

bool hasPadsOnOwnTracks (const Song& song, TrackID track)
{
    for (const auto& state : getTrackStates (song))
        if (state.hasProperty (schema::trackPad)
            && getDeviceChannel (song, DeviceID::fromRaw ((std::uint64_t) (juce::int64) state[schema::trackPadDevice])) == track)
            return true;

    return false;
}
} // namespace phazite
