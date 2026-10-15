// SPDX-License-Identifier: AGPL-3.0-or-later
// A device editor's controls in rows of titled groups, such as a synth's oscillators, filter and envelopes, built from
// the device's description: a knob for each parameter, with its name and value under it, or a list for one that picks
// a choice. Within a group, a name that starts with the group's first word drops it ("Filter Attack" is "Attack" under
// "Filter Envelope"); the control's title, which screen readers read, keeps the whole name.
#pragma once

#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string_view>
#include <vector>

namespace phazite::ui
{
class DevicePanel : public juce::Component,
                    private juce::Timer
{
public:
    struct Group
    {
        juce::String title;
        std::vector<std::string_view> parameterIDs;
    };

    // Each row's groups, side by side.
    DevicePanel (const DeviceHandle&, const std::vector<std::vector<Group>>& rows);
    ~DevicePanel() override;

    // The control for a parameter: a Knob, or a juce::ComboBox for a choice; nullptr if the panel has none.
    juce::Component* findControl (std::string_view parameterID) const;

    // Shows changes made elsewhere (automation, undo); a timer does it 10 times a second.
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Control
    {
        std::string_view id;
        std::unique_ptr<Knob> knob;
        std::unique_ptr<juce::ComboBox> box;
        std::unique_ptr<ParameterAttachment> knobAttachment;
        std::unique_ptr<ChoiceAttachment> boxAttachment;
        std::unique_ptr<juce::Label> name, value;
    };

    struct PlacedGroup
    {
        std::unique_ptr<juce::Label> title;
        int first = 0, count = 0; // its controls, in order
        int width = 0;            // theirs together
    };

    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::vector<PlacedGroup>> groups; // by row

    void timerCallback() override { refresh(); }
};
} // namespace phazite::ui
