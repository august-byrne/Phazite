// SPDX-License-Identifier: AGPL-3.0-or-later
// Project format versions and the upgrades from older ones: the only code that reads an old format.
//   Version 0: before versions existed (saved up to 2026-10-14): a fixed grid's size as Tracktion's snap level, and key
//              scales as Tracktion's scale numbers.
//   Version 1: gridStep, scales by name, and schemaVersion in the Phazite state.
//   Versions 2 to 4: Phazite's own devices, notes' IDs, automation lanes (Schema.h).
//   Version 5: no automation clips. Up to version 4, a MIDI clip could hold a curve that set a parameter while it played;
//              now that curve is the parameter's own automation. Converting it needs the song's tempo, so it is the one
//              upgrade made on the Edit rather than on the saved state: createEditFromState() makes it for a state saved
//              before version 5 (model/engine/internal/EngineSetup.h, model/automation/internal/Automation.h:
//              convertAutomationClips()).
//   Version 6: lanes that collapse.
//   Version 7: the chord lane and parts made from it.
//   Version 8: the devices of step 2.10 and the synth's unison.
// A change to what projects save raises schema::currentVersion (Schema.h) and adds a step here.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
// The format version of a saved Edit state: its Phazite state's schemaVersion, or 0 without one (or with one that
// isn't a version: not a number, or negative).
int getSchemaVersion (const juce::ValueTree& editState);

// Records the current version in the Phazite state, which it adds if there is none, without an undo step. It writes
// only if the version differs, and leaves an invalid tree alone.
void writeCurrentSchemaVersion (juce::ValueTree& editState);

// Upgrades a saved state to the current format before a te::Edit is made from it, and records the version. Upgrading
// again changes nothing, and an invalid tree is left alone (true). A state from a newer Phazite is left unchanged, and
// this returns false: this version can't read it.
[[nodiscard]] bool migrateToCurrentSchema (juce::ValueTree& editState);
} // namespace phazite
