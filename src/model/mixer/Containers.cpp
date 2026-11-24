// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/modulation/internal/Modulation.h"
#include "model/plugins/internal/DeviceStore.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    // A value of a chain's as the song keeps it: to a ten-thousandth, none (0) where it is that within a ten-thousandth (as
    // a channel's: Mixer.cpp).
    float keepChainLevel (float value)
    {
        const auto rounded = std::round ((double) value * 1.0e4) / 1.0e4;
        return std::abs (rounded) < 1.0e-4 ? 0.0f : (float) rounded;
    }

    // A crossover as the song keeps it: to a hundredth of a hertz.
    float keepCrossover (double hertz)
    {
        return (float) (std::round (hertz * 100.0) / 100.0);
    }

    // A container by its ID, a chain by its ID: nullptr for none, or for an item of another form.
    const DeviceValue* findContainer (const Song& song, DeviceID id)
    {
        const auto* value = findDevice (song, id);
        return value != nullptr && value->form == DeviceValue::Form::container ? value : nullptr;
    }

    const DeviceValue* findChain (const Song& song, DeviceID id)
    {
        const auto* value = findDevice (song, id);
        return value != nullptr && value->form == DeviceValue::Form::chain ? value : nullptr;
    }

    // A container's chains, a chain's devices, in their order: none for none.
    std::vector<DeviceID> getList (const immer::flex_vector<DeviceID>* list)
    {
        return list != nullptr ? std::vector<DeviceID> (list->begin(), list->end()) : std::vector<DeviceID>();
    }

    std::vector<DeviceID> getContainerChains (const Song& song, DeviceID container)
    {
        return getList (getDevicesValue (song).chains.find (container.getRaw()));
    }

    // The number the next chain is named with: one more than any "Chain" and a number there is, and more than the chains.
    int getNextChainNumber (const Song& song, DeviceID container)
    {
        const auto chains = getContainerChains (song, container);
        auto number = (int) chains.size() + 1;

        for (const auto chain : chains)
            if (const auto* value = findChain (song, chain); value != nullptr && value->name.startsWith ("Chain "))
                number = std::max (number, value->name.fromFirstOccurrenceOf ("Chain ", false, false).getIntValue() + 1);

        return number;
    }

    // How many parts a split has, for those that have so many: the middle and the side, the left and the right.
    int getFixedParts (ContainerSplit split)
    {
        return split == ContainerSplit::midSide || split == ContainerSplit::leftRight ? 2 : 0;
    }

    // Whether any of a container's chains is soloed, each a look in the store by its ID (model/plugins/internal/DeviceStore.h).
    bool anySoloed (const Song& song, DeviceID container)
    {
        const auto chains = getContainerChains (song, container);
        return std::any_of (chains.begin(), chains.end(), [&song] (DeviceID chain) { return isChainSoloed (song, chain); });
    }

    // The track a place's channel is, as the outline and the channels' readers name a channel: a track's ID, none for the
    // master's.
    TrackID getChannelOf (const DevicePlace& place)
    {
        return place.onMaster ? TrackID() : place.track;
    }

    // The place of the list a container's chain holds its devices in, and of a container's chains.
    DevicePlace inChain (const DevicePlace& chainPlace, DeviceID chain)
    {
        return { .track = chainPlace.track, .onMaster = chainPlace.onMaster, .container = chainPlace.container, .chain = chain };
    }

    DevicePlace inContainer (const DevicePlace& containerPlace, DeviceID container)
    {
        return { .track = containerPlace.track, .onMaster = containerPlace.onMaster, .container = container };
    }

    // A chain's devices among its channel's outline (getDeviceOutline(): no walk of the song): each one's ID and whether it
    // is an instrument, in their order.
    std::vector<DeviceOutline> getChainOutline (const std::vector<DeviceOutline>& outline, DeviceID chain)
    {
        std::vector<DeviceOutline> devices;

        for (const auto& each : outline)
            if (each.chain == chain)
                devices.push_back (each);

        return devices;
    }

    // A chain and the devices in it, the chain first, then its devices in their order: what a copy of it is made from
    // (duplicateChain()), none of its channel's other items read.
    std::vector<DeviceValue> readChainWhole (const Song& song, DeviceID chain)
    {
        std::vector<DeviceValue> values;
        const auto& devices = getDevicesValue (song);

        if (const auto* value = devices.find (chain))
            values.push_back (*value);

        if (const auto* list = devices.chainDevices.find (chain.getRaw()))
            for (const auto device : *list)
                if (const auto* value = devices.find (device))
                    values.push_back (*value);

        return values;
    }

    // Where an effect goes in a chain at a place among its devices (-1 for the end): never before the chain's instrument.
    int findEffectIndex (const std::vector<DeviceOutline>& devices, int index)
    {
        const auto first = ! devices.empty() && devices.front().instrument ? 1 : 0;
        return juce::isPositiveAndBelow (index, (int) devices.size()) ? std::max (index, first) : -1;
    }

    // Devices moved off where they are into a chain, after its devices, in their order, each with what it holds (what the
    // store holds of it is by its ID: an undo brings it back as it was): a sidechain that listened to another track off, as
    // a device in a container hears what passes through it.
    void moveInto (Song& song, const DevicePlace& from, const std::vector<DeviceID>& devices, const DevicePlace& chain)
    {
        moveDevices (song, devices, from, chain, -1, [] (DeviceValue& value) { value.sidechain = {}; });
    }

    // Whether a device is a plug-in, or a container holds one in its chains.
    bool holdsPluginIn (const Song& song, DeviceID id)
    {
        if (isPlugin (song, id))
            return true;

        const auto inside = getContainerDevices (song, id);
        return std::any_of (inside.begin(), inside.end(), [&song] (DeviceID device) { return isPlugin (song, device); });
    }

    // A made container's chain, the one it is made with.
    DeviceID getMadeChain (const MadeDevices& made)
    {
        for (const auto& each : made.devices)
            if (each.form == DeviceValue::Form::chain)
                return each.id;

        return {};
    }
} // namespace

