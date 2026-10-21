// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap step 1.6, devices/dynamics/Compressor.h): its curve, with a dot riding it at the level the
// detector hears and a meter of how far it turns the sound down; beside it the last seconds, as FabFilter's compressor shows
// them, what it hears (pale), what goes out and how far it turned the sound down (red, from the top), with the threshold a
// line across; its Threshold and Ratio, large, then the rest, and the track its sidechain listens to. Dragging across the
// curve, or the threshold's line up or down, moves the threshold, as one undo step.
#pragma once

#include "DeviceEditorBase.h"
#include "LevelHistoryView.h"

#include <memory>
#include <vector>

namespace phazite::ui
{
class CompressorEditor : public DeviceEditorBase,
                         private DeviceTicker::Display
{
public:
    explicit CompressorEditor (const DeviceHandle&);
    ~CompressorEditor() override;

    juce::Component& getGraph();
    LevelHistoryView& getHistory();
    juce::ComboBox& getSidechainBox();

    // Where the graph draws a level in (x) and out (y), in decibels, in its own coordinates.
    juce::Point<float> getGraphPosition (float inputDecibels, float outputDecibels) const;

    // What the meters show: the level the detector hears, and how far the sound goes down (0 or more), in decibels.
    float getShownLevel() const noexcept { return shownLevel; }
    float getShownReduction() const noexcept { return shownReduction; }

    // Brings the curve, the meters, the history and the sidechain's source up to date; the ticker does it.
    void refresh();

private:
    class Graph;
    class SourceBox;
    class Activity;

    std::vector<ParameterHandle> parameters; // every parameter, in the compressor's order, for the curve
    std::vector<float> values;
    float shownLevel, shownReduction = 0.0f;

    Activity* activity = nullptr;
    SourceBox* sourceBox = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui
