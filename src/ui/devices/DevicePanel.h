// SPDX-License-Identifier: AGPL-3.0-or-later
// A device editor's controls in rows of titled groups, such as a synth's oscillators, filter and envelopes, built from
// the device's description: a knob for each parameter, with its name and value under it, a list for one that picks a
// choice, or a switch for one that is on or off. Within a group, a name that starts with the group's title drops it
// ("Envelope 2 Attack" is "Attack" under "Envelope 2"), and else one that starts with its first word drops that ("Filter
// Attack" is "Attack" under "Filter Envelope"); the control's title, which screen readers read, keeps the whole name.
// Given less height than it makes itself, as a Devices view may give it, its groups move side by side, in fewer rows.
#pragma once

#include "DeviceEditors.h"
#include "model/devices/DeviceHandle.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string_view>
#include <vector>

namespace phazite::ui
{
class DevicePanel : public juce::Component,
                    public FlexibleHeight,
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

    // The control for a parameter: a Knob, a juce::ComboBox for a choice, or a juce::ToggleButton for a switch; nullptr if
    // the panel has none.
    juce::Component* findControl (std::string_view parameterID) const;

    // Shows changes made elsewhere (automation, undo); a timer does it 10 times a second.
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

    int getMinimumHeight() const override; // a row of groups
    int getWidthForHeight (int height) const override;

private:
    struct Control
    {
        std::string_view id;
        std::unique_ptr<Knob> knob;
        std::unique_ptr<juce::ComboBox> box;
        std::unique_ptr<juce::ToggleButton> toggle; // for a parameter that is on or off
        std::unique_ptr<ParameterAttachment> knobAttachment;
        std::unique_ptr<ChoiceAttachment> boxAttachment;
        std::unique_ptr<ToggleAttachment> toggleAttachment;
        std::unique_ptr<juce::Label> name, value;
    };

    struct PlacedGroup
    {
        std::unique_ptr<juce::Label> title;
        int first = 0, count = 0; // its controls, in order
        int width = 0;            // theirs together
    };

    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::vector<PlacedGroup>> groups; // by row, as given

    // The groups in as many rows as fit a height, no more than given: in order, each row about as wide as the others.
    std::vector<std::vector<const PlacedGroup*>> flow (int height) const;
    static int getRowWidth (const std::vector<const PlacedGroup*>&);

    void timerCallback() override
    {
        if (isShowing()) // nothing to show behind another tab of the Devices view
            refresh();
    }
};
} // namespace phazite::ui
