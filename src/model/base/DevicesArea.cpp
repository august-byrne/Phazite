// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DevicesArea.h"
#include "Song.h"
#include "internal/Schema.h"
#include "model/mixer/internal/Sidechain.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = SongValues::DeviceChange;
    using Form = DeviceValue::Form;
    using IDList = immer::flex_vector<DeviceID>;

    bool isDeviceElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::CHAIN);
    }

    // What is the devices' in a file and none of the song's tree's: a DEVICE, a CONTAINER, a CHAIN, and a device's VALUES and
    // SOUNDS (a modulator's VALUES are modulation's, and the song's tree holds no modulator either).
    bool isDevicesElement (const juce::ValueTree& element)
    {
        return isDeviceElement (element) || element.hasType (schema::VALUES) || element.hasType (schema::SOUNDS);
    }

    bool isFolder (const juce::ValueTree& track)
    {
        return track[schema::trackKind].toString() == schema::trackIsFolder;
    }

    TrackID readTrackID (const juce::ValueTree& track)
    {
        return track.hasType (schema::TRACK) ? TrackID::fromRaw (readID (track[schema::trackID])) : TrackID();
    }

    // Each TRACK of a song's tree in the track list, a folder before the tracks in it, then the MASTER, each with the place
    // of what is on it.
    void forEachChannel (const juce::ValueTree& song, const std::function<void (const juce::ValueTree&, const DevicePlace&)>& each)
    {
        const std::function<void (const juce::ValueTree&)> inTracks = [&each, &inTracks] (const juce::ValueTree& parent)
        {
            for (const auto& child : parent)
            {
                if (! child.hasType (schema::TRACK))
                    continue;

                each (child, makeChannelPlace (readTrackID (child), false));

                if (isFolder (child))
                    inTracks (child);
            }
        };

        inTracks (song.getChildWithName (schema::TRACKS));

        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
            each (master, makeChannelPlace ({}, true));
    }

    // A channel's element's devices and containers, each container's chains and each chain's devices, read as the song holds
    // them (readHeldDevice()) at their places into a value, in their order, after what it holds: one of an ID it holds
    // already, or that readHeldDevice() reads none of, left out with what it holds. Each element read counted in `taken`, and
    // each DEVICE and CONTAINER of the channel's put in `elements`, for the caller to take out of the tree.
    void readChannelInto (const juce::ValueTree& channel, const DevicePlace& place, SongDevices& devices,
                          std::vector<juce::ValueTree>* elements, int& taken)
    {
        const auto add = [&devices, &taken] (const juce::ValueTree& element, const DevicePlace& at)
        {
            auto value = readHeldDevice (element, at);

            if (! value.has_value() || devices.find (value->id) != nullptr)
                return DeviceID();

            const auto id = value->id;
            devices = withDevice (devices, std::move (*value));
            ++taken;
            return id;
        };

        for (const auto& child : channel)
        {
            if (! (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)))
                continue;

            if (elements != nullptr)
                elements->push_back (child);

            const auto id = add (child, place);

            if (! id.isValid() || ! child.hasType (schema::CONTAINER))
                continue;

            for (const auto& chain : child)
            {
                if (! chain.hasType (schema::CHAIN))
                    continue;

                if (const auto chainID = add (chain, makeContainerPlace (place, id)); chainID.isValid())
                    for (const auto& device : chain)
                        if (device.hasType (schema::DEVICE))
                            add (device, makeContainerPlace (place, id, chainID));
            }
        }
    }

    // Every DEVICE, CONTAINER and CHAIN element in an element and what it holds, taken out: what the adoption leaves of a
    // file's anywhere in the song's tree once those of the song's channels are the area's. So the song's tree holds none.
    void dropDeviceElements (juce::ValueTree element)
    {
        for (auto i = element.getNumChildren(); --i >= 0;)
        {
            if (auto child = element.getChild (i); isDeviceElement (child))
                element.removeChild (i, nullptr);
            else
                dropDeviceElements (child);
        }
    }

    // What of an item differs between two of its values (the same ID).
    std::uint16_t findParts (const DeviceValue& was, const DeviceValue& now)
    {
        std::uint16_t parts = 0;
        const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

        if (was.place != now.place)
            add (Change::place);

        if (was.form != now.form || was.off != now.off || was.sidechain != now.sidechain || was.type != now.type
            || ! (was.plugin == now.plugin) || was.name != now.name || was.split != now.split || was.instrument != now.instrument)
            add (Change::settings);

        if (! juce::exactlyEqual (was.mix, now.mix))
            add (Change::mix);

        // Its values by what differs (immer: by the paths one doesn't share with the other), each asked whether it turns what
        // its sidechain hears on or off.
        if (was.values.identity() != now.values.identity())
        {
            const auto typeID = std::string_view (now.type.toRawUTF8());
            const auto mark = [&add, &typeID] (std::uint32_t index)
            {
                add (Change::values);

                if (switchesSidechain (typeID, (int) index))
                    add (Change::sidechainSwitched);
            };

            immer::diff (
                was.values, now.values, [&mark] (const auto& entry) { mark (entry.first); },
                [&mark] (const auto& entry) { mark (entry.first); },
                [&mark] (const auto& entry, const auto& other)
                {
                    if (! juce::exactlyEqual (entry.second, other.second))
                        mark (entry.first);
                });

            if ((parts & Change::values) != 0 && was.values.empty() != now.values.empty())
                add (Change::valuesAppeared);
        }

        if (was.sounds.identity() != now.sounds.identity() && ! (was.sounds == now.sounds))
        {
            add (Change::sounds);

            if (was.sounds.empty() != now.sounds.empty())
                add (Change::soundsAppeared);
        }

        if (! juce::exactlyEqual (was.volume, now.volume) || ! juce::exactlyEqual (was.pan, now.pan))
            add (Change::chainFader);

        const auto sameCrossover = was.crossover.has_value() == now.crossover.has_value()
                                   && (! was.crossover.has_value() || juce::exactlyEqual (*was.crossover, *now.crossover));

        if (! sameCrossover || was.keys != now.keys || was.velocities != now.velocities)
            add (Change::chainRange);

        return parts;
    }

    Change makeChange (const DeviceValue* was, const DeviceValue* now, std::uint16_t parts)
    {
        Change change;
        change.id = now != nullptr ? now->id : was->id;
        change.form = now != nullptr ? now->form : was->form;
        change.wasThere = was != nullptr;
        change.isThere = now != nullptr;
        change.parts = parts;

        if (was != nullptr)
            change.before = was->place;

        if (now != nullptr)
            change.after = now->place;

        return change;
    }

    // Two entries of the area alike without a look inside (DeviceValue::isSameAs()), and two lists the same one.
    struct SameDevice
    {
        bool operator() (const std::pair<std::uint64_t, DeviceValue>& a, const std::pair<std::uint64_t, DeviceValue>& b) const noexcept
        {
            return a.first == b.first && a.second.isSameAs (b.second);
        }
    };

    struct SameList
    {
        bool operator() (const std::pair<std::uint64_t, IDList>& a, const std::pair<std::uint64_t, IDList>& b) const noexcept
        {
            return a.first == b.first && a.second.identity() == b.second.identity();
        }
    };

    // Whether a list holds those of another that it still holds in another order.
    bool isReordered (const IDList& was, const IDList& now)
    {
        const auto holds = [] (const IDList& list, DeviceID id) { return std::find (list.begin(), list.end(), id) != list.end(); };
        std::vector<DeviceID> keptBefore, keptNow;

        for (const auto id : was)
            if (holds (now, id))
                keptBefore.push_back (id);

        for (const auto id : now)
            if (holds (was, id))
                keptNow.push_back (id);

        return keptBefore != keptNow;
    }

    void setArea (Song& song, ValueArea<SongDevices>& area, SongDevices next)
    {
        if (song.isWritingOutsideHistory())
            area.setOutsideHistory (std::move (next));
        else
            area.set (std::move (next));
    }

    // A channel's list in kept values, none for an element that is no channel (a TRACK by its ID, the MASTER under 0).
    const IDList* findChannelList (const SongDevices& devices, const juce::ValueTree& element)
    {
        if (element.hasType (schema::TRACK))
            return devices.channels.find (readID (element[schema::trackID]));

        return element.hasType (schema::MASTER) ? devices.channels.find (0) : nullptr;
    }

    // Whether two items, each with what it holds (a container's chains, a chain's devices) and what the store holds of each
    // (DeviceStoreArea.cpp: holdSameDeviceStore()), are the same in two songs' kept values.
    bool holdSameItem (const SongState& keptA, const SongState& keptB, DeviceID id)
    {
        const auto& a = keptA.devices;
        const auto& b = keptB.devices;
        const auto* first = a.find (id);
        const auto* second = b.find (id);

        if (first == nullptr || second == nullptr)
            return first == second;

        if (! (first->isSameAs (*second) || *first == *second) || ! holdSameDeviceStore (keptA, keptB, id))
            return false;

        const auto* listA = first->form == Form::container ? a.chains.find (id.getRaw())
                            : first->form == Form::chain   ? a.chainDevices.find (id.getRaw())
                                                           : nullptr;
        const auto* listB = first->form == Form::container ? b.chains.find (id.getRaw())
                            : first->form == Form::chain   ? b.chainDevices.find (id.getRaw())
                                                           : nullptr;

        if (listA == nullptr || listB == nullptr)
            return listA == listB;

        if (! (*listA == *listB))
            return false;

        return std::all_of (listA->begin(), listA->end(),
                            [&keptA, &keptB] (DeviceID inside) { return holdSameItem (keptA, keptB, inside); });
    }
} // namespace

