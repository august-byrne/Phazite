// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DrumKit.h"
#include "internal/DrumSamplerPlugin.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
namespace
{
    DrumSamplerPlugin* asDrumSampler (te::Plugin& plugin, int pad)
    {
        return juce::isPositiveAndBelow (pad, devices::DrumSampler::numPads) ? dynamic_cast<DrumSamplerPlugin*> (&plugin) : nullptr;
    }

    // The project's copy of a sound as a pad keeps it: by its path within the project's folder, written the same on every
    // system.
    juce::String getPadPath (te::Edit& edit, const juce::File& copy)
    {
        const auto projectFile = edit.editFileRetriever != nullptr ? edit.editFileRetriever() : juce::File();
        const auto folder = projectFile.getParentDirectory();
        return projectFile != juce::File() && copy.isAChildOf (folder) ? copy.getRelativePathFrom (folder).replaceCharacter ('\\', '/')
                                                                       : copy.getFullPathName();
    }
} // namespace

bool isDrumSampler (const te::Plugin& plugin)
{
    return dynamic_cast<const DrumSamplerPlugin*> (&plugin) != nullptr;
}

juce::String getPadName (te::Plugin& plugin, int pad)
{
    auto* drums = asDrumSampler (plugin, pad);
    return drums != nullptr ? drums->getPadName (pad) : juce::String();
}

bool hasPadSound (te::Plugin& plugin, int pad)
{
    auto* drums = asDrumSampler (plugin, pad);
    return drums != nullptr && drums->getPadFile (pad) != juce::File();
}

bool setPadSound (te::Plugin& plugin, int pad, const juce::File& file)
{
    auto* drums = asDrumSampler (plugin, pad);

    if (drums == nullptr || ! file.existsAsFile() || getFileKind (plugin.edit.engine, file) != FileKind::audio)
        return false;

    auto& edit = plugin.edit;
    const auto copy = copyIntoProject (edit, file);
    const UndoStep step (edit, "Put " + copy.getFileNameWithoutExtension() + " on Pad " + juce::String (pad + 1));
    plugin.state.setProperty (schema::getPadSound (pad), getPadPath (edit, copy), &edit.getUndoManager());
    return true;
}

void clearPadSound (te::Plugin& plugin, int pad)
{
    if (! hasPadSound (plugin, pad))
        return;

    const UndoStep step (plugin.edit, "Reset Pad " + juce::String (pad + 1));
    plugin.state.removeProperty (schema::getPadSound (pad), &plugin.edit.getUndoManager());
}

void writePadSoundPaths (const te::Edit& edit, juce::ValueTree& state)
{
    const auto projectFile = edit.editFileRetriever != nullptr ? edit.editFileRetriever() : juce::File();

    if (state.hasType (te::IDs::PLUGIN) && state[te::IDs::type].toString() == DrumSamplerPlugin::xmlTypeName && projectFile != juce::File())
    {
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
        {
            const auto& property = schema::getPadSound (pad);

            if (const auto path = state[property].toString(); path.isNotEmpty() && ! juce::File::isAbsolutePath (path))
                state.setProperty (property, projectFile.getParentDirectory().getChildFile (path).getFullPathName(), nullptr);
        }
    }

    for (auto child : state)
        writePadSoundPaths (edit, child);
}

void copyPadSoundsInto (te::Edit& edit, juce::ValueTree& state)
{
    if (state.hasType (te::IDs::PLUGIN) && state[te::IDs::type].toString() == DrumSamplerPlugin::xmlTypeName)
    {
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
        {
            const auto& property = schema::getPadSound (pad);
            const auto path = state[property].toString();

            if (path.isEmpty())
                continue;

            // A sound that isn't there any more: the pad plays the kit's.
            if (const juce::File file (juce::File::isAbsolutePath (path) ? path : juce::String()); file.existsAsFile())
                state.setProperty (property, getPadPath (edit, copyIntoProject (edit, file)), nullptr);
            else
                state.removeProperty (property, nullptr);
        }
    }

    for (auto child : state)
        copyPadSoundsInto (edit, child);
}
} // namespace phazite
