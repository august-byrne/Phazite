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
    juce::Colour loop;       // where playback loops, in the rulers
    juce::Colour modulation; // what modulation does to a control: the ring around a knob, a modulator's output
};

inline Palette getPalette (ThemeName theme)
{
    using juce::Colour;

    switch (theme)
    {
        case ThemeName::light:
            return { Colour (0xfff4f5f7), Colour (0xffffffff), Colour (0xffeceef1), Colour (0xffd3d7dd),
                     Colour (0xff1b1f24), Colour (0xff545d69), Colour (0xff1f6feb), Colour (0xffffffff),
                     Colour (0xff9a5b00), Colour (0xffc62828), Colour (0xff1a7f5a), Colour (0xff7b3fd4) };

        case ThemeName::highContrast:
            return { Colour (0xff000000), Colour (0xff000000), Colour (0xff1a1a1a), Colour (0xffffffff),
                     Colour (0xffffffff), Colour (0xffe6e6e6), Colour (0xffffd400), Colour (0xff000000),
                     Colour (0xffffb000), Colour (0xffff6b6b), Colour (0xff00ff9c), Colour (0xffff5cff) };

        case ThemeName::dark:
        default:
            return { Colour (0xff15171a), Colour (0xff1c1f23), Colour (0xff272b31), Colour (0xff343a42),
                     Colour (0xffe8eaed), Colour (0xffa5adb8), Colour (0xff5b9dff), Colour (0xff0b1220),
                     Colour (0xfff2b84b), Colour (0xffff7a6b), Colour (0xff3fbf8f), Colour (0xffb48cff) };
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

// The knobs of devices' editors, square.
inline constexpr int knobSize = 64;

// Colours that tell apart the parts of a device's editor, such as the EQ's bands. Each part also shows its number,
// with an outline in the theme's text colour, so that colour is never the only sign.
inline constexpr juce::uint32 partColours[] = { 0xffe5534b, 0xfff0883e, 0xffd4a72c, 0xff57ab5a,
                                                0xff39c5cf, 0xff539bf5, 0xff986ee2, 0xffe275ad };

// A device editor's graph, such as the EQ's: the analyzer's fill and line, the grid, the line at 0 dB and a selected
// part's own curve (alphas), then the curve's width, the dots' radius and the graph's height.
namespace GraphTint
{
    inline constexpr float spectrumFill = 0.14f;
    inline constexpr float spectrumLine = 0.35f;
    inline constexpr float grid = 0.1f;
    inline constexpr float zeroLine = 0.25f;
    inline constexpr float partCurve = 0.25f;
    inline constexpr float partOff = 0.5f; // a part that is off, such as an EQ band's button
    inline constexpr float marker = 0.5f;  // a line that marks a setting, such as a compressor's threshold
    inline constexpr float curveWidth = 2.0f;
    inline constexpr float dotRadius = 8.0f;
    inline constexpr int height = 280;
    inline constexpr int minimumHeight = 120;     // the least a graph shrinks to, in a Devices view shorter than its editor
    inline constexpr float levelDotRadius = 5.0f; // the dot that rides a compressor's curve at the level it hears
    inline constexpr int meterWidth = 12;         // a meter beside a graph, such as how far a compressor turns down
    inline constexpr float padLit = 0.85f;        // how far a drum pad's colour fills it as it plays (partCurve when it doesn't)
} // namespace GraphTint

// How far a button that needs attention, such as one asking for the instrument a track's notes need, is mixed with the
// warning color.
inline constexpr float attentionTint = 0.35f;

// The accent color's alpha behind something selected: a device's slot in the mixer, or a browser row while the browser
// hasn't the keyboard focus.
inline constexpr float selectionWash = 0.3f;

// The playhead's alpha while it shows the notes being edited playing in a linked clip elsewhere in the song, rather
// than the song's own position.
inline constexpr float linkedPlayheadAlpha = 0.5f;

// Track colors are saved in projects, so their palette is in the model: phazite::getTrackColours().

// How strongly a track's color tints its header: how far the header's surface is mixed with it. The track's clips
// are in its color; its row in the playlist isn't (the owner's choice, 2026-10-16): a selected track's row is only a
// shade lighter, the alpha of the text color over the background.
namespace TrackTint
{
    inline constexpr float selectedRow = 0.06f;
    inline constexpr float header = 0.3f;
    inline constexpr float selectedHeader = 0.5f;
} // namespace TrackTint

// How an automation lane shows under its track (docs/design.md, section 10): its header's surface mixed with the track's
// color, less than the track's header; its background, the panels' surface this opaque over the playlist's, so that
// the grid shows through; the wash under its line, in the track's color; the dashed line where a parameter without
// automation sits (alphas); and its points' radius, a selected point's (filled with the text color and ringed in the
// accent, so that it stands out on a track of any color), its ring's thickness, the radius of the hollow handle that
// bends the line under the mouse, and how near the pointer must come to take a point, a handle or the line (pixels).
namespace LaneTint
{
    inline constexpr float header = 0.15f;
    inline constexpr float background = 0.6f;
    inline constexpr float wash = 0.12f;
    inline constexpr float idleLine = 0.5f;
    inline constexpr float pointRadius = 4.0f;
    inline constexpr float selectedPointRadius = 5.5f;
    inline constexpr float selectedRing = 2.0f;
    inline constexpr float handleRadius = 4.0f;
    inline constexpr float grabDistance = 8.0f;
} // namespace LaneTint

// The ring around a modulated knob (ui::Knob), in the modulation color: how thick its arc is, how far outside the knob's
// edge, the size of the dot where the value is now, and how far inside the edge a drag takes the ring rather than the
// knob (pixels).
namespace ModulationRing
{
    inline constexpr float width = 1.5f;
    inline constexpr float offset = 1.0f;
    inline constexpr float dotSize = 3.5f;
    inline constexpr float grabWidth = 3.0f;
} // namespace ModulationRing

// How the song's sections show behind the playlist's tracks, as ghosts: a faint wash of each section's color
// (stronger while it is selected) and a line in it where the section starts (alphas).
namespace SectionTint
{
    inline constexpr float wash = 0.04f;
    inline constexpr float selectedWash = 0.1f;
    inline constexpr float edge = 0.6f;
} // namespace SectionTint

// How the chord lane shows the song's chords (ui/playlist/ChordLane.h): each a block in a wash of the accent color,
// stronger under the pointer, with a line where it starts, and a line in the warning color where the key changes
// (alphas); the piano roll's wash over the rows of the notes of the chord there (an alpha of the accent color); the
// lane's height, and how near a chord's right edge a drag takes the edge (pixels).
namespace ChordTint
{
    inline constexpr float block = 0.16f;
    inline constexpr float hoveredBlock = 0.28f;
    inline constexpr float edge = 0.7f;
    inline constexpr float keyChange = 0.9f;
    inline constexpr float pianoRollRows = 0.1f;
    inline constexpr int laneHeight = 24;
    inline constexpr int grabDistance = 6;
} // namespace ChordTint

// How the routing view draws (ui/mixer/RoutingView.h): its channels' boxes, the gap between them and the room above them; a
// jack's radius and how near the pointer takes it; how deep the cables dip under the boxes, at least and at most (pixels);
// the cables' thickness, and the wash of a box a dragged cable would go into (an alpha of the accent color).
namespace RoutingTint
{
    inline constexpr int boxWidth = 124, boxHeight = 92, gap = 20, top = 16;
    inline constexpr int jackRadius = 5, jackSpacing = 28, grabDistance = 10;
    inline constexpr int shallowestDip = 24, deepestDip = 150;
    inline constexpr float cableWidth = 2.5f, dropWash = 0.2f;
} // namespace RoutingTint

// How an audio clip's fades show, in the playlist and the audio clip editor: what a fade takes away is shaded black above
// its curve from silence to full volume (alphas), and the curve's thickness.
namespace FadeTint
{
    inline constexpr float shade = 0.3f;
    inline constexpr float curve = 0.6f;
    inline constexpr float curveWidth = 1.5f;
} // namespace FadeTint

// How the audio clip editor shows its clip (ui/audioclip/AudioClipEditor.h): over the panels' background, the clip in a
// wash of its track's color with its audio in that color, and the audio its edges trim off in the muted text color
// (alphas); the line at each edge, and the grip in its middle, which a drag trims; the square handles at its top, which
// a drag fades; and how near the pointer must come to an edge or a handle to take it (pixels).
namespace AudioClipTint
{
    inline constexpr float clipWash = 0.16f;
    inline constexpr float trimmedAudio = 0.35f;
    inline constexpr float edgeWidth = 2.0f;
    inline constexpr int gripWidth = 6;
    inline constexpr int gripHeight = 28;
    inline constexpr int handleSize = 10;
    inline constexpr int grabDistance = 6;
} // namespace AudioClipTint

// How the loop shows in a ruler: its lane along the top, which a drag loops (a shade of the text color), a bar of
// the loop color in that lane over the part that loops, and a wash of it below (alphas); the lane's height, and how
// near the loop's edge a press takes the edge (pixels).
namespace LoopTint
{
    inline constexpr float lane = 0.06f;
    inline constexpr float bar = 0.9f;
    inline constexpr float wash = 0.14f;
    inline constexpr int laneHeight = 8;
    inline constexpr int edgeGrab = 4;
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
