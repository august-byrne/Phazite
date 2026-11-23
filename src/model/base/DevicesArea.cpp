// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DevicesArea.h"
#include "Song.h"
#include "internal/SavedValues.h"
#include "internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
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

    std::atomic<std::uint64_t> itemsWalked { 0 }; // countDeviceItemsWalked()

    bool isDeviceElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::CHAIN);
    }

    bool isChannelElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACK) || element.hasType (schema::MASTER);
    }

    // Where what is above the devices begins: a channel, the track list, the song.
    bool isAboveDevices (const juce::ValueTree& element)
    {
        return isChannelElement (element) || element.hasType (schema::TRACKS) || element.hasType (schema::PHAZITE);
    }

    bool isFolder (const juce::ValueTree& track)
    {
        return track[schema::trackKind].toString() == schema::trackIsFolder;
    }

    // An ID an element holds, as readID() reads one, without a text made of a number.
    std::uint64_t readIDOf (const juce::var& held)
    {
        return held.isInt() || held.isInt64() ? (std::uint64_t) (juce::int64) held : readID (held);
    }

    // Whether an ID is one the song holds an item by (readHeldDevice()).
    bool isHeldID (DeviceID id)
    {
        return id.isValid() && id.getRaw() < schema::maxID;
    }

    // Whether a property of an element is one the devices' value doesn't cover and keeps for the keyed store (a plug-in's
    // state, program, format ID and version; a chain's mute and solo) or for the value outside the history (a window's
    // place): the line between the devices' listener and the tree's, written once. The store's VALUE children are the other
    // half of that line (isHeardAsDevices()).
    bool isDeviceStoreProperty (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (element.hasType (schema::DEVICE))
            return property == schema::pluginState || property == schema::pluginProgram || property == schema::pluginFormatID
                   || property == schema::pluginVersion || schema::isDeviceViewProperty (property);

        return element.hasType (schema::CHAIN) && (property == schema::chainMute || property == schema::chainSolo);
    }

    // Whether a DEVICE, a CONTAINER or a CHAIN is on a channel: a TRACK or the MASTER above it, through the containers and
    // chains it is in.
    bool isOnChannel (const juce::ValueTree& element)
    {
        for (auto each = element.getParent(); each.isValid(); each = each.getParent())
        {
            if (isChannelElement (each))
                return true;

            if (! isDeviceElement (each))
                return false;
        }

        return false;
    }

    // The DEVICE, CONTAINER or CHAIN an element is, or is in, on a channel, where nothing on the way is a plug-in's VALUE (the
    // store's); an invalid tree for none.
    juce::ValueTree findDevicesElement (const juce::ValueTree& element)
    {
        for (auto each = element; each.isValid(); each = each.getParent())
        {
            if (each.hasType (schema::VALUE))
                return {};

            if (isDeviceElement (each))
                return isOnChannel (each) ? each : juce::ValueTree();

            if (isAboveDevices (each))
                return {};
        }

        return {};
    }

    // Whether a TRACK, the MASTER or the track list holds a DEVICE or a CONTAINER, itself or in a track in it.
    bool holdsDevicesIn (const juce::ValueTree& element)
    {
        for (const auto& child : element)
            if (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)
                || (child.hasType (schema::TRACK) && holdsDevicesIn (child)))
                return true;

        return false;
    }

    // Whether an owner (a channel, a container, a chain) holds an item of the devices' list among its children.
    bool holdsListedIn (const juce::ValueTree& owner)
    {
        return std::any_of (owner.begin(), owner.end(),
                            [&owner] (const juce::ValueTree& child)
                            {
                                if (isChannelElement (owner))
                                    return child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER);

                                return owner.hasType (schema::CONTAINER) ? child.hasType (schema::CHAIN) : child.hasType (schema::DEVICE);
                            });
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
                [&mark] (const auto& entry) { mark (entry.first); }, [&mark] (const auto& entry, const auto&) { mark (entry.first); });

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

    // Each channel of a song's tree as readDevicesValue() reads them (DevicesValue.h): each TRACK of the track list, a folder
    // before the tracks in it, then the MASTER, each with the place of what is on it.
    void forEachChannel (const juce::ValueTree& song, const std::function<void (const juce::ValueTree&, const DevicePlace&)>& each)
    {
        const std::function<void (const juce::ValueTree&)> inTracks = [&each, &inTracks] (const juce::ValueTree& parent)
        {
            for (const auto& child : parent)
            {
                if (! child.hasType (schema::TRACK))
                    continue;

                each (child, { .track = TrackID::fromRaw (readIDOf (child[schema::trackID])) });

                if (isFolder (child))
                    inTracks (child);
            }
        };

        inTracks (song.getChildWithName (schema::TRACKS));

        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
            each (master, { .onMaster = true });
    }

    // The place of what a container (by its ID) or a chain in one holds, on a channel's place.
    DevicePlace inContainer (const DevicePlace& channel, DeviceID container, DeviceID chain = {})
    {
        return { .track = channel.track, .onMaster = channel.onMaster, .container = container, .chain = chain };
    }

    // Each item of a song's tree as readDevicesValue() reads them, with its place and the place of the list it is in (its
    // owner: a channel's, a container's, a chain's), in that order: `take` says whether it is one (an ID read already is
    // none, and what it holds is left out with it). The DEVICE and CONTAINER elements of each channel, each CONTAINER's
    // CHAINs, each CHAIN's DEVICEs.
    using Take = std::function<bool (const juce::ValueTree& element, const DevicePlace& place)>;

    // The same of one channel (a TRACK or the MASTER) at its place, each element counted (countDeviceItemsWalked()).
    void forEachItemOn (const juce::ValueTree& channel, const DevicePlace& place, const Take& take)
    {
        const auto counted = [&take] (const juce::ValueTree& element, const DevicePlace& at)
        {
            itemsWalked.fetch_add (1, std::memory_order_relaxed);
            return take (element, at);
        };

        for (const auto& child : channel)
        {
            if (! (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)) || ! counted (child, place)
                || ! child.hasType (schema::CONTAINER))
                continue;

            const auto container = readDeviceElementID (child);

            for (const auto& chain : child)
                if (chain.hasType (schema::CHAIN) && counted (chain, inContainer (place, container)))
                    for (const auto& device : chain)
                        if (device.hasType (schema::DEVICE))
                            counted (device, inContainer (place, container, readDeviceElementID (chain)));
        }
    }

    void forEachItem (const juce::ValueTree& song, const Take& take)
    {
        forEachChannel (song, [&take] (const juce::ValueTree& channel, const DevicePlace& place) { forEachItemOn (channel, place, take); });
    }

    // An owner's list as a key: its kind (0 a channel's, 1 a container's, 2 a chain's) and its key there.
    std::pair<int, std::uint64_t> getListKey (const DevicePlace& owner)
    {
        if (owner.chain.isValid())
            return { 2, owner.chain.getRaw() };

        if (owner.container.isValid())
            return { 1, owner.container.getRaw() };

        return { 0, owner.onMaster ? 0 : owner.track.getRaw() };
    }

    // Whether those of one list that are in the other too are in the same order in both: each list stepped through, those
    // not in the other passed over, nothing made (a list the same as before, the most of them, compared at once).
    bool keepOrder (const IDList& before, const std::vector<DeviceID>& after)
    {
        if (std::equal (before.begin(), before.end(), after.begin(), after.end()))
            return true;

        const auto inAfter = [&after] (DeviceID id) { return std::find (after.begin(), after.end(), id) != after.end(); };
        const auto inBefore = [&before] (DeviceID id) { return std::find (before.begin(), before.end(), id) != before.end(); };
        auto was = before.begin();
        auto is = after.begin();

        for (;;)
        {
            while (was != before.end() && ! inAfter (*was))
                ++was;

            while (is != after.end() && ! inBefore (*is))
                ++is;

            if (was == before.end() || is == after.end())
                return was == before.end() && is == after.end();

            if (*was != *is)
                return false;

            ++was;
            ++is;
        }
    }
} // namespace

