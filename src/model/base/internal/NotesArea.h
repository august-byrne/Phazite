// SPDX-License-Identifier: AGPL-3.0-or-later
// The notes' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the
// conversion of its elements are ClipNotesValue.h's. A clip's notes come into the song and leave it with their clip: the
// clips' keeper and whole state's writer of the clips (ClipsArea.h) do the notes' part for each clip, through takeClipNotes()
// and the clip's element, so the notes' area has no line of its own in those two lists. Message thread, but for the
// comparison and the IDs, which read only the values they are given.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/ClipNotesValue.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/SongValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
// What the values' listeners are told of the notes (Listener::clipNotesChanged()): the clips whose notes changed, found by
// comparing the two maps, by identity clip by clip (immer's diff, which skips what the two share, as a change leaves most of
// the map shared). It listens to the notes' area from its making to its end.
struct SongValues::NotesTeller final : ValueArea<NotesByClip>::Listener
{
    NotesTeller (ValueArea<NotesByClip>&, juce::ListenerList<SongValues::Listener>&);
    ~NotesTeller() override;

    ValueArea<NotesByClip>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const NotesByClip& before, const NotesByClip& after) override;

    JUCE_DECLARE_NON_COPYABLE (NotesTeller)
};

// The notes of an alias group a clip joins (by its link), where another clip of it holds some: none for a clip in none, or
// a group none of whose other clips holds notes. What a MIDI clip that joins a group takes (SongValues::setClip(), and the
// clips' keeper as it reads a clip in).
const ClipNotes* findGroupNotes (const SongClips& clips, const NotesByClip& notes, std::uint64_t link, ClipID except);

// A clip read from its element into the notes' next value, before the clip goes into the clips' (ClipsArea.cpp: the keeper,
// and the clips' adoption): a MIDI clip's notes, those of the alias group it joins where another clip of it holds some
// (findGroupNotes()), else its own, read from its element (readClipNotes()); an audio clip's none.
void takeClipNotes (NotesByClip& notes, const SongClips& clips, const ClipValue& clip, const juce::ValueTree& element);

// The largest ID the notes' area holds below schema::maxID (IDsHeld.h): each note's ID (an alias group's clips hold one
// value, counted under each of their IDs). 0 for none.
std::uint64_t findLargestIDIn (const NotesByClip&);
} // namespace phazite
