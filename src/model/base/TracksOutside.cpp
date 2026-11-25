// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/TracksOutside.h"
#include "model/base/Numbers.h"
#include "model/base/internal/MapChanges.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    bool isTrackOrMaster (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACK) || element.hasType (schema::MASTER);
    }

    // Whether a view property is described on the element's type (Schema.h: a MASTER's height and collapsed alone).
    bool isDescribedOn (const juce::ValueTree& element, const schema::PropertyDescription& property)
    {
        return property.isOn (element.getType());
    }

    // The view's properties (not its lanes), in the writer's order.
    const std::vector<const schema::PropertyDescription*>& getViewProperties()
    {
        static const std::vector<const schema::PropertyDescription*> properties { &schema::trackHeight, &schema::trackCollapsed,
                                                                                  &schema::trackPinned, &schema::drumMode,
                                                                                  &schema::drumOrder,   &schema::drumHidden };
        return properties;
    }

    std::optional<bool> readSwitchOf (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (! isTrackOrMaster (element) || ! element.hasProperty (property))
            return std::nullopt;

        return (bool) element[property];
    }

    std::optional<juce::String> readTextOf (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (const auto* held = isTrackOrMaster (element) ? element.getPropertyPointer (property) : nullptr)
            return held->toString();

        return std::nullopt;
    }

    // A whole number as the song holds one (within its range, made whole), none for none and what is no finite number.
    std::optional<int> readHeldWhole (const juce::var& held, const schema::PropertyDescription& property)
    {
        const auto number = readValueNumber (held);

        if (! number.has_value() || ! std::isfinite (*number))
            return std::nullopt;

        return roundToWhole<int> (readDescribed (held, property));
    }

    // Each of a view's properties as Phazite's writers write it, given to `each` as its name and its value: the one list of
    // them, which the writer and the loader's rule (keepViewAsHeld()) both go by. Nothing allocated.
    template <typename Each>
    void forEachWrittenProperty (const TrackView& view, Each&& each)
    {
        if (view.height.has_value())
            each (schema::trackHeight.getName(), juce::var (*view.height));

        if (view.collapsed.has_value())
            each (schema::trackCollapsed.getName(), juce::var (*view.collapsed));

        if (view.pinned.has_value())
            each (schema::trackPinned.getName(), juce::var (*view.pinned));

        if (view.drumMode.has_value())
            each (schema::drumMode.getName(), juce::var (*view.drumMode));

        if (view.drumOrder.has_value())
            each (schema::drumOrder.getName(), juce::var (*view.drumOrder));

        if (view.drumHidden.has_value())
            each (schema::drumHidden.getName(), juce::var (*view.drumHidden));
    }

    // A lane's properties as Phazite's writers write them (model/automation/Automation.cpp: writeLane(), setLaneHeight(),
    // setLaneCollapsed()): its device as its ID's text, its parameter, its height where it has one, collapsed only while on.
    template <typename Each>
    void forEachWrittenLaneProperty (const LaneValue& lane, Each&& each)
    {
        each (schema::laneDevice.getName(), juce::var (juce::String ((juce::int64) lane.device.getRaw())));
        each (schema::laneParameter.getName(), juce::var (lane.parameter));

        if (lane.height.has_value())
            each (schema::laneHeight.getName(), juce::var (*lane.height));

        if (lane.collapsed)
            each (schema::laneCollapsed.getName(), juce::var (true));
    }

    bool isLaneProperty (const juce::Identifier& property)
    {
        return property == schema::laneDevice.getName() || property == schema::laneParameter.getName()
               || property == schema::laneHeight.getName() || property == schema::laneCollapsed.getName();
    }

    // A LANE's device as Phazite's writers write it, its ID's digits, looked at without a text made of the ID.
    bool isDeviceAsWritten (const juce::var& there, std::uint64_t id)
    {
        if (! there.isString())
            return false;

        const auto text = there.toString();
        std::uint64_t value = 0;
        int digits = 0;

        for (auto* c = text.toRawUTF8(); *c != 0; ++c, ++digits)
        {
            if (*c < '0' || *c > '9' || digits >= 19 || (digits == 0 && *c == '0'))
                return false;

            value = value * 10 + (std::uint64_t) (*c - '0');
        }

        return digits > 0 && value == id;
    }

    // Whether a LANE holds what the writer writes of the lane the song holds, and no other lane property. Nothing allocated.
    bool isLaneAsWritten (const juce::ValueTree& element, const LaneValue& held)
    {
        if (const auto* device = element.getPropertyPointer (schema::laneDevice);
            device == nullptr || ! isDeviceAsWritten (*device, held.device.getRaw()))
            return false;

        const auto* parameter = element.getPropertyPointer (schema::laneParameter);
        const auto* height = element.getPropertyPointer (schema::laneHeight);
        const auto* collapsed = element.getPropertyPointer (schema::laneCollapsed);

        return parameter != nullptr && isSameValue (*parameter, juce::var (held.parameter))
               && (held.height.has_value() ? height != nullptr && isSameValue (*height, juce::var (*held.height)) : height == nullptr)
               && (held.collapsed ? collapsed != nullptr && isSameValue (*collapsed, juce::var (true)) : collapsed == nullptr);
    }

    // Whether an element's view properties are those the writer writes of what it holds, and no other. Nothing allocated.
    bool holdsViewAsWritten (const juce::ValueTree& element)
    {
        for (const auto* property : getViewProperties())
        {
            if (! isDescribedOn (element, *property))
                continue;

            const auto* there = element.getPropertyPointer (*property);

            if (there == nullptr)
                continue;

            if (property == &schema::trackHeight)
            {
                const auto height = readHeldWhole (*there, schema::trackHeight);

                if (! height.has_value() || ! isSameValue (*there, juce::var (*height)))
                    return false;
            }
            else if (property == &schema::drumOrder || property == &schema::drumHidden)
            {
                if (! there->isString())
                    return false;
            }
            else if (! isSameValue (*there, juce::var ((bool) *there)))
            {
                return false;
            }
        }

        for (const auto& lane : element.getChildWithName (schema::LANES))
        {
            if (! lane.hasType (schema::LANE))
                continue;

            if (const auto held = readHeldLane (lane); held.has_value() && ! isLaneAsWritten (lane, *held))
                return false;
        }

        return true;
    }

    // One view property of an element read into a view, as readView() reads it (held: none where it isn't described there).
    void readViewProperty (const juce::ValueTree& element, const schema::PropertyDescription& property, bool held, TrackView& view)
    {
        if (held && ! isDescribedOn (element, property))
            return;

        if (&property == &schema::trackHeight)
            view.height = held ? readHeldWhole (element[schema::trackHeight], schema::trackHeight) : readSavedHeight (element);
        else if (&property == &schema::trackCollapsed)
            view.collapsed = readCollapsed (element);
        else if (&property == &schema::trackPinned)
            view.pinned = readPinned (element);
        else if (&property == &schema::drumMode)
            view.drumMode = readDrumMode (element);
        else if (&property == &schema::drumOrder)
            view.drumOrder = readTextOf (element, schema::drumOrder);
        else if (&property == &schema::drumHidden)
            view.drumHidden = readTextOf (element, schema::drumHidden);
    }

    TrackView readView (const juce::ValueTree& element, bool held)
    {
        TrackView view;

        if (! isTrackOrMaster (element))
            return view;

        for (const auto* property : getViewProperties())
            readViewProperty (element, *property, held, view);

        for (const auto& lane : element.getChildWithName (schema::LANES))
        {
            if (! lane.hasType (schema::LANE))
                continue;

            if (! held)
                view.lanes = view.lanes.push_back (readLaneValue (lane));
            else if (const auto kept = readHeldLane (lane))
                view.lanes = view.lanes.push_back (*kept);
        }

        return view;
    }
} // namespace