//==============================================================================
bool isContainer (const Song& song, DeviceID id)
{
    return findContainer (song, id) != nullptr;
}

bool isInstrumentContainer (const Song& song, DeviceID id)
{
    const auto* container = findContainer (song, id);
    return container != nullptr && container->instrument;
}

std::vector<DeviceID> getContainerDevices (const Song& song, DeviceID container)
{
    std::vector<DeviceID> ids;

    if (findContainer (song, container) != nullptr)
        for (const auto chain : getContainerChains (song, container))
            for (const auto device : getList (getDevicesValue (song).chainDevices.find (chain.getRaw())))
                ids.push_back (device);

    return ids;
}

DeviceID findContainerOf (const Song& song, DeviceID device)
{
    const auto* value = findDevice (song, device);
    return value != nullptr && value->form != DeviceValue::Form::container ? value->place.container : DeviceID();
}

std::vector<DeviceID> getChains (const Song& song, DeviceID container)
{
    return findContainer (song, container) != nullptr ? getContainerChains (song, container) : std::vector<DeviceID>();
}

std::vector<DeviceID> getChainDevices (const Song& song, DeviceID chain)
{
    return findChain (song, chain) != nullptr ? getList (getDevicesValue (song).chainDevices.find (chain.getRaw()))
                                              : std::vector<DeviceID>();
}

DeviceID findChainOf (const Song& song, DeviceID device)
{
    const auto* value = findDevice (song, device);
    return value != nullptr && (value->form == DeviceValue::Form::own || value->isPlugin()) ? value->place.chain : DeviceID();
}

DeviceID getChainContainer (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr ? value->place.container : DeviceID();
}

int getChainIndex (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);

    if (value == nullptr)
        return -1;

    const auto chains = getContainerChains (song, value->place.container);
    const auto found = std::find (chains.begin(), chains.end(), chain);
    return found != chains.end() ? (int) (found - chains.begin()) : -1;
}

//==============================================================================
bool canAddChain (const Song& song, DeviceID container)
{
    const auto* value = findContainer (song, container);

    if (value == nullptr)
        return false;

    const auto count = (int) getContainerChains (song, container).size();
    const auto fixed = getFixedParts (value->split);
    return count < maximumChains && (fixed == 0 || count < fixed);
}

DeviceID addChain (Song& song, DeviceID container)
{
    if (! canAddChain (song, container))
        return {};

    // After the container's chains, in the step.
    auto chain = makeChain (song, getNextChainNumber (song, container));
    chain.place = inContainer (findContainer (song, container)->place, container);
    const auto id = chain.id;
    MadeDevices made;
    made.devices.push_back (std::move (chain));
    setNewDevices (song, made);
    keepCrossoversInOrder (song, container);
    return id;
}

