// SPDX-License-Identifier: AGPL-3.0-or-later
// The menus that add devices (docs/design.md, section 8): an instrument, which turns a track's notes into sound,
// and effects, which change a sound. Built-in devices come first (each after a container), then the plug-ins found by
// scanning, by maker.
// What is chosen goes in as one named undo step (model/mixer/MixerModel.h, ContainerModel.h).
#pragma once

#include "model/base/IDs.h"
#include "model/mixer/MixerModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class CommandBridge;

// Each menu's items act themselves while target is there, so that a menu works as well as a submenu of another (a device's
// Replace); show...() drops it from target, a button.

// The instruments. The one chosen replaces the track's instrument, if it has one; an instrument container comes first,
// into which the instrument the track has goes (addInstrumentContainer()). makeInstrumentMenu() is the menu it shows, as
// makeEffectMenu() is the effects'.
void showInstrumentMenu (ProjectContext&, TrackID, juce::Component& target);
juce::PopupMenu makeInstrumentMenu (ProjectContext&, TrackID, juce::Component& target);

// The effects. The one chosen goes on the track, or with an empty ID on the master, after its other effects; an empty
// container comes first (addContainer()). For a track whose notes have no instrument, the instruments come first.
void showEffectMenu (ProjectContext&, TrackID, juce::Component& target);
juce::PopupMenu makeEffectMenu (ProjectContext&, TrackID, juce::Component& target);

// The effects, for one to replace: the one chosen takes its place (replaceEffect()).
juce::PopupMenu makeReplaceEffectMenu (ProjectContext&, DeviceID effect, juce::Component& target);

// The effects, for a new return track that the track sends to: the return holds the effect chosen and is named after it.
void showNewReturnMenu (ProjectContext&, TrackID sender, juce::Component& target);

// For a container's chain (model/mixer/ContainerModel.h): the effects, the one chosen going after its devices; and an
// instrument container's chain's instruments, the one chosen in place of its instrument.
void showChainEffectMenu (ProjectContext&, DeviceID chain, juce::Component& target);
void showChainInstrumentMenu (ProjectContext&, DeviceID chain, juce::Component& target);
juce::PopupMenu makeChainInstrumentMenu (ProjectContext&, DeviceID chain, juce::Component& target);

// What a click on a device's name does, in the mixer, the piano roll and the Devices view: selects the device alone and
// shows it in the Devices view (the "device.show" command, where the window has one), and opens a plug-in's own window,
// or brings forward the window one of Phazite's own devices is in.
void showDevice (ProjectContext&, CommandBridge&, const DeviceSlot&);
} // namespace phazite::ui
