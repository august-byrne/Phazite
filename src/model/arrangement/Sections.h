// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections (Intro, Verse, Drop...): named, colored parts of the song that span every track
// (docs/design.md, section 6). They are Tracktion's arranger clips, which are only labels; Phazite adds a
// role, a tip and an energy level to each, and the edits that take what is in a section along with it
// (Arrangement.h): move, copy, delete and resize.
#pragma once

#include "model/commands/CommandRegistry.h"

#include <optional>

namespace phazite
{
// What a new section is.
struct SectionInfo
{
    juce::String name {};
    te::BeatDuration length = te::BeatDuration::fromBeats (32.0);
    juce::String role {};                  // "intro", "verse", "drop"...: what the section does in the song
    juce::String tip {};                   // a one-line suggestion of what to put in it
    float energy = 0.5f;                   // how intense it is, from 0 to 1, for the energy curve
    std::optional<juce::Colour> colour {}; // the role's color if not given
};

// The sections, in song order.
juce::Array<te::ArrangerClip*> getSections (const te::Edit&);
te::ArrangerClip* getSectionAt (const te::Edit&, te::BeatPosition);
juce::Array<te::ArrangerClip*> getSelectedSections (const te::SelectionManager&);

juce::String getSectionRole (const te::Clip&);
juce::String getSectionTip (const te::Clip&);
float getSectionEnergy (const te::Clip&);

// A section's color: its own, if it was given one, or else its role's.
juce::Colour getSectionColour (const te::Clip&);

// The color sections with a role get: intros and outros grey, verses blue, build-ups yellow, choruses,
// hooks and drops vermilion, bridges and breakdowns purple and solos green (Okabe and Ito's palette, as
// tracks use). Other roles get sky blue.
juce::Colour getRoleColour (const juce::String& role);

// Adds a section, without moving anything else. Returns nullptr if the Edit can't hold more clips.
te::ArrangerClip* insertSection (te::Edit&, te::BeatPosition start, const SectionInfo&);

//==============================================================================
// Edits of a section and everything in it, on every track. The destinations are section boundaries
// (getSectionBoundaries()): positions in the song as it is before the edit.

// Moves the section there; the sections between make room.
void moveSection (te::ArrangerClip&, te::BeatPosition destination);

// Copies the section there, after making room; with asAliases, its clips are copied as aliases.
te::ArrangerClip* copySection (te::ArrangerClip&, te::BeatPosition destination, bool asAliases);

// Copies the section right after itself.
te::ArrangerClip* duplicateSection (te::ArrangerClip&, bool asAliases);

// Deletes the section and what is in it; what comes after moves back.
void deleteSection (te::ArrangerClip&);

// Makes the section longer or shorter at its end, at least a beat long. With moveRest, what comes after
// it moves along: time is inserted at its end, or removed from it with what lies there. Otherwise only the
// section changes, and it doesn't grow into the next one.
void resizeSection (te::ArrangerClip&, te::BeatDuration newLength, bool moveRest);

// Where a section can be moved or copied to: the start of each section and the end of the last.
juce::Array<te::BeatPosition> getSectionBoundaries (const te::Edit&);

// The section commands: add a section, and apply each built-in song outline (Outlines.h). Delete,
// duplicate and duplicate as aliases are the selection commands (Selection.h); the sections strip adds
// rename and color.
void addSectionCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
