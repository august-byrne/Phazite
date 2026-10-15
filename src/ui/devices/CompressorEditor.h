// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap step 1.6, devices/dynamics/Compressor.h): its curve, with a dot riding it at the
// level the detector hears and a meter of how far it turns the sound down, its knobs, and the track its sidechain
// listens to. Dragging across the curve moves the threshold, as one undo step.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class CompressorEditor : public juce::Component,
                         private juce::Timer
{
public:
    explicit CompressorEditor (const DeviceHandle&);
    ~CompressorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Component& getGraph();
    juce::ComboBox& getSidechainBox();

    // Where the graph draws a level in (x) and out (y), in decibels, in its own coordinates.
    juce::Point<float> getGraphPosition (float inputDecibels, float outputDecibels) const;

    // What the meters show: the level the detector hears, and how far the sound goes down (0 or more), in decibels.
    float getShownLevel() const noexcept { return shownLevel; }
    float getShownReduction() const noexcept { return shownReduction; }

    // Brings the curve, the knobs' values, the meters and the sidechain's source up to date; a timer does it 30 times a
    // second.
    void refresh();

private:
    class Graph;
    class SourceBox;

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the compressor's order, for the curve
    std::vector<float> values;
    float shownLevel, shownReduction = 0.0f;

    std::unique_ptr<Graph> graph;
    std::unique_ptr<SourceBox> sourceBox;
    juce::Label sourceLabel;
    juce::OwnedArray<Knob> knobs; // one for each parameter, in the compressor's order
    juce::OwnedArray<ParameterAttachment> attachments;
    juce::OwnedArray<juce::Label> names, texts; // each knob's name, and what it is set to

    void timerCallback() override { refresh(); }
};
} // namespace phazite::ui
