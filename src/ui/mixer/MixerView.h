// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer (docs/design.md, section 8; roadmap step 1.4): a channel strip for every track, in the playlist's
// order and colors, so folders come with their tracks after them; then the return tracks, together next to the
// master, as in Ableton Live; and the master on the right, with its effects and its loudness meter. Selecting a
// channel outlines the ones it feeds and those that feed it, through sends and buses.
#pragma once

#include "ChannelStrip.h"
#include "MasterStrip.h"
#include "model/project/ProjectContext.h"

namespace phazite::ui
{
class MixerView : public juce::Component,
                  private juce::ChangeListener,
                  private ProjectContext::Listener,
                  private juce::Timer
{
public:
    // Shows the open project's channels, and none while no project is open.
    MixerView (CommandBridge&, ProjectContext&);
    ~MixerView() override;

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
    juce::TextButton addReturnButton { "+ Return" };

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
