// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceFiles.h"
#include "devices/drums/DrumSampler.h"
#include "devices/sampler/Sampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"

#include <vector>

namespace phazite
{
namespace
{
    juce::File getProjectFolder (const Song& song)
    {
        return song.getFile() != juce::File() ? song.getFile().getParentDirectory() : juce::File();
    }

    // The properties of a device's sounds (Schema.h: SOUNDS) that name files: a drum sampler's pads' and a wavetable synth's
    // oscillators', whose names differ; none for other elements.
    std::vector<juce::Identifier> getFileProperties (const juce::ValueTree& state)
    {
        std::vector<juce::Identifier> properties;

        if (! state.hasType (schema::SOUNDS))
            return properties;

        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            for (int layer = 0; layer < devices::DrumSampler::numLayers; ++layer)
                properties.push_back (schema::getPadSoundFile (pad, layer));

        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
            properties.push_back (schema::getOscillatorWavetableFile (osc));

        properties.push_back (schema::samplerSampleFile);
        return properties;
    }
} // namespace

std::vector<juce::Identifier> getSoundProperties (const devices::DeviceInfo& info)
{
    std::vector<juce::Identifier> properties;

    if (info.typeID == std::string_view (devices::DrumSampler::typeID))
    {
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
        {
            for (int layer = 0; layer < devices::DrumSampler::numLayers; ++layer)
            {
                properties.push_back (schema::getPadSound (pad, layer));
                properties.push_back (schema::getPadSoundFile (pad, layer));
            }
        }
    }

    if (info.typeID == std::string_view (devices::WavetableSynth::typeID))
    {
        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
        {
            properties.push_back (schema::getOscillatorWavetable (osc));
            properties.push_back (schema::getOscillatorWavetableFile (osc));
        }
    }

    if (info.typeID == std::string_view (devices::Sampler::typeID))
    {
        properties.push_back (schema::samplerSample);
        properties.push_back (schema::samplerSampleFile);
        properties.push_back (schema::samplerSlices);
        properties.push_back (schema::samplerSlicesRemoved);
    }

    return properties;
}

juce::String getProjectPath (const Song& song, const juce::File& file)
{
    const auto folder = getProjectFolder (song);
    return folder != juce::File() && file.isAChildOf (folder) ? file.getRelativePathFrom (folder).replaceCharacter ('\\', '/')
                                                              : file.getFullPathName();
}

juce::File findProjectFile (const Song& song, const juce::String& path)
{
    if (path.isEmpty())
        return {};

    if (juce::File::isAbsolutePath (path))
        return juce::File (path);

    const auto folder = getProjectFolder (song);
    return folder != juce::File() ? folder.getChildFile (path) : juce::File();
}

void writeDeviceFilePaths (const Song& song, juce::ValueTree& state)
{
    for (const auto& property : getFileProperties (state))
        if (const auto path = state[property].toString(); path.isNotEmpty() && ! juce::File::isAbsolutePath (path))
            if (const auto file = findProjectFile (song, path); file != juce::File())
                state.setProperty (property, file.getFullPathName(), nullptr);

    for (auto child : state)
        writeDeviceFilePaths (song, child);
}

bool isSoundFileToTake (const juce::String& path)
{
    // Never a network path, which Windows would connect to as it is asked whether the file is there (the first two
    // characters slashes: "\\server\share", "//server/share", "\\?\UNC\..."); only a full path, of a file whose name says it
    // is audio; and one that is there.
    const auto isNetwork = path.length() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');

    if (isNetwork || ! juce::File::isAbsolutePath (path))
        return false;

    const juce::File file (path);
    return getFileKind (file) == FileKind::audio && file.existsAsFile();
}

void copyDeviceFilesInto (Song& song, juce::ValueTree& state)
{
    for (const auto& property : getFileProperties (state))
    {
        const auto path = state[property].toString();

        if (path.isEmpty())
            continue;

        // A file that isn't there any more, or that the device can't take: it plays its own sound.
        if (const juce::File file (isSoundFileToTake (path) ? path : juce::String()); file != juce::File())
            state.setProperty (property, getProjectPath (song, copyIntoProject (song, file)), nullptr);
        else
            state.removeProperty (property, nullptr);
    }

    for (auto child : state)
        copyDeviceFilesInto (song, child);
}
} // namespace phazite
