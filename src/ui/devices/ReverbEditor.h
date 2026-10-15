// SPDX-License-Identifier: AGPL-3.0-or-later
// The reverb's editor (roadmap step 1.6, devices/reverb/Reverb.h): a graph of how its tail fades, after the pre-delay,
// with the treble's faster fade past the damping frequency, and a knob for each control.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class ReverbEditor : public juce::Component,
                     private juce::Timer
{
public:
    explicit ReverbEditor (const DeviceHandle&);
    ~ReverbEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Component& getGraph();

    // The times the graph shows, in seconds: up to where the slowest fade ends, and a little after.
    double getShownSeconds() const;

    // Brings the graph and the knobs' values up to date; a timer does it 10 times a second.
    void refresh();

private:
    class Graph;

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the reverb's order, for the graph
    std::vector<float> values;

    std::unique_ptr<Graph> graph;
    juce::OwnedArray<Knob> knobs; // one for each parameter, in the reverb's order
    juce::OwnedArray<ParameterAttachment> attachments;
    juce::OwnedArray<juce::Label> names, texts; // each knob's name, and what it is set to

    void timerCallback() override { refresh(); }
};
} // namespace phazite::ui
