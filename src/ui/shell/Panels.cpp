// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"

namespace phazite::ui
{
namespace
{
    constexpr int tabBarDepth = 32;
} // namespace

SongViews::SongViews (juce::Component& playlistPanel, juce::Component& mixerPanel)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Song");
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each view paints its own
    addTab ("Playlist", background, &playlistPanel, false);
    addTab ("Mixer", background, &mixerPanel, false);
}

EditorDock::EditorDock (juce::Component& pianoRollPanel, juce::Component& devicesPanel)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Editor");
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each panel paints its own

    // Automation has no tab: it is edited in its lanes, under its tracks in the playlist (docs/design.md, section 10).
    // Audio clips get an editor of their own here (roadmap step 2.6).
    addTab ("Piano roll", background, &pianoRollPanel, false);
    addTab ("Devices", background, &devicesPanel, false);
}
} // namespace phazite::ui