//==============================================================================
DevicePlace getDeviceListPlace (const DevicePlace& place, DeviceValue::Form form)
{
    if (form == Form::chain)
        return makeContainerPlace (place, place.container);

    if (place.chain.isValid())
        return place;

    return makeChannelPlace (place.track, place.onMaster);
}

DevicePlace makeChannelPlace (TrackID track, bool onMaster)
{
    return { .track = onMaster ? TrackID() : track, .onMaster = onMaster };
}

DevicePlace makeContainerPlace (const DevicePlace& channel, DeviceID container, DeviceID chain)
{
    return { .track = channel.track, .onMaster = channel.onMaster, .container = container, .chain = chain };
}

//==============================================================================
SongValues::DevicesTeller::DevicesTeller (ValueArea<SongDevices>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::DevicesTeller::~DevicesTeller()
{
    area.removeListener (this);
}

void SongValues::DevicesTeller::valueAreaChanged (const SongDevices& before, const SongDevices& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // Each value before and after, in copies of the values told, which stay while the listeners are told whatever one of them
    // changes meanwhile (cheap: they share what they hold).
    const auto was = before;
    const auto now = after;
    std::vector<DeviceChange> changes;

    was.devices.impl().template diff<SameDevice> (
        now.devices.impl(),
        immer::make_differ ([&changes] (const std::pair<std::uint64_t, DeviceValue>& added)
                            { changes.push_back (makeChange (nullptr, &added.second, Change::made)); },
                            [&changes] (const std::pair<std::uint64_t, DeviceValue>& removed)
                            { changes.push_back (makeChange (&removed.second, nullptr, Change::made)); },
                            [&changes] (const std::pair<std::uint64_t, DeviceValue>& old, const std::pair<std::uint64_t, DeviceValue>& is)
                            {
                                if (const auto parts = findParts (old.second, is.second); parts != 0)
                                    changes.push_back (makeChange (&old.second, &is.second, parts));
                            }));

    // Each owner whose list holds those it held before in another order: a channel's (by its track's ID, the master's under 0),
    // a container's chains (its place found in the value after), a chain's devices (the same).
    std::vector<DevicePlace> reordered;
    const auto ignore = [] (const std::pair<std::uint64_t, IDList>&) {};
    const auto compareLists = [&reordered, &now, &ignore] (const auto& listsBefore, const auto& listsAfter, int kind)
    {
        listsBefore.impl().template diff<SameList> (
            listsAfter.impl(),
            immer::make_differ (ignore, ignore,
                                [&reordered, &now, kind] (const std::pair<std::uint64_t, IDList>& listBefore,
                                                          const std::pair<std::uint64_t, IDList>& listAfter)
                                {
                                    if (! isReordered (listBefore.second, listAfter.second))
                                        return;

                                    const auto owner = DeviceID::fromRaw (listAfter.first);

                                    if (kind == 0)
                                    {
                                        reordered.push_back (makeChannelPlace (TrackID::fromRaw (listAfter.first), listAfter.first == 0));
                                    }
                                    else if (const auto* found = now.find (owner))
                                    {
                                        reordered.push_back (kind == 1 ? makeContainerPlace (found->place, owner)
                                                                       : makeContainerPlace (found->place, found->place.container, owner));
                                    }
                                }));
    };

    compareLists (was.channels, now.channels, 0);
    compareLists (was.chains, now.chains, 1);
    compareLists (was.chainDevices, now.chainDevices, 2);

    if (changes.empty() && reordered.empty())
        return;

    std::sort (changes.begin(), changes.end(), [] (const DeviceChange& a, const DeviceChange& b) { return a.id < b.id; });

    for (auto& change : changes)
    {
        change.was = change.wasThere ? was.find (change.id) : nullptr;
        change.now = change.isThere ? now.find (change.id) : nullptr;
    }

    listeners.call ([&changes, &reordered, &now] (SongValues::Listener& listener) { listener.devicesChanged (changes, reordered, now); });
}

//==============================================================================
SongValues::DevicesKeeper::DevicesKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

bool SongValues::DevicesKeeper::isInSong (const juce::ValueTree& element) const
{
    return element == state || element.isAChildOf (state);
}

void SongValues::DevicesKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    // A lone one, added to an element of the song's: refused in a step; outside the history, a device or a container on the
    // master (the one channel in the song's tree) read in, and taken out.
    if (! isDevicesElement (child) || ! isInSong (parent))
        return;

    if (! song.isWritingOutsideHistory())
    {
        jassertfalse; // a device's element in the song's tree: the devices' setters set one (model/mixer/internal/Mixer.h)
        return;
    }

    if ((child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)) && parent.hasType (schema::MASTER))
    {
        auto next = values.devices.get();
        auto taken = 0;
        juce::ValueTree alone (parent.getType());
        alone.copyPropertiesFrom (parent, nullptr);
        alone.appendChild (child.createCopy(), nullptr);
        readChannelInto (alone, makeChannelPlace ({}, true), next, nullptr, taken);

        if (taken > 0)
        {
            values.devicesAdopted += taken;
            values.devices.setOutsideHistory (std::move (next));
        }
    }

    parent.removeChild (child, nullptr);
}

