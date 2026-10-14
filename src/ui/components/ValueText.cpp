// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ValueText.h"

namespace phazite
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
} // namespace phazite
