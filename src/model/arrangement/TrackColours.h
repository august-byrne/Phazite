// SPDX-License-Identifier: AGPL-3.0-or-later
// The palette tracks get their colors from, which sections, song outlines and imported files use too.
#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <optional>

namespace phazite
{
// The colors tracks get, in turn: Okabe and Ito's palette (2008), which color-blind users can tell
// apart, with grey instead of black so it shows on dark backgrounds.
struct TrackColour
{
    juce::Colour colour;
    const char* name; // for menus and screen readers
};

const std::array<TrackColour, 8>& getTrackColours();

// The palette's color of this name ("Sky blue", "Vermilion"...), whatever its case; nothing if there is none.
std::optional<juce::Colour> findTrackColour (const juce::String& name);
} // namespace phazite
