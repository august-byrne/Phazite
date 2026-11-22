// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's sections as a value (roadmap step S5.20, the sections' area): each section, by its ID, in the song's order, in
// Phazite's own types, and the conversion between the value and the SECTIONS element with its SECTION children (Schema.h),
// the one place that reads and writes those by name. The song doesn't hold this value yet: its sections are still its tree's
// elements, which getSectionsValue() below reads into it, so that what reads the song's sections reads them as a value,
// whatever stores them.
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

// The song's sections (the song's tree holds them still: this reads them from its SECTIONS element at each call, by value;
// the sections' switch, roadmap step S5.20, makes this the area's value, which it then gives without a read).
SectionsValue getSectionsValue (const Song&);

// Whether the song holds a section with this ID (an invalid one: no), without a value made of its sections: what a path that
// asks it of each section, as a paint does (Selection.h: isSelected()), asks. On tree storage it walks the SECTIONS element's
// SECTION children comparing their IDs; the sections' switch, roadmap step S5.20, makes it a look in the area's value.
bool holdsSection (const Song&, SectionID);

// The song's sections read from its SECTIONS element (an invalid tree reads as none): each SECTION child in the element's
// order, each property as the sections' code has always read it (setSectionProperty()). A child of another type is no section,
// and a property Phazite doesn't know no part of one.
SectionsValue readSectionsValue (const juce::ValueTree& sectionsElement);

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

// The song's sections appended to a SECTIONS element (a copy of the song's, emptied, outside any song: WholeState.h), each a
// SECTION as the sections' code writes one: its ID, start, length, name and energy always, its role and tip only where it has
// one, and its colour where it has one, as written (an empty one empty: the sections' code writes none empty).
void writeSections (const SectionsValue&, juce::ValueTree& sectionsCopy);
} // namespace phazite
