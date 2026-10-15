// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ValueText.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace phazite::ui
{
namespace
{
    std::optional<double> parseNumber (const juce::String& text)
    {
        const auto t = text.trim();

        if (t.isEmpty() || ! t.containsOnly ("+-.0123456789") || ! t.containsAnyOf ("0123456789"))
            return {};

        return t.getDoubleValue();
    }

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // Three significant digits, as audio tools show values: "1.20", "12.5", "125".
    juce::String withThreeDigits (double value)
    {
        const auto size = std::abs (value);
        return juce::String (value, size < 10.0 ? 2 : (size < 100.0 ? 1 : 0));
    }

    // "+7", "-15", and "0" without a sign, with the decimals given.
    juce::String withSign (double value, int decimals)
    {
        const auto size = juce::String (std::abs (value), decimals);

        if (size.removeCharacters ("0.").isEmpty()) // it rounds to zero
            return size;

        return (value > 0.0 ? "+" : "-") + size;
    }

    // The text without the first of these endings that it has, and whether it had one.
    bool removeEnding (juce::String& text, std::initializer_list<const char*> endings)
    {
        for (const auto* ending : endings)
        {
            if (text.endsWith (ending))
            {
                text = text.dropLastCharacters ((int) std::strlen (ending)).trim();
                return true;
            }
        }

        return false;
    }
} // namespace

juce::String formatDecibels (double dB)
{
    if (dB <= silenceDb)
        return "-inf dB";

    const auto tenths = juce::roundToInt (dB * 10.0);

    if (tenths == 0)
        return "0.0 dB"; // not "-0.0 dB"

    return (tenths > 0 ? "+" : "") + juce::String (tenths / 10.0, 1) + " dB";
}

std::optional<double> parseDecibels (const juce::String& text)
{
    const auto number = text.trim().toLowerCase().upToFirstOccurrenceOf ("db", false, false).trim();

    if (number == "-inf" || number == "-infinity")
        return silenceDb;

    return parseNumber (number);
}

juce::String formatPan (double pan)
{
    const auto percent = juce::roundToInt (std::abs (pan) * 100.0);

    if (percent == 0)
        return "Center";

    return juce::String (percent) + (pan < 0.0 ? "% left" : "% right");
}

std::optional<double> parsePan (const juce::String& text)
{
    const auto t = text.trim().toLowerCase();

    if (t == "center" || t == "centre" || t == "c")
        return 0.0;

    const bool left = t.endsWith ("left") || t.endsWith ("l");
    const bool right = t.endsWith ("right") || t.endsWith ("r");

    if (auto percent = parseNumber (t.retainCharacters ("+-.0123456789"));
        percent.has_value() && (left || right || t.containsOnly ("+-.0123456789 %")))
    {
        const auto amount = juce::jlimit (0.0, 100.0, std::abs (*percent)) / 100.0;
        const bool isLeft = left || (! right && *percent < 0.0);
        return isLeft ? -amount : amount;
    }

    return {};
}
} // namespace phazite::ui

namespace phazite::ui
{
juce::String formatParameterValue (const devices::ParameterInfo& parameter, float value)
{
    using devices::Unit;

    switch (parameter.unit)
    {
        case Unit::hertz:
            return value >= 1000.0f ? withThreeDigits (value / 1000.0) + " kHz" : withThreeDigits (value) + " Hz";

        case Unit::decibels:
            return formatDecibels (value);

        case Unit::percent:
            return juce::String (juce::roundToInt (value * 100.0f)) + "%";

        case Unit::milliseconds:
            return value >= 1000.0f ? withThreeDigits (value / 1000.0) + " s" : withThreeDigits (value) + " ms";

        case Unit::seconds:
            return value < 1.0f ? withThreeDigits (value * 1000.0) + " ms" : withThreeDigits (value) + " s";

        case Unit::semitones:
            return withSign (value, std::abs (value - std::round (value)) < 0.05f ? 0 : 1) + " st";

        case Unit::cents:
            return withSign (std::round (value), 0) + " ct";

        case Unit::ratio:
            return juce::String (value, 1) + ":1";

        case Unit::choice:
        {
            const auto index = juce::roundToInt (value - parameter.minimum);

            if (juce::isPositiveAndBelow (index, (int) parameter.choices.size()))
                return toString (parameter.choices[(size_t) index]);

            return juce::String (index + 1);
        }

        case Unit::toggle:
            return value > 0.5f ? "On" : "Off";

        case Unit::none:
            break;
    }

    return juce::String (value, 2);
}

std::optional<float> parseParameterValue (const devices::ParameterInfo& parameter, const juce::String& text)
{
    using devices::Unit;
    auto t = text.trim().toLowerCase();
    std::optional<double> value;

    switch (parameter.unit)
    {
        case Unit::choice:
        {
            for (size_t i = 0; i < parameter.choices.size(); ++i)
                if (t == toString (parameter.choices[i]).toLowerCase())
                    return devices::constrain (parameter, parameter.minimum + (float) i);

            // A choice's number, counted from 1.
            if (const auto number = parseNumber (t))
                return devices::constrain (parameter, parameter.minimum + (float) (*number - 1.0));

            return {};
        }

        case Unit::toggle:
            if (t == "on" || t == "1" || t == "yes" || t == "true")
                return 1.0f;

            if (t == "off" || t == "0" || t == "no" || t == "false")
                return 0.0f;

            return {};

        case Unit::decibels:
            value = parseDecibels (t);
            break;

        case Unit::hertz:
        {
            const auto kilo = removeEnding (t, { "khz", "k" });

            if (! kilo)
                removeEnding (t, { "hz" });

            value = parseNumber (t);

            if (value.has_value() && kilo)
                *value *= 1000.0;

            break;
        }

        case Unit::milliseconds:
        case Unit::seconds:
        {
            // Milliseconds or seconds, whichever was typed; a plain number is in the parameter's unit.
            const auto inMilliseconds = removeEnding (t, { "ms" }) || (! removeEnding (t, { "s" }) && parameter.unit == Unit::milliseconds);
            value = parseNumber (t);

            if (value.has_value())
            {
                if (parameter.unit == Unit::seconds && inMilliseconds)
                    *value /= 1000.0;
                else if (parameter.unit == Unit::milliseconds && ! inMilliseconds)
                    *value *= 1000.0;
            }

            break;
        }

        case Unit::percent:
            removeEnding (t, { "%" });
            value = parseNumber (t);

            if (value.has_value())
                *value /= 100.0;

            break;

        case Unit::semitones:
            removeEnding (t, { "semitones", "st" });
            value = parseNumber (t);
            break;

        case Unit::cents:
            removeEnding (t, { "cents", "ct" });
            value = parseNumber (t);
            break;

        case Unit::ratio:
            removeEnding (t, { ":1" });
            value = parseNumber (t);
            break;

        case Unit::none:
            value = parseNumber (t);
            break;
    }

    if (! value.has_value())
        return {};

    return devices::constrain (parameter, (float) *value);
}
} // namespace phazite::ui
