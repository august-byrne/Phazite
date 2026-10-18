// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceFiles.h"
#include "devices/drums/DrumSampler.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"

#include <vector>

namespace phazite
{
namespace
{
    juce::File getProjectFolder (const te::Edit& edit)
    {
        const auto projectFile = edit.editFileRetriever != nullptr ? edit.editFileRetriever() : juce::File();
        return projectFile != juce::File() ? projectFile.getParentDirectory() : juce::File();
    }

    // The properties of a device's state that name files: none for most devices.
    std::vector<juce::Identifier> getFileProperties (const juce::ValueTree& state)
    {
        std::vector<juce::Identifier> properties;

        if (! state.hasType (te::IDs::PLUGIN))
            return properties;

        const auto type = state[te::IDs::type].toString();

        if (type == devices::DrumSampler::typeID)
            for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
                properties.push_back (schema::getPadSound (pad));

        if (type == devices::WavetableSynth::typeID)
            for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
                properties.push_back (schema::getOscillatorWavetableFile (osc));

        return properties;
    }
} // namespace

juce::String getProjectPath (const te::Edit& edit, const juce::File& file)
{
    const auto folder = getProjectFolder (edit);
    return folder != juce::File() && file.isAChildOf (folder) ? file.getRelativePathFrom (folder).replaceCharacter ('\\', '/')
                                                              : file.getFullPathName();
}

juce::File findProjectFile (const te::Edit& edit, const juce::String& path)
{
    if (path.isEmpty())
        return {};

    if (juce::File::isAbsolutePath (path))
        return juce::File (path);

    const auto folder = getProjectFolder (edit);
    return folder != juce::File() ? folder.getChildFile (path) : juce::File();
}

void writeDeviceFilePaths (const te::Edit& edit, juce::ValueTree& state)
{
    for (const auto& property : getFileProperties (state))
        if (const auto path = state[property].toString(); path.isNotEmpty() && ! juce::File::isAbsolutePath (path))
            if (const auto file = findProjectFile (edit, path); file != juce::File())
                state.setProperty (property, file.getFullPathName(), nullptr);

    for (auto child : state)
        writeDeviceFilePaths (edit, child);
}

void copyDeviceFilesInto (te::Edit& edit, juce::ValueTree& state)
{
    for (const auto& property : getFileProperties (state))
    {
        const auto path = state[property].toString();

        if (path.isEmpty())
            continue;

        // A file that isn't there any more: the device plays its own sound.
        if (const juce::File file (juce::File::isAbsolutePath (path) ? path : juce::String()); file.existsAsFile())
            state.setProperty (property, getProjectPath (edit, copyIntoProject (edit, file)), nullptr);
        else
            state.removeProperty (property, nullptr);
    }

    for (auto child : state)
        copyDeviceFilesInto (edit, child);
}
} // namespace phazite
