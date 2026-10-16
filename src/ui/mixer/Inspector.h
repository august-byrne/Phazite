// SPDX-License-Identifier: AGPL-3.0-or-later
// The inspector, the panel on the right (docs/design.md, section 8): the channel strip of a track.
#pragma once

#include "ChannelStrip.h"

namespace phazite::ui
{
// The panel on the right. It shows the channel of the selected track; with none selected, that of the
// track live MIDI plays, or else the first track.
class Inspector : public juce::Component,
                  private juce::ChangeListener,
                  private juce::Timer
{
public:
    Inspector (te::Engine&, CommandBridge&, ProjectContext&);
    ~Inspector() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ProjectContext& projectContext;
    te::SelectionManager& selection; // the project's
    ChannelStrip strip;
    juce::Viewport stripView; // scrolls the strip up and down when the panel is shorter than it needs

    void showTrack();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { showTrack(); } // the selection changed
    void timerCallback() override { showTrack(); }                                   // or live MIDI moved
};
} // namespace phazite::ui