bool canRemoveChain (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr && getContainerChains (song, value->place.container).size() > 1;
}

void removeChain (Song& song, DeviceID chain)
{
    if (! canRemoveChain (song, chain))
        return;

    // What its plug-ins keep of their own stored first, so that an undo brings them back as they were.
    const auto place = findChainPlace (song, chain);

    if (! place.has_value())
        return;

    song.storeDeviceStates (getChannelOf (*place).getRaw());
    eraseDevice (song, chain, *place);
    forgetWhatNamesTheGone (song); // what modulated the chain or its devices

    // One chain left is heard: its mute or its solo would be the container's own switch. Switched off in this step, though a
    // switch alone is none, so that undoing it brings the switches back with the chain.
    if (const auto left = getChains (song, place->container); left.size() == 1)
        clearChainSwitchesInStep (song, left.front());
}

DeviceID duplicateChain (Song& song, DeviceID chain)
{
    // Found once, its place read from its value.
    const auto place = findChainPlace (song, chain);

    if (! place.has_value() || ! canAddChain (song, place->container))
        return {};

    // A copy of it, with what its plug-ins keep of their own (stored first), every item in it given a new ID in the order a
    // copy of its element was given them (Schema.h: giveNewIDs()), its chain's first, and its parameters' automation and
    // modulation copied, as a copy of a track keeps both; on a container split into bands, its band between its original's
    // and the next. Its own items alone read whole (the chain and its devices), none of its channel's others.
    const auto channel = getChannelOf (*place);
    song.storeDeviceStates (channel.getRaw());
    const auto values = readChainWhole (song, chain);

    if (values.empty() || values.front().id != chain)
        return {};

    CopiedIDs copied;
    MadeDevices made;
    auto copy = values.front();
    copy.id = DeviceID::fromRaw (song.makeID());
    copy.crossover = std::nullopt;
    copied[chain.getRaw()] = copy.id.getRaw();
    made.devices.push_back (copy);

    for (size_t i = 1; i < values.size(); ++i)
    {
        auto device = values[i];
        device.id = DeviceID::fromRaw (song.makeID());
        device.place = inChain (*place, copy.id);
        copied[values[i].id.getRaw()] = device.id.getRaw();
        made.devices.push_back (std::move (device));
    }

    // After its original among its container's chains.
    const auto index = getChainIndex (song, chain) + 1;

    made.store = copyStoreOf (song, copied);
    setNewDevices (song, made, index);
    copyWindowPlaces (song, copied); // its devices' windows' places, view state outside the history

    copyCurves (song, copied);
    copyModulations (song, copied); // the container's macros and the track's modulators move the copy's devices too
    keepCrossoversInOrder (song, place->container);
    return copy.id;
}

bool canMoveChains (const Song& song, DeviceID container)
{
    const auto* value = findContainer (song, container);
    return value != nullptr && value->split != ContainerSplit::bands;
}

void moveChain (Song& song, DeviceID chain, int newIndex)
{
    // To the place of the chain that is there now: after it when moving down, before it when moving up.
    if (const auto place = findChainPlace (song, chain); place.has_value() && canMoveChains (song, place->container))
        moveInDeviceList (song, chain, newIndex, ListMove::chains);
}

juce::String getChainName (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr ? value->name : juce::String();
}

void setChainName (Song& song, DeviceID chain, const juce::String& name)
{
    if (const auto trimmed = name.trim(); trimmed.isNotEmpty())
        setChainFields (song, chain, [&trimmed] (DeviceValue& value) { value.name = trimmed; });
}

bool isChainHeard (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr && ! isChainMuted (song, chain) && (! anySoloed (song, value->place.container) || isChainSoloed (song, chain));
}

void setChainMuted (Song& song, DeviceID chain, bool muted)
{
    setChainSwitch (song, chain, ChainFlag::mute, muted);
}

void setChainSoloed (Song& song, DeviceID chain, bool soloed)
{
    setChainSwitch (song, chain, ChainFlag::solo, soloed);
}

float getChainVolume (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr ? value->volume : 0.0f;
}

float getChainPan (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr ? value->pan : 0.0f;
}

void setChainVolume (Song& song, DeviceID chain, float decibels)
{
    const auto volume = keepChainLevel (keepVolumeInRange (decibels));
    setChainFields (song, chain, [volume] (DeviceValue& value) { value.volume = volume; });
}

