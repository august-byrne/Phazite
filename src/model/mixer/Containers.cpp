// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    DeviceID getChainID (const juce::ValueTree& chain)
    {
        return DeviceID::fromRaw ((std::uint64_t) (juce::int64) chain[schema::chainID]);
    }

    // A value of a chain's, to a ten-thousandth, none where it is the one it has without (as a channel's: Mixer.cpp).
    void setChainValue (juce::ValueTree chain, const juce::Identifier& property, float value, float none, juce::UndoManager* undo)
    {
        if (const auto rounded = std::round ((double) value * 1.0e4) / 1.0e4; std::abs (rounded - (double) none) < 1.0e-4)
            chain.removeProperty (property, undo);
        else
            chain.setProperty (property, rounded, undo);
    }

    // A whole number of a chain's, none at its default.
    void setChainNumber (juce::ValueTree chain, const juce::Identifier& property, int value, int none, juce::UndoManager* undo)
    {
        if (value == none)
            chain.removeProperty (property, undo);
        else if ((int) chain.getProperty (property, none) != value)
            chain.setProperty (property, value, undo);
    }

    // A switch of a chain's, true or none, set without an undo step (as a track's mute and solo: Tracks.cpp).
    void setChainSwitch (juce::ValueTree chain, const juce::Identifier& property, bool on)
    {
        if ((bool) chain[property] == on)
            return;

        if (on)
            chain.setProperty (property, true, nullptr);
        else
            chain.removeProperty (property, nullptr);
    }

    // The number the next chain is named with: one more than any "Chain" and a number there is, and more than the chains.
    int getNextChainNumber (const juce::ValueTree& container)
    {
        const auto chains = getChainStates (container);
        auto number = (int) chains.size() + 1;

        for (const auto& chain : chains)
            if (const auto name = chain[schema::chainName].toString(); name.startsWith ("Chain "))
                number = std::max (number, name.fromFirstOccurrenceOf ("Chain ", false, false).getIntValue() + 1);

        return number;
    }

    // How many parts a split has, for those that have so many: the middle and the side, the left and the right.
    int getFixedParts (ContainerSplit split)
    {
        return split == ContainerSplit::midSide || split == ContainerSplit::leftRight ? 2 : 0;
    }

    ContainerSplit readSplit (const juce::ValueTree& container)
    {
        const auto split = container[schema::containerSplit].toString();
        return split == schema::splitBands       ? ContainerSplit::bands
               : split == schema::splitMidSide   ? ContainerSplit::midSide
               : split == schema::splitLeftRight ? ContainerSplit::leftRight
                                                 : ContainerSplit::layers;
    }

    float readCrossover (const juce::ValueTree& chain, int index, int numChains)
    {
        return (float) chain.getProperty (schema::chainCrossover, getDefaultCrossover (index, numChains));
    }

    bool anySoloed (const juce::ValueTree& container)
    {
        const auto chains = getChainStates (container);
        return std::any_of (chains.begin(), chains.end(), [] (const juce::ValueTree& chain) { return (bool) chain[schema::chainSolo]; });
    }

    // The channel's element a device of it is on (its TRACK or MASTER), whose devices' states are stored before they move.
    void storeStatesOf (Song& song, const juce::ValueTree& element)
    {
        auto channel = element;

        while (channel.isValid() && ! channel.hasType (schema::TRACK) && ! channel.hasType (schema::MASTER))
            channel = channel.getParent();

        song.storeDeviceStates (channel.hasType (schema::TRACK) ? getTrackID (channel).getRaw() : 0);
    }

    // Takes devices' elements off where they are into a chain, in their order (with what they hold of their states, which an
    // undo brings back): a sidechain that listened to another track off, as a device in a container hears what passes
    // through it.
    void moveInto (Song& song, juce::ValueTree chain, const std::vector<juce::ValueTree>& devices)
    {
        auto* undo = getUndo (song);

        for (auto state : devices)
        {
            state.removeProperty (schema::deviceSidechain, undo);
            state.getParent().removeChild (state, undo);
            chain.appendChild (state, undo);
        }
    }
} // namespace

//==============================================================================
bool isContainer (const Song& song, DeviceID id)
{
    return isContainerState (findDeviceState (song, id));
}

bool isInstrumentContainer (const Song& song, DeviceID id)
{
    const auto state = findDeviceState (song, id);
    return isContainerState (state) && (bool) state[schema::containerInstrument];
}

