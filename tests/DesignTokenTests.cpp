// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "ui/theme/DesignTokens.h"

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

        // The loop, in the rulers.
        CHECK (getContrastRatio (p.loop, p.surfaceRaised) >= 3.0);

        // Modulation, around knobs on panels: a part of a control.
        CHECK (getContrastRatio (p.modulation, p.surface) >= 3.0);
        CHECK (getContrastRatio (p.modulation, p.background) >= 3.0);

        // Text on a button that needs attention.
        CHECK (getContrastRatio (p.text, p.surfaceRaised.interpolatedWith (p.warning, attentionTint)) >= 4.5);
    }
}

TEST_CASE ("Track names and handles stay readable on their headers, in every theme and track color")
{
    for (auto theme : { ThemeName::dark, ThemeName::light, ThemeName::highContrast })
    {
        const auto p = getPalette (theme);

        for (const auto& trackColour : phazite::getTrackColours())
        {
            for (auto tint : { TrackTint::header, TrackTint::selectedHeader })
            {
                CAPTURE ((int) theme);
                CAPTURE (trackColour.name);
                CAPTURE (tint);
                const auto background = p.surface.interpolatedWith (trackColour.colour, tint);
                CHECK (getContrastRatio (getInkFor (background), background) >= 4.5);
            }
        }
    }
}
