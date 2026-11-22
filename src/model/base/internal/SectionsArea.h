// SPDX-License-Identifier: AGPL-3.0-or-later
// The sections' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are SectionsValue.h's. It holds IDs (each section's: IDsHeld.h asks findLargestIDIn()). Message thread, but for
// whole state's parts, which read only the values they are given.
//
// The sections' SECTIONS element stays in the song's tree, empty, where the song has one: added with the first section, as
// it always was, never taken away by the sections' edits (a song whose sections all went keeps it), and the place a whole
// copy writes the sections in. No reader asks whether it is there (the sections' readers read the value); its presence is
// written into the song's file and compared with the saved copy (an empty SECTIONS is a difference from none), so it keeps
// its rules: who makes and takes away the element is the sections' code (model/arrangement/internal/Sections.h: a section
// made, in that step, as it was made with the first section; and a version's sections made the song's, makeSectionsAs(),
// which adds or takes it away as the other song has it), never a reader; the element's own coming and going is the tree's,
// which its listeners hear as before.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <vector>

namespace phazite
{
class Song;

// What the values' listeners are told of the sections (Listener::sectionsChanged()): once for each change of its value, the
// IDs of the sections added, taken out or changed (findSectionsChanged()). It listens to the sections' area from its making
// to its end.
struct SongValues::SectionsTeller final : ValueArea<SectionsValue>::Listener
{
    SectionsTeller (ValueArea<SectionsValue>&, juce::ListenerList<SongValues::Listener>&);
    ~SectionsTeller() override;

    ValueArea<SectionsValue>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SectionsValue& before, const SectionsValue& after) override;

    JUCE_DECLARE_NON_COPYABLE (SectionsTeller)
};

// The sections that differ from one value to the other, by their IDs, each once: those the second holds that the first
// hasn't or holds otherwise (any property), in the second's order, then those the first holds that the second hasn't, in
// the first's. None for two values that hold the same sections (in another order: a version's, which the listeners hear as a
// change of the value with no section named).
std::vector<SectionID> findSectionsChanged (const SectionsValue& before, const SectionsValue& after);

// The sections' part of the keeper (SongValues.h), its safety net: a SECTIONS that comes into the song with sections in it has
// them read in as the song's sections (readHeldSections()), and is emptied as the adoption empties the song's own (what
// Phazite doesn't know of it goes too); a SECTION added to the song's SECTIONS by itself is read into the sections
// (withSectionTaken()) and taken out again while the song is written outside the history, and refused in a step. Nothing when
// one of its elements leaves the tree. And the sections' adoption from the loaded tree, which the song's holdValues() asks.
struct SongValues::SectionsKeeper
{
    SectionsKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree (the keeper's callback, but while the history undoes or redoes, for
    // every element, whoever's it is): nothing, at once, but for the song's SECTIONS or a child added to it.
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);

    // The sections the loader made consistent into their area, outside the history, and their element emptied (SongValues.cpp:
    // adopt()). A song without a SECTIONS keeps none, its sections none.
    static void adopt (SongValues& into, Song& loaded);

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    JUCE_DECLARE_NON_COPYABLE (SectionsKeeper)
};

// The sections' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's SECTIONS (the child of its PHAZITE element); and a copy's SECTIONS given the sections kept (SectionsValue.h:
// writeSections(), in their order), where some are kept (a song that keeps everything in its tree keeps none: its SECTIONS
// holds its sections). Nothing for an element that isn't a SECTIONS.
bool leadsToSections (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeSectionsIn (juce::ValueTree& element, const ValuesKept&);

// The sections' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): where
// either is a song's element that holds a SECTIONS, the same sections (holdSameSections()); and whether a child is the place
// of the sections, which the comparison of the elements in them leaves out (the song's SECTIONS).
bool holdSameSectionsOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);
bool isSectionsPlace (const juce::ValueTree& parent, const juce::ValueTree& child);

// The largest ID the sections' area holds below schema::maxID (IDsHeld.h): each section's. 0 for none.
std::uint64_t findLargestIDIn (const SectionsValue&);
} // namespace phazite
