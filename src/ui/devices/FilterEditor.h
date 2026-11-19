// SPDX-License-Identifier: AGPL-3.0-or-later
// The Filter's editor (devices/filter/FilterEffect.h): its response, lit over the spectrum of what it plays (and fainter, what
// comes in), as Ableton's Auto Filter shows it, so each type looks like itself: a slope, a peak, a notch, a comb's teeth, a
// vowel's formants or a phaser's notches; Driven, its response to a quiet sound, whose peak goes off the top where it sings,
// and the word "Driven" in its readout. The curve is what is heard, the Mix among it: under 100 %, what came in fills the cut
// in (FilterEffect::getHeardMagnitude()); Sample & Hold, which has no single response, says its Mix in the readout instead.
// A dot sits at the cutoff, as high as the resonance: a drag from anywhere moves both, as one undo step, as far as the pointer
// goes (GraphDot, which keeps every graph's grammar), and the wheel turns the drive. Then its Cutoff, large, its Resonance; its Character, Clean or Driven, as
// two buttons over its type, slope and drive; and its Quality.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class FilterEditor : public DeviceEditorBase
{
public:
    explicit FilterEditor (const DeviceHandle&);
    ~FilterEditor() override;

    juce::Component& getGraph() const noexcept;

    // Where the graph draws its dot (the cutoff, as high as the resonance), in its own coordinates.
    juce::Point<float> getDotPosition() const;

    // What the graph draws: its curve's level at a frequency, in decibels, and its readout along the top at the left.
    float getCurveDecibels (double frequency) const;
    juce::String getReadout() const;

    // The spectrum of what it plays, at a frequency, tilted, in decibels (graph::SpectrumTrace): afresh when it shows again.
    float getSpectrumLevel (double frequency) const;

private:
    class Graph;
    class CharacterPart;
    Graph* graph = nullptr;
};
} // namespace phazite::ui
