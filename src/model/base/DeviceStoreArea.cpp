// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DeviceStoreArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = SongValues::DeviceStoreChange;
    using Kind = Change::Kind;

    bool isDeviceOrChain (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) || element.hasType (schema::CHAIN);
    }

    // What can hold a DEVICE or a CHAIN, itself or in what it holds: the track list, a track (a folder's tracks too), the
    // master, a container, a chain.
    bool mayHoldDevices (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACKS) || element.hasType (schema::TRACK) || element.hasType (schema::MASTER)
               || element.hasType (schema::CONTAINER) || element.hasType (schema::CHAIN);
    }

    // Each DEVICE and CHAIN an element is or holds, through what can hold one (never a clip's notes), in the tree's order.
    void forEachItemIn (const juce::ValueTree& element, const std::function<void (const juce::ValueTree&)>& each)
    {
        if (isDeviceOrChain (element))
            each (element);

        if (mayHoldDevices (element))
            for (const auto& child : element)
                forEachItemIn (child, each);
    }

    // Each of one map's entries that differ, told as a change of a kind; the song's entries all (no held one in them).
    template <typename Entry>
    void addChanges (std::vector<Change>& told, const std::vector<MapChange<std::uint64_t, Entry>>& changes, Kind kind)
    {
        for (const auto& each : changes)
        {
            Change change;
            change.kind = kind;
            change.item = DeviceID::fromRaw (each.key);
            change.appeared = ! each.before.has_value();
            change.went = ! each.after.has_value();
            told.push_back (std::move (change));
        }
    }
} // namespace

//==============================================================================
SongValues::DeviceStoreTeller::DeviceStoreTeller (SongValues& v, juce::ListenerList<SongValues::Listener>& l)
    : values (v),
      area (v.deviceStore),
      listeners (l)
{
    area.addListener (this);
}

SongValues::DeviceStoreTeller::~DeviceStoreTeller()
{
    area.removeListener (this);
}

void SongValues::DeviceStoreTeller::valueAreaChanged (const DeviceStore& before, const DeviceStore& after)
{
    if (listeners.isEmpty() || before.isSameAs (after))
        return;

    // The store after, in a copy of the value told, which stays while the listeners are told (cheap: it shares what it holds).
    const auto now = after;
    const auto changes = findChanges (before, now);
    std::vector<Change> told;

    for (const auto& each : changes.pluginValues)
    {
        const auto songsBefore = each.before.has_value() && each.before->isSongs;
        const auto songsAfter = each.after.has_value() && each.after->isSongs;

        Change change;
        change.kind = Kind::pluginValue;
        change.item = each.key.device;
        change.parameter = each.key.parameter;
        change.appeared = ! songsBefore && songsAfter;
        change.went = songsBefore && ! songsAfter;
        change.changesSong = songsBefore != songsAfter || (songsBefore && ! juce::exactlyEqual (each.before->value, each.after->value));

        if (each.after.has_value())
            change.valueToGive = each.after->value;

        told.push_back (std::move (change));
    }

    addChanges (told, changes.states, Kind::state);
    addChanges (told, changes.programs, Kind::program);
    addChanges (told, changes.formatIDs, Kind::formatID);
    addChanges (told, changes.versions, Kind::version);
    addChanges (told, changes.chainMutes, Kind::chainMute);
    addChanges (told, changes.chainSolos, Kind::chainSolo);

    if (told.empty())
        return;

    // Each item's place, as the devices' area holds it: none for one the song doesn't hold.
    const auto held = values.devices.get();

    for (auto& change : told)
    {
        if (const auto* item = held.find (change.item))
        {
            change.place = item->place;
            change.isInSong = true;
        }
    }

    std::sort (told.begin(), told.end(), [] (const Change& a, const Change& b)
               { return std::tie (a.item, a.kind, a.parameter) < std::tie (b.item, b.kind, b.parameter); });

    listeners.call ([&told, &now] (SongValues::Listener& listener) { listener.deviceStoreChanged (told, now); });
}

//==============================================================================
void readHeldDeviceStoreIn (const juce::ValueTree& song, SongState& into)
{
    // Every DEVICE and CHAIN of the song's channels, their containers' and chains', each read as the song holds it (the first
    // of an ID, as the loader leaves each once), into the store and the windows' places.
    auto store = into.deviceStore;
    auto outside = into.devicesOutside;

    for (const auto& child : song)
    {
        if (! (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER)))
            continue;

        forEachItemIn (child,
                       [&store, &outside] (const juce::ValueTree& item)
                       {
                           jassert (isStoreWrittenAsHeld (item)); // the loader keeps each as held
                           readHeldStoreOf (item, store);

                           if (const auto window = item.hasType (schema::DEVICE) ? readWindowPlaceOf (item) : std::nullopt)
                               outside = outside.withWindow (readDeviceElementID (item), *window);
                       });
    }

    into.deviceStore = std::move (store);
    into.devicesOutside = std::move (outside);
}

//==============================================================================
bool SongValues::setDeviceStore (Song& song, DeviceStore next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next.isSameAs (deviceStore.get()))
        return false;

    if (song.isWritingOutsideHistory())
        return storeOutsideHistory (song, std::move (next));

    // The same of the song's (a held value alone differs): no undo action, the held value kept outside the history.
    if (next == deviceStore.get())
    {
        deviceStore.setOutsideHistory (std::move (next));
        return false;
    }

    deviceStore.set (std::move (next));
    return true;
}

bool SongValues::storeOutsideHistory (Song& song, DeviceStore next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);
    juce::ignoreUnused (song);

    if (next.isSameAs (deviceStore.get()))
        return false;

    const auto changesSong = ! (next == deviceStore.get());
    deviceStore.setOutsideHistory (std::move (next));
    return changesSong;
}

void SongValues::setWindowPlace (DeviceID id, std::optional<WindowPlace> place)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (devicesOutside.get().findWindow (id) != place)
        devicesOutside.set (devicesOutside.get().withWindow (id, place));
}

//==============================================================================
bool leadsToDeviceStore (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (parent.hasType (schema::PHAZITE))
        return child.hasType (schema::MASTER);

    if (parent.hasType (schema::TRACK) || parent.hasType (schema::MASTER))
        return child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER);

    if (parent.hasType (schema::CONTAINER))
        return child.hasType (schema::CHAIN);

    return parent.hasType (schema::CHAIN) && child.hasType (schema::DEVICE);
}

void writeDeviceStoreIn (juce::ValueTree& element, const SongState& kept)
{
    if (! isDeviceOrChain (element))
        return;

    const auto id = readDeviceElementID (element);

    if (! id.isValid())
        return;

    // A plug-in's and a chain's entries (none of Phazite's own devices'), and a device's window's place.
    if (element.hasType (schema::CHAIN) || readDeviceForm (element) == DeviceValue::Form::plugin)
        writeStoreInto (element, kept.deviceStore, id);

    if (element.hasType (schema::DEVICE))
        if (const auto window = kept.devicesOutside.findWindow (id))
            writeWindowPlaceInto (element, *window);
}

bool holdSameDeviceStore (const SongState& a, const SongState& b, DeviceID id)
{
    return holdSameEntriesOf (a.deviceStore, b.deviceStore, id);
}
} // namespace phazite
