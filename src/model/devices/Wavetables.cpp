// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Wavetables.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "internal/DeviceFiles.h"
#include "internal/WavetablePlugin.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
namespace
{
    WavetablePlugin* asWavetableSynth (te::Plugin& plugin, int osc)
    {
        return juce::isPositiveAndBelow (osc, devices::WavetableSynth::numOscillators) ? dynamic_cast<WavetablePlugin*> (&plugin) : nullptr;
    }

    juce::String getStepName (const juce::String& table, int osc)
    {
        return "Put " + table + " on Osc " + juce::String (osc + 1);
    }
} // namespace

bool isWavetableSynth (const te::Plugin& plugin)
{
    return dynamic_cast<const WavetablePlugin*> (&plugin) != nullptr;
}

juce::String getWavetableName (te::Plugin& plugin, int osc)
{
    auto* synth = asWavetableSynth (plugin, osc);
    return synth != nullptr ? synth->getTableName (osc) : juce::String();
}

bool hasWavetableFile (te::Plugin& plugin, int osc)
{
    auto* synth = asWavetableSynth (plugin, osc);
    return synth != nullptr && synth->getTableFile (osc) != juce::File();
}

std::shared_ptr<const devices::Wavetable> getWavetable (te::Plugin& plugin, int osc)
{
    auto* synth = asWavetableSynth (plugin, osc);
    return synth != nullptr ? synth->getSynth().getTable (osc) : nullptr;
}

void setFactoryWavetable (te::Plugin& plugin, int osc, int index)
{
    auto* synth = asWavetableSynth (plugin, osc);
    const auto names = devices::getFactoryWavetableNames();

    if (synth == nullptr || ! juce::isPositiveAndBelow (index, (int) names.size()))
        return;

    const juce::String name (names[(size_t) index].data(), names[(size_t) index].size());

    if (! hasWavetableFile (plugin, osc) && synth->getTableName (osc) == name)
        return;

    auto& edit = plugin.edit;
    auto* undoManager = &edit.getUndoManager();
    const UndoStep step (edit, getStepName (name, osc));
    plugin.state.removeProperty (schema::getOscillatorWavetableFile (osc), undoManager);
    plugin.state.setProperty (schema::getOscillatorWavetable (osc), name, undoManager);
}

bool setWavetableFile (te::Plugin& plugin, int osc, const juce::File& file)
{
    if (asWavetableSynth (plugin, osc) == nullptr || ! file.existsAsFile() || getFileKind (plugin.edit.engine, file) != FileKind::audio)
        return false;

    auto& edit = plugin.edit;
    const auto copy = copyIntoProject (edit, file);
    const UndoStep step (edit, getStepName (copy.getFileNameWithoutExtension(), osc));
    plugin.state.setProperty (schema::getOscillatorWavetableFile (osc), getProjectPath (edit, copy), &edit.getUndoManager());
    return true;
}
} // namespace phazite