std::vector<DeviceID> getContainerDevices (const Song& song, DeviceID container)
{
    std::vector<DeviceID> ids;

    if (const auto state = findDeviceState (song, container); isContainerState (state))
        for (const auto& inside : getDeviceStates (state))
            ids.push_back (getDeviceID (inside));

    return ids;
}

std::vector<ModulatorID> getContainerMacros (const Song& song, DeviceID container)
{
    std::vector<ModulatorID> ids;

    if (const auto state = findDeviceState (song, container); isContainerState (state))
        for (const auto& child : state)
            if (child.hasType (schema::MACRO))
                ids.push_back (ModulatorID::fromRaw ((std::uint64_t) (juce::int64) child[schema::modulatorID]));

    return ids;
}

DeviceID findContainerOf (const Song& song, DeviceID device)
{
    const auto container = getOwningContainer (findDeviceState (song, device));
    return container.isValid() ? getDeviceID (container) : DeviceID();
}

//==============================================================================
std::vector<juce::ValueTree> getChainStates (const juce::ValueTree& container)
{
    std::vector<juce::ValueTree> chains;

    if (isContainerState (container))
        for (const auto& child : container)
            if (child.hasType (schema::CHAIN))
                chains.push_back (child);

    return chains;
}

bool isChainState (const juce::ValueTree& state)
{
    return state.hasType (schema::CHAIN);
}

juce::ValueTree findContainerChain (const Song& song, DeviceID id)
{
    if (! id.isValid())
        return {};

    auto channels = getTrackStates (song);
    channels.push_back (findChainState (song, {}));

    for (const auto& channel : channels)
        for (const auto& device : getDeviceStates (channel))
            for (const auto& chain : getChainStates (device))
                if (getChainID (chain) == id)
                    return chain;

    return {};
}

juce::ValueTree getOwningContainer (const juce::ValueTree& element)
{
    const auto chain = isChainState (element) ? element : element.getParent();
    return isChainState (chain) && isContainerState (chain.getParent()) ? chain.getParent() : juce::ValueTree();
}

std::vector<DeviceID> getChains (const Song& song, DeviceID container)
{
    std::vector<DeviceID> ids;

    for (const auto& chain : getChainStates (findDeviceState (song, container)))
        ids.push_back (getChainID (chain));

    return ids;
}

std::vector<DeviceID> getChainDevices (const Song& song, DeviceID chain)
{
    std::vector<DeviceID> ids;

    for (const auto& device : getDeviceStates (findContainerChain (song, chain)))
        ids.push_back (getDeviceID (device));

    return ids;
}

DeviceID findChainOf (const Song& song, DeviceID device)
{
    const auto parent = findDeviceState (song, device).getParent();
    return isChainState (parent) ? getChainID (parent) : DeviceID();
}

DeviceID getChainContainer (const Song& song, DeviceID chain)
{
    const auto container = getOwningContainer (findContainerChain (song, chain));
    return container.isValid() ? getDeviceID (container) : DeviceID();
}

int getChainIndex (const Song& song, DeviceID chain)
{
    const auto state = findContainerChain (song, chain);
    const auto chains = getChainStates (state.getParent());
    const auto found = std::find (chains.begin(), chains.end(), state);
    return found != chains.end() ? (int) (found - chains.begin()) : -1;
}

//==============================================================================
bool canAddChain (const Song& song, DeviceID container)
{
    const auto state = findDeviceState (song, container);
    const auto count = (int) getChainStates (state).size();
    const auto fixed = getFixedParts (readSplit (state));
    return isContainerState (state) && count < maximumChains && (fixed == 0 || count < fixed);
}

DeviceID addChain (Song& song, DeviceID container)
{
    if (! canAddChain (song, container))
        return {};

    auto state = findDeviceState (song, container);
    auto chain = makeChainState (song, getNextChainNumber (state));
    const auto chains = getChainStates (state);
    state.addChild (chain, state.indexOf (chains.back()) + 1, getUndo (song));
    keepCrossoversInOrder (song, container);
    return getChainID (chain);
}

bool canRemoveChain (const Song& song, DeviceID chain)
{
    const auto state = findContainerChain (song, chain);
    return state.isValid() && getChainStates (state.getParent()).size() > 1;
}

