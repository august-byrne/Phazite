// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DrumKit.h"
#include "devices/drums/FactoryDrums.h"
#include "internal/DeviceFiles.h"
#include "internal/DrumSamplerPlugin.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"

namespace phazite
{
namespace
{
    DrumSamplerPlugin* asDrumSampler (te::Plugin& plugin, int pad)
    {
        return juce::isPositiveAndBelow (pad, devices::DrumSampler::numPads) ? dynamic_cast<DrumSamplerPlugin*> (&plugin) : nullptr;
    }
} // namespace

bool isDrumSampler (const te::Plugin& plugin)
{
    return dynamic_cast<const DrumSamplerPlugin*> (&plugin) != nullptr;
}

bool isDrumSampler (const juce::ValueTree& deviceState)
{
    return deviceState.hasType (schema::DEVICE) && deviceState[schema::deviceType].toString() == devices::DrumSampler::typeID;
}

juce::String getPadName (const Song& song, const juce::ValueTree& deviceState, int pad)
{
    const auto sounds = deviceState.getChildWithName (schema::SOUNDS);

    if (const auto file = findProjectFile (song, sounds[schema::getPadSoundFile (pad)].toString()); file != juce::File())
        return file.getFileNameWithoutExtension();

    // Its factory sound, or else the default kit's for the pad.
    const auto index = devices::findFactoryDrum (sounds[schema::getPadSound (pad)].toString().toStdString());
    const auto name = devices::getFactoryDrumNames()[(size_t) (index >= 0 ? index : pad)];
    return juce::String (name.data(), name.size());
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

    if (drums == nullptr || ! file.existsAsFile() || getFileKind (file) != FileKind::audio)
        return false;

    auto& edit = plugin.edit;
    const auto copy = copyIntoProject (getSong (edit), file);
    const UndoStep step (edit, "Put " + copy.getFileNameWithoutExtension() + " on Pad " + juce::String (pad + 1));
    setDeviceSound (plugin, schema::getPadSoundFile (pad), getProjectPath (getSong (edit), copy));
    return true;
}

void clearPadSound (te::Plugin& plugin, int pad)
{
    if (! hasPadSound (plugin, pad))
        return;

    const UndoStep step (plugin.edit, "Reset Pad " + juce::String (pad + 1));
    setDeviceSound (plugin, schema::getPadSoundFile (pad), {});
}
} // namespace phazite
