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
        return parameter.choices.size() == (size_t) std::lround (parameter.maximum - parameter.minimum) + 1;

    return parameter.choices.empty();
}

juce::NormalisableRange<float> getRange (const ParameterInfo& parameter)
{
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
