// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"
#include "LookAndFeel.h"

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
EditorDock::EditorDock()
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Editor");
    setTabBarDepth (32);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each panel paints its own

    addTab ("Piano roll", background, new Panel ("Piano roll", "Notes of the selected MIDI clip. Coming in roadmap step 1.3."), true);
    addTab ("Audio", background, new Panel ("Audio editor", "The selected audio clip. Coming in roadmap step 1.1."), true);
    addTab ("Automation", background, new Panel ("Automation", "Automation lanes and clips. Coming in roadmap step 2.1."), true);
    addTab ("Mixer", background, new Panel ("Mixer", "A channel for every track. Coming in roadmap step 1.4."), true);
}
} // namespace phazite::ui
