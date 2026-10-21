// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's icons: line drawings from Lucide (ISC licence; a few, from Feather, MIT; see
// resources/icons/lucide/LICENSE), compiled into the app. They take any color, so they follow the theme.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
enum class Icon
{
    play,
    stop,
    record,
    capture,
    keyboard,
    search,
    detach,
    plus,
    magnet,
    loop,
    metronome,
    grip,     // a handle to drag something by
    expanded, // a chevron pointing down: something open that can fold
    folded,   // pointing right: folded
    // The playlist's tools
    smartTool,
    draw,
    paint,
    slice,
    slip,
    mute,
    erase,
    select,
    zoom,
    // The browser
    folder,
    addFolder,
    favorite,
    recent,
    sound,
    midi,
    instrument,
    effect,
    preview,
    download,
    close,     // closes something
    frozen,    // a frozen track: it plays a file of its sound
    lowLatency // the Low Latency switch: a lightning bolt
};

// The icon, drawn in black (IconButton recolors it), or nullptr if it couldn't be read. A filled icon has its shape
// filled as well as outlined, such as a favorite's star.
std::unique_ptr<juce::Drawable> createIcon (Icon, bool filled = false);

//==============================================================================
// A button that shows an icon, with its text beside it if it has any, in the theme's colors.
class IconButton : public juce::Button
{
public:
    // The title is what screen readers say; it matters most for buttons that show only an icon.
    IconButton (const juce::String& title, Icon);

    void setIcon (Icon);

    // A quiet button shows only its icon, without a background until the mouse is over it: for small buttons
    // inside other controls, such as the chevron that collapses a track.
    void setQuiet (bool);

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    Icon icon;
    bool quiet = false;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour drawableColour { juce::Colours::black }; // the color the drawing is in now
};
} // namespace phazite::ui
