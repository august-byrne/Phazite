// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackColours.h"

namespace phazite
{
const std::array<TrackColour, 8>& getTrackColours()
{
    static const std::array<TrackColour, 8> colours { { { juce::Colour (0xffe69f00), "Orange" },
                                                        { juce::Colour (0xff56b4e9), "Sky blue" },
                                                        { juce::Colour (0xff009e73), "Bluish green" },
                                                        { juce::Colour (0xfff0e442), "Yellow" },
                                                        { juce::Colour (0xff0072b2), "Blue" },
                                                        { juce::Colour (0xffd55e00), "Vermilion" },
                                                        { juce::Colour (0xffcc79a7), "Reddish purple" },
                                                        { juce::Colour (0xff999999), "Gray" } } };
    return colours;
}

std::optional<juce::Colour> findTrackColour (const juce::String& name)
{
    for (const auto& colour : getTrackColours())
        if (name.equalsIgnoreCase (colour.name))
            return colour.colour;

    return std::nullopt;
}
} // namespace phazite
