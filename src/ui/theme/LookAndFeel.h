// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// Phazite's look: the design tokens applied to JUCE's components, and the Inter typeface. While it lives (the app's, for
// the whole session), it keeps every one of JUCE's standard mouse pointers: JUCE keeps a pointer only while something
// holds it, so a view that switches between two (a clip's halves, select and move) would make one again at each switch,
// the hand's from an image (docs/performance-audit.md, section 8).
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    void setTheme (ThemeName);
    ThemeName getTheme() const noexcept { return theme; }
    const Palette& getPalette() const noexcept { return palette; }

    // A font for numbers that change (tempo, time, levels): fixed-width digits, so they don't jitter.
    static juce::Font getNumberFont (float height);

    // True if Inter's files were found next to the app; otherwise the system font is used.
    bool hasInter() const noexcept { return regular != nullptr; }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // Knobs, faders and buttons in the design tokens' colors, with a visible keyboard focus.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;

    // A bar slider is a number field (ui::NumberField): a box with its value, in the number font, typed in after a click.
    juce::Label* createSliderTextBox (juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool isHighlighted,
                               bool isDown) override;

    // Menu bars: each name lit under the pointer and while its menu is open; the title bar's (ui/shell/TitleBar.h), with
    // onBarProperty set, on the bar's own background, and others, such as a plug-in window's, on the raised surface.
    static constexpr const char* onBarProperty = "phaziteMenuBarOnBar";
    void drawMenuBarBackground (juce::Graphics&, int width, int height, bool isMouseOverBar, juce::MenuBarComponent&) override;
    void drawMenuBarItem (juce::Graphics&, int width, int height, int itemIndex, const juce::String& itemText, bool isMouseOverItem,
                          bool isMenuOpen, bool isMouseOverBar, juce::MenuBarComponent&) override;
    juce::Font getMenuBarFont (juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;
    int getMenuBarItemWidth (juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;

private:
    ThemeName theme = ThemeName::dark;
    Palette palette;
    juce::Typeface::Ptr regular, medium, semiBold;
    std::vector<juce::MouseCursor> standardPointers;
};

// The palette of the LookAndFeel a component uses (the dark theme if that isn't Phazite's).
const Palette& getPalette (const juce::Component&);
} // namespace phazite::ui
