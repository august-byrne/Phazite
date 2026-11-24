// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestDeviceValues.h"

#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/DeviceStore.h"

namespace phazite::test
{
namespace
{
    // The element of a song's whole copy of a type with an ID (Schema.h: trackID, deviceID, chainID: "id"), invalid for none.
    juce::ValueTree findInCopy (const juce::ValueTree& tree, const juce::Identifier& type, std::uint64_t id)
    {
        if (tree.hasType (type) && readID (tree[schema::deviceID]) == id)
            return tree;

        for (const auto& child : tree)
            if (const auto found = findInCopy (child, type, id); found.isValid())
                return found;

        return {};
    }

} // namespace

DeviceID tryAddOwnDevice (Song& song, TrackID channel, std::string_view typeID, const OwnValues& values, int index)
{
    return insertDevice (song, channel, makeOwnDevice (song, typeID, values), index);
}

DeviceID trySetOwnInstrument (Song& song, TrackID track, std::string_view typeID, const OwnValues& values)
{
    return setInstrument (song, track, makeOwnDevice (song, typeID, values));
}

DeviceID tryAddOwnDeviceToChain (Song& song, DeviceID chain, std::string_view typeID, int index)
{
    return insertIntoChain (song, chain, makeOwnDevice (song, typeID), index);
}

DeviceID addOwnDevice (Song& song, TrackID channel, std::string_view typeID, const OwnValues& values, int index)
{
    const auto id = tryAddOwnDevice (song, channel, typeID, values, index);
    REQUIRE (id.isValid());
    return id;
}

DeviceID setOwnInstrument (Song& song, TrackID track, std::string_view typeID, const OwnValues& values)
{
    const auto id = trySetOwnInstrument (song, track, typeID, values);
    REQUIRE (id.isValid());
    return id;
}

DeviceID addOwnDeviceToChain (Song& song, DeviceID chain, std::string_view typeID, int index)
{
    const auto id = tryAddOwnDeviceToChain (song, chain, typeID, index);
    REQUIRE (id.isValid());
    return id;
}

DeviceID addPluginDevice (Song& song, TrackID channel, const juce::PluginDescription& description, int index)
{
    const auto id = insertDevice (song, channel, makeDevice (song, makeDeviceType (description)), index);
    REQUIRE (id.isValid());
    return id;
}

DeviceID addPluginDevice (Song& song, TrackID channel, const juce::PluginDescription& description, const juce::MemoryBlock& state,
                          int index)
{
    auto device = makeDevice (song, makeDeviceType (description));
    const auto made = device.devices.front().id;
    device.store.states =
        device.store.states.set (made.getRaw(), PluginBytes { .bytes = std::make_shared<const juce::MemoryBlock> (state) });
    const auto id = insertDevice (song, channel, std::move (device), index);
    REQUIRE (id.isValid());
    return id;
}

DeviceType findOwnDeviceType (std::string_view typeID)
{
    const auto wanted = juce::String (typeID.data(), typeID.size());

    for (const auto& type : getDeviceTypes (getPluginHost (getEngineContext())))
        if (type.isBuiltIn && type.description.fileOrIdentifier == wanted)
            return type;

    FAIL ("Phazite offers no device of type " << wanted);
    return {};
}

TrackID addReturnWithOwnEffect (Song& song, TrackID sender, std::string_view typeID)
{
    const auto made = addReturnWithEffect (song, sender, makeOwnDevice (song, typeID));
    REQUIRE (made.isValid());
    return made;
}

//==============================================================================
juce::ValueTree copyDevice (const Song& song, DeviceID id)
{
    // Its element with what it holds, made whole (the store's, a window's place, a container's macros), as a whole copy of the
    // song writes it.
    return phazite::findDevice (song, id) != nullptr ? copyWhole (writeDeviceWhole (getDevicesValue (song), id), keepValues (song))
                                                     : juce::ValueTree();
}

bool hasDevice (const Song& song, DeviceID id)
{
    return phazite::findDevice (song, id) != nullptr;
}

juce::ValueTree copyWholeSongWithDevice (const Song& song, TrackID channel, const juce::ValueTree& device)
{
    auto copy = phazite::test::copyWholeSong (song);
    auto element = channel.isValid() && channel != getMasterID (song) ? findInCopy (copy, schema::TRACK, channel.getRaw())
                                                                      : copy.getChildWithName (schema::MASTER);
    REQUIRE (element.isValid());
    element.appendChild (device.createCopy(), nullptr);
    return copy;
}

juce::ValueTree copyWholeSongWithDeviceEdited (const Song& song, DeviceID id, const std::function<void (juce::ValueTree&)>& edit)
{
    auto copy = phazite::test::copyWholeSong (song);
    auto element = findInCopy (copy, schema::DEVICE, id.getRaw());

    if (! element.isValid())
        element = findInCopy (copy, schema::CONTAINER, id.getRaw());

    if (! element.isValid())
        element = findInCopy (copy, schema::CHAIN, id.getRaw());

    REQUIRE (element.isValid());
    edit (element);
    return copy;
}

void writeValueSetByHand (juce::ValueTree element, const devices::ParameterInfo& info, float value)
{
    const juce::Identifier parameter (juce::String (info.id.data(), info.id.size()));
    auto values = element.getChildWithName (schema::VALUES);

    if (! isDefaultValue (info, value))
    {
        element.getOrCreateChildWithName (schema::VALUES, nullptr).setProperty (parameter, toSavedValue (info, value), nullptr);
        return;
    }

    values.removeProperty (parameter, nullptr);

    if (values.isValid() && values.getNumProperties() == 0 && values.getNumChildren() == 0)
        element.removeChild (values, nullptr);
}

juce::String getDeviceTypeOf (const Song& song, DeviceID id)
{
    const auto* value = phazite::findDevice (song, id);
    return value == nullptr ? juce::String() : value->isPlugin() ? juce::String (schema::deviceIsPlugin) : value->type;
}

//==============================================================================
void storePluginState (Song& song, DeviceID id, const juce::MemoryBlock& state)
{
    // As a plug-in's instance stores it, outside the history, by the device's ID (model/plugins/internal/DeviceStore.h).
    REQUIRE (phazite::findDevice (song, id) != nullptr);
    StoredPlugin stored;
    stored.state = &state;
    storePluginOutsideHistory (song, id, stored);
}

void storePluginState (Song& song, DeviceID id, const juce::String& base64)
{
    const auto bytes = readBytes (base64);
    REQUIRE (bytes.has_value());
    storePluginState (song, id, *bytes);
}

void setPluginStateInStep (Song& song, DeviceID id, const juce::MemoryBlock& state)
{
    REQUIRE (state.getSize() > 0); // the model's takes an empty state away
    phazite::setPluginStateInStep (song, id, state);
}

void writeDeviceSoundByHand (Song& song, DeviceID id, const juce::Identifier& property, const juce::String& value)
{
    REQUIRE (phazite::findDevice (song, id) != nullptr);
    const Song::OutsideHistory outside (song);
    setDeviceSound (song, id, property, value);
}
} // namespace phazite::test
