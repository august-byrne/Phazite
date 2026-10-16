// SPDX-License-Identifier: AGPL-3.0-or-later
// The editors of Phazite's own devices (roadmap step 1.6), which device windows (src/app/PluginWindow.h) show: a
// device's own editor, or until it has one, a knob for each of its parameters. They work through the model's
// DeviceHandle and ParameterHandles, so the model owns every undo step and the editors use no Tracktion types.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace phazite::ui
{
// The editor for one of Phazite's devices, or nullptr if the handle has none.
std::unique_ptr<juce::Component> createDeviceEditor (const DeviceHandle&);

//==============================================================================
// A knob for each of a device's parameters, titled with its name and showing its value in its unit.
class DeviceKnobs : public juce::Component,
                    private juce::Timer
{
public:
    explicit DeviceKnobs (const DeviceHandle&);
    ~DeviceKnobs() override;

    int getNumKnobs() const noexcept { return knobs.size(); }
    Knob& getKnob (int index) { return *knobs[index]; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int columns = 6;

    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    juce::OwnedArray<juce::Label> labels;

    void timerCallback() override; // shows changes from automation, undo and elsewhere
};
} // namespace phazite::ui
