// SPDX-License-Identifier: AGPL-3.0-or-later
// A clip in the playlist: its name and color, and a preview of what it holds (an audio clip's waveform
// and fades, a MIDI clip's notes). It only draws: the playlist handles the mouse, with the Smart Tool.
#pragma once

#include "ClipWaveform.h"
#include "model/arrangement/PlaylistModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

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

    void paint (juce::Graphics&) override;

private:
    ProjectContext& context;
    const ClipID id;
    ClipInfo info;
    std::vector<NoteShape> notes; // a MIDI clip's
    juce::Colour colour;
    bool selected = false;
    std::unique_ptr<ClipWaveform> waveform; // an audio clip's, made when it first shows

    void paintFades (juce::Graphics&, juce::Rectangle<int>);
    void paintNotes (juce::Graphics&, juce::Rectangle<int>, juce::Colour);
};
} // namespace phazite::ui