void removeChain (Song& song, DeviceID chain)
{
    if (! canRemoveChain (song, chain))
        return;

    // What its plug-ins keep of their own stored first, so that an undo brings them back as they were.
    auto state = findContainerChain (song, chain);
    auto container = state.getParent();
    storeStatesOf (song, container);
    container.removeChild (state, getUndo (song));

    // One chain left is heard: its mute or its solo would be the container's own switch.
    if (const auto left = getChainStates (container); left.size() == 1)
    {
        setChainSwitch (left.front(), schema::chainMute, false);
        setChainSwitch (left.front(), schema::chainSolo, false);
    }
}

DeviceID duplicateChain (Song& song, DeviceID chain)
{
    const auto state = findContainerChain (song, chain);
    const auto container = getOwningContainer (state);

    if (! state.isValid() || ! canAddChain (song, getDeviceID (container)))
        return {};

    // A copy of its element, with what its plug-ins keep of their own, every item in it given a new ID, and its parameters'
    // automation copied; on a container split into bands, its band between its original's and the next.
    storeStatesOf (song, container);
    auto copy = state.createCopy();
    const auto copied = giveNewIDs (song, copy);
    copy.removeProperty (schema::chainCrossover, nullptr);
    auto parent = container;
    parent.addChild (copy, parent.indexOf (state) + 1, getUndo (song));
    copyCurves (song, copied);
    keepCrossoversInOrder (song, getDeviceID (container));
    return getChainID (copy);
}

bool canMoveChains (const Song& song, DeviceID container)
{
    const auto state = findDeviceState (song, container);
    return isContainerState (state) && readSplit (state) != ContainerSplit::bands;
}

void moveChain (Song& song, DeviceID chain, int newIndex)
{
    auto state = findContainerChain (song, chain);
    auto container = state.getParent();

    if (! state.isValid() || ! canMoveChains (song, getDeviceID (container)))
        return;

    // To the place of the chain that is there now: after it when moving down, before it when moving up.
    const auto chains = getChainStates (container);
    const auto& target = chains[(size_t) juce::jlimit (0, (int) chains.size() - 1, newIndex)];
    container.moveChild (container.indexOf (state), container.indexOf (target), getUndo (song));
}

juce::String getChainName (const Song& song, DeviceID chain)
{
    return findContainerChain (song, chain)[schema::chainName].toString();
}

void setChainName (Song& song, DeviceID chain, const juce::String& name)
{
    if (auto state = findContainerChain (song, chain); state.isValid() && name.trim().isNotEmpty())
        state.setProperty (schema::chainName, name.trim(), getUndo (song));
}

bool isChainMuted (const Song& song, DeviceID chain)
{
    return findContainerChain (song, chain)[schema::chainMute];
}

bool isChainSoloed (const Song& song, DeviceID chain)
{
    return findContainerChain (song, chain)[schema::chainSolo];
}

bool isChainHeard (const Song& song, DeviceID chain)
{
    const auto state = findContainerChain (song, chain);
    return state.isValid() && ! (bool) state[schema::chainMute] && (! anySoloed (state.getParent()) || (bool) state[schema::chainSolo]);
}

void setChainMuted (Song& song, DeviceID chain, bool muted)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
        setChainSwitch (state, schema::chainMute, muted);
}

void setChainSoloed (Song& song, DeviceID chain, bool soloed)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
        setChainSwitch (state, schema::chainSolo, soloed);
}

float getChainVolume (const Song& song, DeviceID chain)
{
    return (float) findContainerChain (song, chain).getProperty (schema::channelVolume, 0.0f);
}

float getChainPan (const Song& song, DeviceID chain)
{
    return (float) findContainerChain (song, chain).getProperty (schema::channelPan, 0.0f);
}

void setChainVolume (Song& song, DeviceID chain, float decibels)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
        setChainValue (state, schema::channelVolume, std::max (decibels, -100.0f), 0.0f, getUndo (song));
}

void setChainPan (Song& song, DeviceID chain, float pan)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
        setChainValue (state, schema::channelPan, juce::jlimit (-1.0f, 1.0f, pan), 0.0f, getUndo (song));
}

//==============================================================================
ContainerSplit getContainerSplit (const Song& song, DeviceID container)
{
    return readSplit (findDeviceState (song, container));
}

