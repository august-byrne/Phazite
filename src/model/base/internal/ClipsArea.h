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
#include <utility>
#include <vector>

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

// The clips' part of the keeper (SongValues.h), with their notes: a track taken out of the song (alone, in a folder, the
// track list) has its clips (their values, their order and their notes) remembered with its element and erased from both
// areas; the same element put back gets them back; a track that comes into the song's tree with its clips' elements in it
// has them read into both areas and taken out of the tree; a CLIP element added to a track of the song by itself is
// refused. Each in the mode of the moment. And the clips' adoption from the loaded tree, which the song's holdValues()
// asks.
struct SongValues::ClipsKeeper
{
    ClipsKeeper (SongValues&, Song&);

    // A child added to an element of the song's tree, or taken out of one (the keeper's callbacks, but while the history
    // undoes or redoes, for every element, whoever's it is): nothing, at once, but for what is the clips' (a track, what
    // holds tracks, or a CLIP, in the song's track list).
    void childAdded (juce::ValueTree& parent, juce::ValueTree& child);
    void childRemoved (juce::ValueTree& parent, juce::ValueTree& child);

    // Every CLIP of the tree the loader made consistent into both areas, outside the history, and every CLIP element out of
    // the tree (SongValues.cpp: adopt()).
    static void adopt (SongValues& into, Song& loaded);

    // Whether it remembers a track's element with its clips, taken out of the song, and the IDs of the tracks it remembers
    // (SongValues::remembers(), getRemembered()).
    bool remembers (const juce::ValueTree& track) const;
    std::vector<TrackID> getRemembered() const;

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    // The tracks taken out of the song, each element with its clips as they went (their values in its order, and their
    // notes): given back if the same element comes back (a move), let go of once nothing else holds the element (the history
    // let go of the step that could bring it back), or it is back in the song by an undo, which brought its clips back itself.
    struct Remembered
    {
        juce::ValueTree track;
        std::vector<ClipValue> clips;
        std::vector<std::pair<ClipID, ClipNotes>> notes;
    };

    std::vector<Remembered> remembered;
    size_t sweepAt = 64;

    std::vector<Remembered>::iterator findRemembered (const juce::ValueTree& track);
    void sweep();

    // Whether a child of an element is in the song's track list (a track, a folder's tracks, or the track list itself): only
    // there do tracks hold the song's clips.
    bool isInTrackList (const juce::ValueTree& parent, const juce::ValueTree& child) const;

    JUCE_DECLARE_NON_COPYABLE (ClipsKeeper)
};

// The clips' part of whole state (WholeState.cpp: writeIn()): a copy's track given its clips from the values kept, first among
// its elements, in the track's order, each with its notes (ClipsValue.h: writeClipElement()). Nothing for an element that
// isn't a track. Whole state writes each TRACK itself (TracksArea.h: writeTracksIn()), asking this of each.
void writeClipsIn (juce::ValueTree& element, const ValuesKept&);

// The CLIP elements on a track's element (one that comes into the song with them: the tracks' safety net, TracksArea.h) read
// into the areas' next values as the keeper takes a clip in, on the track of this ID, after its others: each kept within what
// a clip holds, a MIDI clip's notes those of the alias group it joins where another clip of it holds some, else its own, an
// audio clip of a group sharing the group's sound alike. Nothing for an element whose kind holds no clips. How many it read.
int takeTrackClipsIn (SongClips&, NotesByClip&, const juce::ValueTree& track, TrackID);

// The clips' part of the comparison of two elements with what their songs kept (WholeState.h: holdSameKeptOf()): a track's
// clips, the same in the same order (holdSameTrackClips()); true for an element that isn't a track.
bool holdSameClipsOf (const juce::ValueTree& element, const ValuesKept&, const juce::ValueTree& otherElement, const ValuesKept&);

// The largest ID the clips' area holds below schema::maxID (IDsHeld.h): each clip's ID and its alias group's. 0 for none.
std::uint64_t findLargestIDIn (const SongClips&);
} // namespace phazite
