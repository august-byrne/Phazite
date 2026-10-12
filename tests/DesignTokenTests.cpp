// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "ui/DesignTokens.h"

using namespace phazite::ui;

TEST_CASE ("The contrast ratio matches WCAG's reference values")
{
    CHECK (getContrastRatio (juce::Colours::black, juce::Colours::white) == doctest::Approx (21.0));
    CHECK (getContrastRatio (juce::Colours::white, juce::Colours::white) == doctest::Approx (1.0));
    CHECK (getContrastRatio (juce::Colour (0xff767676), juce::Colours::white) == doctest::Approx (4.54).epsilon (0.01));
}

TEST_CASE ("Every theme meets WCAG 2.2's contrast for text and controls")
{
    for (auto theme : { ThemeName::dark, ThemeName::light, ThemeName::highContrast })
    {
        CAPTURE ((int) theme);
        const auto p = getPalette (theme);

        // Text, on everything it is drawn on: 4.5:1.
        for (const auto& background : { p.background, p.surface, p.surfaceRaised })
        {
            CHECK (getContrastRatio (p.text, background) >= 4.5);
            CHECK (getContrastRatio (p.textMuted, background) >= 4.5);
        }

        CHECK (getContrastRatio (p.textOnAccent, p.accent) >= 4.5);

        // The accent marks selection and focus on panels: 3:1, like other parts of controls.
        CHECK (getContrastRatio (p.accent, p.surface) >= 3.0);
        CHECK (getContrastRatio (p.warning, p.surface) >= 3.0);
        CHECK (getContrastRatio (p.danger, p.surface) >= 3.0);
    }
}

TEST_CASE ("Track colours are distinct")
{
    const auto colours = getTrackColours();
    CHECK (colours.size() >= 8);

    for (int i = 0; i < colours.size(); ++i)
        for (int j = i + 1; j < colours.size(); ++j)
            CHECK (colours[i] != colours[j]);
}