void SongValues::DevicesKeeper::adopt (SongValues&, Song& loaded)
{
    // Every DEVICE, CONTAINER and CHAIN element out of the tree, those anywhere else too, which are no devices of the song's,
    // its reader having read the song's into the area before (readHeldDevicesIn(): SongValues.cpp, adopt()), and what the store
    // holds of each (DeviceStoreArea.h) and a container's macros (ModulationArea.h) taken out of them before.
    dropDeviceElements (loaded.getState());
}

void readHeldDevicesIn (const juce::ValueTree& song, SongState& into)
{
    // Every device, container and chain the loader made consistent, each read once as the song holds it, in the elements'
    // order (each track's, a folder's before the tracks in it, then the master's; each channel's in its order, a container's
    // chains and a chain's devices in theirs; the first of an ID); what the loader left of each is what the song holds of it
    // (DevicesValue.h: keepDeviceAsHeld()), so reading it changes nothing of what the file held. What the store holds of each
    // and a container's macros are no part of a device's value: their areas' (readHeldDeviceStoreIn(), readHeldModulationIn()).
    auto held = into.devices;
    auto read = 0;

    forEachChannel (song, [&held, &read] (const juce::ValueTree& channel, const DevicePlace& place)
                    { readChannelInto (channel, place, held, nullptr, read); });

    jassert (holdsItsLists (held));
    into.devices = std::move (held);
}

