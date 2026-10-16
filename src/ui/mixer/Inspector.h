// SPDX-License-Identifier: AGPL-3.0-or-later
// The inspector, the panel on the right (docs/design.md, section 8): the channel strip of a track.
#pragma once

#include "ChannelStrip.h"

namespace phazite::ui
{
// The panel on the right. It shows the channel of the selected track; with none selected, that of the
// track live MIDI plays, or else the first track (phazite::getInspectedChannel()).
class Inspector : public juce::Component,
                  private ProjectContext::Listener,
                  private juce::ChangeListener,
                  private juce::Timer
{
public:
    Inspector (CommandBridge&, ProjectContext&);
    ~Inspector() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ProjectContext& context;
    ChannelStrip strip;
    juce::Viewport stripView; // scrolls the strip up and down when the panel is shorter than it needs

    void showTrack();
    void projectChanged() override { strip.setTrack (getInspectedChannel (context)); } // even one with the same ID
    void songChanged (const SongChanges& changes) override
    {
        if (changes.tracks)
            showTrack(); // the track shown may have been deleted
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { showTrack(); } // the selection changed
    void timerCallback() override { showTrack(); }                                   // or live MIDI moved
};
} // namespace phazite::ui