//==============================================================================
DevicePlace getDeviceListPlace (const DevicePlace& place, DeviceValue::Form form)
{
    if (form == Form::chain)
        return inContainer (place, place.container);

    if (place.chain.isValid())
        return place;

    return { .track = place.track, .onMaster = place.onMaster };
}

//==============================================================================
SongValues::DevicesKeeper::DevicesKeeper (SongValues& v, Song& s)
    : values (v),
      song (s)
{
    Heard heard;
    settle (heard); // what the song holds at the start, each read once, which no one is told
}

void SongValues::DevicesKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    const auto before = told;
    Heard heard;

    if (isDeviceElement (tree))
    {
        if (isDeviceStoreProperty (tree, property) || ! isOnChannel (tree))
            return;

        // Given another ID: the song's devices found again. Another type (or a plug-in's or none): read whole again, as its
        // values and sounds are another type's.
        if (property == schema::deviceID || property == schema::chainID)
        {
            settle (heard);
        }
        else if (property == schema::deviceType)
        {
            if (findKept (tree) != nullptr)
                rereadWhole (tree, heard);
            else
                settle (heard); // one the song didn't read may be one it reads now (a type Phazite plays)
        }
        else
        {
            rereadFields (tree, heard);
        }
    }
    else if (tree.hasType (schema::VALUES) && tree.getParent().hasType (schema::DEVICE))
    {
        rereadValue (tree.getParent(), tree, property, heard);
    }
    else if (tree.hasType (schema::SOUNDS) && tree.getParent().hasType (schema::DEVICE))
    {
        rereadSound (tree.getParent(), tree, property, heard);
    }
    else if (tree.hasType (schema::TRACK) && (property == schema::trackID || property == schema::trackKind) && holdsDevicesIn (tree))
    {
        settle (heard); // its devices on another channel, or its tracks' read or not (a folder's)
    }

    tell (heard, before);
}

