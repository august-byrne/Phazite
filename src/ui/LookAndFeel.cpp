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
    setColour (juce::KeyMappingEditorComponent::backgroundColourId, p.surface);
    setColour (juce::KeyMappingEditorComponent::textColourId, p.text);
    setColour (juce::TreeView::backgroundColourId, p.surface);
    setColour (juce::TreeView::linesColourId, p.border);
    setColour (juce::TreeView::selectedItemBackgroundColourId, p.accent.withAlpha (0.25f));
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

const Palette& getPalette (const juce::Component& component)
{
    if (auto* lookAndFeel = dynamic_cast<const LookAndFeel*> (&component.getLookAndFeel()))
        return lookAndFeel->getPalette();

    static const auto fallback = getPalette (ThemeName::dark);
    return fallback;
}
} // namespace phazite::ui
