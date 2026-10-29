// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's editor (roadmap step 1.6, devices/eq/Equalizer.h): its curve over an analyzer of what it plays (and, fainter, what
// comes in), with a dot for each band to drag (frequency and gain, as one undo step; the wheel changes Q; a double-click turns
// the band on or off; a right-click picks its shape), and beside it the selected band's controls and the output's, which work
// from the keyboard too. Its mix is its header's.
#pragma once

#include "DeviceEditors.h"
#include "DeviceGraph.h"
#include "devices/core/Spectrum.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class EqEditor : public juce::Component,
                 public FlexibleHeight,
                 public ShowsPictureOnly,
                 private DisplayTicker::Display
{
public:
    explicit EqEditor (const DeviceHandle&);
    ~EqEditor() override;

    void showPictureOnly() override; // its graph, filling it
    bool isPictureOnly() const noexcept override { return pictureOnly; }

    static constexpr int numKnobs = 4; // the band's frequency, gain and Q, and the EQ's output

    int getMinimumHeight() const override; // its graph shrinks, down to GraphTint::minimumHeight
    int getWidthForHeight (int) const override { return getWidth(); }

    void paint (juce::Graphics&) override;
    void resized() override;

    // The band whose controls show: the one clicked last.
    int getSelectedBand() const noexcept { return selectedBand; }
    void selectBand (int band);

    juce::Component& getGraph();

    // Where a band's dot is, in the graph's coordinates.
    juce::Point<float> getDotPosition (int band) const;

    // Brings the curve, the controls and the analyzer up to date; the ticker does it 30 times a second.
    void refresh();

private:
    class Graph;

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the EQ's order, for the curve
    std::vector<float> values;
    int selectedBand = 2;
    bool pictureOnly = false;

    std::unique_ptr<Graph> graph;
    juce::OwnedArray<juce::TextButton> bandButtons;
    juce::ToggleButton onButton { "On" };
    juce::ComboBox typeBox, slopeBox;
    Knob frequencyKnob { "Frequency", 1000.0 }, gainKnob { "Gain", 0.0 }, qKnob { "Q", 1.0 }, outputKnob { "Output", 0.0 };
    juce::OwnedArray<juce::Label> labels;

    ToggleAttachment onAttachment { onButton };
    ChoiceAttachment typeAttachment { typeBox }, slopeAttachment { slopeBox };
    ParameterAttachment frequencyAttachment { frequencyKnob }, gainAttachment { gainKnob }, qAttachment { qKnob },
        outputAttachment { outputKnob };

    devices::Spectrum spectrum;
    graph::SpectrumTrace input; // what comes in
    std::vector<float> pulled;
    DrawnNumbers drawnGraph; // what its graph shows

    void tick() override { refresh(); }
    juce::Label& addLabel (const juce::String&);
    double getSampleRate() const; // the EQ's, for its curve near the top of the range
};
} // namespace phazite::ui
