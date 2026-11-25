// SPDX-License-Identifier: AGPL-3.0-or-later
// A merge's clash on a device, a container or a chain chosen through the devices' values (the choosers declared
// in model/versions/internal/AreaClashes.h): what the device's own value covers through its setter
// (internal/Mixer.h: setDevice()), what the store holds of it through the store's functions
// (model/plugins/internal/DeviceStore.h: setStoreEntries()).
#include "internal/Mixer.h"
#include "model/base/Numbers.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/versions/internal/AreaClashes.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    bool isStepOf (const juce::String& step, const juce::Identifier& type)
    {
        return step.startsWith (type.toString() + ":");
    }

    std::uint64_t readStepID (const juce::String& step)
    {
        return readID (step.fromFirstOccurrenceOf (":", false, false));
    }

    // The item a clash's path goes through: its last step that is a DEVICE's, a CONTAINER's or a CHAIN's ("TRACKS:",
    // "TRACK:12", "CONTAINER:40", "CHAIN:41", "DEVICE:44", "VALUES:VALUES"), with the place the steps before it name (the last
    // track's, or the master's; the container; the chain), and what the path names in it (the step after it, empty for none).
    struct ClashItem
    {
        int step = -1;
        juce::Identifier type {};
        DeviceID id {};
        DevicePlace place {};
        juce::String inside {};
    };

    std::optional<ClashItem> findClashItem (const MergeClash& clash)
    {
        ClashItem found;

        for (int i = 0; i < clash.path.size(); ++i)
            for (const auto* type : { &schema::DEVICE, &schema::CONTAINER, &schema::CHAIN })
                if (isStepOf (clash.path[i], *type))
                    found.step = i;

        if (found.step < 0)
            return std::nullopt;

        const auto& step = clash.path[found.step];
        found.type = isStepOf (step, schema::DEVICE)      ? schema::DEVICE.getName()
                     : isStepOf (step, schema::CONTAINER) ? schema::CONTAINER.getName()
                                                          : schema::CHAIN.getName();
        found.id = DeviceID::fromRaw (readStepID (step));

        for (int i = 0; i < found.step; ++i)
        {
            const auto& before = clash.path[i];

            if (isStepOf (before, schema::TRACK))
                found.place.track = TrackID::fromRaw (readStepID (before));
            else if (isStepOf (before, schema::MASTER))
                found.place.onMaster = true;
            else if (isStepOf (before, schema::CONTAINER))
                found.place.container = DeviceID::fromRaw (readStepID (before));
            else if (isStepOf (before, schema::CHAIN))
                found.place.chain = DeviceID::fromRaw (readStepID (before));
        }

        if (found.place.onMaster)
            found.place.track = {};

        if (found.step + 1 < clash.path.size())
            found.inside = clash.path[found.step + 1];

        return found;
    }

    // Whether a clash is the store's: a plug-in's state, program, format ID or version, or one of its values set by hand (a
    // VALUE's value), or a chain's mute or solo.
    bool isStoreClash (const MergeClash& clash, const ClashItem& item)
    {
        if (isStepOf (item.inside, schema::VALUE))
            return true;

        if (item.inside.isNotEmpty() || ! clash.property.isValid())
            return false;

        if (item.type == schema::DEVICE.getName())
            return clash.property == schema::pluginState || clash.property == schema::pluginProgram
                   || clash.property == schema::pluginFormatID || clash.property == schema::pluginVersion;

        return item.type == schema::CHAIN.getName() && (clash.property == schema::chainMute || clash.property == schema::chainSolo);
    }

    // The song's item a clash names: the song holds it, at the place the path names, of the kind its step names.
    std::optional<DeviceValue> findItemNamed (const Song& song, const ClashItem& item)
    {
        const auto* held = findDevice (song, item.id);

        if (held == nullptr || ! (held->place == item.place))
            return std::nullopt;

        const auto isDevice = held->form == DeviceValue::Form::own || held->form == DeviceValue::Form::plugin;
        const auto kindNamed = item.type == schema::DEVICE.getName()      ? isDevice
                               : item.type == schema::CONTAINER.getName() ? held->form == DeviceValue::Form::container
                                                                          : held->form == DeviceValue::Form::chain;
        return kindNamed ? std::optional (*held) : std::nullopt;
    }

    const juce::var& getChosen (const MergeClash& clash, int value)
    {
        const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };
        return *values[value];
    }
} // namespace

