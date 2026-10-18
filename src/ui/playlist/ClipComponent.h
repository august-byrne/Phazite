// SPDX-License-Identifier: AGPL-3.0-or-later
// A clip in the playlist: its name and color, and a preview of what it holds (an audio clip's waveform
// and fades, a MIDI clip's notes). It only draws: the playlist handles the mouse, with the Smart Tool.
#pragma once

#include "ClipWaveform.h"
#include "model/arrangement/PlaylistModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class ClipComponent : public juce::Component
{
public:
    ClipComponent (ProjectContext&, ClipID);
    ~ClipComponent() override;

    ClipID getClip() const noexcept { return id; }
    const ClipInfo& getInfo() const noexcept { return info; } // as it was shown last

    // Shows the clip as it is now (getClip() in model/arrangement/PlaylistModel.h), with what it holds; call it when
    // the song changes.
    void showClip (const ClipInfo&);

    // Shows it in its track's color, and whether it is selected.
    void showLook (juce::Colour, bool isSelected);
    juce::Colour getColour() const noexcept { return colour; }
    juce::Colour getInk() const; // what is drawn on it: its name and its notes

    void paint (juce::Graphics&) override;

private:
    ProjectContext& context;
    const ClipID id;
    ClipInfo info;
    std::vector<NoteShape> notes; // a MIDI clip's
    juce::Colour colour;
    bool selected = false;
    std::unique_ptr<ClipWaveform> waveform; // an audio clip's, made when it first shows

    juce::Colour getFill() const;
    void paintFades (juce::Graphics&, juce::Rectangle<int>);
    void paintNotes (juce::Graphics&, juce::Rectangle<int>, juce::Colour);
};

// Shades what a fade takes away from an audio clip, above its curve from silence (at x0) to full volume (at x1) between
// a top and a bottom, as the playlist and the audio clip editor draw fades.
void paintFade (juce::Graphics&, float x0, float x1, float top, float bottom, FadeShape);
} // namespace phazite::ui
