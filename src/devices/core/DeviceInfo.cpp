// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceInfo.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace phazite::devices
{
namespace
{
    bool takesWholeNumbers (Unit unit)
    {
        return unit == Unit::choice || unit == Unit::toggle;
    }

    bool isWhole (float value)
    {
        return std::abs (value - std::round (value)) < 1.0e-6f;
    }

    bool isNear (float a, float b)
    {
        return std::abs (a - b) < 1.0e-6f;
    }
} // namespace

bool isValid (const ParameterInfo& parameter)
{
    if (parameter.id.empty() || parameter.name.empty() || ! (parameter.minimum < parameter.maximum))
        return false;

    if (parameter.defaultValue < parameter.minimum || parameter.defaultValue > parameter.maximum)
        return false;

    if (parameter.midpoint.has_value() && ! (*parameter.midpoint > parameter.minimum && *parameter.midpoint < parameter.maximum))
        return false;

    if (parameter.step.has_value() && ! (*parameter.step > 0.0f))
        return false;

    // A frequency's knob goes through as many octaves in each part of its travel.
    if ((parameter.unit == Unit::hertz && parameter.minimum > 0.0f) != (parameter.scale == Scale::logarithmic))
        return false;

    if (parameter.scale == Scale::logarithmic && (parameter.midpoint.has_value() || parameter.step.has_value()))
        return false;

    if (takesWholeNumbers (parameter.unit))
    {
        // Whole steps, which a skew would only bend.
        if (parameter.midpoint.has_value() || ! isWhole (parameter.minimum) || ! isWhole (parameter.maximum)
            || ! isWhole (parameter.defaultValue))
            return false;
    }

    if (parameter.unit == Unit::toggle)
        return isNear (parameter.minimum, 0.0f) && isNear (parameter.maximum, 1.0f) && parameter.choices.empty();

    if (parameter.unit == Unit::choice)
    {
        if (parameter.choices.size() != (size_t) std::lround (parameter.maximum - parameter.minimum) + 1
            || (! parameter.choiceIDs.empty() && parameter.choiceIDs.size() != parameter.choices.size()))
            return false;

        // Each saved by a name of its own.
        for (size_t i = 0; i < parameter.choices.size(); ++i)
            if (const auto name = getChoiceName (parameter, (int) i); name.empty() || findChoice (parameter, name) != (int) i)
                return false;

        return true;
    }

    return parameter.choices.empty() && parameter.choiceIDs.empty();
}

std::string_view getChoiceName (const ParameterInfo& parameter, int index)
{
    if (! juce::isPositiveAndBelow (index, (int) parameter.choices.size()))
        return {};

    return parameter.choiceIDs.empty() ? parameter.choices[(size_t) index] : parameter.choiceIDs[(size_t) index];
}

std::optional<int> findChoice (const ParameterInfo& parameter, std::string_view name)
{
    for (int i = 0; i < (int) parameter.choices.size(); ++i)
        if (getChoiceName (parameter, i) == name)
            return i;

    return std::nullopt;
}

float Travel::toPosition (float value) const noexcept
{
    if (! (maximum > minimum) || std::isnan (value))
        return 0.0f;

    value = std::clamp (value, minimum, maximum);

    if (logarithmic)
        return std::clamp (std::log (value / minimum) / std::log (maximum / minimum), 0.0f, 1.0f);

    // As juce::NormalisableRange::convertTo0to1() works it out, so that the two agree.
    const auto proportion = std::clamp ((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    return juce::exactlyEqual (skew, 1.0f) ? proportion : std::pow (proportion, skew);
}

float Travel::fromPosition (float position) const noexcept
{
    position = std::isnan (position) ? 0.0f : std::clamp (position, 0.0f, 1.0f);

    if (logarithmic)
        return std::clamp (minimum * std::exp (position * std::log (maximum / minimum)), minimum, maximum);

    if (! juce::exactlyEqual (skew, 1.0f) && position > 0.0f)
        position = std::exp (std::log (position) / skew);

    return minimum + (maximum - minimum) * position;
}

float Travel::snap (float value) const noexcept
{
    if (step > 0.0f)
        value = minimum + step * std::round ((value - minimum) / step);

    return std::clamp (value, minimum, maximum);
}

Travel getTravel (const ParameterInfo& parameter)
{
    Travel travel { parameter.minimum, parameter.maximum };
    travel.step = takesWholeNumbers (parameter.unit) ? 1.0f : parameter.step.value_or (0.0f);
    travel.logarithmic = parameter.scale == Scale::logarithmic && parameter.minimum > 0.0f;

    // As juce::NormalisableRange::setSkewForCentre() works it out.
    if (parameter.midpoint.has_value() && ! travel.logarithmic)
        travel.skew = std::log (0.5f) / std::log ((*parameter.midpoint - parameter.minimum) / (parameter.maximum - parameter.minimum));

    return travel;
}

juce::NormalisableRange<float> getRange (const ParameterInfo& parameter)
{
    // A logarithmic one through the travel's own mapping, which JUCE's range takes as functions (copying it may allocate:
    // for the message thread; the audio thread uses the Travel).
    if (const auto travel = getTravel (parameter); travel.logarithmic)
        return { parameter.minimum, parameter.maximum, [travel] (float, float, float position) { return travel.fromPosition (position); },
                 [travel] (float, float, float value) { return travel.toPosition (value); } };

    juce::NormalisableRange<float> range (parameter.minimum, parameter.maximum,
                                          takesWholeNumbers (parameter.unit) ? 1.0f : parameter.step.value_or (0.0f));

    if (parameter.midpoint.has_value())
        range.setSkewForCentre (*parameter.midpoint);

    return range;
}

float constrain (const ParameterInfo& parameter, float value)
{
    if (std::isnan (value))
        return parameter.defaultValue;

    value = juce::jlimit (parameter.minimum, parameter.maximum, value);

    if (takesWholeNumbers (parameter.unit))
        return std::round (value);

    if (parameter.step.has_value())
        return juce::jmin (parameter.maximum,
                           parameter.minimum + *parameter.step * std::round ((value - parameter.minimum) / *parameter.step));

    return value;
}

int findParameter (const DeviceInfo& device, std::string_view id)
{
    if (device.findByID != nullptr)
        return device.findByID (id);

    for (size_t i = 0; i < device.parameters.size(); ++i)
        if (device.parameters[i].id == id)
            return (int) i;

    return -1;
}

bool isValid (const DeviceInfo& device)
{
    if (device.typeID.empty() || device.name.empty())
        return false;

    // Each parameter is valid, with an ID of its own, which a device that finds its parameters itself finds it by.
    std::vector<std::string_view> ids;
    ids.reserve (device.parameters.size());

    for (size_t i = 0; i < device.parameters.size(); ++i)
    {
        const auto& parameter = device.parameters[i];

        if (! isValid (parameter) || parameter.id.empty() || (device.findByID != nullptr && device.findByID (parameter.id) != (int) i))
            return false;

        ids.push_back (parameter.id);
    }

    std::sort (ids.begin(), ids.end());
    return std::adjacent_find (ids.begin(), ids.end()) == ids.end();
}
} // namespace phazite::devices
