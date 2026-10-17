// SPDX-License-Identifier: AGPL-3.0-or-later
// A modulator's editor (docs/design.md, section 10; roadmap step 2.3), which its slot in a channel strip opens
// (ui/mixer/ModulatorSlots.h): its output over the last seconds, a step sequencer's steps, drawn with the mouse, and a
// knob for each of its parameters, by name, showing its value; a macro's is its one knob. It works through the model's
// ModulationModel.h and ParameterHandles, so the model owns every undo step.
#pragma once

#include "model/modulation/ModulationModel.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace phazite::ui
{
class ModulatorEditor : public juce::Component,
                        private juce::Timer
{
public:
    ModulatorEditor (ProjectContext&, ModulatorID);
    ~ModulatorEditor() override;

    ModulatorID getModulator() const noexcept { return modulator; }

    // A parameter's knob, by its ID; nullptr if the editor has none.
    Knob* findKnob (const juce::String& parameterID) const;

    void paint (juce::Graphics&) override;
    void resized() override;

    struct Scope;
    struct Steps;

private:
    static constexpr int columns = 4;

    ProjectContext& context;
    const ModulatorID modulator;
    juce::Label title;
    std::unique_ptr<Scope> scope;
    std::unique_ptr<Steps> steps; // a step sequencer's
    std::vector<juce::String> parameterIDs;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    juce::OwnedArray<juce::Label> names;

    void refresh();                // the output, the steps and the knobs, as they are now
    void timerCallback() override; // refresh(), while it shows
};
} // namespace phazite::ui
