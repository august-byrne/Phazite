// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceStates.h"

namespace phazite
{
namespace
{
    // Sets a property as ValueTree::setProperty() does with an undo manager, which leaves a value that is the same as JUCE
    // compares values alone (a number read from a file is text), but without one.
    void store (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = state.getPropertyPointer (property); stored == nullptr || *stored != value)
            state.setProperty (property, value, nullptr);
    }

    // The values set by hand of the parameters that automation or modulation moves away from them, which a device that
    // opens sets again, as AutomatableEditItem::saveChangedParametersToState() stores them: each one's ID and value.
    void storeChangedParameters (te::AutomatableEditItem& item)
    {
        juce::MemoryOutputStream stream;

        for (auto* parameter : item.getAutomatableParameters())
        {
            if (! juce::exactlyEqual (parameter->getCurrentValue(), parameter->getCurrentExplicitValue()))
            {
                stream.writeString (parameter->paramID);
                stream.writeFloat (parameter->getCurrentExplicitValue());
            }
        }

        stream.flush();

        if (stream.getDataSize() > 0)
            store (item.elementState, te::IDs::parameters, stream.getMemoryBlock());
        else
            item.elementState.removeProperty (te::IDs::parameters, nullptr);
    }

    // Where its window was (Plugin::flushPluginStateToValueTree()), which Phazite stores as the window moves too
    // (rememberWindowPosition() in model/mixer/internal/Mixer.h).
    void storeWindowPosition (te::Plugin& device)
    {
        if (const auto& bounds = device.windowState->lastWindowBounds; bounds.has_value() && ! bounds->isEmpty())
        {
            store (device.state, te::IDs::windowX, bounds->getX());
            store (device.state, te::IDs::windowY, bounds->getY());
        }
    }

    // A plug-in's channel layout as ExternalPlugin::flushBusesLayoutToValueTree() stores it: the short names of each of
    // its buses' channels, in a block.
    juce::MemoryBlock makeLayoutBlock (const juce::AudioProcessor::BusesLayout& layout)
    {
        const auto makeBuses = [] (const juce::Array<juce::AudioChannelSet>& sets, const juce::Identifier& type)
        {
            juce::ValueTree buses (type);

            for (int i = 0; i < sets.size(); ++i)
            {
                const auto& set = sets.getReference (i);
                const auto channels = set.isDisabled() ? juce::String ("disabled") : set.getSpeakerArrangementAsString();
                buses.appendChild (te::createValueTree (te::IDs::BUS, te::IDs::index, i, te::IDs::layout, channels), nullptr);
            }

            return buses;
        };

        juce::ValueTree tree (te::IDs::LAYOUT);

        for (const auto& buses : { makeBuses (layout.inputBuses, te::IDs::INPUTS), makeBuses (layout.outputBuses, te::IDs::OUTPUTS) })
            if (buses.getNumChildren() > 0)
                tree.appendChild (buses, nullptr);

        juce::MemoryBlock block;

        if (tree.getNumChildren() > 0)
        {
            juce::MemoryOutputStream stream (block, false);
            tree.writeToStream (stream);
        }

        return block;
    }

    // What ExternalPlugin::flushPluginStateToValueTree() stores besides: which plug-in it is, its program, its own
    // settings and its channel layout.
    void storePluginState (te::ExternalPlugin& plugin)
    {
        auto& state = plugin.state;

        if (const auto& description = plugin.desc; description.fileOrIdentifier.isNotEmpty())
        {
            store (state, te::IDs::manufacturer, description.manufacturerName);
            store (state, te::IDs::name, description.name);
            store (state, te::IDs::id, plugin.itemID.toVar());
            store (state, te::IDs::uid, plugin.getPluginUID());
            store (state, te::IDs::filename, description.fileOrIdentifier);
        }

        auto* instance = plugin.getAudioPluginInstance();

        if (instance == nullptr)
            return;

        if (instance->getNumPrograms() > 0)
            store (state, te::IDs::programNum, instance->getCurrentProgram());

        juce::MemoryBlock settings;
        instance->suspendProcessing (true);
        instance->getStateInformation (settings);
        instance->suspendProcessing (false);

        plugin.edit.engine.getEngineBehaviour().saveCustomPluginProperties (state, *instance, nullptr);

        if (settings.getSize() > 0)
            store (state, te::IDs::state, settings.toBase64Encoding());
        else
            state.removeProperty (te::IDs::state, nullptr);

        if (const auto layout = makeLayoutBlock (instance->getBusesLayout()); layout.getSize() > 0)
            store (state, te::IDs::layout, layout);
        else
            state.removeProperty (te::IDs::layout, nullptr);
    }
} // namespace

void storeDeviceState (te::Plugin& device)
{
    storeChangedParameters (device);
    storeWindowPosition (device);

    if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (&device))
        storePluginState (*plugin);
}

void storeDeviceStates (te::Track& track)
{
    auto tracks = track.getAllSubTracks (true);
    tracks.insert (0, &track);

    for (auto* each : tracks)
        for (auto* device : each->getAllPlugins())
            storeDeviceState (*device);
}

void storeDeviceStates (te::RackType& rack)
{
    for (auto* device : rack.getPlugins())
        storeDeviceState (*device);
}

void storeDeviceStates (te::Edit& edit)
{
    auto& storage = edit.engine.getPropertyStorage();
    store (edit.state, te::IDs::appVersion, storage.getApplicationVersion());
    store (edit.state, te::IDs::modifiedBy, storage.getUserName()); // Phazite's name, not the user's (EngineSetup.h)
    store (edit.state, te::IDs::projectID, edit.getProjectItemRef().toString());

    for (auto* device : te::getAllPlugins (edit, true))
        storeDeviceState (*device);

    for (auto* modulator : te::getAllModifiers (edit))
        storeChangedParameters (*modulator);

    // Tracktion's save also renames a track's output to another track after that track's number, which changes as tracks
    // move; Phazite's tracks never output to another track (a bus is a folder they are in: model/mixer/internal/Buses.h).
}
} // namespace phazite
