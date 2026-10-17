// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
Panel::Panel (const juce::String& title, const juce::String& text)
    : placeholder (text)
{
    setTitle (title); // read by screen readers
    setDescription (text);
}

void Panel::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    auto area = getLocalBounds().reduced (Space::m);

    g.setColour (p.text);
    g.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    g.drawText (getTitle(), area.removeFromTop (24), juce::Justification::centredLeft);

    area.removeFromTop (Space::s);
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::body));
    g.drawFittedText (placeholder, area.removeFromTop (80), juce::Justification::topLeft, 4);
}

//==============================================================================
EditorDock::EditorDock (juce::Component& pianoRollPanel, juce::Component& mixerPanel)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Editor");
    setTabBarDepth (32);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each panel paints its own

    // Automation has no tab: it is edited in its lanes, under its tracks in the playlist (docs/design.md, section 10).
    addTab ("Piano roll", background, &pianoRollPanel, false);
    addTab ("Audio", background, new Panel ("Audio editor", "The selected audio clip, in detail. Coming in a later roadmap step."), true);
    addTab ("Mixer", background, &mixerPanel, false);
}
} // namespace phazite::ui