void SongValues::DevicesKeeper::childAdded (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    const auto before = told;
    Heard heard;

    if (isDeviceElement (child))
        settleIn (parent, heard); // one added to its owner, with what it holds: its channel walked
    else if ((child.hasType (schema::VALUES) || child.hasType (schema::SOUNDS)) && parent.hasType (schema::DEVICE))
        rereadWhole (parent, heard); // a device's values or sounds come whole (an undo, a preset's)
    else if ((child.hasType (schema::TRACK) || child.hasType (schema::TRACKS) || child.hasType (schema::MASTER)) && holdsDevicesIn (child))
        settle (heard); // channels that came with theirs

    tell (heard, before);
}

void SongValues::DevicesKeeper::childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    const auto before = told;
    Heard heard;

    if (isDeviceElement (child))
        settleIn (parent, heard); // its channel walked
    else if ((child.hasType (schema::VALUES) || child.hasType (schema::SOUNDS)) && parent.hasType (schema::DEVICE))
        rereadWhole (parent, heard);
    else if ((child.hasType (schema::TRACK) || child.hasType (schema::TRACKS) || child.hasType (schema::MASTER)) && holdsDevicesIn (child))
        settle (heard); // channels that went with theirs

    tell (heard, before);
}

void SongValues::DevicesKeeper::childOrderChanged (const juce::ValueTree& parent)
{
    const auto before = told;
    Heard heard;

    // A channel's devices and containers, a container's chains or a chain's devices in another order (or something else of
    // the owner moved among them: nothing told then).
    if ((isChannelElement (parent) || parent.hasType (schema::CONTAINER) || parent.hasType (schema::CHAIN)) && holdsListedIn (parent))
        settleIn (parent, heard);

    tell (heard, before);
}

const DeviceValue* SongValues::DevicesKeeper::findKept (const juce::ValueTree& element) const
{
    const auto id = readDeviceElementID (element);
    const auto kept = elements.find (id.getRaw());
    return kept != elements.end() && kept->second.element == element ? told.find (id) : nullptr;
}

