// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceStore.h"
#include "model/base/Numbers.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"

#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Sets a property as ValueTree::setProperty() does with an undo manager, which leaves a value that is the same as JUCE
    // compares values alone (a number read from a file is text), but without one. JUCE workaround W8
    // (docs/juce-workarounds.md).
    void storeProperty (juce::ValueTree& element, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = element.getPropertyPointer (property); stored == nullptr || *stored != value)
            element.setProperty (property, value, nullptr);
    }

    // A plug-in's element in the song by its ID, invalid for a device the song hasn't or one that isn't a plug-in.
    juce::ValueTree findPlugin (const Song& song, DeviceID id)
    {
        const auto device = findDeviceState (song, id);
        return isPluginState (device) ? device : juce::ValueTree();
    }

    // A plug-in's VALUE of a parameter, the first, invalid for none.
    juce::ValueTree findValueOf (const juce::ValueTree& device, const juce::String& parameter)
    {
        return device.getChildWithProperty (schema::valueParameter, parameter);
    }

    juce::MemoryBlock readState (const juce::ValueTree& device)
    {
        if (const auto text = device[schema::pluginState].toString(); text.isNotEmpty())
            if (auto bytes = readBytes (text))
                return std::move (*bytes);

        return {};
    }

    const juce::Identifier& getProperty (ChainFlag flag)
    {
        switch (flag)
        {
            case ChainFlag::mute:
                return schema::chainMute.getName();
            case ChainFlag::solo:
                break;
        }

        return schema::chainSolo.getName();
    }
} // namespace

//==============================================================================
juce::MemoryBlock getPluginState (const Song& song, DeviceID id)
{
    return getPluginState (findPlugin (song, id));
}

std::optional<int> getPluginProgram (const Song& song, DeviceID id)
{
    return getPluginProgram (findPlugin (song, id));
}

juce::MemoryBlock getPluginState (const juce::ValueTree& device)
{
    return readState (device);
}

std::optional<int> getPluginProgram (const juce::ValueTree& device)
{
    if (const auto* program = device.getPropertyPointer (schema::pluginProgram))
        return toWhole<int> (*program);

    return std::nullopt;
}

juce::String getPluginFormatID (const Song& song, DeviceID id)
{
    return findPlugin (song, id)[schema::pluginFormatID].toString();
}

juce::String getPluginVersion (const Song& song, DeviceID id)
{
    return findPlugin (song, id)[schema::pluginVersion].toString();
}

std::vector<PluginValueSet> getPluginValues (const Song& song, DeviceID id)
{
    return getPluginValues (findPlugin (song, id));
}

std::optional<float> findPluginValue (const Song& song, DeviceID id, const juce::String& parameter)
{
    return findPluginValue (findPlugin (song, id), parameter);
}

std::vector<PluginValueSet> getPluginValues (const juce::ValueTree& device)
{
    std::vector<PluginValueSet> values;

    for (const auto& child : device)
        if (child.hasType (schema::VALUE))
            values.push_back (readPluginValue (child));

    return values;
}

std::optional<float> findPluginValue (const juce::ValueTree& device, const juce::String& parameter)
{
    if (const auto value = findValueOf (device, parameter); value.isValid())
        return (float) value[schema::valueSetByHand];

    return std::nullopt;
}

PluginValueSet readPluginValue (const juce::ValueTree& value)
{
    return { .parameter = value[schema::valueParameter].toString(), .value = (float) value[schema::valueSetByHand] };
}

//==============================================================================
void storePluginIntoHeld (juce::ValueTree device, const StoredPlugin& stored)
{
    if (! device.isValid())
        return;

    if (stored.state != nullptr)
    {
        if (stored.state->getSize() > 0)
            storeProperty (device, schema::pluginState, writeBytes (*stored.state));
        else
            device.removeProperty (schema::pluginState, nullptr);
    }

    if (stored.formatID.isNotEmpty())
        storeProperty (device, schema::pluginFormatID, stored.formatID);

    if (stored.version.isNotEmpty())
        storeProperty (device, schema::pluginVersion, stored.version);

    if (stored.program.has_value())
    {
        if (*stored.program >= 0)
            storeProperty (device, schema::pluginProgram, *stored.program);
        else
            device.removeProperty (schema::pluginProgram, nullptr);
    }

    for (const auto& [parameter, value] : stored.values)
    {
        auto held = findValueOf (device, parameter);

        if (! held.isValid())
        {
            held = juce::ValueTree (schema::VALUE, { { schema::valueParameter, parameter } });
            device.appendChild (held, nullptr);
        }

        storeProperty (held, schema::valueSetByHand, value);
    }
}

