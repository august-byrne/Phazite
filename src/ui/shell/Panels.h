// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's two tabbed areas (docs/design.md, section 4): the whole song in the centre, and the selection, in detail,
// in the editor dock under it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
//==============================================================================
// The centre: the whole song, as the playlist (the clips over time) or the mixer (every channel side by side), chosen by
// the tabs at its top, so that no key needs remembering (F5 and F9 do the same, as in FL Studio). The views belong to the
// window; this only shows them.
class SongViews : public juce::TabbedComponent
{
public:
    enum Tab
    {
        playlist,
        mixer
    };

    SongViews (juce::Component& playlistPanel, juce::Component& mixerPanel);

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
    Tab getTab() const { return getCurrentTabIndex() == mixer ? mixer : playlist; }
};

//==============================================================================
// The editor dock under the centre: the selection, in detail, as the piano roll or the selected track's devices
// (DevicesView.h). The views belong to the window; the dock only shows them.
class EditorDock : public juce::TabbedComponent
{
public:
    enum Tab
    {
        pianoRoll,
        devices
    };

    EditorDock (juce::Component& pianoRollPanel, juce::Component& devicesPanel);

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
};
} // namespace phazite::ui
