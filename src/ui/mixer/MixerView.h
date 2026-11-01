// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer (docs/design.md, section 8; roadmap step 1.4): a channel strip for every track, in the playlist's
// order and colors, so folders come with their tracks after them; then the return tracks, together next to the
// master, as in Ableton Live, with a slim column after them whose "+" adds a return; and the master on the right, with its
// effects and its loudness meter. Selecting a channel outlines the ones it feeds and those that feed it, through sends,
// buses and sidechains. The Routing switch, in the tab bar above it (Panels.h), shows every channel's wires instead, in
// the routing view (RoutingView.h). It has no row of its own (the owner, 2026-11-01: the row took height the strips need).
#pragma once

#include "ChannelStrip.h"
#include "MasterStrip.h"
#include "RoutingView.h"
#include "model/project/ProjectContext.h"
#include "ui/theme/Icons.h"

namespace phazite::ui
{
class MixerView : public juce::Component,
                  public juce::DragAndDropContainer, // an effect dragged by its name in a strip, to move it
                  private juce::ChangeListener,
                  private ProjectContext::Listener,
                  private juce::Timer
{
public:
    // Shows the open project's channels, and none while no project is open.
    MixerView (CommandBridge&, ProjectContext&);
    ~MixerView() override;

    // Every channel's wires in place of the strips, or the strips again (the tab bar's Routing switch).
    void showRouting (bool);
    bool isShowingRouting() const noexcept { return routingViewport.isVisible(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CommandBridge& commands;
    ProjectContext& context;

    // The tracks' strips side by side in a view that scrolls sideways, and the master on the right. When the mixer is
    // shorter than the strips need, the whole of it scrolls up and down.
    juce::Viewport verticalView, viewport;
    juce::Component content;               // in verticalView: viewport and master
    juce::Component stripHolder;           // in viewport: the tracks' strips
    juce::OwnedArray<ChannelStrip> strips; // the tracks', then the returns'
    int firstReturn = 0;                   // the index of the first return's strip
    MasterStrip master;
    IconButton addReturnButton { "Add Return Track", Icon::plus }; // in a slim column after the returns, left of the master
    juce::Viewport routingViewport;
    RoutingView routing;

    void showProject(); // the open project's channels, or none while no project is open
    void rebuild();     // after tracks were added, deleted or moved
    void updateSelection();

    void projectChanged() override { showProject(); }
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateSelection(); } // the selection changed

    void timerCallback() override // a while after devices changed
    {
        stopTimer();
        updateSelection();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
} // namespace phazite::ui
