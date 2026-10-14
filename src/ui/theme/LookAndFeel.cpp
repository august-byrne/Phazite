// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LookAndFeel.h"

#include <juce_gui_extra/juce_gui_extra.h> // KeyMappingEditorComponent

namespace phazite::ui
{
namespace
{
    // Inter ships as separate files next to the app (fonts/Inter), under the SIL Open Font License.
    juce::Typeface::Ptr loadInter (const char* fileName)
    {
        const auto file = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                              .getSiblingFile ("fonts")
                              .getChildFile ("Inter")
                              .getChildFile (fileName);

        juce::MemoryBlock data;

        if (! file.loadFileAsData (data) || data.isEmpty())
            return {};

        return juce::Typeface::createSystemTypefaceFor (data.getData(), data.getSize());
    }
} // namespace

LookAndFeel::LookAndFeel()
    : regular (loadInter ("Inter-Regular.ttf")),
      medium (loadInter ("Inter-Medium.ttf")),
      semiBold (loadInter ("Inter-SemiBold.ttf"))
{
    setTheme (ThemeName::dark);
}

void LookAndFeel::setTheme (ThemeName newTheme)
{
    theme = newTheme;
    palette = phazite::ui::getPalette (theme);
    const auto& p = palette;

    setColourScheme ({ p.background, p.surfaceRaised, p.surfaceRaised, p.border, p.text, p.accent, p.textOnAccent, p.accent, p.text });

    setColour (juce::ResizableWindow::backgroundColourId, p.background);
    setColour (juce::DocumentWindow::textColourId, p.text);
    setColour (juce::TextButton::buttonColourId, p.surfaceRaised);
    setColour (juce::TextButton::buttonOnColourId, p.accent);
    setColour (juce::TextButton::textColourOffId, p.text);
    setColour (juce::TextButton::textColourOnId, p.textOnAccent);
    setColour (juce::Label::textColourId, p.text);
    setColour (juce::TextEditor::backgroundColourId, p.surfaceRaised);
    setColour (juce::TextEditor::textColourId, p.text);
    setColour (juce::TextEditor::highlightColourId, p.accent.withAlpha (0.4f));
    setColour (juce::TextEditor::outlineColourId, p.border);
    setColour (juce::TextEditor::focusedOutlineColourId, p.accent);
    setColour (juce::ListBox::backgroundColourId, p.surface);
    setColour (juce::PopupMenu::backgroundColourId, p.surfaceRaised);
    setColour (juce::PopupMenu::textColourId, p.text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, p.accent);
    setColour (juce::PopupMenu::highlightedTextColourId, p.textOnAccent);
    setColour (juce::TabbedButtonBar::tabTextColourId, p.textMuted);
    setColour (juce::TabbedButtonBar::frontTextColourId, p.text);
    setColour (juce::TabbedComponent::backgroundColourId, p.surface);
    setColour (juce::TabbedComponent::outlineColourId, p.border);
    setColour (juce::TooltipWindow::backgroundColourId, p.surfaceRaised);
    setColour (juce::TooltipWindow::textColourId, p.text);
    setColour (juce::TooltipWindow::outlineColourId, p.border);
    setColour (juce::AlertWindow::backgroundColourId, p.surfaceRaised);
    setColour (juce::AlertWindow::textColourId, p.text);
    setColour (juce::AlertWindow::outlineColourId, p.border);
    setColour (juce::ScrollBar::thumbColourId, p.textMuted.withAlpha (0.5f));
    setColour (juce::ToggleButton::textColourId, p.text);
    setColour (juce::ToggleButton::tickColourId, p.accent);
    setColour (juce::ToggleButton::tickDisabledColourId, p.textMuted); // also the box's outline
    setColour (juce::ComboBox::backgroundColourId, p.surfaceRaised);
    setColour (juce::ComboBox::textColourId, p.text);
    setColour (juce::ComboBox::outlineColourId, p.border);
    setColour (juce::ComboBox::arrowColourId, p.textMuted);
    setColour (juce::ComboBox::focusedOutlineColourId, p.accent);
    setColour (juce::BubbleComponent::backgroundColourId, p.surfaceRaised); // a slider's value while dragging
    setColour (juce::BubbleComponent::outlineColourId, p.border);
    setColour (juce::KeyMappingEditorComponent::backgroundColourId, p.surface);
    setColour (juce::KeyMappingEditorComponent::textColourId, p.text);
    setColour (juce::TreeView::backgroundColourId, p.surface);
    setColour (juce::TreeView::linesColourId, p.border);
    setColour (juce::TreeView::selectedItemBackgroundColourId, p.accent.withAlpha (0.25f));
    setColour (juce::ProgressBar::backgroundColourId, p.surfaceRaised);
    setColour (juce::ProgressBar::foregroundColourId, p.accent);
}

juce::Font LookAndFeel::getNumberFont (float height)
{
    return juce::Font (juce::FontOptions().withHeight (height).withFeatureEnabled ("tnum"));
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont (const juce::Font& font)
{
    // Fonts that ask for a specific family keep it; everything else (the default sans-serif) is Inter.
    if (regular != nullptr && font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
    {
        if (font.isBold())
            return semiBold != nullptr ? semiBold : regular;

        if (font.getTypefaceStyle().containsIgnoreCase ("Medium"))
            return medium != nullptr ? medium : regular;

        return regular;
    }

    return juce::LookAndFeel_V4::getTypefaceForFont (font);
}

//==============================================================================
void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float startAngle,
                                    float endAngle, juce::Slider& slider)
{
    const auto& p = palette;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const auto lineWidth = juce::jmax (2.0f, radius * 0.16f);
    const auto arcRadius = radius - lineWidth;
    const auto angle = startAngle + sliderPos * (endAngle - startAngle);

    const auto strokeArc = [&] (float from, float to, juce::Colour colour)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, from, to, true);
        g.setColour (colour);
        g.strokePath (arc, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    strokeArc (startAngle, endAngle, p.border);

    // The value's arc starts at the bottom of the range, or in the middle of ranges such as pan.
    if (slider.isEnabled())
    {
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const auto origin = bipolar ? startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;
        strokeArc (juce::jmin (origin, angle), juce::jmax (origin, angle), p.accent);
    }

    g.setColour (slider.isEnabled() ? p.text : p.textMuted);
    g.drawLine (juce::Line<float> (centre.getPointOnCircumference (arcRadius * 0.3f, angle),
                                   centre.getPointOnCircumference (arcRadius * 0.85f, angle)),
                lineWidth);

    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (p.accent);
        g.drawEllipse (bounds.withSizeKeepingCentre (radius * 2.0f, radius * 2.0f).reduced (0.75f), 1.5f);
    }
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                                    float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto& p = palette;
    const bool vertical = style == juce::Slider::LinearVertical;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    constexpr float trackThickness = 4.0f, thumbLength = 10.0f, thumbWidth = 22.0f;

    const auto track = vertical ? bounds.withSizeKeepingCentre (trackThickness, bounds.getHeight())
                                : bounds.withSizeKeepingCentre (bounds.getWidth(), trackThickness);
    g.setColour (p.border);
    g.fillRoundedRectangle (track, trackThickness / 2.0f);

    // Filled from the bottom (or left) to the value.
    g.setColour (slider.isEnabled() ? p.accent : p.textMuted);
    g.fillRoundedRectangle (vertical ? track.withTop (sliderPos) : track.withRight (sliderPos), trackThickness / 2.0f);

    // The thumb, outlined in the accent when it has the keyboard focus.
    const auto thumb = vertical ? juce::Rectangle<float> (juce::jmin (bounds.getWidth(), thumbWidth), thumbLength)
                                      .withCentre ({ bounds.getCentreX(), sliderPos })
                                : juce::Rectangle<float> (thumbLength, juce::jmin (bounds.getHeight(), thumbWidth))
                                      .withCentre ({ sliderPos, bounds.getCentreY() });
    const bool focused = slider.hasKeyboardFocus (false);

    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (thumb, 2.0f);
    g.setColour (focused ? p.accent : p.textMuted);
    g.drawRoundedRectangle (thumb.reduced (0.5f), 2.0f, focused ? 2.0f : 1.0f);
}

int LookAndFeel::getSliderThumbRadius (juce::Slider& slider)
{
    if (slider.getSliderStyle() == juce::Slider::LinearVertical || slider.getSliderStyle() == juce::Slider::LinearHorizontal)
        return 6; // half the thumb, so it stays inside the fader at both ends

    return juce::LookAndFeel_V4::getSliderThumbRadius (slider);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour, bool isHighlighted,
                                        bool isDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = backgroundColour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);

    if (isDown || isHighlighted)
        colour = colour.contrasting (isDown ? 0.15f : 0.06f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, cornerRadius);

    // The keyboard focus shows as a thick outline in the text color, which stands out on every background.
    const bool focused = button.hasKeyboardFocus (false);
    g.setColour (focused ? palette.text : palette.border);
    g.drawRoundedRectangle (focused ? bounds.reduced (1.0f) : bounds, cornerRadius, focused ? 2.0f : 1.0f);
}

const Palette& getPalette (const juce::Component& component)
{
    if (auto* lookAndFeel = dynamic_cast<const LookAndFeel*> (&component.getLookAndFeel()))
        return lookAndFeel->getPalette();

    static const auto fallback = getPalette (ThemeName::dark);
    return fallback;
}
} // namespace phazite::ui
