// SPDX-License-Identifier: AGPL-3.0-or-later
// The drum sampler's editor (roadmap step 1.6, devices/drums/DrumSampler.h): its 16 pads in a grid, the first at the bottom
// left as on FL Studio's FPC and an MPC, each named after what it plays, its choke group in its corner, and lit when it
// plays. Clicking a pad, or Space or Enter on it, plays it and shows it: the sound it plays, drawn, with its attack's ramp at
// the start and its length's cut, each dragged sideways as one undo step, and a line where it is as it plays; Load... or a
// sound dropped on the pad from the browser or Explorer replaces it, and Reset puts the kit's back; then its level, pan,
// tuning, attack, length and choke group, and the output. The pads fit the dock's height.
#pragma once

#include "DeviceEditors.h"
#include "DeviceTicker.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace phazite::ui
{
class DrumSamplerEditor : public juce::Component,
                          public FlexibleHeight,
                          private DeviceTicker::Display
{
public:
    explicit DrumSamplerEditor (const DeviceHandle&);
    ~DrumSamplerEditor() override;

    int getMinimumHeight() const override;
    int getWidthForHeight (int height) const override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The pad whose controls show: the one played or dropped on last.
    int getSelectedPad() const noexcept { return selectedPad; }
    void selectPad (int pad);

    // A pad's button, from 0; what dropping a file on it does, as a drop from the browser or Explorer.
    juce::Button& getPad (int pad);
    bool dropFile (int pad, const juce::File&);

    // The selected pad's sound, drawn; where it draws the attack's end and the length's cut, in its own coordinates.
    juce::Component& getWaveform() const;
    float getAttackX() const;
    float getLengthX() const;

    // Brings the pads' names and lights, the waveform and the controls up to date; the ticker does it 30 times a second.
    void refresh();

private:
    class Pad;
    class Waveform;
    static constexpr int numPads = 16;

    DeviceHandle device;
    int selectedPad = 0;
    std::array<std::unique_ptr<Pad>, numPads> pads;
    std::unique_ptr<Waveform> waveform;

    juce::Label padName;
    juce::TextButton loadButton { "Load..." }, resetButton { "Reset" };
    std::unique_ptr<juce::FileChooser> chooser;

    static constexpr int numKnobs = 6; // the pad's level, pan, tune, attack and length, and the output
    std::array<std::unique_ptr<Knob>, numKnobs> knobs;
    std::array<std::unique_ptr<ParameterAttachment>, numKnobs> attachments;
    juce::ComboBox chokeBox;
    ChoiceAttachment chokeAttachment { chokeBox };
    juce::OwnedArray<juce::Label> names, texts; // the knobs' and the choke group's names, and the knobs' values

    int getGridSize (int height) const; // the pads' square, at a height
    void play (int pad);
    void chooseFile();
    void tick() override { refresh(); }
};
} // namespace phazite::ui