void setChainPan (Song& song, DeviceID chain, float pan)
{
    const auto kept = keepChainLevel (std::isfinite (pan) ? juce::jlimit (-1.0f, 1.0f, pan) : 0.0f);
    setChainFields (song, chain, [kept] (DeviceValue& value) { value.pan = kept; });
}

//==============================================================================
ContainerSplit getContainerSplit (const Song& song, DeviceID container)
{
    const auto* value = findContainer (song, container);
    return value != nullptr ? value->split : ContainerSplit::layers;
}

bool canSplit (const Song& song, DeviceID container, ContainerSplit split)
{
    const auto* value = findContainer (song, container);

    if (value == nullptr || value->instrument)
        return false;

    const auto fixed = getFixedParts (split);
    return fixed == 0 || (int) getContainerChains (song, container).size() <= fixed;
}

void setContainerSplit (Song& song, DeviceID container, ContainerSplit split)
{
    if (! canSplit (song, container, split) || getContainerSplit (song, container) == split)
        return;

    setDeviceFields (song, container, [split] (DeviceValue& value) { value.split = split; });

    // The middle and the side, or the left and the right, are two parts: the second's chain is there too, after the others.
    while ((int) getContainerChains (song, container).size() < getFixedParts (split))
    {
        auto chain = makeChain (song, getNextChainNumber (song, container));
        chain.place = inContainer (findContainer (song, container)->place, container);
        MadeDevices made;
        made.devices.push_back (std::move (chain));

        if (! setNewDevices (song, made))
            break;
    }

    keepCrossoversInOrder (song, container);
}

float getDefaultCrossover (int chainIndex, int numChains)
{
    // Where multiband effects start: two bands at 250 Hz; three at 120 Hz and 2.5 kHz, as Ableton's Multiband Dynamics; four
    // and five spread from there; more, evenly in octaves from 100 Hz to 8 kHz.
    static constexpr float two[] = { 250.0f };
    static constexpr float three[] = { 120.0f, 2500.0f };
    static constexpr float four[] = { 120.0f, 800.0f, 5000.0f };
    static constexpr float five[] = { 100.0f, 400.0f, 1600.0f, 6400.0f };

    if (chainIndex <= 0 || chainIndex >= numChains)
        return 0.0f;

    switch (numChains)
    {
        case 2:
            return two[chainIndex - 1];
        case 3:
            return three[chainIndex - 1];
        case 4:
            return four[chainIndex - 1];
        case 5:
            return five[chainIndex - 1];
        default:
            break;
    }

    return 100.0f * std::pow (80.0f, (float) (chainIndex - 1) / (float) (numChains - 2));
}

float getChainCrossover (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    const auto index = getChainIndex (song, chain);

    if (value == nullptr || index <= 0)
        return 0.0f;

    return value->crossover.value_or (getDefaultCrossover (index, (int) getContainerChains (song, value->place.container).size()));
}

void setChainCrossover (Song& song, DeviceID chain, float hertz)
{
    const auto crossover = keepCrossover ((double) juce::jlimit (lowestCrossover, highestCrossover, hertz));
    setChainFields (song, chain, [crossover] (DeviceValue& value) { value.crossover = crossover; });
}

void keepCrossoversInOrder (Song& song, DeviceID container)
{
    if (getContainerSplit (song, container) != ContainerSplit::bands)
        return;

    // Each chain's as the song holds it, put in order, and each that changed set.
    const auto chains = getContainerChains (song, container);
    std::vector<std::optional<float>> crossovers;

    for (const auto chain : chains)
        crossovers.push_back (findChain (song, chain)->crossover);

    auto kept = crossovers;
    keepCrossoversInOrder (kept);

    for (size_t i = 0; i < chains.size(); ++i)
    {
        const auto same =
            kept[i].has_value() == crossovers[i].has_value() && (! kept[i].has_value() || juce::exactlyEqual (*kept[i], *crossovers[i]));

        if (! same)
            setChainFields (song, chains[i], [crossover = kept[i]] (DeviceValue& value) { value.crossover = crossover; });
    }
}