bool TracksOutside::isSameAs (const TracksOutside& other) const noexcept
{
    return tracks.identity() == other.tracks.identity() && master == other.master;
}

bool TracksOutside::operator== (const TracksOutside& other) const
{
    return isSameAs (other) || (tracks == other.tracks && master == other.master);
}

TracksOutside withTrackView (const TracksOutside& outside, std::uint64_t id, TrackView view)
{
    const auto* held = outside.tracks.find (id);

    if (view == TrackView())
    {
        if (held == nullptr)
            return outside;

        auto next = outside;
        next.tracks = outside.tracks.erase (id);
        return next;
    }

    if (held != nullptr && *held == view)
        return outside;

    auto next = outside;
    next.tracks = outside.tracks.set (id, std::move (view));
    return next;
}

TracksOutside withMasterView (const TracksOutside& outside, TrackView view)
{
    if (outside.master == view)
        return outside;

    auto next = outside;
    next.master = std::move (view);
    return next;
}

TrackView readViewOf (const juce::ValueTree& element)
{
    return readView (element, false);
}

TrackView readHeldViewOf (const juce::ValueTree& element)
{
    return readView (element, true);
}

LaneValue readLaneValue (const juce::ValueTree& lane)
{
    LaneValue value;

    if (! lane.hasType (schema::LANE))
        return value;

    value.device = DeviceID::fromRaw ((std::uint64_t) toWhole<juce::int64> (lane[schema::laneDevice]));
    value.parameter = lane[schema::laneParameter].toString();

    if (lane.hasProperty (schema::laneHeight))
        value.height = toWhole<int> (lane[schema::laneHeight]);

    value.collapsed = (bool) lane[schema::laneCollapsed];
    return value;
}

