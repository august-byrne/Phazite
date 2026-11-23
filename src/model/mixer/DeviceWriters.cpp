// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' setters (internal/Mixer.h: setDevice() and those beside it), while the song's tree is the devices' storage:
// the one place, with the versions' passes until they move and the store's code (model/plugins/internal/DeviceStore.h), that
// writes a DEVICE, CONTAINER or CHAIN element of a live song. Each finds the elements it writes (by an ID, a walk of the song
// as findDeviceState() walks it; by a place, its channel's element and the container and chain in it, no walk) and writes
// them by what differs through the conversion (model/base/internal/DevicesValue.h), in the mode of the moment.
#include "internal/Containers.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/plugins/internal/DeviceStore.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Form = DeviceValue::Form;

    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getTreeUndo (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // A device's or a container's element by its ID (a walk), a chain's (a walk), or either (a walk, then another for a chain).
    juce::ValueTree findItemElement (const Song& song, DeviceID id, std::optional<Form> form = {})
    {
        if (form != Form::chain)
            if (auto device = findDeviceState (song, id); device.isValid() || form.has_value())
                return device;

        return findContainerChain (song, id);
    }

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

    // A child of an element with an ID, of the types a list holds, invalid for none: what a place's owner is looked for by.
    juce::ValueTree findChildWithID (const juce::ValueTree& parent, DeviceID id, const juce::Identifier& type)
    {
        for (const auto& child : parent)
            if (child.hasType (type) && readDeviceElementID (child) == id)
                return child;

        return {};
    }

    // The element that holds the list of what is at a place (a chain's CHAIN, a container's CONTAINER, else the channel's
    // TRACK or MASTER), found from its channel's element down, no walk of the song; invalid for one the song hasn't.
    juce::ValueTree findOwnerElement (const Song& song, const DevicePlace& place)
    {
        auto owner = place.onMaster ? song.getState().getChildWithName (schema::MASTER)
                                    : (place.track.isValid() ? findTrackState (song, place.track) : juce::ValueTree());

        if (owner.isValid() && place.container.isValid())
        {
            owner = findChildWithID (owner, place.container, schema::CONTAINER);

            if (owner.isValid() && place.chain.isValid())
                owner = findChildWithID (owner, place.chain, schema::CHAIN);
        }

        return owner;
    }

    // Whether a child of an owner's element is in the owner's list (DevicesValue.h: readDeviceList() lists the same).
    bool isListed (const juce::ValueTree& owner, const juce::ValueTree& child)
    {
        if (owner.hasType (schema::TRACK) || owner.hasType (schema::MASTER))
            return child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER);

        if (owner.hasType (schema::CONTAINER))
            return child.hasType (schema::CHAIN);

        return owner.hasType (schema::CHAIN) && child.hasType (schema::DEVICE);
    }

    // The child index at which what goes to a place in an owner's list (from 0; -1, or one past the list, after the others)
    // is put: before the one there, else after the list's last, else, for an empty list, after the owner's other children
    // (as the writers have always put a channel's first device: after its lanes, a folder's after its tracks).
    int findChildIndex (const juce::ValueTree& owner, int listIndex)
    {
        auto count = 0, last = -1;

        for (int i = 0; i < owner.getNumChildren(); ++i)
        {
            if (! isListed (owner, owner.getChild (i)))
                continue;

            if (count++ == listIndex)
                return i;

            last = i;
        }

        return last >= 0 ? last + 1 : owner.getNumChildren();
    }

    // An owner's list's members: each one's child index and ID, in order.
    std::vector<std::pair<int, DeviceID>> readListed (const juce::ValueTree& owner)
    {
        std::vector<std::pair<int, DeviceID>> listed;

        for (int i = 0; i < owner.getNumChildren(); ++i)
            if (const auto child = owner.getChild (i); isListed (owner, child))
                listed.emplace_back (i, readDeviceElementID (child));

        return listed;
    }

    // Whether two places name one list (DevicesArea.h: getDeviceListPlace()).
    bool isSameList (const DevicePlace& a, const DevicePlace& b, Form form)
    {
        return getDeviceListPlace (a, form) == getDeviceListPlace (b, form);
    }

    // An owner's list put in the order wanted (the same IDs), each moved as the same element: one moved alone (a drag's
    // step) in one move, to the place of the one there, as the writers have always moved one; else each in its turn, from
    // the first that differs.
    bool putInOrder (juce::ValueTree& owner, const std::vector<DeviceID>& wanted, juce::UndoManager* undo)
    {
        auto moved = false;

        for (;;)
        {
            const auto listed = readListed (owner);

            if (listed.size() != wanted.size())
                return moved;

            size_t k = 0;

            while (k < listed.size() && listed[k].second == wanted[k])
                ++k;

            if (k == listed.size())
                return moved;

            // The one at k wanted later, alone: moved there.
            const auto later = (size_t) std::distance (wanted.begin(), std::find (wanted.begin(), wanted.end(), listed[k].second));
            std::vector<DeviceID> held;

            for (const auto& [index, id] : listed)
                held.push_back (id);

            auto tried = held;
            tried.erase (tried.begin() + (std::ptrdiff_t) k);
            tried.insert (tried.begin() + (std::ptrdiff_t) std::min (later, tried.size()), listed[k].second);

            if (later > k && later < listed.size() && tried == wanted)
            {
                owner.moveChild (listed[k].first, listed[later].first, undo);
                return true;
            }

            // Else the one wanted at k, from where it is, put there.
            const auto from = (size_t) std::distance (held.begin(), std::find (held.begin(), held.end(), wanted[k]));

            if (from >= listed.size())
                return moved;

            owner.moveChild (listed[from].first, listed[k].first, undo);
            moved = true;
        }
    }

    // A made item's element, with what the store holds of it.
    juce::ValueTree writeMade (const DeviceValue& value, const DeviceStore& store)
    {
        auto element = writeDeviceElement (value);
        writeStoreIntoMade (element, store, value.id);
        return element;
    }
} // namespace

