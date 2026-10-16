// SPDX-License-Identifier: AGPL-3.0-or-later
// Song outlines' Tracktion side (model/arrangement/Outlines.h), for the model, the app and tests: applying one to a song,
// and the commands that do. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class CommandRegistry;
struct Outline;

// Replaces the song's sections with the outline's, from the start of the song, in bars of the song's time
// signature (so they last as long as the tempo makes them). What is on the tracks stays where it is.
void applyOutline (te::Edit&, const Outline&);

// Applies the built-in outline with that id (findOutline()). Returns false, and changes nothing, if there is none.
bool applyOutline (te::Edit&, const juce::String& outlineID);

// The id of the outline applied last, or an empty string.
juce::String getAppliedOutline (const te::Edit&);

// Whether the outline with that id was applied last and the song still has sections.
bool isOutlineApplied (const te::Edit&, const juce::String& outlineID);

// The outline commands: one for each built-in outline, so the palette finds them by name, each one undo step and
// ticked while its outline is applied (isOutlineApplied()). The app registers them right after the section commands.
void addOutlineCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
