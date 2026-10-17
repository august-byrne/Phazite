// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's devices of one kind, as the mixer's strips show them (docs/design.md, section 8): its instrument, or its
// effects, each in a slot; and what a device's name does there and in the Devices view (DevicesView.h): a click shows
// the device, a right-click its menu.
#pragma once

#include "model/mixer/MixerModel.h"
#include "ui/components/Controls.h"

#include <functional>
#include <vector>

namespace phazite::ui
{
class CommandBridge;

// A channel's devices of one kind, each as a slot: its name (click to show it in the Devices view, Ctrl- or Shift-click
// to select it, right-click for more), an on/off switch and its key controls, and a button that adds one. A track has
// an instrument, which turns its notes into sound, and effects, which change the sound; folders, returns and the master
// have effects only. A container's slot shows its first macros.
class DeviceSlots : public juce::Component,
                    private juce::ChangeListener
{
public:
    enum class Kind
    {
        instrument, // a track's instrument, or a button that chooses one
        effects
    };

    // Menus show the commands for the selected devices (grouping them into a container, saving a preset).
    DeviceSlots (Kind, ProjectContext&, CommandBridge&);
    ~DeviceSlots() override;

    // The track whose devices to show (an empty ID for none), or the master's. Each shows them anew, so call it again
    // for one of another project, even one with the same ID.
    void setTrack (TrackID);
    void setMaster();

    // Shows the devices as they are now: new slots if they changed. Call it when the song changes.
    void refresh();

    // Shows whether a track without an instrument has notes that need one. Call it when clips change.
    void refreshNeed();

    // Shows the knobs' values. Call it from a timer.
    void updateValues();

    int getPreferredHeight() const;
    void resized() override;
    void lookAndFeelChanged() override; // the add button's color

private:
    struct Slot;

    const Kind kind;
    ProjectContext& context;
    CommandBridge& commands;
    TrackID track;
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

// Where a device is, for its menu.
enum class DevicePlace
{
    instrument, // a track's
    effect,     // a channel's
    inContainer // one of a container's devices
};

// A device's menu, as its slot and its panel in the Devices view show it, for the selected devices (this one alone,
// unless it is one of them): a plug-in's window, On, Automate, moving it among its neighbours (it is index of count; left
// and right where they show in a row, up and down in a strip), grouping effects into a container or ungrouping one, a
// preset, and Remove; for an instrument, Replace instead of the moves. changed() runs after an edit, so that what shows
// the device shows it at once.
void showDeviceMenu (ProjectContext&, CommandBridge&, const DeviceSlot&, DevicePlace, TrackID track, int index, int count, bool inRow,
                     juce::Component& target, std::function<void()> changed);
} // namespace phazite::ui