void storePluginOutsideHistory (Song& song, DeviceID id, const StoredPlugin& stored)
{
    storePluginOutsideHistory (findPlugin (song, id), stored);
}

void storePluginOutsideHistory (juce::ValueTree device, const StoredPlugin& stored)
{
    storePluginIntoHeld (std::move (device), stored);
}

void setPluginValueOutsideHistory (Song& song, DeviceID id, const juce::String& parameter, float value)
{
    if (auto device = findDeviceState (song, id); device.isValid())
        for (auto child : device)
            if (child.hasType (schema::VALUE) && child[schema::valueParameter].toString() == parameter)
                storeProperty (child, schema::valueSetByHand, value);
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::String& base64)
{
    setPluginStateInStep (song, findPlugin (song, id), base64);
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::MemoryBlock& state)
{
    setPluginStateInStep (song, id, writeBytes (state));
}

void setPluginProgramInStep (Song& song, DeviceID id, int program)
{
    setPluginProgramInStep (song, findPlugin (song, id), program);
}

void setPluginStateInStep (Song& song, juce::ValueTree device, const juce::String& base64)
{
    if (! device.isValid())
        return;

    if (base64.isNotEmpty())
        device.setProperty (schema::pluginState, base64, &song.getUndoManager());
    else
        device.removeProperty (schema::pluginState, &song.getUndoManager());
}

void setPluginProgramInStep (Song& song, juce::ValueTree device, int program)
{
    if (device.isValid())
        device.setProperty (schema::pluginProgram, program, &song.getUndoManager());
}

void setPluginValueSetByHand (Song& song, DeviceID id, const juce::String& parameter, float value, float held)
{
    setPluginValueSetByHand (song, findPlugin (song, id), parameter, value, held);
}

void removePluginValueSetByHand (Song& song, DeviceID id, const juce::String& parameter)
{
    removePluginValueSetByHand (song, findPlugin (song, id), parameter);
}

void setPluginValueSetByHand (Song& song, juce::ValueTree device, const juce::String& parameter, float value, float held)
{
    if (! device.isValid())
        return;

    auto* undo = &song.getUndoManager();
    auto stored = findValueOf (device, parameter);

    if (! stored.isValid())
    {
        stored = juce::ValueTree (schema::VALUE, { { schema::valueParameter, parameter }, { schema::valueSetByHand, held } });
        device.appendChild (stored, undo);
    }

    stored.setProperty (schema::valueSetByHand, value, undo);
}

void removePluginValueSetByHand (Song& song, juce::ValueTree device, const juce::String& parameter)
{
    if (device.isValid())
        if (const auto stored = findValueOf (device, parameter); stored.isValid())
            device.removeChild (stored, &song.getUndoManager());
}

//==============================================================================
bool isChainMuted (const Song& song, DeviceID chain)
{
    return isChainMuted (findContainerChain (song, chain));
}

bool isChainSoloed (const Song& song, DeviceID chain)
{
    return isChainSoloed (findContainerChain (song, chain));
}

bool isChainMuted (const juce::ValueTree& chain)
{
    return chain[schema::chainMute];
}

bool isChainSoloed (const juce::ValueTree& chain)
{
    return chain[schema::chainSolo];
}

void setChainSwitch (Song& song, DeviceID chain, ChainFlag flag, bool on)
{
    auto state = findContainerChain (song, chain);
    const auto& property = getProperty (flag);

    if (! state.isValid() || (bool) state[property] == on)
        return;

    if (on)
        state.setProperty (property, true, nullptr);
    else
        state.removeProperty (property, nullptr);
}

void clearChainSwitchesInStep (Song& song, DeviceID chain)
{
    clearChainSwitchesInStep (song, findContainerChain (song, chain));
}

void clearChainSwitchesInStep (Song& song, juce::ValueTree chain)
{
    if (chain.isValid())
    {
        chain.removeProperty (schema::chainMute, &song.getUndoManager());
        chain.removeProperty (schema::chainSolo, &song.getUndoManager());
    }
}

//==============================================================================
std::optional<WindowPlace> getWindowPlace (const Song& song, DeviceID id)
{
    if (const auto state = findDeviceState (song, id); state.hasProperty (schema::deviceWindowX))
        return WindowPlace { .x = toWhole<int> (state[schema::deviceWindowX]), .y = toWhole<int> (state[schema::deviceWindowY]) };

    return std::nullopt;
}

void setWindowPlace (Song& song, DeviceID id, WindowPlace place)
{
    if (auto state = findDeviceState (song, id); state.isValid())
    {
        storeProperty (state, schema::deviceWindowX, place.x);
        storeProperty (state, schema::deviceWindowY, place.y);
    }
}
} // namespace phazite
