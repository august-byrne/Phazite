// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"

namespace phazite::ui
{
EditorDock::EditorDock (juce::Component& pianoRollPanel, juce::Component& devicesPanel, juce::Component& mixerPanel)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Editor");
    setTabBarDepth (32);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each panel paints its own

    // Automation has no tab: it is edited in its lanes, under its tracks in the playlist (docs/design.md, section 10).
    // Audio clips get an editor of their own here (roadmap step 2.6).
    addTab ("Piano roll", background, &pianoRollPanel, false);
    addTab ("Devices", background, &devicesPanel, false);
    addTab ("Mixer", background, &mixerPanel, false);
}
} // namespace phazite::ui