void keepCrossoversInOrder (std::vector<std::optional<float>>& crossovers)
{
    // Each chain's band above the one before it: a chain without a crossover, or with one no higher than the one before it
    // (chains moved while the container was split otherwise keep theirs), gets its place's default if that is between its
    // neighbours', else the middle between them, in octaves.
    const auto count = (int) crossovers.size();
    auto below = lowestCrossover;

    for (int i = 1; i < count; ++i)
    {
        auto& crossover = crossovers[(size_t) i];

        if (! crossover.has_value() || *crossover <= below)
        {
            auto above = highestCrossover;

            for (int j = i + 1; j < count; ++j)
            {
                if (const auto& next = crossovers[(size_t) j]; next.has_value() && *next > below)
                {
                    above = *next;
                    break;
                }
            }

            auto value = getDefaultCrossover (i, count);

            if (value <= below || value >= above)
                value = std::sqrt (below * above);

            crossover = keepCrossover ((double) value);
        }

        below = *crossover;
    }
}

NoteRange getChainKeys (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr ? value->keys : allKeys;
}

NoteRange getChainVelocities (const Song& song, DeviceID chain)
{
    const auto* value = findChain (song, chain);
    return value != nullptr ? value->velocities : allVelocities;
}

void setChainKeys (Song& song, DeviceID chain, NoteRange keys)
{
    const auto low = juce::jlimit (allKeys.low, allKeys.high, keys.low);
    const NoteRange kept { low, juce::jlimit (low, allKeys.high, keys.high) };
    setChainFields (song, chain, [kept] (DeviceValue& value) { value.keys = kept; });
}

void setChainVelocities (Song& song, DeviceID chain, NoteRange velocities)
{
    const auto low = juce::jlimit (allVelocities.low, allVelocities.high, velocities.low);
    const NoteRange kept { low, juce::jlimit (low, allVelocities.high, velocities.high) };
    setChainFields (song, chain, [kept] (DeviceValue& value) { value.velocities = kept; });
}

//==============================================================================
bool canGroupIntoContainer (const Song& song, const std::vector<DeviceID>& devices)
{
    if (devices.empty())
        return false;

    // Devices on one channel, none in a container nor a container itself; an instrument only as its track's, its first.
    const auto* first = findDevice (song, devices.front());

    if (first == nullptr || first->place.container.isValid())
        return false;

    const auto channel = first->place;
    const auto* onChannel = getDevicesValue (song).findList (channel);

    return onChannel != nullptr
           && std::all_of (devices.begin(), devices.end(),
                           [&song, &channel, onChannel] (DeviceID id)
                           {
                               const auto* value = findDevice (song, id);
                               return value != nullptr && value->place == channel
                                      && (value->form == DeviceValue::Form::own || value->isPlugin())
                                      && (! value->isInstrument() || id == onChannel->front());
                           });
}

DeviceID groupIntoContainer (Song& song, const std::vector<DeviceID>& devices)
{
    if (! canGroupIntoContainer (song, devices))
        return {};

    // In their order on the channel, in the first one's place: with the instrument, an instrument container in its place.
    const auto place = findDevicePlace (song, devices.front());

    if (! place.has_value())
        return {};

    const auto channel = getChannelOf (*place);
    std::vector<DeviceID> grouped;
    auto index = 0, at = -1;
    auto instrument = false;

    for (const auto& each : getDeviceOutline (song, channel))
    {
        if (each.container.isValid())
            continue;

        if (std::find (devices.begin(), devices.end(), each.id) != devices.end())
        {
            if (grouped.empty())
            {
                at = index;
                instrument = each.instrument;
            }

            grouped.push_back (each.id);
        }

        ++index;
    }

    song.storeDeviceStates (channel.getRaw());
    auto made = makeContainer (song, instrument);
    const auto chain = getMadeChain (made);
    const auto container = insertDevice (song, channel, std::move (made), at);

    if (container.isValid())
        moveInto (song, *place, grouped, inChain (inContainer (*place, container), chain));

    return container;
}

DeviceID addContainer (Song& song, TrackID channel)
{
    return insertDevice (song, channel, makeContainer (song));
}

DeviceID addInstrumentContainer (Song& song, TrackID track)
{
    // A chain holds devices: an instrument container already the track's would go into the new one's and play nothing.
    const auto old = getInstrument (song, track);

    if (old.isValid() && isContainer (song, old))
        return {};

    auto made = makeContainer (song, true);
    const auto chain = getMadeChain (made);
    const auto container = insertDevice (song, track, std::move (made));

    if (! container.isValid())
        return {};

    // The instrument the track had, first in its chain, the container in its place.
    if (old.isValid())
    {
        const auto place = getDeviceChannelPlace (song, track);
        song.storeDeviceStates (track.getRaw());
        moveInto (song, place, { old }, inChain (inContainer (place, container), chain));
    }

    return container;
}

