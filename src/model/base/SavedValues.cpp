// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/SavedValues.h"
#include "model/base/Numbers.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace phazite
{
bool isDefaultValue (const devices::ParameterInfo& info, float value)
{
    // To a millionth of its size: a value back from a control's range can be a little off.
    return std::abs (value - info.defaultValue) <= 1.0e-6f * std::max (1.0f, std::abs (info.defaultValue));
}

juce::var toSavedValue (const devices::ParameterInfo& info, float value)
{
    if (info.unit == devices::Unit::choice)
        if (const auto name = devices::getChoiceName (info, juce::roundToInt (value - info.minimum)); ! name.empty())
            return juce::String::fromUTF8 (name.data(), (int) name.size());

    return value;
}

float readSavedValue (const devices::ParameterInfo& info, const juce::var& saved)
{
    if (saved.isVoid())
        return info.defaultValue;

    if (info.unit == devices::Unit::choice && saved.isString())
        if (const auto choice = devices::findChoice (info, saved.toString().toStdString()))
            return info.minimum + (float) *choice;

    return (float) saved;
}

std::optional<float> readHeldSavedValue (const devices::ParameterInfo& info, const juce::var& saved)
{
    if (saved.isVoid())
        return std::nullopt;

    if (info.unit == devices::Unit::choice && saved.isString())
    {
        if (const auto name = saved.toString(); const auto choice = devices::findChoice (info, std::string_view (name.toRawUTF8())))
            return keepHeldValue (info, info.minimum + (float) *choice);

        return std::nullopt;
    }

    if (const auto number = readValueNumber (saved); number.has_value() && std::isfinite (*number))
        return keepHeldValue (info, toFloat (*number));

    return std::nullopt;
}

std::optional<float> keepHeldValue (const devices::ParameterInfo& info, float value)
{
    if (! std::isfinite (value))
        return std::nullopt;

    const auto held = devices::constrain (info, value);
    return isDefaultValue (info, held) ? std::nullopt : std::optional<float> (held);
}
} // namespace phazite