std::optional<LaneValue> readHeldLane (const juce::ValueTree& lane)
{
    if (! lane.hasType (schema::LANE))
        return std::nullopt;

    const auto device = readID (lane[schema::laneDevice]);

    if (device == 0 || device >= schema::maxID)
        return std::nullopt;

    // A lane names a parameter, always (Schema.h: laneParameter): one without names nothing.
    if (lane[schema::laneParameter].toString().isEmpty())
        return std::nullopt;

    LaneValue value;
    value.device = DeviceID::fromRaw (device);
    value.parameter = lane[schema::laneParameter].toString();
    value.height = readHeldWhole (lane[schema::laneHeight], schema::laneHeight);
    value.collapsed = (bool) lane[schema::laneCollapsed];
    return value;
}

void writeViewInto (juce::ValueTree& element, const TrackView& view)
{
    if (! isTrackOrMaster (element))
        return;

    forEachWrittenProperty (view, [&element] (const juce::Identifier& property, const juce::var& value)
                            { element.setProperty (property, value, nullptr); });

    if (view.lanes.empty())
        return;

    juce::ValueTree lanes (schema::LANES);

    for (const auto& lane : view.lanes)
    {
        juce::ValueTree written (schema::LANE);
        forEachWrittenLaneProperty (lane, [&written] (const juce::Identifier& property, const juce::var& value)
                                    { written.setProperty (property, value, nullptr); });
        lanes.appendChild (written, nullptr);
    }

    element.appendChild (lanes, nullptr);
}

bool keepViewAsHeld (juce::ValueTree element)
{
    if (! isTrackOrMaster (element) || holdsViewAsWritten (element))
        return false;

    // Its view properties made as the writer writes what the song holds of them, in place; each lane the same, in its place.
    const auto held = readHeldViewOf (element);

    for (const auto* property : getViewProperties())
        if (isDescribedOn (element, *property))
            element.removeProperty (*property, nullptr);

    forEachWrittenProperty (held, [&element] (const juce::Identifier& property, const juce::var& value)
                            { element.setProperty (property, value, nullptr); });

    for (auto lane : element.getChildWithName (schema::LANES))
    {
        if (! lane.hasType (schema::LANE))
            continue;

        const auto kept = readHeldLane (lane);

        if (! kept.has_value() || isLaneAsWritten (lane, *kept))
            continue;

        for (auto i = lane.getNumProperties(); --i >= 0;)
            if (const auto property = lane.getPropertyName (i); isLaneProperty (property))
                lane.removeProperty (property, nullptr);

        forEachWrittenLaneProperty (*kept, [&lane] (const juce::Identifier& property, const juce::var& value)
                                    { lane.setProperty (property, value, nullptr); });
    }

    return true;
}

bool isViewProperty (const juce::ValueTree& element, const juce::Identifier& property)
{
    if (! isTrackOrMaster (element))
        return false;

    return std::any_of (getViewProperties().begin(), getViewProperties().end(),
                        [&element, &property] (const auto* each) { return each->getName() == property && isDescribedOn (element, *each); });
}

void forEachIDNamed (const TracksOutside& outside, const std::function<void (std::uint64_t)>& each)
{
    const auto visit = [&each] (const TrackView& view)
    {
        for (const auto& lane : view.lanes)
            if (lane.device.isValid())
                each (lane.device.getRaw());
    };

    for (const auto& [id, view] : outside.tracks)
        visit (view);

    visit (outside.master);
}

std::optional<int> readSavedHeight (const juce::ValueTree& element)
{
    if (const auto* height = isTrackOrMaster (element) ? element.getPropertyPointer (schema::trackHeight) : nullptr)
        return toWhole<int> (*height);

    return std::nullopt;
}

std::optional<bool> readCollapsed (const juce::ValueTree& element)
{
    return readSwitchOf (element, schema::trackCollapsed);
}

std::optional<bool> readPinned (const juce::ValueTree& element)
{
    return readSwitchOf (element, schema::trackPinned);
}

std::optional<bool> readDrumMode (const juce::ValueTree& element)
{
    return readSwitchOf (element, schema::drumMode);
}
} // namespace phazite