//==============================================================================
bool setNewDevices (Song& song, const MadeDevices& made, int index)
{
    if (made.devices.empty())
        return false;

    const auto& first = made.devices.front();
    auto owner = findOwnerElement (song, first.place);

    if (! owner.isValid())
        return false;

    // The first with the rest in it: a container's chains, each chain's devices, each in its owner's list in their order.
    auto element = writeMade (first, made.store);
    std::vector<std::pair<DeviceID, juce::ValueTree>> owners { { first.id, element } };

    for (size_t i = 1; i < made.devices.size(); ++i)
    {
        const auto& inside = made.devices[i];
        const auto ownerID = inside.form == Form::chain ? inside.place.container : inside.place.chain;
        const auto found = std::find_if (owners.begin(), owners.end(), [ownerID] (const auto& each) { return each.first == ownerID; });

        if (found == owners.end())
        {
            jassertfalse; // made in nothing it holds
            continue;
        }

        auto child = writeMade (inside, made.store);
        found->second.appendChild (child, nullptr);

        if (inside.form == Form::chain)
            owners.emplace_back (inside.id, child);
    }

    owner.addChild (element, findChildIndex (owner, index), getTreeUndo (song));
    return true;
}

bool setDevice (Song& song, DeviceValue value, int index)
{
    // Looked for among all the song's items, whatever their kind (a walk, and another for a chain), and refused where the
    // song holds its ID as another kind of item: a second item of the ID is none the song can hold.
    auto element = findItemElement (song, value.id);

    if (const auto form = readDeviceForm (element); form.has_value() && getKind (*form) != getKind (value.form))
        return false;

    const auto held = element.isValid() ? readHeldDevice (element, readDevicePlace (element)) : std::nullopt;
    auto* undo = getTreeUndo (song);

    // One the song hasn't, or holds as another form of a device (an element no reader reads as this one): made anew at its
    // place.
    if (! held.has_value() || held->form != value.form)
    {
        if (element.isValid() && ! findOwnerElement (song, value.place).isValid())
            return false;

        if (element.isValid())
            element.getParent().removeChild (element, undo);

        MadeDevices made;
        made.devices.push_back (std::move (value));
        return setNewDevices (song, made, index);
    }

    // One whose list is another: moved there as the same element, with all it holds.
    auto moved = false;

    if (! isSameList (held->place, value.place, value.form))
    {
        auto owner = findOwnerElement (song, value.place);

        if (! owner.isValid())
            return false;

        element.getParent().removeChild (element, undo);
        owner.addChild (element, findChildIndex (owner, index), undo);
        moved = true;
    }

    return writeDeviceLike (element, *held, value, undo) || moved;
}

bool eraseDevice (Song& song, DeviceID id)
{
    auto element = findItemElement (song, id);

    if (! element.isValid())
        return false;

    element.getParent().removeChild (element, getTreeUndo (song));
    return true;
}

bool eraseDevice (Song& song, DeviceID id, const DevicePlace& at)
{
    // Among its owner's children: a chain's devices, a container's chains, a channel's devices and containers.
    auto owner = findOwnerElement (song, at);

    for (const auto* type : { &schema::DEVICE, &schema::CONTAINER, &schema::CHAIN })
    {
        if (const auto element = findChildWithID (owner, id, *type); element.isValid() && isListed (owner, element))
        {
            owner.removeChild (element, getTreeUndo (song));
            return true;
        }
    }

    return false;
}

