// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceInfo.h"

#include <cmath>

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
    juce::NormalisableRange<float> range (parameter.minimum, parameter.maximum, takesWholeNumbers (parameter.unit) ? 1.0f : 0.0f);

    if (parameter.midpoint.has_value())
        range.setSkewForCentre (*parameter.midpoint);

    return range;
}

float constrain (const ParameterInfo& parameter, float value)
{
    if (std::isnan (value))
        return parameter.defaultValue;

    value = juce::jlimit (parameter.minimum, parameter.maximum, value);
    return takesWholeNumbers (parameter.unit) ? std::round (value) : value;
}

int findParameter (const DeviceInfo& device, std::string_view id)
{
    for (size_t i = 0; i < device.parameters.size(); ++i)
        if (device.parameters[i].id == id)
            return (int) i;

    return -1;
}

bool isValid (const DeviceInfo& device)
{
    if (device.typeID.empty() || device.name.empty())
        return false;

    // Each parameter is valid, and the first with its ID is itself.
    for (size_t i = 0; i < device.parameters.size(); ++i)
        if (! isValid (device.parameters[i]) || findParameter (device, device.parameters[i].id) != (int) i)
            return false;

    for (const auto id : device.keyControls)
        if (findParameter (device, id) < 0)
            return false;

    return true;
}
} // namespace phazite::devices
