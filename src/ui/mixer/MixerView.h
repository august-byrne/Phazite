// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer (docs/design.md, section 8; roadmap step 1.4): a channel strip for every track, in the playlist's
// order and colors, so folders come with their tracks after them; then the return tracks, together next to the
// master, as in Ableton Live, with a slim column after them whose "+" adds a return; and the master on the right, with its
// effects and its loudness meter. Selecting a channel outlines the ones it feeds and those that feed it, through sends,
// buses and sidechains: routing is set on the strips and shows from the selection, with no view apart (the owner,
// 2026-11-01, after an audit of how DAWs route: docs/design.md, section 8). It has no row of its own (the owner,
// 2026-11-01: the row took height the strips need).
//
// A song can have hundreds of channels, so only those in view have strips, and one more at either end, which the Tab key
// goes on to (scrolling it into view), as a list that shows a window of its rows: a strip is made as its channel comes into
// view and goes as it leaves, but not while it is in use (it has the keyboard focus, or the mouse is pressed on it). A screen
// reader hears the strips in view, and from the channels' view how many there are and which show.
#pragma once

#include "ChannelStrip.h"
#include "MasterStrip.h"
#include "model/project/ProjectContext.h"
#include "ui/theme/Icons.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CommandBridge& commands;
    ProjectContext& context;

    // The channels' view, which tells the mixer when it scrolls.
    struct StripsView : public juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override
        {
            if (onScroll != nullptr)
                onScroll();
        }
    };

    // The tracks' strips side by side in a view that scrolls sideways, and the master on the right. When the mixer is
    // shorter than the strips need, the whole of it scrolls up and down.
    juce::Viewport verticalView;
    StripsView viewport;
    juce::Component content;                                       // in verticalView: viewport and master
    juce::Component stripHolder;                                   // in viewport: the tracks' strips, as wide as every channel's would be
    std::vector<TrackID> channels;                                 // the tracks', then the returns'
    std::unordered_map<TrackID, int> channelIndex;                 // channels[i] by its ID
    std::vector<std::unique_ptr<ChannelStrip>> strips;             // of the channels in view (layOutStrips()), in no order
    int firstReturn = 0;                                           // the index of the first return's channel
    std::unordered_set<TrackID> selectedChannels, relatedChannels; // as updateSelection() found them, for strips made later
    MasterStrip master;
    IconButton addReturnButton { "Add Return Track", Icon::plus }; // in a slim column after the returns, left of the master

    void showProject(); // the open project's channels, or none while no project is open
    void rebuild();     // after tracks were added, deleted or moved
    void updateSelection();
    void showSelectionOn (ChannelStrip&) const;

    int getChannelX (int index) const;            // where a channel's strip goes, in stripHolder
    void layOutStrips();                          // a strip for each channel in view, and one more at either end, where it goes
    static constexpr double makingBudgetMs = 6.0; // what part of a frame a jump makes strips for (layOutStrips())
    bool restPosted = false;                      // the strips a jump left are made a turn of the message loop later
    void showWhole (TrackID);                     // scrolls the channel's strip into view (the Tab key gave it the focus)

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
