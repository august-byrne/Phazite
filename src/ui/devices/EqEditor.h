// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's editor (roadmap steps 1.6 and S6.4, devices/eq/Equalizer.h): its curve over an analyzer of what it plays (and,
// fainter, what comes in; DeviceGraph.h's SpectrumTrace, which shows short sounds too), with a dot for each band to drag
// (frequency and gain, as one undo step, from where it was taken, Escape taking it back: graph::HandleDrag; the wheel over a
// dot changes its Q; a double-click turns the band on or off; a right-click picks its shape and placement), which the pointer
// over it shows can be grabbed, and beside it the selected band's controls and the output's, which work from the keyboard
// too. A band that isn't on Stereo has its placement's letter in its dot, and while any band is on Mid or Side the curve is
// two, the middle's and the side's (and the left's and the right's while any is on Left or Right). A dynamic band's dot has
// a line to where its gain goes at its whole Dynamic, with a handle there that drags the Dynamic, and a mark on it where its
// gain is now; the curves are drawn with the gains as they play now, and with what came in mixed back in at the Mix: under
// 100 % a cut is filled in, as it is heard. Its Phase, Minimum or Linear, is two buttons beside the output, with the latency
// Linear plays at beside them. Its mix is its row's in the Devices view.
#pragma once

#include "DeviceEditors.h"
#include "DeviceGraph.h"
#include "ParameterControls.h"
#include "devices/eq/Equalizer.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"
#include "ui/components/PowerSwitch.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>
#include <vector>

namespace phazite::ui
{
class EqEditor : public juce::Component,
                 public FlexibleHeight,
                 private DisplayTicker::Display
{
public:
    explicit EqEditor (const DeviceHandle&);
    ~EqEditor() override;

    static constexpr int numKnobs = 6; // the band's frequency, gain, Q, dynamic and threshold, and the EQ's output

    // Its graph shrinks, down to GraphTint::minimumHeight, and narrows, down to GraphTint::minimumWidth, beside the controls.
    int getMinimumHeight() const override;
    int getWidthForHeight (int) const override;
    int getMinimumWidth() const override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The band whose controls show: the one clicked last.
    int getSelectedBand() const noexcept { return selectedBand; }
    void selectBand (int band);

    juce::Component& getGraph();

    // Where a band's dot is, in the graph's coordinates; and, for a dynamic band, where the handle that drags its Dynamic is
    // (none for a band that isn't dynamic).
    juce::Point<float> getDotPosition (int band) const;
    std::optional<juce::Point<float>> getDynamicHandlePosition (int band) const;

    // What a dot's right-click menu offers a bell or a shelf that isn't dynamic: its Dynamic at -6 dB, as one undo step, which
    // gives it a handle to drag.
    void makeDynamic (int band);

    // What a band's dot says: its number, and its placement's letter (M, S, L or R) when it isn't on Stereo.
    juce::String getDotText (int band) const;

    // The ways through the EQ whose curves the graph draws (devices::Equalizer::getMagnitude()): Stereo alone while every band
    // is on it; else the middle and the side while any is on Mid or Side, and the left and the right while any is on Left or
    // Right.
    std::vector<devices::Equalizer::Placement> getCurvesShown() const;

    // The gain a band plays now, as the curves are drawn with it: its Gain, moved by its Dynamic as the EQ plays it.
    float getShownGain (int band) const;

    // Brings the curve, the controls and the analyzer up to date; the ticker does it 30 times a second.
    void refresh();

    // How many times the graph has worked its curves out: once for each change of what they show, not at each repaint.
    int getCurvesWorkedOut() const noexcept;

    // A way's curve at a frequency, in decibels, as the graph draws it: what is heard, the Mix among it
    // (devices::Equalizer::getHeardMagnitude()).
    float getCurveDecibels (devices::Equalizer::Placement way, double frequency) const;

    // The colour of a way's curve; and of the spectrum of what the EQ plays on that way, which the graph draws for each way
    // whose curve it draws (the middle's and the side's, the left's and the right's, each in its curve's colour, or with every
    // band on Stereo one trace, the louder of the two sides, in the device's colour), and its level at a frequency, tilted, in
    // decibels (graph::SpectrumTrace).
    juce::Colour getCurveColour (devices::Equalizer::Placement way) const;
    juce::Colour getSpectrumColour (devices::Equalizer::Placement way) const;
    float getSpectrumLevel (devices::Equalizer::Placement way, double frequency) const;

    // What shows beside the Phase: the latency the EQ reports while it plays Linear, in milliseconds ("85 ms"); nothing in
    // Minimum.
    juce::String getLatencyText() const { return latencyLabel.getText(); }

private:
    class Graph;

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the EQ's order, for the curve
    std::vector<float> values;
    std::vector<float> playing; // the same, each dynamic band's gain as it plays now: what the curves show
    int selectedBand = 2;

    std::unique_ptr<Graph> graph;
    juce::OwnedArray<juce::TextButton> bandButtons;
    PowerSwitch onButton { "Band On" }; // the selected band's
    juce::ComboBox typeBox, slopeBox, placementBox;
    Knob frequencyKnob { "Frequency", 1000.0 }, gainKnob { "Gain", 0.0 }, qKnob { "Q", 1.0 }, dynamicKnob { "Dynamic", 0.0 },
        thresholdKnob { "Threshold", -24.0 }, outputKnob { "Output", 0.0 };
    juce::OwnedArray<juce::Label> labels;
    std::unique_ptr<ChoiceButtons> phaseButtons;
    juce::Label latencyLabel;

    ToggleAttachment onAttachment { onButton };
    ChoiceAttachment typeAttachment { typeBox }, slopeAttachment { slopeBox }, placementAttachment { placementBox };
    ParameterAttachment frequencyAttachment { frequencyKnob }, gainAttachment { gainKnob }, qAttachment { qKnob },
        dynamicAttachment { dynamicKnob }, thresholdAttachment { thresholdKnob }, outputAttachment { outputKnob };

    graph::SpectrumTrace input, output; // what comes in, and what it plays
    DrawnNumbers drawnGraph;            // what its graph shows

    void tick() override { refresh(); }
    void shownAgain() override; // the spectra of the sound now, not of what played while nobody looked
    juce::Label& addLabel (const juce::String&);
    double getSampleRate() const; // the EQ's, for its curve near the top of the range
};
} // namespace phazite::ui
