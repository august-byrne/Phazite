// SPDX-License-Identifier: AGPL-3.0-or-later
// The menus that add devices (docs/design.md, section 8): an instrument, which turns a track's notes into sound,
// and effects, which change a sound. Built-in devices come first, then the plug-ins found by scanning, by maker.
// What is chosen goes in as one named undo step.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

namespace phazite::ui
{
namespace te = tracktion;

// The instruments. The one chosen replaces the track's instrument, if it has one.
void showInstrumentMenu (te::AudioTrack&, juce::Component& target);

// The effects. The one chosen goes on the track, or with nullptr on the Edit's master, after its other effects.
void showEffectMenu (te::Edit&, te::Track*, juce::Component& target);

// The effects, for a new return track that the track sends to: the return holds the effect chosen and is named after it.
void showNewReturnMenu (te::Track& sender, juce::Component& target);
} // namespace phazite::ui
