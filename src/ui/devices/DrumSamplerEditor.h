// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's editor (roadmap step 1.6, devices/drums/DrumSampler.h): its 16 pads in a grid, the first at the
// bottom left as on FL Studio's FPC and an MPC, each named after what it plays and lit when it plays. Clicking a pad,
// or Space or Enter on it, plays it and shows its controls: level, pan, tuning, attack, length and choke group, and
// the sound it plays, which Load... or a sound dropped on the pad from the browser or Explorer replaces, and Reset
// puts back.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace phazite::ui
{
class DrumSamplerEditor : public juce::Component,
                          private juce::Timer
{
public:
    explicit DrumSamplerEditor (const DeviceHandle&);
    ~DrumSamplerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The pad whose controls show: the one played or dropped on last.
    int getSelectedPad() const noexcept { return selectedPad; }
    void selectPad (int pad);

    // A pad's button, from 0; what dropping a file on it does, as a drop from the browser or Explorer.
    juce::Button& getPad (int pad);
    bool dropFile (int pad, const juce::File&);

    // Brings the pads' names and lights and the controls up to date; a timer does it 30 times a second.
    void refresh();

private:
    class Pad;
    static constexpr int numPads = 16;

    DeviceHandle device;
    int selectedPad = 0;
    std::array<std::unique_ptr<Pad>, numPads> pads;

    juce::Label padName;
    juce::TextButton loadButton { "Load..." }, resetButton { "Reset" };
    std::unique_ptr<juce::FileChooser> chooser;

    static constexpr int numKnobs = 6; // the pad's level, pan, tune, attack and length, and the output
    std::array<std::unique_ptr<Knob>, numKnobs> knobs;
    std::array<std::unique_ptr<ParameterAttachment>, numKnobs> attachments;
    juce::ComboBox chokeBox;
    ChoiceAttachment chokeAttachment { chokeBox };
    juce::OwnedArray<juce::Label> names, texts; // the knobs' and the choke group's names, and the knobs' values

    void play (int pad);
    void chooseFile();
    void timerCallback() override
    {
        if (isShowing()) // nothing to show behind another tab of the Devices view
            refresh();
    }
};
} // namespace phazite::ui
