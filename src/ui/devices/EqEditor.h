// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's editor (roadmap step 1.6, devices/eq/Equalizer.h): its curve over an analyzer of what it plays, with a dot
// for each band to drag (frequency and gain, as one undo step; the wheel changes Q; a double-click turns the band on
// or off; a right-click picks its shape), and the selected band's controls below, which work from the keyboard too.
#pragma once

#include "devices/core/Spectrum.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class EqEditor : public juce::Component,
                 private juce::Timer
{
public:
    explicit EqEditor (const DeviceHandle&);
    ~EqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The band whose controls show: the one clicked last.
    int getSelectedBand() const noexcept { return selectedBand; }
    void selectBand (int band);

    juce::Component& getGraph();

    // Where a band's dot is, in the graph's coordinates.
    juce::Point<float> getDotPosition (int band) const;

    // Brings the curve, the controls and the analyzer up to date; a timer does it 30 times a second.
    void refresh();

private:
    class Graph;

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the EQ's order, for the curve
    std::vector<float> values;
    int selectedBand = 2;

    std::unique_ptr<Graph> graph;
    juce::OwnedArray<juce::TextButton> bandButtons;
    juce::ToggleButton onButton { "On" };
    juce::ComboBox typeBox, slopeBox;
    static constexpr int numKnobs = 5; // the band's frequency, gain and Q, and the EQ's mix and output
    Knob frequencyKnob { "Frequency", 1000.0 }, gainKnob { "Gain", 0.0 }, qKnob { "Q", 1.0 }, mixKnob { "Mix", 1.0 },
        outputKnob { "Output", 0.0 };
    juce::OwnedArray<juce::Label> labels;

    ToggleAttachment onAttachment { onButton };
    ChoiceAttachment typeAttachment { typeBox }, slopeAttachment { slopeBox };
    ParameterAttachment frequencyAttachment { frequencyKnob }, gainAttachment { gainKnob }, qAttachment { qKnob },
        mixAttachment { mixKnob }, outputAttachment { outputKnob };

    devices::Spectrum spectrum;
    std::vector<float> pulled;

    void timerCallback() override { refresh(); }
    juce::Label& addLabel (const juce::String&);
    double getSampleRate() const; // the EQ's, for its curve near the top of the range
};
} // namespace phazite::ui
