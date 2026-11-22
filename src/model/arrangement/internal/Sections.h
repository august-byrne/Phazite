// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections (Intro, Verse, Drop...): named, colored parts of the song that span every track
// (docs/design.md, section 6). Phazite keeps them in the project itself (schema::SECTIONS), by their IDs, as it keeps
// the chord lane: each with a role, a tip and an energy level, and the edits that take what is in a section along with
// it (Arrangement.h): move, copy, delete and resize. Sections never overlap. The song holds its sections as a value in an
// area of its values (model/base/internal/SongValues.h, SectionsArea.h): every edit below reads the value, works the next
// one out and sets it once (SongValues::setSections()), in the step in progress, or outside the history while the song is
// written outside it; the song's SECTIONS element, the place a whole copy writes them in, is added with the first section, in
// that step, and never taken away by an edit.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/Selection.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

// What a new section is.
struct SectionInfo
{
    juce::String name {};
    BeatDuration length = BeatDuration::fromBeats (32.0);
    juce::String role {};                  // "intro", "verse", "drop"...: what the section does in the song
    juce::String tip {};                   // a one-line suggestion of what to put in it
    float energy = 0.5f;                   // how intense it is, from 0 to 1, as the inspector shows it
    std::optional<juce::Colour> colour {}; // the role's color if not given
};

// A section as it is.
struct Section
{
    SectionID id {};
    BeatRange range {};
    juce::String name {}, role {}, tip {};
    float energy = 0.5f;
    juce::Colour colour {}; // its own, if it was given one, or else its role's
};

// The sections, in song order.
std::vector<Section> getSections (const Song&);

// How many lists of sections getSections() built from the sections' value (each a list of every section, sorted: the
// selected sections' and the boundaries' too, which it builds) since Phazite started, on any thread: for the performance
// gate that asking whether a section is selected builds none, and that an edit and a paint build one for each paint that
// draws them (tests/PerformanceGateTests.cpp). SectionsValue.h's countSectionsRead() counts the tree's reader.
std::uint64_t countSectionListsBuilt() noexcept;
std::optional<Section> getSection (const Song&, SectionID);
std::optional<Section> getSectionAt (const Song&, BeatPosition);

// Where the last section ends, or the song's start if there are none.
BeatPosition getSectionsEnd (const Song&);

// The selected sections, in song order.
std::vector<Section> getSelectedSections (const Selection&);
bool hasSelectedSections (const Selection&);

std::vector<SectionID> getIDs (const std::vector<Section>&);

// The color sections with a role get: intros and outros grey, verses blue, build-ups yellow, choruses,
// hooks and drops vermilion, bridges and breakdowns purple and solos green (Okabe and Ito's palette, as
// tracks use). Other roles get sky blue.
juce::Colour getRoleColour (const juce::String& role);

// Adds a section, without moving anything else, and returns its ID.
SectionID insertSection (Song&, BeatPosition start, const SectionInfo&);

// Removes every section, without moving anything: a song outline puts its own in their place.
void removeSections (Song&);

// A new section is this long, and named "Section" and its number, after the sections there are.
inline constexpr int barsPerNewSection = 8;
juce::String getNewSectionName (const Song&);

// Adds a new section after the last one, or at the start of the song if there is none, and selects it alone if a
// selection is given.
SectionID appendSection (Song&, Selection*);

// Adds a new section where the user asks for one, as one undo step: from the start of the bar the beat is in (or the
// end of the section there, if that ends later) for barsPerNewSection bars, or up to the next section, since sections
// never overlap. Selects it alone if a selection is given. Returns no ID if there is no room.
SectionID addSectionAt (Song&, BeatPosition, Selection*);

// Renames a section, as one undo step; an empty name or the same one changes nothing.
void renameSection (Song&, SectionID, const juce::String& name);

// Gives a section another color, as one undo step.
void setSectionColour (Song&, SectionID, juce::Colour);

// From the earliest section's start to the latest one's end: an empty range for none.
BeatRange getSectionsRange (const std::vector<Section>&);

//==============================================================================
// Edits of a section and everything in it, on every track. The destinations are section boundaries
// (getSectionBoundaries()): positions in the song as it is before the edit.

// Moves the section there, as one undo step; the sections between make room.
void moveSection (Song&, SectionID, BeatPosition destination);

// Copies the section there, after making room, as one undo step; with asAliases, its clips are copied as aliases.
// Returns the copy's ID.
SectionID copySection (Song&, SectionID, BeatPosition destination, bool asAliases);

// Copies the section right after itself.
SectionID duplicateSection (Song&, SectionID, bool asAliases);

// Deletes the section and what is in it; what comes after moves back.
void deleteSection (Song&, SectionID);

// The same for several sections, the last first, so that deleting one doesn't move another still to be deleted.
// Deleting one can delete another inside it.
void deleteSections (Song&, const std::vector<SectionID>&);

// Copies each section right after itself, the last first, and returns the copies. With a selection, the copies are
// selected instead of what was.
std::vector<SectionID> duplicateSections (Song&, const std::vector<SectionID>&, bool asAliases, Selection*);

// Makes the section longer or shorter at its end, at least a beat long, as one undo step. With moveRest, what comes after
// it moves along: time is inserted at its end, or removed from it with what lies there. Otherwise only the
// section changes, and it doesn't grow into the next one.
void resizeSection (Song&, SectionID, BeatDuration newLength, bool moveRest);

// Where a section can be moved or copied to: the start of each section and the end of the last.
std::vector<BeatPosition> getSectionBoundaries (const Song&);

//==============================================================================
// The sections' part of the time edits (Arrangement.h), which make them longer, shorter, move or copy them as they
// change the time they are in; the caller's change.

// Sections from the time on move later by its length; one the time is inside of gets longer.
void insertSectionTime (Song&, BeatPosition at, BeatDuration);

// Sections inside the range go; those that reach into it get shorter; those after it move back.
void removeSectionTime (Song&, BeatRange);

// The sections that start in the range go to the destination, and those that start between it and the destination
// move the other way, by the range's length, as the time does.
void moveSectionTime (Song&, BeatRange, BeatPosition destination);

// Copies of the sections wholly inside a range, kept to be put in elsewhere, by how far into the range each starts: values,
// as the song holds them, in song order.
struct SectionsCopy
{
    BeatPosition rangeStart {};
    std::vector<SectionValue> sections {};
};

SectionsCopy copySections (const Song&, BeatRange);
void pasteSections (Song&, const SectionsCopy&, BeatPosition destination); // with IDs of their own

// The song's sections made the ones an element holds (a SECTIONS, read as the song holds them: SectionsValue.h,
// readHeldSections()), or none for an invalid tree: its SECTIONS in the song's tree added (the element first) or taken out
// (the value first) where it has one and the other none, and its value set, in the mode of the moment (the step in progress,
// or outside the history while the song is written outside it). What gives the song a version's sections
// (model/versions/ApplySong.cpp). Whether it wrote anything.
bool makeSectionsAs (Song&, const juce::ValueTree& sectionsOrNone);

// The section commands: add a section. Delete, duplicate and duplicate as aliases are the selection
// commands (Selection.h); the song outlines' are Outlines.h's; the sections strip adds rename and color.
void addSectionCommands (CommandRegistry&, Selection&);
} // namespace phazite
