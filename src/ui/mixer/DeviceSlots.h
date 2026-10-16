// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's devices of one kind, as the mixer's strips show them (docs/design.md, section 8): its instrument, or its
// effects, each in a slot.
#pragma once

#include "model/mixer/MixerModel.h"
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

    DeviceSlots (Kind, ProjectContext&);
    ~DeviceSlots() override;

    // The track whose devices to show (an empty ID for none), or the master's. Either shows them anew, so call it again
    // for a channel of another project, even one with the same ID.
    void setTrack (TrackID);
    void setMaster();

    // Shows the channel's devices as they are now: new slots if they changed. Call it when the song changes.
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
};

} // namespace phazite::ui
