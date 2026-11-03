// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceStates.h"
#include "internal/PluginParameters.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>

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

    // Where its window was, in the song's element of the device (Schema.h: deviceWindowX, deviceWindowY), which Phazite
    // stores as the window moves too (rememberWindowPosition() in model/mixer/internal/Mixer.h); and in its state, as
    // Plugin::flushPluginStateToValueTree() stores it.
    void storeWindowPosition (te::Plugin& device)
    {
        if (const auto& bounds = device.windowState->lastWindowBounds; bounds.has_value() && ! bounds->isEmpty())
        {
            store (device.state, te::IDs::windowX, bounds->getX());
            store (device.state, te::IDs::windowY, bounds->getY());
            rememberWindowPosition (getSong (device.edit), getDeviceID (device), bounds->getPosition());
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

    void storeOrRemove (juce::ValueTree& state, const juce::Identifier& property, const juce::String& value)
    {
        if (value.isNotEmpty())
            store (state, property, value);
        else
            state.removeProperty (property, nullptr);
    }

    // A plug-in's buses' channels as the song keeps them (Schema.h: pluginInputs, pluginOutputs): each as JUCE abbreviates
    // them, or "disabled", separated by commas.
    juce::String describeBuses (const juce::Array<juce::AudioChannelSet>& sets)
    {
        juce::StringArray buses;

        for (const auto& set : sets)
            buses.add (set.isDisabled() ? juce::String ("disabled") : set.getSpeakerArrangementAsString());

        return buses.joinIntoString (",");
    }

    juce::Array<juce::AudioChannelSet> readBuses (const juce::String& buses)
    {
        juce::Array<juce::AudioChannelSet> sets;

        for (const auto& bus : juce::StringArray::fromTokens (buses, ",", {}))
            sets.add (juce::AudioChannelSet::fromAbbreviatedString (bus)); // none of its channels for "disabled"

        return sets;
    }

    // In a plug-in's element in the song, the values set by hand of its parameters that automation or modulation moves
    // (Schema.h: VALUE), as storeChangedParameters() stores them in its state; but its mix's levels, which the song keeps
    // as its mix (Schema.h: deviceMix). Written again only if they differ.
    void storeValuesSetByHand (te::ExternalPlugin& plugin, juce::ValueTree& device)
    {
        juce::Array<juce::ValueTree> values, stored;

        for (auto* parameter : plugin.getAutomatableParameters())
            if (parameter != plugin.dryGain.get() && parameter != plugin.wetGain.get()
                && ! juce::exactlyEqual (parameter->getCurrentValue(), parameter->getCurrentExplicitValue()))
                values.add (juce::ValueTree (schema::VALUE, { { schema::valueParameter, parameter->paramID },
                                                              { schema::valueSetByHand, parameter->getCurrentExplicitValue() } }));

        for (const auto& child : device)
            if (child.hasType (schema::VALUE))
                stored.add (child);

        const auto same = values.size() == stored.size()
                          && std::equal (values.begin(), values.end(), stored.begin(),
                                         [] (const juce::ValueTree& a, const juce::ValueTree& b) { return a.isEquivalentTo (b); });

        if (same)
            return;

        for (const auto& child : stored)
            device.removeChild (child, nullptr);

        for (const auto& value : values)
            device.appendChild (value, nullptr);
    }

    // What ExternalPlugin::flushPluginStateToValueTree() stores besides: which plug-in it is, its program, its own
    // settings and its channel layout; in its PLUGIN, and in its element in the song (none for one the song hasn't: a
    // preset's, made on no channel yet), with its values set by hand.
    void storePluginState (te::ExternalPlugin& plugin)
    {
        auto& state = plugin.state;
        auto device = findDeviceState (plugin.edit, getDeviceID (plugin));

        if (const auto& description = plugin.desc; description.fileOrIdentifier.isNotEmpty())
        {
            store (state, te::IDs::manufacturer, description.manufacturerName);
            store (state, te::IDs::name, description.name);
            store (state, te::IDs::id, plugin.itemID.toVar());
            store (state, te::IDs::uid, plugin.getPluginUID());
            store (state, te::IDs::filename, description.fileOrIdentifier);

            if (device.isValid())
                setPluginIdentity (device, description);
        }

        auto* instance = plugin.getAudioPluginInstance();

        if (instance == nullptr)
            return;

        juce::MemoryBlock block;
        instance->suspendProcessing (true);
        instance->getStateInformation (block);
        instance->suspendProcessing (false);

        const auto settings = block.getSize() > 0 ? block.toBase64Encoding() : juce::String();
        const auto layout = instance->getBusesLayout();
        const auto hasPrograms = instance->getNumPrograms() > 0;

        if (hasPrograms)
            store (state, te::IDs::programNum, instance->getCurrentProgram());

        plugin.edit.engine.getEngineBehaviour().saveCustomPluginProperties (state, *instance, nullptr);
        storeOrRemove (state, te::IDs::state, settings);

        if (const auto layoutBlock = makeLayoutBlock (layout); layoutBlock.getSize() > 0)
            store (state, te::IDs::layout, layoutBlock);
        else
            state.removeProperty (te::IDs::layout, nullptr);

        if (! device.isValid())
            return;

        if (hasPrograms)
            store (device, schema::pluginProgram, instance->getCurrentProgram());
        else
            device.removeProperty (schema::pluginProgram, nullptr);

        storeOrRemove (device, schema::pluginState, settings);
        storeOrRemove (device, schema::pluginInputs, describeBuses (layout.inputBuses));
        storeOrRemove (device, schema::pluginOutputs, describeBuses (layout.outputBuses));
        storeValuesSetByHand (plugin, device);
    }
} // namespace

void storeDeviceState (te::Plugin& device)
{
    storeChangedParameters (device);
    storeWindowPosition (device);

    if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (&device))
    {
        storePluginState (*plugin);
        storeParameterIDs (*plugin); // after the values set by hand, which name parameters too
    }
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
    for (auto* device : te::getAllPlugins (edit, true))
        storeDeviceState (*device);

    for (auto* modulator : te::getAllModifiers (edit))
        storeChangedParameters (*modulator);

    // Tracktion's save also renames a track's output to another track after that track's number, which changes as tracks
    // move; Phazite's tracks never output to another track (a bus is a folder they are in: model/mixer/internal/Buses.h).
}

juce::MemoryBlock makeTracktionLayout (const juce::ValueTree& device)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses = readBuses (device[schema::pluginInputs].toString());
    layout.outputBuses = readBuses (device[schema::pluginOutputs].toString());
    return makeLayoutBlock (layout);
}

juce::MemoryBlock makeTracktionValues (const juce::ValueTree& device)
{
    juce::MemoryOutputStream stream;

    for (const auto& child : device)
    {
        if (child.hasType (schema::VALUE))
        {
            stream.writeString (child[schema::valueParameter].toString());
            stream.writeFloat ((float) child[schema::valueSetByHand]);
        }
    }

    stream.flush();
    return stream.getMemoryBlock();
}
} // namespace phazite