void SongValues::DevicesKeeper::take (const DeviceValue& was, DeviceValue now, Heard& heard)
{
    if (const auto parts = findParts (was, now); parts != 0)
    {
        heard.changes.push_back (makeChange (&was, &now, parts));
        told = withDevice (told, std::move (now));
    }
}

void SongValues::DevicesKeeper::rereadFields (const juce::ValueTree& element, Heard& heard)
{
    // Only the element the song reads that item from: another of its ID is read by no one.
    const auto* was = findKept (element);

    if (was == nullptr)
        return;

    auto now = readHeldDeviceFields (element, was->place);

    if (! now.has_value())
    {
        forget (element);
        return settle (heard); // one the song reads no more (an ID it can't count on)
    }

    now->values = was->values;
    now->sounds = was->sounds;
    take (*was, std::move (*now), heard);
}

void SongValues::DevicesKeeper::rereadWhole (const juce::ValueTree& device, Heard& heard)
{
    const auto* was = findKept (device);

    if (was == nullptr)
        return;

    if (auto now = readHeldDevice (device, was->place); now.has_value())
    {
        take (*was, std::move (*now), heard);
        return;
    }

    forget (device);
    settle (heard); // a type Phazite doesn't play: the song reads it no more
}

void SongValues::DevicesKeeper::forget (const juce::ValueTree& element)
{
    // Its element no longer the one its value was read from, so that the walk reads it again (and finds it none).
    if (const auto kept = elements.find (readDeviceElementID (element).getRaw()); kept != elements.end() && kept->second.element == element)
        kept->second.element = {};
}

void SongValues::DevicesKeeper::rereadValue (const juce::ValueTree& device, const juce::ValueTree& valuesElement,
                                             const juce::Identifier& parameter, Heard& heard)
{
    // One value of Phazite's own device, as readHeldDevice() reads it, from its first VALUES alone: none at its default.
    const auto* was = findKept (device);

    if (was == nullptr || was->form != Form::own || device.getChildWithName (schema::VALUES) != valuesElement)
        return;

    const auto* info = findOwnDevice (std::string_view (was->type.toRawUTF8()));
    const auto index = info != nullptr ? devices::findParameter (*info, std::string_view (parameter.getCharPointer().getAddress())) : -1;

    if (index < 0)
        return;

    const auto held = readHeldSavedValue (info->parameters[(size_t) index], valuesElement[parameter]);
    auto now = *was;
    now.values = held.has_value() ? was->values.set ((std::uint32_t) index, *held) : was->values.erase ((std::uint32_t) index);
    take (*was, std::move (now), heard);
}

void SongValues::DevicesKeeper::rereadSound (const juce::ValueTree& device, const juce::ValueTree& soundsElement,
                                             const juce::Identifier& property, Heard& heard)
{
    // One sound of Phazite's own device, from its first SOUNDS alone: none for an empty one, or a property its type hasn't.
    const auto* was = findKept (device);

    if (was == nullptr || was->form != Form::own || device.getChildWithName (schema::SOUNDS) != soundsElement)
        return;

    const auto index = findSoundIndex (std::string_view (was->type.toRawUTF8()), property);

    if (index < 0)
        return;

    const auto sound = soundsElement[property].toString();
    auto now = *was;
    now.sounds = sound.isNotEmpty() ? was->sounds.set ((std::uint16_t) index, sound) : was->sounds.erase ((std::uint16_t) index);
    take (*was, std::move (now), heard);
}