bool canSplit (const Song& song, DeviceID container, ContainerSplit split)
{
    const auto state = findDeviceState (song, container);

    if (! isContainerState (state) || (bool) state[schema::containerInstrument])
        return false;

    const auto fixed = getFixedParts (split);
    return fixed == 0 || (int) getChainStates (state).size() <= fixed;
}

void setContainerSplit (Song& song, DeviceID container, ContainerSplit split)
{
    if (! canSplit (song, container, split) || getContainerSplit (song, container) == split)
        return;

    auto state = findDeviceState (song, container);
    auto* undo = getUndo (song);

    switch (split)
    {
        case ContainerSplit::layers:
            state.removeProperty (schema::containerSplit, undo);
            break;
        case ContainerSplit::bands:
            state.setProperty (schema::containerSplit, schema::splitBands, undo);
            break;
        case ContainerSplit::midSide:
            state.setProperty (schema::containerSplit, schema::splitMidSide, undo);
            break;
        case ContainerSplit::leftRight:
            state.setProperty (schema::containerSplit, schema::splitLeftRight, undo);
            break;
    }

    // The middle and the side, or the left and the right, are two parts: the second's chain is there too.
    while ((int) getChainStates (state).size() < getFixedParts (split))
        state.addChild (makeChainState (song, getNextChainNumber (state)), state.indexOf (getChainStates (state).back()) + 1, undo);

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
    const auto state = findContainerChain (song, chain);
    const auto chains = getChainStates (state.getParent());
    const auto index = (int) (std::find (chains.begin(), chains.end(), state) - chains.begin());
    return state.isValid() && index > 0 ? readCrossover (state, index, (int) chains.size()) : 0.0f;
}

void setChainCrossover (Song& song, DeviceID chain, float hertz)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
        state.setProperty (schema::chainCrossover,
                           std::round ((double) juce::jlimit (lowestCrossover, highestCrossover, hertz) * 100.0) / 100.0, getUndo (song));
}

void keepCrossoversInOrder (Song& song, DeviceID container)
{
    keepCrossoversInOrder (findDeviceState (song, container), getUndo (song));
}

void keepCrossoversInOrder (juce::ValueTree state, juce::UndoManager* undo)
{
    if (readSplit (state) != ContainerSplit::bands)
        return;

    // Each chain's band above the one before it: a chain without a crossover gets its place's default if that is between its
    // neighbours', else the middle between them, in octaves.
    const auto chains = getChainStates (state);
    const auto count = (int) chains.size();
    auto below = lowestCrossover;

    for (int i = 1; i < count; ++i)
    {
        auto chain = chains[(size_t) i];

        if (! chain.hasProperty (schema::chainCrossover))
        {
            auto above = highestCrossover;

            for (int j = i + 1; j < count; ++j)
            {
                if (chains[(size_t) j].hasProperty (schema::chainCrossover))
                {
                    above = (float) chains[(size_t) j][schema::chainCrossover];
                    break;
                }
            }

            auto value = getDefaultCrossover (i, count);

            if (value <= below || value >= above)
                value = std::sqrt (below * above);

            chain.setProperty (schema::chainCrossover, std::round ((double) value * 100.0) / 100.0, undo);
        }

        below = (float) chain[schema::chainCrossover];
    }
}

NoteRange getChainKeys (const Song& song, DeviceID chain)
{
    const auto state = findContainerChain (song, chain);
    return { (int) state.getProperty (schema::chainLowKey, allKeys.low), (int) state.getProperty (schema::chainHighKey, allKeys.high) };
}

NoteRange getChainVelocities (const Song& song, DeviceID chain)
{
    const auto state = findContainerChain (song, chain);
    return { (int) state.getProperty (schema::chainLowVelocity, allVelocities.low),
             (int) state.getProperty (schema::chainHighVelocity, allVelocities.high) };
}

void setChainKeys (Song& song, DeviceID chain, NoteRange keys)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
    {
        const auto low = juce::jlimit (allKeys.low, allKeys.high, keys.low);
        const auto high = juce::jlimit (low, allKeys.high, keys.high);
        setChainNumber (state, schema::chainLowKey, low, allKeys.low, getUndo (song));
        setChainNumber (state, schema::chainHighKey, high, allKeys.high, getUndo (song));
    }
}