//==============================================================================
bool SongValues::setDevice (Song& song, DeviceValue value, int index)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (! value.id.isValid())
        return false;

    const auto& held = devices.get();

    if (const auto* was = held.find (value.id); was != nullptr && *was == value)
        return false;

    setArea (song, devices, withDevice (held, std::move (value), index));
    return true;
}

bool SongValues::eraseDevice (Song& song, DeviceID id)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (devices.get().find (id) == nullptr)
        return false;

    setArea (song, devices, withoutDevice (devices.get(), id));
    return true;
}

bool SongValues::setDeviceListOrder (Song& song, const DevicePlace& owner, immer::flex_vector<DeviceID> order)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (const auto* list = devices.get().findList (owner); list == nullptr || *list == order)
        return false;

    auto next = withListOrder (devices.get(), owner, std::move (order));

    if (next.isSameAs (devices.get()))
        return false;

    setArea (song, devices, std::move (next));
    return true;
}

bool SongValues::setAllDevices (Song& song, SongDevices next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    // A pass's, an edit's of several and the keeper's, each held together as it was worked out: holdsItsLists() is the check's
    // (tests/SongCheck.cpp), not an assertion here, which would walk every device of the song at every add.
    if (next.isSameAs (devices.get()) || next == devices.get())
        return false;

    setArea (song, devices, std::move (next));
    return true;
}