void SongValues::DevicesKeeper::settle (Heard& heard)
{
    // The song's items as readDevicesValue() finds them, each with its place and its owner's list, in the song's order: one
    // whose element and place are as before read no more; one whose element is as before come to another place given it; any
    // other read whole (readHeldDevice()), as the song holds it. Whether an ID, or a channel's track's, is the song's twice
    // (shadowed): while one is, every settle walks the song whole.
    countDeviceWalk();
    const auto walk = ++walks;
    std::vector<DeviceValue> taken; // new, moved or read again, in the song's order
    Lists lists;                    // each owner's, in its order
    std::vector<std::uint64_t> channels;
    auto twice = false;

    const Take take = [this, walk, &taken, &lists, &twice] (const juce::ValueTree& element, const DevicePlace& place)
    {
        const auto id = readDeviceElementID (element);

        if (! isHeldID (id))
            return false;

        const auto found = elements.find (id.getRaw());

        if (found != elements.end() && found->second.seenIn == walk)
        {
            twice = true;
            return false; // an ID read already: the first is the song's
        }

        const auto* was = told.find (id);

        if (was != nullptr && found != elements.end() && found->second.element == element)
        {
            if (was->place != place)
            {
                auto moved = *was;
                moved.place = place;
                taken.push_back (std::move (moved));
            }
        }
        else
        {
            auto now = readHeldDevice (element, place);

            if (! now.has_value())
                return false;

            taken.push_back (std::move (*now));
        }

        auto& kept = elements[id.getRaw()];
        kept.element = element;
        kept.seenIn = walk;
        addToList (lists, element, place, id);
        return true;
    };

    forEachChannel (song.getState(),
                    [&take, &channels] (const juce::ValueTree& channel, const DevicePlace& place)
                    {
                        channels.push_back (place.onMaster ? 0 : place.track.getRaw());
                        forEachItemOn (channel, place, take);
                    });

    std::sort (channels.begin(), channels.end());
    shadowed = twice || std::adjacent_find (channels.begin(), channels.end()) != channels.end();
    auto next = told;

    // Those it held that it holds no longer: taken out (a container with its chains and their devices, each told).
    for (auto kept = elements.begin(); kept != elements.end();)
    {
        if (kept->second.seenIn == walk)
        {
            ++kept;
            continue;
        }

        const auto id = DeviceID::fromRaw (kept->first);

        if (const auto* was = told.find (id))
        {
            heard.changes.push_back (makeChange (was, nullptr, Change::made));
            next = withoutDevice (next, id);
        }

        kept = elements.erase (kept);
    }

    finish (heard, std::move (next), taken, lists);
}

void SongValues::DevicesKeeper::settleIn (const juce::ValueTree& element, Heard& heard)
{
    // Only while no ID nor channel is the song's twice (which is first, a walk of the song alone knows), and for an element
    // on a channel the song's walk reads; else the song walked whole.
    const auto channel = shadowed ? std::nullopt : findChannelOf (element);

    if (! channel.has_value())
        return settle (heard);

    // The channel's items as the song's walk finds them there (each read whole that the listeners didn't know or that is
    // another element now), and those the listeners were told on it (the channel's list, its containers' chains, their
    // devices): nothing kept changed until both are known, so that the song can still be walked whole instead, for an item
    // kept from another element the song still holds, or an ID found twice.
    countDeviceWalk();
    const auto root = song.getState();
    std::vector<Found> found;
    auto whole = false;

    forEachItemOn (channel->element, channel->place,
                   [this, &root, &found, &whole] (const juce::ValueTree& item, const DevicePlace& place)
                   {
                       const auto id = readDeviceElementID (item);

                       if (whole || ! isHeldID (id))
                           return false;

                       const auto kept = elements.find (id.getRaw());
                       const auto isKept = kept != elements.end() && kept->second.element == item;

                       if (std::any_of (found.begin(), found.end(), [id] (const Found& each) { return each.id == id; })
                           || (kept != elements.end() && ! isKept && kept->second.element.getRoot() == root))
                       {
                           whole = true;
                           return false;
                       }

                       Found each { .id = id, .element = item, .place = place };
                       const auto* was = isKept ? told.find (id) : nullptr;

                       if (was != nullptr)
                       {
                           if (was->place != place)
                           {
                               each.taken = *was;
                               each.taken->place = place;
                           }
                       }
                       else
                       {
                           each.taken = readHeldDevice (item, place);

                           if (! each.taken.has_value())
                               return false;
                       }

                       found.push_back (std::move (each));
                       return true;
                   });

    std::vector<DeviceID> gone;
    const auto isFound = [&found] (DeviceID id)
    { return std::any_of (found.begin(), found.end(), [id] (const Found& each) { return each.id == id; }); };

    const auto goneFrom = [this, &isFound, &gone, &root, &whole] (const DevicePlace& owner)
    {
        if (const auto* list = told.findList (owner))
        {
            for (const auto id : *list)
            {
                if (isFound (id))
                    continue;

                if (const auto kept = elements.find (id.getRaw()); kept != elements.end() && kept->second.element.getRoot() == root)
                    whole = true; // one the song still holds elsewhere
                else
                    gone.push_back (id);
            }
        }
    };

    if (const auto* list = told.findList (channel->place))
    {
        for (const auto id : *list)
        {
            const auto* value = told.find (id);

            if (value == nullptr || value->form != Form::container)
                continue;

            if (const auto* chains = told.findList (inContainer (channel->place, id)))
                for (const auto chain : *chains)
                    goneFrom (inContainer (channel->place, id, chain));

            goneFrom (inContainer (channel->place, id));
        }
    }

    goneFrom (channel->place);

    if (whole)
        return settle (heard);

    // Each found kept as found now; each gone told taken out (a container with its chains and their devices); each that came,
    // moved or is read from another element told, in the channel's order.
    const auto walk = ++walks;
    std::vector<DeviceValue> taken;
    Lists lists;
    auto next = told;

    for (auto& each : found)
    {
        auto& kept = elements[each.id.getRaw()];
        kept.element = each.element;
        kept.seenIn = walk;
        addToList (lists, each.element, each.place, each.id);

        if (each.taken.has_value())
            taken.push_back (std::move (*each.taken));
    }

    for (const auto id : gone)
    {
        if (const auto* was = told.find (id))
        {
            heard.changes.push_back (makeChange (was, nullptr, Change::made));
            next = withoutDevice (next, id);
        }

        elements.erase (id.getRaw());
    }

    finish (heard, std::move (next), taken, lists);
}

