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
    keyboard,
    search,
    detach
};

// The icon, drawn in black (IconButton recolors it), or nullptr if it couldn't be read.
std::unique_ptr<juce::Drawable> createIcon (Icon);

//==============================================================================
// A button that shows an icon, with its text beside it if it has any, in the theme's colors.
class IconButton : public juce::Button
{
public:
    // The title is what screen readers say; it matters most for buttons that show only an icon.
    IconButton (const juce::String& title, Icon);

    void setIcon (Icon);
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    Icon icon;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour drawableColour { juce::Colours::black }; // the color the drawing is in now
};
} // namespace phazite::ui
