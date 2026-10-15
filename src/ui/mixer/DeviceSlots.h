// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's devices of one kind, as the mixer's strips show them (docs/design.md, section 8): its instrument, or its
// effects, each in a slot.
#pragma once

#include "ui/components/Controls.h"

#include <vector>

namespace phazite::ui
{
// A channel's devices of one kind, each as a slot: its name (click to open its window, right-click for more), an
// on/off switch and its key controls, and a button that adds one. A track has an instrument, which turns its
// notes into sound, and effects, which change the sound; folders, returns and the master have effects only.
class DeviceSlots : public juce::Component
{
public:
    enum class Kind
    {
        instrument, // a track's instrument, or a button that chooses one
        effects
    };

    explicit DeviceSlots (Kind);
    ~DeviceSlots() override;

    // The track whose devices to show, or with nullptr the master's of the Edit.
    void setChannel (te::Track*, te::Edit*);

    // Rebuilds the slots if the devices changed, and shows the controls' values. Call it from a timer.
    void update();

    int getPreferredHeight() const;
    void resized() override;
    void lookAndFeelChanged() override; // the add button's color

private:
    struct Slot;

    const Kind kind;
    te::SafeSelectable<te::Track> track;
    te::SafeSelectable<te::Edit> edit;
    juce::OwnedArray<Slot> slots;
    juce::TextButton addButton;
    std::vector<te::EditItemID> shownDevices; // the devices the slots show
    int shownNeed = -1;                       // whether the add button asks for an instrument, as it shows now

    juce::Array<te::Plugin*> getChannelDevices() const;
    void rebuild();
    void updateAddButton();
    void showAddMenu();
    void showSlotMenu (te::Plugin&);
};

} // namespace phazite::ui
