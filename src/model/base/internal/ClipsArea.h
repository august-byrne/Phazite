// SPDX-License-Identifier: AGPL-3.0-or-later
// The clips' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows): its parts, each of
// which a list of SongValues.h's, WholeState.h's or IDsHeld.h's asks in turn, a line for each area. Its value and the
// conversion of its elements are ClipsValue.h's. A clip's notes come in and go out of the song with the clip, in the same
// loop over its track's elements (NotesArea.h: takeClipNotes()), so the clips' keeper and whole state's writer here do the
// notes' part for each clip too, the clip first: the notes' area has no line of its own in those two lists. Message thread,
// but for whole state's parts and the IDs, which read only the values they are given.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
class Song;

// What the values' listeners are told of the clips (Listener::clipsChanged()): each clip whose value differs between the two
// (immer's diff, which skips what the two share), with its track before and after; and each track whose list holds the same
// clips in another order, from the lists that differ by identity. It listens to the clips' area from its making to its end.
struct SongValues::ClipsTeller final : ValueArea<SongClips>::Listener
{
    ClipsTeller (ValueArea<SongClips>&, juce::ListenerList<SongValues::Listener>&);
    ~ClipsTeller() override;

    ValueArea<SongClips>& area;
    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongClips& before, const SongClips& after) override;

    JUCE_DECLARE_NON_COPYABLE (ClipsTeller)
};

// The clips' adoption from the loaded tree, with their notes, which the song's holdValues() asks: every CLIP element out of
// the tree the loader made consistent, those anywhere else too, once its reader read the song's into both areas
// (readHeldClipsIn(), SongValues.cpp: adopt()). The keeper asks the clips of nothing: no track is in a live song's tree for a CLIP to come into it on (the
// tracks' safety net reads a TRACK's: takeTrackClipsIn(), below).
struct SongValues::ClipsKeeper
{
    static void adopt (SongValues& into, Song& loaded);
};

// The clips' part of whole state (WholeState.cpp: writeIn()): a copy's track given its clips from the values kept, first among
// its elements, in the track's order, each with its notes (ClipsValue.h: writeClipElement()). Nothing for an element that
// isn't a track. Whole state writes each TRACK itself (TracksArea.h: writeTracksIn()), asking this of each.
void writeClipsIn (juce::ValueTree& element, const SongState&);

// The clips' reader of a song's tree as the loader made it consistent (SongState.h: readSongTree(), a line of it): each track's
// CLIP elements into the state's clips and notes, as the song holds them; no tree changed (the adoption takes them out after:
// ClipsKeeper::adopt()). Any thread.
void readHeldClipsIn (const juce::ValueTree& song, SongState&);

// The CLIP elements on a track's element (one that comes into the song with them: the tracks' safety net, TracksArea.h) read
// into the areas' next values as the keeper takes a clip in, on the track of this ID, after its others: each kept within what
// a clip holds, a MIDI clip's notes those of the alias group it joins where another clip of it holds some, else its own, an
// audio clip of a group sharing the group's sound alike. Nothing for an element whose kind holds no clips. How many it read.
int takeTrackClipsIn (SongClips&, NotesByClip&, const juce::ValueTree& track, TrackID);

// The clips' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a track's
// clips, the same in the same order (holdSameTrackClips()); true for an element that isn't a track.
bool holdSameClipsOf (const juce::ValueTree& element, const SongState&, const juce::ValueTree& otherElement, const SongState&);

// The largest ID the clips' area holds below schema::maxID (IDsHeld.h): each clip's ID and its alias group's. 0 for none.
std::uint64_t findLargestIDIn (const SongClips&);
} // namespace phazite
