// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's design tokens: the named colours, text sizes and spacings that every component uses
// (docs/design.md, "Design language"). Change the look here rather than in components.
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace phazite::ui
{
enum class ThemeName
{
    dark,
    light,
    highContrast
};

struct Palette
{
    juce::Colour background;    // the window behind everything
    juce::Colour surface;       // panels
    juce::Colour surfaceRaised; // buttons, fields, the top bar
    juce::Colour border;        // lines between panels (decorative)
    juce::Colour text;
    juce::Colour textMuted; // secondary text, still readable
    juce::Colour accent;    // selection, focus and the main button
    juce::Colour textOnAccent;
    juce::Colour warning;
    juce::Colour danger;
    juce::Colour loop; // where playback loops, in the rulers
};

inline Palette getPalette (ThemeName theme)
{
    using juce::Colour;

    switch (theme)
    {
        case ThemeName::light:
            return { Colour (0xfff4f5f7), Colour (0xffffffff), Colour (0xffeceef1), Colour (0xffd3d7dd),
                     Colour (0xff1b1f24), Colour (0xff545d69), Colour (0xff1f6feb), Colour (0xffffffff),
                     Colour (0xff9a5b00), Colour (0xffc62828), Colour (0xff1a7f5a) };

        case ThemeName::highContrast:
            return { Colour (0xff000000), Colour (0xff000000), Colour (0xff1a1a1a), Colour (0xffffffff),
                     Colour (0xffffffff), Colour (0xffe6e6e6), Colour (0xffffd400), Colour (0xff000000),
                     Colour (0xffffb000), Colour (0xffff6b6b), Colour (0xff00ff9c) };

        case ThemeName::dark:
        default:
            return { Colour (0xff15171a), Colour (0xff1c1f23), Colour (0xff272b31), Colour (0xff343a42),
                     Colour (0xffe8eaed), Colour (0xffa5adb8), Colour (0xff5b9dff), Colour (0xff0b1220),
                     Colour (0xfff2b84b), Colour (0xffff7a6b), Colour (0xff3fbf8f) };
    }
}

// Text sizes at 100 % scale (JUCE font heights, in pixels).
namespace TextSize
{
    inline constexpr float small = 12.0f;
    inline constexpr float body = 14.0f;
    inline constexpr float title = 16.0f;
    inline constexpr float heading = 20.0f;
} // namespace TextSize

// Spacing, on a 4-pixel grid.
namespace Space
{
    inline constexpr int xs = 4;
    inline constexpr int s = 8;
    inline constexpr int m = 12;
    inline constexpr int l = 16;
    inline constexpr int xl = 24;
} // namespace Space

inline constexpr int controlHeight = 28;
inline constexpr float cornerRadius = 4.0f;

// How far a button that needs attention, such as one asking for the instrument a track's notes need, is mixed with the
// warning color.
inline constexpr float attentionTint = 0.35f;

// Track colors are saved in projects, so their palette is in the model: phazite::getTrackColours().

// How strongly a track's color tints what belongs to the track, so that its color runs through its row: the
// alpha of the color over the playlist's background, or how far a header's surface is mixed with it.
namespace TrackTint
{
    inline constexpr float row = 0.12f;
    inline constexpr float folderRow = 0.18f;
    inline constexpr float selectedRow = 0.24f;
    inline constexpr float header = 0.3f;
    inline constexpr float selectedHeader = 0.5f;
} // namespace TrackTint

// How the song's sections show behind the playlist's tracks, as ghosts: a faint wash of each section's color
// (stronger while it is selected) and a line in it where the section starts (alphas).
namespace SectionTint
{
    inline constexpr float wash = 0.04f;
    inline constexpr float selectedWash = 0.1f;
    inline constexpr float edge = 0.6f;
} // namespace SectionTint

// How the loop shows in a ruler: a wash of the loop color over the part that loops, and a bar along its top
// (alphas, and the bar's height in pixels).
namespace LoopTint
{
    inline constexpr float wash = 0.14f;
    inline constexpr float bar = 0.9f;
    inline constexpr int barHeight = 3;
} // namespace LoopTint

// WCAG 2.2's contrast ratio between two colours, from 1 (none) to 21 (black on white). Text needs at
// least 4.5, and the parts of a control that identify it at least 3.
inline double getContrastRatio (juce::Colour a, juce::Colour b)
{
    const auto luminance = [] (juce::Colour c)
    {
        const auto channel = [] (juce::uint8 value)
        {
            const auto v = value / 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4);
        };

        return 0.2126 * channel (c.getRed()) + 0.7152 * channel (c.getGreen()) + 0.0722 * channel (c.getBlue());
    };

    const auto la = luminance (a), lb = luminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

// Black or white, whichever reads better on a color: for text on track, clip and section colors.
inline juce::Colour getInkFor (juce::Colour background)
{
    return getContrastRatio (juce::Colours::black, background) >= getContrastRatio (juce::Colours::white, background)
               ? juce::Colours::black
               : juce::Colours::white;
}
} // namespace phazite::ui
