// SPDX-License-Identifier: AGPL-3.0-or-later
// The menus that add devices (docs/design.md, section 8): an instrument, which turns a track's notes into sound,
// and effects, which change a sound. Built-in devices come first, then the plug-ins found by scanning, by maker.
// What is chosen goes in as one named undo step (model/mixer/MixerModel.h).
#pragma once

#include "model/base/IDs.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
// The instruments. The one chosen replaces the track's instrument, if it has one.
void showInstrumentMenu (ProjectContext&, TrackID, juce::Component& target);

// The effects. The one chosen goes on the track, or with an empty ID on the master, after its other effects.
void showEffectMenu (ProjectContext&, TrackID, juce::Component& target);

// The effects, for a new return track that the track sends to: the return holds the effect chosen and is named after it.
void showNewReturnMenu (ProjectContext&, TrackID sender, juce::Component& target);

// The effects, for a container (model/mixer/ContainerModel.h): the one chosen goes after its devices.
void showContainerEffectMenu (ProjectContext&, DeviceID container, juce::Component& target);
} // namespace phazite::ui
