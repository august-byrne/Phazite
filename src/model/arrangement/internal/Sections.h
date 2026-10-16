// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections (Intro, Verse, Drop...): named, colored parts of the song that span every track
// (docs/design.md, section 6). They are Tracktion's arranger clips, which are only labels; Phazite adds a
// role, a tip and an energy level to each, and the edits that take what is in a section along with it
// (Arrangement.h): move, copy, delete and resize.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <optional>

namespace phazite
{
class CommandRegistry;

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
bool hasSelectedSections (const te::SelectionManager&);

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

// A new section is this long, and named "Section" and its number, after the sections there are.
inline constexpr int barsPerNewSection = 8;
juce::String getNewSectionName (const te::Edit&);

// Adds a new section after the last one, or at the start of the song if there is none, and selects it alone if a
// selection manager is given. Returns nullptr if the Edit can't hold more clips.
te::ArrangerClip* appendSection (te::Edit&, te::SelectionManager*);

// Adds a new section where the user asks for one, as one undo step: from the start of the bar the beat is in (or the
// end of the section there, if that ends later) for barsPerNewSection bars, or up to the next section, since sections
// never overlap. Selects it alone if a selection manager is given. Returns nullptr if there is no room.
te::ArrangerClip* addSectionAt (te::Edit&, te::BeatPosition, te::SelectionManager*);

// Renames a section, as one undo step; an empty name or the same one changes nothing.
void renameSection (te::ArrangerClip&, const juce::String& name);

// Gives a section another color, as one undo step.
void setSectionColour (te::ArrangerClip&, juce::Colour);

// The time from the earliest section's start to the latest one's end.
te::TimeRange getSectionsTimeRange (const juce::Array<te::ArrangerClip*>&);

//==============================================================================
// Edits of a section and everything in it, on every track. The destinations are section boundaries
// (getSectionBoundaries()): positions in the song as it is before the edit.

// Moves the section there, as one undo step; the sections between make room.
void moveSection (te::ArrangerClip&, te::BeatPosition destination);

// Copies the section there, after making room, as one undo step; with asAliases, its clips are copied as aliases.
te::ArrangerClip* copySection (te::ArrangerClip&, te::BeatPosition destination, bool asAliases);

// Copies the section right after itself.
te::ArrangerClip* duplicateSection (te::ArrangerClip&, bool asAliases);

// Deletes the section and what is in it; what comes after moves back.
void deleteSection (te::ArrangerClip&);

// The same for several sections, the last first, so that deleting one doesn't move another still to be deleted.
// Deleting one can delete another inside it.
void deleteSections (const juce::Array<te::ArrangerClip*>&);

// Copies each section right after itself, the last first, and returns the copies. With a selection manager, the
// copies are selected instead of what was.
juce::Array<te::ArrangerClip*> duplicateSections (const juce::Array<te::ArrangerClip*>&, bool asAliases, te::SelectionManager*);

// Makes the section longer or shorter at its end, at least a beat long, as one undo step. With moveRest, what comes after
// it moves along: time is inserted at its end, or removed from it with what lies there. Otherwise only the
// section changes, and it doesn't grow into the next one.
void resizeSection (te::ArrangerClip&, te::BeatDuration newLength, bool moveRest);

// Where a section can be moved or copied to: the start of each section and the end of the last.
juce::Array<te::BeatPosition> getSectionBoundaries (const te::Edit&);

// The section commands: add a section. Delete, duplicate and duplicate as aliases are the selection
// commands (Selection.h); the song outlines' are Outlines.h's; the sections strip adds rename and color.
void addSectionCommands (CommandRegistry&, te::SelectionManager&);
} // namespace phazite