bool setDeviceListOrder (Song& song, const DevicePlace& owner, immer::flex_vector<DeviceID> order)
{
    auto element = findOwnerElement (song, owner);

    if (! element.isValid())
        return false;

    // The same IDs, each once, else nothing.
    std::vector<DeviceID> held, wanted (order.begin(), order.end());

    for (const auto& [index, id] : readListed (element))
        held.push_back (id);

    auto sortedHeld = held, sortedWanted = wanted;
    std::sort (sortedHeld.begin(), sortedHeld.end());
    std::sort (sortedWanted.begin(), sortedWanted.end());

    if (sortedHeld != sortedWanted || std::adjacent_find (sortedWanted.begin(), sortedWanted.end()) != sortedWanted.end())
        return false;

    return putInOrder (element, wanted, getTreeUndo (song));
}

bool moveInDeviceList (Song& song, DeviceID id, int newIndex, ListMove move)
{
    auto element = findItemElement (song, id, move == ListMove::chains ? std::optional (Form::chain) : std::optional (Form::own));
    auto owner = element.getParent();

    if (! element.isValid() || (move == ListMove::inChain && (! owner.hasType (schema::CHAIN) || readIsInstrument (element))))
        return false;

    // To the place of the one there now, never before a chain's instrument where the move keeps it first.
    const auto listed = readListed (owner);
    const auto first = move == ListMove::inChain && ! listed.empty() && readIsInstrument (owner.getChild (listed.front().first)) ? 1 : 0;

    if ((int) listed.size() <= first)
        return false;

    const auto at = (size_t) juce::jlimit (first, (int) listed.size() - 1, newIndex);
    const auto from = owner.indexOf (element);

    if (listed[at].first == from)
        return false;

    owner.moveChild (from, listed[at].first, getTreeUndo (song));
    return true;
}

bool moveDevices (Song& song, const std::vector<DeviceID>& ids, const DevicePlace& from, const DevicePlace& to, int index,
                  const std::function<void (DeviceValue&)>& edit)
{
    auto source = findOwnerElement (song, from);
    auto target = findOwnerElement (song, to);

    if (! source.isValid() || ! target.isValid())
        return false;

    auto* undo = getTreeUndo (song);
    auto moved = false;

    for (const auto id : ids)
    {
        auto element = findChildWithID (source, id, schema::DEVICE);

        if (! element.isValid())
            element = findChildWithID (source, id, schema::CONTAINER);

        if (! element.isValid())
            element = findChildWithID (source, id, schema::CHAIN);

        if (! element.isValid() || ! isListed (target, element))
            continue;

        source.removeChild (element, undo);
        target.addChild (element, findChildIndex (target, index), undo);
        moved = true;

        if (index >= 0)
            ++index;

        if (edit != nullptr)
            setDeviceFields (song, element, edit);
    }

    return moved;
}

bool setDeviceFields (Song& song, DeviceID id, const std::function<void (DeviceValue&)>& edit)
{
    return setDeviceFields (song, findDeviceState (song, id), edit);
}

bool setChainFields (Song& song, DeviceID chain, const std::function<void (DeviceValue&)>& edit)
{
    return setDeviceFields (song, findContainerChain (song, chain), edit);
}

bool setDeviceFields (Song& song, juce::ValueTree element, const std::function<void (DeviceValue&)>& edit)
{
    // Its own fields alone, read without its values and sounds (no map made), which its fields' writer doesn't write.
    auto fields = element.isValid() ? readHeldDeviceFields (element, {}) : std::nullopt;

    if (! fields.has_value())
        return false;

    edit (*fields);
    return writeDeviceFields (element, *fields, getTreeUndo (song));
}

bool setDeviceValue (Song& song, DeviceID id, int parameterIndex, float value)
{
    const auto device = findDeviceState (song, id);
    const auto* info = readDeviceForm (device) == Form::own
                           ? findOwnDevice (std::string_view (device[schema::deviceType].toString().toRawUTF8()))
                           : nullptr;

    if (info == nullptr || ! juce::isPositiveAndBelow (parameterIndex, (int) info->parameters.size()))
        return false;

    return setDeviceValue (song, device, info->parameters[(size_t) parameterIndex], value);
}

bool setDeviceValue (Song& song, juce::ValueTree device, const devices::ParameterInfo& info, float value)
{
    if (readDeviceForm (device) != Form::own)
        return false;

    return writeDeviceValueOf (device, info, keepHeldValue (info, value), getTreeUndo (song));
}

void setDeviceSound (Song& song, DeviceID id, const juce::Identifier& property, const juce::String& value)
{
    // One of its type's sounds (a property it hasn't would be one the song reads none of: DevicesValue.h).
    if (auto device = findDeviceState (song, id);
        readDeviceForm (device) == Form::own && findSoundIndex (std::string_view (readDeviceType (device).toRawUTF8()), property) >= 0)
        writeDeviceSoundOf (device, property, value, getTreeUndo (song));
}
} // namespace phazite