void SongValues::DevicesKeeper::finish (Heard& heard, SongDevices next, std::vector<DeviceValue>& taken, const Lists& lists)
{
    // Those that came, from nowhere or from another place, or that are read from another element now, each in the song's
    // order (a container before its chains, a chain before its devices).
    for (auto& now : taken)
    {
        const auto* was = told.find (now.id);
        const auto parts = was != nullptr ? findParts (*was, now) : (std::uint16_t) Change::made;

        if (parts == 0)
            continue;

        heard.changes.push_back (makeChange (was, &now, parts));
        next = withDevice (next, std::move (now));
    }

    // Each list in its element's order; reordered where those it held before are in another order.
    for (const auto& [key, list] : lists)
    {
        const auto& [owner, order] = list;
        const auto* held = next.findList (owner);

        if (held != nullptr && ! std::equal (held->begin(), held->end(), order.begin(), order.end()))
            next = withListOrder (next, owner, IDList (order.begin(), order.end()));

        if (const auto* was = told.findList (owner); was != nullptr && ! keepOrder (*was, order))
            heard.reordered.push_back (owner);
    }

    // Whether its lists hold its items (holdsItsLists()) the tests ask (tests/SongCheck.cpp): it reads the song's devices whole.
    told = std::move (next);
}

std::optional<SongValues::DevicesKeeper::Channel> SongValues::DevicesKeeper::findChannelOf (const juce::ValueTree& element) const
{
    // Up through the devices, containers and chains it is in, to a channel the song's walk reads (forEachChannel()): the
    // song's MASTER, or a TRACK of its track list or of a folder there, by an ID the song can count on.
    auto channel = element;

    while (isDeviceElement (channel))
        channel = channel.getParent();

    const auto root = song.getState();

    if (channel.hasType (schema::MASTER))
    {
        if (channel.getParent() != root || root.getChildWithName (schema::MASTER) != channel)
            return std::nullopt;

        return Channel { .element = channel, .place = { .onMaster = true } };
    }

    if (! channel.hasType (schema::TRACK))
        return std::nullopt;

    for (auto parent = channel.getParent();; parent = parent.getParent())
    {
        if (parent.hasType (schema::TRACKS))
        {
            if (parent.getParent() != root || root.getChildWithName (schema::TRACKS) != parent)
                return std::nullopt;

            break;
        }

        if (! parent.hasType (schema::TRACK) || ! isFolder (parent))
            return std::nullopt;
    }

    const auto track = TrackID::fromRaw (readIDOf (channel[schema::trackID]));

    if (! track.isValid())
        return std::nullopt;

    return Channel { .element = channel, .place = { .track = track } };
}