//==============================================================================
juce::ValueTree writeDeviceWhole (const SongDevices& devices, DeviceID id)
{
    const auto* value = devices.find (id);

    if (value == nullptr)
        return {};

    auto element = writeDeviceElement (*value);
    const auto* inside = value->form == Form::container ? devices.chains.find (id.getRaw())
                         : value->form == Form::chain   ? devices.chainDevices.find (id.getRaw())
                                                        : nullptr;

    if (inside != nullptr)
        for (const auto each : *inside)
            if (auto child = writeDeviceWhole (devices, each); child.isValid())
                element.appendChild (child, nullptr);

    return element;
}

int takeChannelDevicesIn (SongDevices& devices, const juce::ValueTree& channel, const DevicePlace& place)
{
    auto taken = 0;
    readChannelInto (channel, place, devices, nullptr, taken);
    return taken;
}

bool leadsToDevices (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::MASTER);
}

void writeDevicesIn (juce::ValueTree& element, const SongState& kept)
{
    const auto* list = findChannelList (kept.devices, element);

    if (list == nullptr)
        return;

    // Right after its clips, which the clips' part writes first among its children (ClipsArea.h: writeClipsIn()).
    auto place = 0;

    while (place < element.getNumChildren() && element.getChild (place).hasType (schema::CLIP))
        ++place;

    for (const auto id : *list)
        if (auto child = writeDeviceWhole (kept.devices, id); child.isValid())
            element.addChild (child, place++, nullptr);
}

bool holdSameDevices (const SongState& a, const SongState& b)
{
    return a.devices.isSameAs (b.devices) || a.devices == b.devices;
}

bool holdSameDevice (const SongState& a, const SongState& b, DeviceID id)
{
    const auto* first = a.devices.find (id);
    const auto* second = b.devices.find (id);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->isSameAs (*second) || *first == *second;
}

bool holdSameDeviceOrder (const SongState& a, const SongState& b, const DevicePlace& owner)
{
    const auto* first = a.devices.findList (owner);
    const auto* second = b.devices.findList (owner);

    if (first == nullptr || second == nullptr)
        return first == second;

    return first->identity() == second->identity() || *first == *second;
}

bool holdSameDevicesOf (const juce::ValueTree& element, const SongState& kept, const juce::ValueTree& otherElement,
                        const SongState& otherKept)
{
    if (! (element.hasType (schema::TRACK) || element.hasType (schema::MASTER)))
        return true;

    const auto* list = findChannelList (kept.devices, element);
    const auto* otherList = findChannelList (otherKept.devices, otherElement);

    if (list == nullptr || otherList == nullptr)
        return list == otherList;

    if (! (*list == *otherList))
        return false;

    return std::all_of (list->begin(), list->end(), [&kept, &otherKept] (DeviceID id) { return holdSameItem (kept, otherKept, id); });
}

//==============================================================================
bool isHeardAsDevices (const juce::ValueTree& element)
{
    // What the keeper takes out of a channel that came in with it (its safety net), and what is in one.
    for (auto each = element; each.isValid(); each = each.getParent())
        if (isDeviceElement (each))
            return true;

    return false;
}

bool isHeardAsDevices (const juce::ValueTree& element, const juce::Identifier&)
{
    return isHeardAsDevices (element);
}

bool isHeardAsDevices (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return isDevicesElement (child) || isHeardAsDevices (parent);
}
} // namespace phazite
