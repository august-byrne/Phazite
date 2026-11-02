// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's devices of one kind, as the mixer's strips show them (docs/design.md, section 8): its instrument, or its
// effects, each in a slot; and what a device's name does there and in the Devices view (DevicesView.h): a click shows
// the device, a right-click its menu, and a drag moves an effect among its neighbours.
#pragma once

#include "model/mixer/MixerModel.h"
#include "ui/components/Controls.h"

#include <functional>
#include <optional>
#include <vector>

namespace phazite::ui
{
class CommandBridge;

// A channel's devices of one kind, each a slot of one line: its name (click to show it in the Devices view, where its
// controls are, Ctrl- or Shift-click to select it, right-click for more, drag an effect's up or down to move it) and an
// on/off switch; and a button that adds one. A track has an instrument, which turns its notes into sound, and effects,
// which change the sound; folders, returns and the master have effects only.
class DeviceSlots : public juce::Component,
                    public juce::DragAndDropTarget,
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

    int getPreferredHeight() const;
    void resized() override;
    void lookAndFeelChanged() override;                // the add button's color
    void paintOverChildren (juce::Graphics&) override; // where a dragged effect would go

    // An effect of these slots, dragged by its name: dropped among them, it moves there.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

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
    int dropIndex = -1;                 // where a dragged effect would go: before that slot, or after the last; -1 for none

    std::vector<DeviceSlot> getChannelDevices() const;
    TrackID getEffectsChannel() const noexcept { return master ? TrackID() : track; } // the master's, by an empty ID
    void rebuild (const std::vector<DeviceSlot>&);
    void updateAddButton();
    void showAddMenu();
    void showSlotMenu (const DeviceSlot&);
    void select (const DeviceSlot&, const juce::ModifierKeys&);      // a click with Ctrl or Shift: in or out of the selection
    int getDropIndex (juce::Point<int>) const;                       // the slot a drop there goes before
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
};

// What an effect dragged by its name carries, from a mixer slot or a card in the Devices view, which a drop among its
// neighbours moves it to; and how many places a drop moves it, from where it is in an order to before the device at an
// insertion index (the order's size: after the last), 0 for none.
juce::var describeEffectDrag (DeviceID);
std::optional<DeviceID> getDraggedEffect (const juce::var& description);
int getPlacesMoved (const std::vector<DeviceID>& order, DeviceID, int insertionIndex);

// Where a device is, for its menu.
enum class DevicePlace
{
    instrument, // a track's
    effect,     // a channel's
    inContainer // one of a container's devices
};

// A device's menu, as its slot and its panel in the Devices view show it, for the selected devices (this one alone,
// unless it is one of them): Reload for a plug-in that crashed, a plug-in's window, On, Automate, moving it among its
// neighbours (it is index of count; left and right where they show in a row, up and down in a strip), grouping effects
// into a container or ungrouping one, a preset, and Remove; for an instrument, Replace instead of the moves. changed()
// runs after an edit, so that what shows the device shows it at once.
void showDeviceMenu (ProjectContext&, CommandBridge&, const DeviceSlot&, DevicePlace, TrackID track, int index, int count, bool inRow,
                     juce::Component& target, std::function<void()> changed);

// A plug-in that crashed (DeviceSlot::crashed) reloaded, as its slot's click and its menu's Reload do; one that didn't
// load says why, over the target.
void reloadDevice (ProjectContext&, DeviceID, juce::Component& target);
} // namespace phazite::ui