void SongValues::DevicesKeeper::addToList (Lists& lists, const juce::ValueTree& element, const DevicePlace& place, DeviceID id)
{
    const auto owner = getDeviceListPlace (place, element.hasType (schema::CHAIN) ? Form::chain : Form::own);
    auto& list = lists[getListKey (owner)];
    list.first = owner;
    list.second.push_back (id);
}

void SongValues::DevicesKeeper::tell (Heard& heard, const SongDevices& before) const
{
    if (heard.changes.empty() && heard.reordered.empty())
        return;

    // Each value before and after, in copies of what the listeners were told and are told now, which stay while they are told
    // whatever one of them changes meanwhile (cheap: they share what they hold).
    const auto now = told;

    for (auto& change : heard.changes)
    {
        change.was = change.wasThere ? before.find (change.id) : nullptr;
        change.now = change.isThere ? now.find (change.id) : nullptr;
    }

    values.listeners.call ([&heard, &now] (SongValues::Listener& listener)
                           { listener.devicesChanged (heard.changes, heard.reordered, now); });
}

//==============================================================================
bool isHeardAsDevices (const juce::ValueTree& element)
{
    // A CONTAINER's or a CHAIN's children (its chains, its devices), or what is in a device: never a DEVICE's own children,
    // whose order the value holds none of, nor a plug-in's VALUE or what is in one.
    const auto found = findDevicesElement (element);
    return found.isValid() && (found != element || ! found.hasType (schema::DEVICE));
}

bool isHeardAsDevices (const juce::ValueTree& element, const juce::Identifier& property)
{
    const auto found = findDevicesElement (element);
    return found.isValid() && (found != element || ! isDeviceStoreProperty (element, property));
}

bool isHeardAsDevices (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    // A plug-in's VALUE is the store's; a DEVICE or a CONTAINER on a channel, and anything in one, the devices'. No child
    // (what a listener asks of children in another order): as the element's.
    if (! child.isValid())
        return isHeardAsDevices (parent);

    if (child.hasType (schema::VALUE) && parent.hasType (schema::DEVICE))
        return false;

    if ((child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER)) && isChannelElement (parent))
        return true;

    return findDevicesElement (parent).isValid();
}

//==============================================================================
DeviceElements::Found DeviceElements::find (DeviceID id) const
{
    const auto found = items.find (id.getRaw());
    return found != items.end() ? found->second : Found();
}

const std::vector<DeviceID>& DeviceElements::getOrder (const DevicePlace& owner) const
{
    static const std::vector<DeviceID> none;
    const auto found = lists.find (getListKey (owner));
    return found != lists.end() ? found->second : none;
}

DeviceElements indexDeviceElementsIn (const juce::ValueTree& song)
{
    // Each element's ID claimed as it is first met, whether or not the song reads it (one the song reads none of: found as
    // none, and another of its ID after it neither), as the comparison always found them.
    DeviceElements index;

    forEachItem (
        song,
        [&index] (const juce::ValueTree& element, const DevicePlace& place)
        {
            const auto id = readDeviceElementID (element);

            if (! isHeldID (id) || ! index.items.try_emplace (id.getRaw()).second)
                return false;

            if (! readHeldDeviceFields (element, place).has_value())
                return false;

            index.items[id.getRaw()] = { .element = element, .place = place };
            index.lists[getListKey (getDeviceListPlace (place, element.hasType (schema::CHAIN) ? Form::chain : Form::own))].push_back (id);
            return true;
        });

    return index;
}

std::uint64_t countDeviceItemsWalked() noexcept
{
    return itemsWalked.load (std::memory_order_relaxed);
}
} // namespace phazite
