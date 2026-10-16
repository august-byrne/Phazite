// SPDX-License-Identifier: AGPL-3.0-or-later
// A clip in the playlist: its name and color, and a preview of what it holds (an audio clip's waveform
// and fades, a MIDI clip's notes). It only draws: the playlist handles the mouse, with the Smart Tool.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
namespace te = tracktion;

class ClipComponent : public juce::Component
{
public:
    explicit ClipComponent (te::Clip&);
    ~ClipComponent() override;

    te::Clip* getClip() const { return clip.get(); }

    // Shows the clip as it is now, in this color, and whether it is selected.
    void update (juce::Colour, bool isSelected);
    juce::Colour getColour() const noexcept { return colour; }

    void paint (juce::Graphics&) override;

private:
    te::SafeSelectable<te::Clip> clip;
    juce::Colour colour;
    bool selected = false;

    std::unique_ptr<te::SmartThumbnail> thumbnail; // an audio clip's waveform
    juce::File thumbnailFile;                      // what it shows

    void updateThumbnail (te::AudioClipBase&);
    double xToTime (double x) const; // in seconds, in the Edit
    void paintWaveform (juce::Graphics&, te::AudioClipBase&, juce::Rectangle<int>, juce::Colour);
    void paintFades (juce::Graphics&, te::AudioClipBase&, juce::Rectangle<int>);
    void paintNotes (juce::Graphics&, te::MidiClip&, juce::Rectangle<int>, juce::Colour);
};
} // namespace phazite::ui
