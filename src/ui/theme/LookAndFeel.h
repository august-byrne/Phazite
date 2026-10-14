// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// Phazite's look: the design tokens applied to JUCE's components, and the Inter typeface.
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
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool isHighlighted,
                               bool isDown) override;

private:
    ThemeName theme = ThemeName::dark;
    Palette palette;
    juce::Typeface::Ptr regular, medium, semiBold;
};

// The palette of the LookAndFeel a component uses (the dark theme if that isn't Phazite's).
const Palette& getPalette (const juce::Component&);
} // namespace phazite::ui
