// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' setters (internal/Mixer.h: setDevice() and those beside it): the one place, with the versions' pass, that
// sets the devices' area of the song's values (model/base/internal/SongValues.h: devices). Each finds what it
// changes by a look in the area (getDevicesValue(), findDevice(): no walk of the song), computes the area's next value and
// sets it once, in the mode of the moment.
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/internal/Devices.h"
#include "model/modulation/internal/Modulation.h"
#include "model/plugins/internal/DeviceStore.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Form = DeviceValue::Form;

    // The kind of item a form is: a device (one of Phazite's own, or a plug-in), a container or a chain. No two items of the
    // song have one ID, whatever their kinds.
    enum class Kind
    {
        device,
        container,
        chain
    };

    Kind getKind (Form form)
    {
        switch (form)
        {
            case Form::own:
            case Form::plugin:
                return Kind::device;
            case Form::container:
                return Kind::container;
            case Form::chain:
                return Kind::chain;
        }

        return Kind::device;
    }

    // Whether the song holds the owner of the list of what is at a place: its channel (a track of the song's, or the master),
    // and its container and chain where it names them, each where its place says.
    bool holdsOwner (const Song& song, const DevicePlace& place)
    {
        if (! place.onMaster && ! (place.track.isValid() && hasTrack (song, place.track)))
            return false;

        const auto& devices = getDevicesValue (song);
        const auto onChannel = makeChannelPlace (place.track, place.onMaster);

        if (place.container.isValid())
        {
            const auto* container = devices.find (place.container);

            if (container == nullptr || container->form != Form::container || container->place != onChannel)
                return false;
        }

        if (place.chain.isValid())
        {
            const auto* chain = devices.find (place.chain);

            if (chain == nullptr || chain->form != Form::chain || chain->place != makeContainerPlace (onChannel, place.container))
                return false;
        }

        return true;
    }

    // Whether what is of a form can be in the list of what is at a place: a device or a container on a channel, a chain in a
    // container, a device in a chain.
    bool canBeListedAt (Form form, const DevicePlace& place)
    {
        if (place.chain.isValid())
            return getKind (form) == Kind::device;

        if (place.container.isValid())
            return form == Form::chain;

        return form != Form::chain;
    }

    // A container's macros go with it to the channel it is put on, in the same step and the mode of the moment, as its
    // element carried them while the tree held the devices: their places name its channel (ModulationValue.h:
    // ModulatorPlace), set through modulation's setter of the whole value (model/modulation/internal/Modulation.h:
    // setAllModulation()). Nothing for anything else, nor for a container that stays on its channel.
    void moveMacrosWith (Song& song, const SongValues& values, const DeviceValue& was, const DevicePlace& to)
    {
        if (was.form != Form::container || (was.place.track == to.track && was.place.onMaster == to.onMaster))
            return;

        auto modulation = values.modulation.get();
        const auto* list = modulation.containers.find (was.id.getRaw());

        if (list == nullptr)
            return;

        const auto macros = *list;

        for (const auto id : macros)
        {
            if (const auto* found = modulation.find (id))
            {
                auto macro = *found;
                macro.place.track = to.track;
                macro.place.onMaster = to.onMaster;
                modulation = withModulator (modulation, std::move (macro));
            }
        }

        setAllModulation (song, std::move (modulation));
    }
} // namespace

//==============================================================================
bool setNewDevices (Song& song, const MadeDevices& made, int index)
{
    auto* values = song.getValues();

    if (made.devices.empty() || ! holdsOwner (song, made.devices.front().place))
        return false;

    // What the store holds of each made, set first (model/plugins/internal/DeviceStore.h: setMadeStore(): outside the history
    // for a new ID, in the step for one the store knows), so that a plug-in comes in with its state.
    std::vector<DeviceID> ids;

    for (const auto& each : made.devices)
        ids.push_back (each.id);

    setMadeStore (song, ids, made.store);

    // The first at its index, the rest in it after it, each in its list in their order: one set of the area.
    auto next = withDevice (values->devices.get(), made.devices.front(), index);

    for (size_t i = 1; i < made.devices.size(); ++i)
        next = withDevice (next, made.devices[i]);

    return values->setAllDevices (song, std::move (next));
}

bool setDevice (Song& song, DeviceValue value, int index)
{
    auto* values = song.getValues();

    if (! value.id.isValid() || ! holdsOwner (song, value.place) || ! canBeListedAt (value.form, value.place))
        return false;

    // Refused where the song holds its ID as another kind of item: a second item of the ID is none the song can hold.
    const auto& held = values->devices.get();
    const auto* was = held.find (value.id);

    if (was != nullptr && getKind (was->form) != getKind (value.form))
        return false;

    // One held as another form of a device: made anew at its place.
    if (was != nullptr && was->form != value.form)
    {
        const auto without = withoutDevice (held, value.id);
        return values->setAllDevices (song, withDevice (without, std::move (value), index));
    }

    // A container put on another channel takes its macros along (a version's line sends it there).
    const auto before = was != nullptr ? std::optional (*was) : std::nullopt;
    const auto to = value.place;

    if (! values->setDevice (song, std::move (value), index))
        return false;

    if (before.has_value())
        moveMacrosWith (song, *values, *before, to);

    return true;
}

bool eraseDevice (Song& song, DeviceID id)
{
    auto* values = song.getValues();
    return values->eraseDevice (song, id);
}

bool eraseDevice (Song& song, DeviceID id, const DevicePlace& at)
{
    // Only where it is: in the list of what is at its place.
    auto* values = song.getValues();
    const auto* found = values->devices.get().find (id);

    if (found == nullptr || getDeviceListPlace (found->place, found->form) != getDeviceListPlace (at, found->form))
        return false;

    return values->eraseDevice (song, id);
}

