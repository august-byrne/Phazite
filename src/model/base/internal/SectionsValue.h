// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections as a value (roadmap step S5.20, the sections' area): each section, by its ID, in the song's order, in
// Phazite's own types, and the conversion between the value and the SECTIONS element with its SECTION children (Schema.h),
// the one place that reads and writes those by name. The song holds this value in an area of its values (SongValues.h,
// SectionsArea.h), which the sections' code (model/arrangement/internal/Sections.h) computes each edit on; its tree's
// SECTIONS, where it has one, is an empty placeholder (added with the first section), the place a whole copy writes the
// sections in.
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values). The song's reader is here,
// in the model's base, rather than with the sections' code (model/arrangement/internal/Sections.h), because the selection
// (Selection.h), which the base holds, asks whether a section is there, and the base includes no other area. immer is the
// model's alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <optional>

namespace phazite
{
class Song;

// A section as the song holds it (Schema.h: SECTION): its ID, where it starts and how long it lasts, in beats; its name, its
// role ("intro", "verse"...: empty for none) and its tip (empty for none); its energy (0 to 1); and its colour as written
// (juce::Colour::toString(): an empty one stays empty), none where the element has none (it shows its role's then:
// model/arrangement/internal/Sections.h; one written empty shows as juce::Colour::fromString() reads it, transparent black).
struct SectionValue
{
    SectionID id;
    BeatPosition start;
    BeatDuration length = BeatDuration::fromBeats (4.0);
    juce::String name, role, tip;
    float energy = 0.5f;
    std::optional<juce::String> colour;

    // Written out, not defaulted: a defaulted one would compare the energy with a floating-point ==, which GCC's -Wfloat-equal
    // refuses.
    bool operator== (const SectionValue&) const noexcept;
};

// The song's sections, in the element's order (the readers sort them by their starts, two at one start in that order).
// Copies are cheap and share what they hold.
struct SectionsValue
{
    immer::flex_vector<SectionValue> sections;

    // A section by its ID: the first with it, or none. A pointer into this value, valid while it lives: keep the value in a
    // local of its own, never a temporary's.
    const SectionValue* find (SectionID) const;

    // The same value without a look inside: its sections shared (ValueArea.h). And the same sections, compared.
    bool isSameAs (const SectionsValue& other) const noexcept { return sections.identity() == other.sections.identity(); }
    bool operator== (const SectionsValue& other) const { return sections == other.sections; }
};

// The song's sections: the area's value, cheap to copy; for a song that keeps everything in its tree (one made to be read or
// checked, without values: Song::getValues()), read from its SECTIONS element (readSectionsValue()). None for a song without.
SectionsValue getSectionsValue (const Song&);

// Whether the song holds a section with this ID (an invalid one: no), without a value made of its sections: what a path that
// asks it of each section, as a paint does (Selection.h: isSelected()), asks. A look in the area's value, which copies
// nothing; for a song that keeps everything in its tree, a walk of its SECTIONS element's SECTION children.
bool holdsSection (const Song&, SectionID);

// The song's sections read from its SECTIONS element (an invalid tree reads as none): each SECTION child in the element's
// order, each property as the sections' code has always read it (setSectionProperty()). A child of another type is no section,
// and a property Phazite doesn't know no part of one. What reads the sections of a song that keeps everything in its tree
// (getSectionsValue()), and the tests.
SectionsValue readSectionsValue (const juce::ValueTree& sectionsElement);

// The song's sections read from a SECTIONS as the song holds them (SongValues.h: the loaded sections Song::holdValues() takes
// in, the keeper's safety net, a version's sections and a merge's choice, SectionsArea.h): each SECTION child in the element's
// order (readHeldSection()), kept as the sections' writers keep them (keepSectionsHeld()). A child of another type, and a
// property Phazite doesn't know, are no part of it.
SectionsValue readHeldSections (const juce::ValueTree& sectionsElement);

// One section read so from its SECTION element: its ID as readID() reads one, text or a number; its start and its length
// each by its description (SongText.h: readDescribed(): within its range, and its fallback where it has none or what it has
// is no finite number, so that a length is more than 0); its energy within 0 to 1, its description's default (0.5) where it
// has none or no finite number; its name, role and tip as text; its colour as written (an empty one empty, none where it has
// none). None for an element that is no SECTION, and for one without an ID the song can count on from (0, or at or past
// schema::maxID), which no section holds.
std::optional<SectionValue> readHeldSection (const juce::ValueTree& sectionElement);

// The sections with one more, read from its SECTION element as readHeldSection() reads one, after the others (where an
// element appended to the song's SECTIONS is read), kept as keepSectionsHeld() keeps them; the sections as they are for any
// other element, and for one the song can't hold. The keeper's safety net.
SectionsValue withSectionTaken (const SectionsValue&, const juce::ValueTree& sectionElement);

// The sections as the song holds them: one of each ID (of two, the first in the value's order stays), and none without an ID
// the song can count on from. The one rule for what enters the song's sections (readHeldSections(), withSectionTaken()) and
// for the sections' writers (model/arrangement/Sections.cpp), which give each section an ID of its own already. The same
// value where it keeps them all. Sections may overlap, as the loader leaves them (the sections' code places a new one
// between the others, but a file, an outline or an import may hold two that overlap).
SectionsValue keepSectionsHeld (const SectionsValue&);

// Whether the sections are held as keepSectionsHeld() keeps them (it would change nothing): what the song's consistency check
// asks of its value.
bool holdsSectionsHeld (const SectionsValue&);

// How many sections were read into values (readSectionsValue()) since Phazite started, on any thread: for the test that
// asking whether each section is selected, as a paint does, reads none (tests/PerformanceGateTests.cpp). Allocations can't
// tell it: immer takes its nodes from lists of freed ones.
std::uint64_t countSectionsRead() noexcept;

// A section with a property given a value as the section would read it from an element that held it, or taken away (a void
// value: as if the element hadn't it): an ID, a start or a length absent reads as 0; a name, a role or a tip as empty; an
// energy as 0.5, and any kept within 0 to 1; a colour as it is written (one written empty stays empty, so that it isn't taken
// for none), absent as none. What a merge's clash on a section's property chosen sets (model/versions/VersionsModel.cpp),
// and what the conversion reads each property with. Nothing else is kept within a range here, which the loader does
// (model/project/internal/Project.h, makeConsistent()). Whether the property is one of a section's (else nothing changes).
bool setSectionProperty (SectionValue&, const juce::Identifier& property, const juce::var& value);

// A section as a SECTION element, as the sections' code writes one: its ID, start, length, name and energy always, its role
// and tip only where it has one, and its colour where it has one, as written (an empty one empty: the sections' code writes
// none empty). And the song's sections appended to a SECTIONS element (a copy of the song's, emptied, outside any song:
// WholeState.h), each so, in their order.
juce::ValueTree writeSectionElement (const SectionValue&);
void writeSections (const SectionsValue&, juce::ValueTree& sectionsCopy);
} // namespace phazite
