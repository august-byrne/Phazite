// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer (docs/design.md, section 8; roadmap step 1.4): a channel strip for every track, in the playlist's
// order and colors, so folders come with their tracks after them; then the return tracks, together next to the
// master, as in Ableton Live; and the master on the right, with its effects and its loudness meter. Selecting a
// channel outlines the ones it feeds and those that feed it, through sends and buses.
#pragma once

#include "ChannelStrip.h"
#include "MasterStrip.h"

namespace phazite::ui
{
class MixerView : public juce::Component,
                  private juce::ValueTree::Listener,
                  private juce::ChangeListener,
                  private juce::AsyncUpdater,
                  private juce::Timer
{
public:
    MixerView (te::Engine&, CommandBridge&, te::SelectionManager&);
    ~MixerView() override;

    // The open project's Edit, or nullptr while none is open.
    void setEdit (te::Edit*);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    te::Engine& engine;
    CommandBridge& commands;
    te::SelectionManager& selection;
    te::SafeSelectable<te::Edit> edit;
    juce::ValueTree editState; // listened to for tracks being added, deleted and moved

    // The tracks' strips side by side in a view that scrolls sideways, and the master on the right. When the mixer is
    // shorter than the strips need, the whole of it scrolls up and down.
    juce::Viewport verticalView, viewport;
    juce::Component content;               // in verticalView: viewport and master
    juce::Component stripHolder;           // in viewport: the tracks' strips
    juce::OwnedArray<ChannelStrip> strips; // the tracks', then the returns'
    int firstReturn = 0;                   // the index of the first return's strip
    MasterStrip master;
    juce::TextButton addReturnButton { "+ Return" };

    void rebuild(); // after tracks were added, deleted or moved
    void updateSelection();

    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateSelection(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;
    void handleAsyncUpdate() override { rebuild(); }
    void timerCallback() override { updateSelection(); } // sends added or removed change what is related

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
} // namespace phazite::ui
