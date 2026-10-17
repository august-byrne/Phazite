// SPDX-License-Identifier: AGPL-3.0-or-later
// The editor dock under the playlist (docs/design.md, section 4).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
//==============================================================================
// The editor dock under the playlist: tabs for the piano roll, the selected track's devices (DevicesView.h) and the
// mixer. The views belong to the window; the dock only shows them.
class EditorDock : public juce::TabbedComponent
{
public:
    enum Tab
    {
        pianoRoll,
        devices,
        mixer
    };

    EditorDock (juce::Component& pianoRollPanel, juce::Component& devicesPanel, juce::Component& mixerPanel);

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
};
} // namespace phazite::ui
