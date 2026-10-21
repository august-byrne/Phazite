// SPDX-License-Identifier: AGPL-3.0-or-later
// Choosing what a device's sidechain listens to (docs/design.md, section 8), in one place: a button in the device's header
// in the Devices view, showing the track it listens to (a dot in that track's colour, and its name) or "Sidechain" while
// it listens to the sound passing through it; a click lists Off and every other track in the song's order, each with
// its colour, those that would make a loop greyed with why. Choosing one is one undo step, in which Phazite's compressor,
// still as it was made, starts ducking. And the track menu's Duck Under, which adds a compressor that does it in one step.
#pragma once

#include "model/base/IDs.h"
#include "model/devices/DeviceHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
// The menus, built when they open: a device's sidechain (Off, then the tracks), and the tracks a track can duck under.
// Choosing an item makes the change; `changed` is called after it.
juce::PopupMenu makeSidechainMenu (ProjectContext&, DeviceID, const std::function<void()>& changed = {});
juce::PopupMenu makeDuckUnderMenu (ProjectContext&, TrackID ducked);

// The header's button.
class SidechainButton : public juce::Button
{
public:
    SidechainButton (ProjectContext&, DeviceID);

    // What it shows: the track the sidechain listens to, or none.
    void show (const SidechainChoice& source);

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;
    void clicked() override;

    // Called after the menu changed the sidechain.
    std::function<void()> onChange;

private:
    ProjectContext& context;
    const DeviceID device;
    SidechainChoice source;
};
} // namespace phazite::ui
