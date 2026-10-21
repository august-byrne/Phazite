// SPDX-License-Identifier: AGPL-3.0-or-later
// The Filter's editor (devices/filter/FilterEffect.h): its response, lit over the spectrum of what it plays (and fainter, what
// comes in), as Ableton's Auto Filter shows it, so each type looks like itself: a slope, a peak, a notch, a comb's teeth, a
// vowel's formants or a phaser's notches. A dot sits at the cutoff, as high as the resonance: a drag moves both, as one undo
// step, and the wheel turns the drive. Then its Cutoff, large, its Resonance, its type, slope and drive.
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

private:
    class Graph;
    Graph* graph = nullptr;
};
} // namespace phazite::ui