void setChainVelocities (Song& song, DeviceID chain, NoteRange velocities)
{
    if (auto state = findContainerChain (song, chain); state.isValid())
    {
        const auto low = juce::jlimit (allVelocities.low, allVelocities.high, velocities.low);
        const auto high = juce::jlimit (low, allVelocities.high, velocities.high);
        setChainNumber (state, schema::chainLowVelocity, low, allVelocities.low, getUndo (song));
        setChainNumber (state, schema::chainHighVelocity, high, allVelocities.high, getUndo (song));
    }
}

//==============================================================================
bool canGroupIntoContainer (const Song& song, const std::vector<DeviceID>& devices)
{
    if (devices.empty())
        return false;

    // Devices on one channel, none in a container nor a container itself; an instrument only as its track's, its first.
    const auto channel = findDeviceState (song, devices.front()).getParent();

    if (! channel.hasType (schema::TRACK) && ! channel.hasType (schema::MASTER))
        return false;

    const auto onChannel = getDeviceStates (channel);

    return std::all_of (devices.begin(), devices.end(),
                        [&song, &channel, &onChannel] (DeviceID id)
                        {
                            const auto state = findDeviceState (song, id);
                            return state.getParent() == channel && state.hasType (schema::DEVICE)
                                   && (! isInstrumentState (state) || state == onChannel.front());
                        });
}

DeviceID groupIntoContainer (Song& song, const std::vector<DeviceID>& devices)
{
    if (! canGroupIntoContainer (song, devices))
        return {};

    // In their order on the channel, in the first one's place: with the instrument, an instrument container in its place.
    auto channel = findDeviceState (song, devices.front()).getParent();
    std::vector<juce::ValueTree> grouped;

    for (const auto& state : getDeviceStates (channel))
        if (std::find (devices.begin(), devices.end(), getDeviceID (state)) != devices.end())
            grouped.push_back (state);

    const auto instrument = isInstrumentState (grouped.front());
    storeStatesOf (song, channel);
    auto container = makeContainerState (song, instrument);
    channel.addChild (container, channel.indexOf (grouped.front()), getUndo (song));
    moveInto (song, getChainStates (container).front(), grouped);
    return getDeviceID (container);
}

DeviceID addContainer (Song& song, TrackID channel)
{
    return insertDeviceState (song, channel, makeContainerState (song));
}

DeviceID addInstrumentContainer (Song& song, TrackID track)
{
    auto container = makeContainerState (song, true);
    const auto old = findDeviceState (song, getInstrument (song, track));

    if (! insertDeviceState (song, track, container).isValid())
        return {};

    // The instrument the track had, first in its chain, the container in its place.
    if (old.isValid())
    {
        storeStatesOf (song, old);
        moveInto (song, getChainStates (container).front(), { old });
    }

    return getDeviceID (container);
}

bool canUngroupContainer (const Song& song, DeviceID container)
{
    return getChainStates (findDeviceState (song, container)).size() == 1;
}

void ungroupContainer (Song& song, DeviceID id)
{
    if (! canUngroupContainer (song, id))
        return;

    // Its devices' elements go back in its place, in their order, and it goes with its chain and its macros and what they
    // modulated.
    auto state = findDeviceState (song, id);
    auto* undo = getUndo (song);
    auto channel = state.getParent();
    auto index = channel.indexOf (state);
    storeStatesOf (song, channel);

    for (auto inside : getDeviceStates (state))
    {
        inside.getParent().removeChild (inside, undo);
        channel.addChild (inside, index++, undo);
    }

    channel.removeChild (state, undo);
}

void setContainerName (Song& song, DeviceID id, const juce::String& name)
{
    if (auto state = findDeviceState (song, id); isContainerState (state))
        state.setProperty (schema::containerName, name, &song.getUndoManager());
}

bool canHoldInChain (const Song& song, DeviceID chain, const juce::ValueTree& device)
{
    const auto container = getOwningContainer (findContainerChain (song, chain));
    return container.isValid() && device.hasType (schema::DEVICE)
           && (! isInstrumentState (device) || (bool) container[schema::containerInstrument]);
}

