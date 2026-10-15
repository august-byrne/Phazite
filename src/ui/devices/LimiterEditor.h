// SPDX-License-Identifier: AGPL-3.0-or-later
// The limiter's editor (roadmap step 1.6, devices/dynamics/Limiter.h): meters of the loudest sound coming in, how far it
// turns the sound down and the loudest going out, with the ceiling marked, and a knob for each control.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class LimiterEditor : public juce::Component,
                      private juce::Timer
{
public:
    explicit LimiterEditor (const DeviceHandle&);
    ~LimiterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The meters, in decibels: the loudest coming in (after the gain), how far the sound goes down, the loudest going out.
    const ValueMeter& getInputMeter() const noexcept { return inputMeter; }
    const ValueMeter& getReductionMeter() const noexcept { return reductionMeter; }
    const ValueMeter& getOutputMeter() const noexcept { return outputMeter; }

    // Brings the knobs' values and the meters up to date; a timer does it 30 times a second.
    void refresh();

private:
    DeviceHandle device;
    ParameterHandle ceiling;

    ValueMeter inputMeter { "Input", -48.0f, 0.0f, ValueMeter::Fill::fromBottom },
        reductionMeter { "Gain Reduction", 0.0f, 24.0f, ValueMeter::Fill::fromTop },
        outputMeter { "Output", -48.0f, 0.0f, ValueMeter::Fill::fromBottom };
    juce::OwnedArray<juce::Label> meterNames;

    juce::OwnedArray<Knob> knobs; // one for each parameter, in the limiter's order
    juce::OwnedArray<ParameterAttachment> attachments;
    juce::OwnedArray<juce::Label> names, texts; // each knob's name, and what it is set to

    void timerCallback() override { refresh(); }
};
} // namespace phazite::ui
