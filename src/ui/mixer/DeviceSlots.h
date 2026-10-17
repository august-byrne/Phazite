// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's devices of one kind, as the mixer's strips show them (docs/design.md, section 8): its instrument, or its
// effects, each in a slot; or a container's devices, in a row, as its window shows them (ContainerEditor.h).
#pragma once

#include "model/mixer/MixerModel.h"
#include "ui/components/Controls.h"

#include <vector>

namespace phazite::ui
{
class CommandBridge;

// A channel's devices of one kind, each as a slot: its name (click to open its window, Ctrl- or Shift-click to select
// it, right-click for more), an on/off switch and its key controls, and a button that adds one. A track has an
// instrument, which turns its notes into sound, and effects, which change the sound; folders, returns and the master
// have effects only. A container's slot shows its first macros, and a container's devices show in a row.
class DeviceSlots : public juce::Component,
                    private juce::ChangeListener
{
public:
    enum class Kind
    {
        instrument, // a track's instrument, or a button that chooses one
        effects,
        containerDevices // a container's, in a row
    };

    // Menus show the commands for the selected devices (grouping them into a container, saving a preset).
    DeviceSlots (Kind, ProjectContext&, CommandBridge&);
    ~DeviceSlots() override;

    // The track whose devices to show (an empty ID for none), the master's, or a container's. Each shows them anew, so
    // call it again for one of another project, even one with the same ID.
    void setTrack (TrackID);
    void setMaster();
    void setContainer (DeviceID);

    // Shows the devices as they are now: new slots if they changed. Call it when the song changes.
    void refresh();

    // Shows whether a track without an instrument has notes that need one. Call it when clips change.
    void refreshNeed();

    // Shows the knobs' values. Call it from a timer.
    void updateValues();

    int getPreferredHeight() const;
    int getPreferredWidth() const; // a container's devices', in a row
    void resized() override;
    void lookAndFeelChanged() override; // the add button's color

    static constexpr int rowSlotWidth = 150; // a container's device's slot

private:
    struct Slot;

    const Kind kind;
    ProjectContext& context;
    CommandBridge& commands;
    TrackID track;
    DeviceID container;
    bool master = false;
    juce::OwnedArray<Slot> slots;
    juce::TextButton addButton;
    std::vector<DeviceID> shownDevices; // the devices the slots show
    int shownNeed = -1;                 // whether the add button asks for an instrument, as it shows now

    std::vector<DeviceSlot> getChannelDevices() const;
    TrackID getEffectsChannel() const noexcept { return master ? TrackID() : track; } // the master's, by an empty ID
    void rebuild (const std::vector<DeviceSlot>&);
    void updateAddButton();
    void showAddMenu();
    void showSlotMenu (const DeviceSlot&);
    void select (const DeviceSlot&, const juce::ModifierKeys&);      // a click with Ctrl or Shift: in or out of the selection
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
};

} // namespace phazite::ui