DeviceID insertIntoChain (Song& song, DeviceID chain, juce::ValueTree device, int index)
{
    if (! canHoldInChain (song, chain, device))
        return {};

    auto state = findContainerChain (song, chain);
    auto* undo = getUndo (song);

    // An instrument first, in place of the chain's; an effect where it is put, after the chain's instrument.
    if (isInstrumentState (device))
    {
        if (const auto old = getChainInstrument (song, chain); old.isValid())
        {
            storeStatesOf (song, state);
            state.removeChild (findDeviceState (song, old), undo);
        }

        state.addChild (device, 0, undo);
        return getDeviceID (device);
    }

    const auto devices = getDeviceStates (state);
    const auto first = ! devices.empty() && isInstrumentState (devices.front()) ? 1 : 0;
    const auto at = juce::isPositiveAndBelow (index, (int) devices.size()) ? std::max (index, first) : (int) devices.size();
    state.addChild (device, at < (int) devices.size() ? state.indexOf (devices[(size_t) at]) : -1, undo);
    return getDeviceID (device);
}

void moveInChain (Song& song, DeviceID device, int newIndex)
{
    auto state = findDeviceState (song, device);
    auto chain = state.getParent();

    if (! isChainState (chain) || isInstrumentState (state))
        return;

    // To the place of the device that is there now: after it when moving later, before it when moving earlier; never
    // before the chain's instrument.
    const auto devices = getDeviceStates (chain);
    const auto first = isInstrumentState (devices.front()) ? 1 : 0;
    const auto& target = devices[(size_t) juce::jlimit (first, (int) devices.size() - 1, newIndex)];
    chain.moveChild (chain.indexOf (state), chain.indexOf (target), getUndo (song));
}

DeviceID moveToChain (Song& song, DeviceID device, DeviceID chain, int index)
{
    auto state = findDeviceState (song, device);
    auto from = state.getParent();
    auto to = findContainerChain (song, chain);

    // An effect, into another chain of its container.
    if (! isChainState (from) || ! to.isValid() || from.getParent() != to.getParent() || isInstrumentState (state))
        return {};

    if (from == to)
    {
        moveInChain (song, device, index);
        return device;
    }

    storeStatesOf (song, from);
    from.removeChild (state, getUndo (song));
    return insertIntoChain (song, chain, state, index);
}

DeviceID getChainInstrument (const Song& song, DeviceID chain)
{
    const auto devices = getDeviceStates (findContainerChain (song, chain));
    return ! devices.empty() && isInstrumentState (devices.front()) ? getDeviceID (devices.front()) : DeviceID();
}

juce::ValueTree makeContainerState (Song& song, bool instrument)
{
    juce::ValueTree container (schema::CONTAINER);
    container.setProperty (schema::deviceID, (juce::int64) song.makeID(), nullptr);
    container.setProperty (schema::containerName, makeContainerName (song), nullptr);

    if (instrument)
        container.setProperty (schema::containerInstrument, true, nullptr);

    container.appendChild (makeChainState (song, 1), nullptr);

    for (int number = 1; number <= macrosPerContainer; ++number)
    {
        juce::ValueTree macro (schema::MACRO);
        macro.setProperty (schema::modulatorID, (juce::int64) song.makeID(), nullptr);
        macro.setProperty (schema::macroName, "Macro " + juce::String (number), nullptr);
        container.appendChild (macro, nullptr);
    }

    return container;
}

juce::ValueTree makeChainState (Song& song, int number)
{
    juce::ValueTree chain (schema::CHAIN);
    chain.setProperty (schema::chainID, (juce::int64) song.makeID(), nullptr);
    chain.setProperty (schema::chainName, "Chain " + juce::String (number), nullptr);
    return chain;
}

juce::String makeContainerName (const Song& song)
{
    juce::StringArray taken;
    auto chains = getTrackStates (song);
    chains.push_back (findChainState (song, {}));

    for (const auto& chain : chains)
        for (const auto& state : getDeviceStates (chain))
            if (isContainerState (state))
                taken.add (state[schema::containerName].toString());

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
    registry.add ({ .id = "device.group",
                    .name = "Group into Container",
                    .category = "Device",
                    .description = "Groups the selected devices into a container, with eight macros that turn several knobs at once "
                                   "and chains that play side by side: effects, or a track's instrument with its effects.",
                    .synonyms = { "rack", "chain", "fx chain", "combine", "macro", "group devices", "layer", "instrument rack" },
                    .perform = [&selection] { groupSelectedDevices (selection); },
                    .isEnabled = [&selection] { return canGroupSelectedDevices (selection); },
                    .undoTarget = undoTargetOf (selection),
                    .keyPriority = Command::notePriority }); // shares Ctrl+G with grouping tracks: devices are selected on their own

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