bool setDeviceListOrder (Song& song, const DevicePlace& owner, immer::flex_vector<DeviceID> order)
{
    auto* values = song.getValues();
    return values->setDeviceListOrder (song, owner, std::move (order));
}

bool moveInDeviceList (Song& song, DeviceID id, int newIndex, ListMove move)
{
    auto* values = song.getValues();
    const auto* found = values->devices.get().find (id);

    if (found == nullptr || (move == ListMove::chains) != (found->form == Form::chain)
        || (move == ListMove::inChain && (! found->place.chain.isValid() || found->isInstrument())))
        return false;

    const auto owner = getDeviceListPlace (found->place, found->form);
    const auto& devices = values->devices.get();
    const auto* list = devices.findList (owner);

    if (list == nullptr)
        return false;

    // To the place of the one there now, never before a chain's instrument where the move keeps it first.
    const auto* firstItem = list->empty() ? nullptr : devices.find (list->front());
    const auto first = move == ListMove::inChain && firstItem != nullptr && firstItem->isInstrument() ? 1 : 0;

    if ((int) list->size() <= first)
        return false;

    const auto at = (size_t) juce::jlimit (first, (int) list->size() - 1, newIndex);
    const auto from = (size_t) std::distance (list->begin(), std::find (list->begin(), list->end(), id));

    if (from >= list->size() || from == at)
        return false;

    return values->setDeviceListOrder (song, owner, list->erase (from).insert (at, id));
}

bool moveDevices (Song& song, const std::vector<DeviceID>& ids, const DevicePlace& from, const DevicePlace& to, int index,
                  const std::function<void (DeviceValue&)>& edit)
{
    auto* values = song.getValues();

    if (! holdsOwner (song, from) || ! holdsOwner (song, to))
        return false;

    // Each from the list at `from` into the one at `to`, with what it holds, in their order: one set of the area.
    auto next = values->devices.get();
    auto moved = false;
    std::vector<DeviceValue> containers; // as they were, for their macros (moveMacrosWith())

    for (const auto id : ids)
    {
        const auto* found = next.find (id);

        if (found == nullptr || getDeviceListPlace (found->place, found->form) != getDeviceListPlace (from, found->form)
            || ! canBeListedAt (found->form, to))
            continue;

        if (found->form == Form::container)
            containers.push_back (*found);

        auto value = *found;
        value.place = to;

        if (edit != nullptr)
        {
            const auto keep = value;
            edit (value);
            value.id = keep.id;
            value.form = keep.form;
            value.place = keep.place;
            value.values = keep.values;
            value.sounds = keep.sounds;
        }

        next = withDevice (next, std::move (value), index);
        moved = true;

        if (index >= 0)
            ++index;
    }

    if (! moved || ! values->setAllDevices (song, std::move (next)))
        return false;

    for (const auto& container : containers)
        moveMacrosWith (song, *values, container, to);

    return true;
}

bool setDeviceFields (Song& song, DeviceID id, const std::function<void (DeviceValue&)>& edit)
{
    // A device's or a container's (not a chain's), its own fields alone: its place, values and sounds kept.
    auto* values = song.getValues();
    const auto* found = values->devices.get().find (id);

    if (found == nullptr || found->form == Form::chain)
        return false;

    auto next = *found;
    edit (next);
    next.id = found->id;
    next.form = found->form;
    next.place = found->place;
    next.values = found->values;
    next.sounds = found->sounds;
    return values->setDevice (song, std::move (next));
}

bool setChainFields (Song& song, DeviceID chain, const std::function<void (DeviceValue&)>& edit)
{
    auto* values = song.getValues();
    const auto* found = values->devices.get().find (chain);

    if (found == nullptr || found->form != Form::chain)
        return false;

    auto next = *found;
    edit (next);
    next.id = found->id;
    next.form = found->form;
    next.place = found->place;
    return values->setDevice (song, std::move (next));
}

bool setDeviceValue (Song& song, DeviceID id, int parameterIndex, float value)
{
    // One entry of one map: the device's values', by the parameter's index, held as the song holds it.
    auto* values = song.getValues();
    const auto* found = values->devices.get().find (id);
    const auto* info = found != nullptr && found->form == Form::own ? findOwnDevice (std::string_view (found->type.toRawUTF8())) : nullptr;

    if (info == nullptr || ! juce::isPositiveAndBelow (parameterIndex, (int) info->parameters.size()))
        return false;

    const auto held = keepHeldValue (info->parameters[(size_t) parameterIndex], value);
    const auto key = (std::uint32_t) parameterIndex;
    const auto* was = found->values.find (key);

    if (held.has_value() ? (was != nullptr && juce::exactlyEqual (*was, *held)) : was == nullptr)
        return false;

    auto next = *found;
    next.values = held.has_value() ? found->values.set (key, *held) : found->values.erase (key);
    return values->setDevice (song, std::move (next));
}

void setDeviceSound (Song& song, DeviceID id, const juce::Identifier& property, const juce::String& value)
{
    // One of its type's sounds (a property it hasn't would be one the song reads none of: DevicesValue.h), one entry of one
    // map; empty for none.
    auto* values = song.getValues();
    const auto* found = values->devices.get().find (id);
    const auto index =
        found != nullptr && found->form == Form::own ? findSoundIndex (std::string_view (found->type.toRawUTF8()), property) : -1;

    if (index < 0)
        return;

    const auto key = (std::uint16_t) index;
    const auto* was = found->sounds.find (key);

    if (value.isEmpty() ? was == nullptr : (was != nullptr && *was == value))
        return;

    auto next = *found;
    next.sounds = value.isNotEmpty() ? found->sounds.set (key, value) : found->sounds.erase (key);
    values->setDevice (song, std::move (next));
}
} // namespace phazite