bool canUngroupContainer (const Song& song, DeviceID container)
{
    return getChains (song, container).size() == 1;
}

void ungroupContainer (Song& song, DeviceID id)
{
    if (! canUngroupContainer (song, id))
        return;

    const auto place = findDevicePlace (song, id);

    if (! place.has_value())
        return;

    // Its chain and its devices, and its place among the channel's, by the channel's outline.
    const auto channel = getChannelOf (*place);
    DeviceID chain;
    std::vector<DeviceID> inside;
    auto index = 0, at = 0;

    for (const auto& each : getDeviceOutline (song, channel))
    {
        if (each.id == id)
            at = index;

        if (! each.container.isValid())
            ++index;
        else if (each.container == id && each.form == DeviceValue::Form::chain)
            chain = each.id;
        else if (each.container == id)
            inside.push_back (each.id);
    }

    // Its devices go back in its place, in their order, each the same element, and it goes with its chain and its macros and
    // what they modulated.
    song.storeDeviceStates (channel.getRaw());
    moveDevices (song, inside, inChain (inContainer (*place, id), chain), *place, at);
    eraseDevice (song, id, *place);
    forgetWhatNamesTheGone (song); // what modulated its chain
}

void setContainerName (Song& song, DeviceID id, const juce::String& name)
{
    setDeviceFields (song, id,
                     [&name] (DeviceValue& value)
                     {
                         if (value.form == DeviceValue::Form::container)
                             value.name = name;
                     });
}

DeviceID insertIntoChain (Song& song, DeviceID chain, MadeDevices device, int index)
{
    const auto place = findChainPlace (song, chain);

    if (device.devices.empty() || ! place.has_value())
        return {};

    // A device, and an instrument only into an instrument container's chain (the channel's outline says what each is).
    auto& made = device.devices.front();
    const auto id = made.id;
    const auto instrument = made.isInstrument();
    const auto outline = getDeviceOutline (song, getChannelOf (*place));
    const auto container =
        std::find_if (outline.begin(), outline.end(), [&place] (const DeviceOutline& each) { return each.id == place->container; });

    if ((made.form != DeviceValue::Form::own && made.form != DeviceValue::Form::plugin) || container == outline.end()
        || (instrument && ! container->instrument))
        return {};

    const auto devices = getChainOutline (outline, chain);
    made.place = inChain (*place, chain);

    // An instrument first, in place of the chain's; an effect where it is put, after the chain's instrument.
    if (instrument)
    {
        if (! devices.empty() && devices.front().instrument)
        {
            song.storeDeviceStates (getChannelOf (*place).getRaw());
            eraseDevice (song, devices.front().id, made.place);
            forgetWhatNamesTheGone (song);
        }

        return setNewDevices (song, device, 0) ? id : DeviceID();
    }

    return setNewDevices (song, device, findEffectIndex (devices, index)) ? id : DeviceID();
}

void moveInChain (Song& song, DeviceID device, int newIndex)
{
    // To the place of the device that is there now: after it when moving later, before it when moving earlier; never
    // before the chain's instrument.
    moveInDeviceList (song, device, newIndex, ListMove::inChain);
}

DeviceID moveToChain (Song& song, DeviceID device, DeviceID chain, int index)
{
    const auto from = findDevicePlace (song, device);
    const auto to = findChainPlace (song, chain);

    // An effect, into another chain of its container.
    if (! from.has_value() || ! to.has_value() || ! from->chain.isValid() || from->container != to->container)
        return {};

    const auto outline = getDeviceOutline (song, getChannelOf (*from));
    const auto moved = std::find_if (outline.begin(), outline.end(), [device] (const DeviceOutline& each) { return each.id == device; });

    if (moved == outline.end() || moved->instrument)
        return {};

    if (from->chain == chain)
    {
        moveInChain (song, device, index);
        return device;
    }

    song.storeDeviceStates (getChannelOf (*from).getRaw());
    moveDevices (song, { device }, *from, inChain (*to, chain), findEffectIndex (getChainOutline (outline, chain), index));
    return device;
}

DeviceID getChainInstrument (const Song& song, DeviceID chain)
{
    const auto devices = getChainDevices (song, chain);
    return ! devices.empty() && isInstrument (song, devices.front()) ? devices.front() : DeviceID();
}