AreaChoice chooseDeviceStoreClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A path through a device or a chain to what the store holds of it (DeviceStore.h): a plug-in's state, program, format
    // ID or version, a value set by hand of its (its VALUE's value), a chain's mute or solo. Any other path through a device,
    // a container or a chain is the devices' (chooseDeviceClash()).
    const auto item = findClashItem (clash);

    if (! item.has_value() || ! isStoreClash (clash, *item))
        return AreaChoice::notTheArea;

    // Refused (nothing written) for what a merge of two songs Phazite holds never makes, but a commit's song may come from anyone
    // (tests/FuzzTests.cpp chooses every clash of a damaged merge): a path past a VALUE, a VALUE's property that isn't its value
    // or a VALUE of what isn't a plug-in, an item the song hasn't at the place the path names, and a value the song would hold
    // otherwise than it was chosen (DeviceStoreValue.h: readHeldStoreOf(): a state that isn't base 64 of some bytes, a value set
    // by hand outside 0 to 1).
    const auto isValue = isStepOf (item->inside, schema::VALUE);

    if (item->step == 0 || ! juce::isPositiveAndBelow (value, 3) || clash.path.size() > item->step + 2
        || (isValue && clash.property != schema::valueSetByHand))
        return AreaChoice::refused;

    const auto held = findItemNamed (song, *item);

    if (! held.has_value() || ((isValue || item->type == schema::DEVICE.getName()) && ! held->isPlugin()))
        return AreaChoice::refused;

    const auto& chosen = getChosen (clash, value);
    const auto raw = item->id.getRaw();
    auto entries = getStoreOf (song, item->id);

    if (isValue)
    {
        PluginValueKey key;
        key.device = item->id;
        key.parameter = item->inside.fromFirstOccurrenceOf (":", false, false);

        // A value both lines set of a parameter the parting's song held no value of has no value before: what was before
        // takes it away, so that the plug-in plays what its state holds, rather than leave a VALUE without a value, which
        // read as 0 (known bug 239's shape).
        if (chosen.isVoid())
        {
            entries.pluginValues = entries.pluginValues.erase (key);
        }
        else
        {
            const auto number = readValueNumber (chosen);

            if (! number.has_value() || ! std::isfinite (*number) || *number < 0.0 || *number > 1.0)
                return AreaChoice::refused;

            entries.pluginValues = entries.pluginValues.set (key, makeSongsPluginValue ((float) *number));
        }
    }
    else if (clash.property == schema::pluginState)
    {
        if (chosen.isVoid())
        {
            entries.states = entries.states.erase (raw);
        }
        else
        {
            auto bytes = readBytes (chosen.toString());

            if (! bytes.has_value() || bytes->getSize() == 0)
                return AreaChoice::refused;

            entries.states =
                entries.states.set (raw, PluginBytes { .bytes = std::make_shared<const juce::MemoryBlock> (std::move (*bytes)) });
        }
    }
    else if (clash.property == schema::pluginProgram)
    {
        entries.programs = chosen.isVoid() ? entries.programs.erase (raw) : entries.programs.set (raw, toWhole<int> (chosen));
    }
    else if (clash.property == schema::pluginFormatID || clash.property == schema::pluginVersion)
    {
        auto& texts = clash.property == schema::pluginFormatID ? entries.formatIDs : entries.versions;
        const auto text = chosen.toString();
        texts = text.isEmpty() ? texts.erase (raw) : texts.set (raw, text);
    }
    else
    {
        auto& flags = clash.property == schema::chainMute ? entries.chainMutes : entries.chainSolos;
        flags = (bool) chosen ? flags.set (raw, true) : flags.erase (raw);
    }

    setStoreEntries (song, item->id, entries);
    return AreaChoice::chosen;
}

AreaChoice chooseDeviceClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A path through a device, a container or a chain ("TRACKS:", "TRACK:12", "CONTAINER:40", "CHAIN:41", "DEVICE:44"):
    // they merge as items by their IDs, each of their properties at a time, a device's values set by hand (its VALUES) by
    // their parameters and its sounds (its SOUNDS) by their names (SongMerge.cpp); so a clash is a property of one, of its
    // VALUES or of its SOUNDS, set in the song's value of it (DevicesValue.h: setDeviceProperty(), a void value taking it
    // away: the default) through the devices' setter. What the store holds of one is chosen before (chooseDeviceStoreClash()).
    const auto item = findClashItem (clash);

    if (! item.has_value())
        return AreaChoice::notTheArea;

    // Refused (nothing written) for what a merge of two songs Phazite holds never makes, but a commit's song may come from
    // anyone (tests/FuzzTests.cpp chooses every clash of a damaged merge): an item whole (devices never merge whole, so no
    // clash names one), a path past what is in one, what is in one but its VALUES and its SOUNDS, its ID, an item the song
    // hasn't at the place the path names or holds as another kind, a property its value doesn't cover (DevicesValue.h:
    // setDeviceProperty(): a window's place, one Phazite doesn't know), and a value the song would read otherwise than it was
    // chosen (readHeldDevice(): a number outside its range, a choice that is none of its names).
    if (item->step == 0 || ! clash.property.isValid() || clash.path.size() > item->step + 2 || ! juce::isPositiveAndBelow (value, 3)
        || clash.property == schema::deviceID || clash.property == schema::chainID)
        return AreaChoice::refused;

    const auto element = item->inside.isEmpty()                    ? item->type
                         : isStepOf (item->inside, schema::VALUES) ? schema::VALUES.getName()
                         : isStepOf (item->inside, schema::SOUNDS) ? schema::SOUNDS.getName()
                                                                   : juce::Identifier();
    const auto held = findItemNamed (song, *item);

    if (! element.isValid() || ! held.has_value())
        return AreaChoice::refused;

    auto next = *held;

    if (const auto read = setDeviceProperty (next, element, clash.property, getChosen (clash, value))
                              ? readHeldDevice (writeDeviceElement (next), next.place)
                              : std::nullopt;
        ! read.has_value() || ! (*read == next))
        return AreaChoice::refused;

    setDevice (song, next);
    return AreaChoice::chosen;
}
} // namespace phazite
