// SPDX-License-Identifier: AGPL-3.0-or-later
// The sections' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's or WholeState.h's asks in turn, a line for each area. Its value and the conversion of its
// elements are SectionsValue.h's. It holds IDs (each section's: IDsHeld.h asks findLargestIDIn()). Message thread, but for
// whole state's parts, which read only the values they are given.
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

// The sections' part of whole state (WholeState.cpp: writeIn()): whether a child of an element in a copy of the song is the
// song's SECTIONS (the child of its PHAZITE element); and a copy's SECTIONS given the sections kept (SectionsValue.h:
// writeSections(), in their order). Nothing for an element that isn't a SECTIONS: whole state writes one while the song
// holds a section, none for none (SongState.h).
bool leadsToSections (const juce::ValueTree& parent, const juce::ValueTree& child);
void writeSectionsIn (juce::ValueTree& element, const SongState&);

// The sections' reader of a song's tree (SongState.h: readSongTree(), a line of it): its SECTIONS into the state, as the song
// holds them (none where it has none); no tree changed. Any thread.
void readHeldSectionsIn (const juce::ValueTree& song, SongState&);

// The largest ID the sections' area holds below schema::maxID (IDsHeld.h): each section's. 0 for none.
std::uint64_t findLargestIDIn (const SectionsValue&);
} // namespace phazite