MadeDevices makeContainer (Song& song, bool instrument)
{
    DeviceValue container;
    container.id = DeviceID::fromRaw (song.makeID());
    container.form = DeviceValue::Form::container;
    container.name = makeContainerName (song);
    container.instrument = instrument;

    auto chain = makeChain (song, 1);
    chain.place.container = container.id;

    MadeDevices made;
    made.devices.push_back (container);
    made.devices.push_back (std::move (chain));

    // Its eight macros as modulation's values, which its inserter sets right after it is the song's (Mixer.h: insertDevice()).
    for (int number = 1; number <= macrosPerContainer; ++number)
    {
        ModulatorValue macro;
        macro.id = ModulatorID::fromRaw (song.makeID());
        macro.place.container = container.id;
        macro.name = "Macro " + juce::String (number);
        made.macros.push_back (std::move (macro));
    }

    return made;
}

DeviceValue makeChain (Song& song, int number)
{
    DeviceValue chain;
    chain.id = DeviceID::fromRaw (song.makeID());
    chain.form = DeviceValue::Form::chain;
    chain.name = "Chain " + juce::String (number);
    return chain;
}

juce::String makeContainerName (const Song& song)
{
    juce::StringArray taken;

    for (const auto& [id, value] : getDevicesValue (song).devices)
        if (value.form == DeviceValue::Form::container)
            taken.add (value.name);

    juce::String name ("Container");

    for (int number = 2; taken.contains (name); ++number)
        name = "Container " + juce::String (number);

    return name;
}

//==============================================================================
bool canGroupSelectedDevices (const Selection& selection)
{
    auto* song = selection.getSong();
    return song != nullptr && canGroupIntoContainer (*song, selection.getDevices());
}

bool doSelectedDevicesHoldPlugins (const Selection& selection)
{
    auto* song = selection.getSong();
    const auto devices = selection.getDevices();
    return song != nullptr && std::any_of (devices.begin(), devices.end(), [song] (DeviceID id) { return holdsPluginIn (*song, id); });
}

void groupSelectedDevices (Selection& selection)
{
    if (auto* song = selection.getSong())
        if (const auto container = groupIntoContainer (*song, selection.getDevices()); container.isValid())
            selection.selectOnly (container);
}

bool canUngroupSelectedContainer (const Selection& selection)
{
    auto* song = selection.getSong();
    const auto devices = selection.getDevices();
    return song != nullptr && devices.size() == 1 && isContainer (*song, devices.front()) && canUngroupContainer (*song, devices.front());
}

void ungroupSelectedContainer (Selection& selection)
{
    if (! canUngroupSelectedContainer (selection))
        return;

    // Its devices, selected once they are back on its channel.
    auto& song = *selection.getSong();
    const auto container = selection.getDevices().front();
    const auto inside = getContainerDevices (song, container);
    ungroupContainer (song, container);
    selection.deselectAll();

    for (const auto device : inside)
        selection.addToSelection (device);
}

void addContainerCommands (CommandRegistry& registry, Selection& selection)
{
    registry.add (
        { .id = "device.group",
          .name = "Group into Container",
          .category = "Device",
          .description = "Groups the selected devices into a container, with eight macros that turn several knobs at once "
                         "and chains that play side by side: effects, or a track's instrument with its effects.",
          .synonyms = { "rack", "chain", "fx chain", "combine", "macro", "group devices", "layer", "instrument rack" },
          .perform = [&selection] { groupSelectedDevices (selection); },
          .isEnabled = [&selection] { return canGroupSelectedDevices (selection); },
          .undoTarget = undoTargetOf (selection),
          .keyPriority = Command::notePriority,
          .readsPluginStates = [&selection]
          { return doSelectedDevicesHoldPlugins (selection); } }); // shares Ctrl+G with grouping tracks: devices are selected on their own

    registry.add ({ .id = "device.ungroup",
                    .name = "Ungroup Container",
                    .category = "Device",
                    .description = "Puts the devices of the selected container of one chain back in its place, without the "
                                   "container and its macros.",
                    .synonyms = { "rack", "chain", "split", "unpack" },
                    .perform = [&selection] { ungroupSelectedContainer (selection); },
                    .isEnabled = [&selection] { return canUngroupSelectedContainer (selection); },
                    .undoTarget = undoTargetOf (selection),
                    .keyPriority = Command::notePriority });
}
} // namespace phazite
