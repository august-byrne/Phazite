// SPDX-License-Identifier: AGPL-3.0-or-later
// The sends' area of the song's values (roadmap step S5.20; SongValues.h, whose recipe this follows), as far as it is built:
// the song's sends are still the song's tree's (Schema.h: SEND under a TRACK), and their values and the conversion are
// SendsValue.h's. Here: the sends' part of the keeper, which tells the values' listeners what the tree tells of them
// (Listener::sendsChanged()), and what the unsaved check compares of them with the saved copy while the trees hold them.
// Message thread, but for the comparisons' readers, which read only the trees they are given.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace phazite
{
class Song;

// The sends' part of the keeper (SongValues.h), while the song's tree holds the sends: for each of the tree's notifications of
// the song's SEND elements (isHeardAsSends()), for a track given another ID or whose children came in another order, and for
// each TRACK or track list that comes into the song or leaves it with sends on it, whatever the history does (an undo's and a
// redo's, a write's outside the history), the values' listeners told at once, in one call, each send it changed
// (Listener::sendsChanged()), so that a level set plays in the same call: exactly what differs between the song's sends as
// the listeners were last told them (told, which is the song's tree read by readSendsValue() as the last notification left
// it) and the tree read so now, whatever the tree holds (a send's ID on two tracks, or twice on one, a track without an ID or
// two with one, as a version being applied may pass through). A property of a send changed: the send read again where it is
// the element the song reads that send from (the first of its ID: elements), its parts compared (its return or its place
// before the fader, its route; its level), and nothing for another element of its ID, which the song doesn't read. A send
// added, taken out or given another ID, a track that holds sends (or a folder whose tracks do) added, taken out, given
// another ID or its children in another order, and the track list in another order while the song has sends: the song's
// SEND elements found again as readSendsValue() finds them (SendsValue.h: forEachTrackHeldSends(), a walk reading each one's
// ID alone), each send read again whose element or track is another than before, and each that came, went or changed told;
// each track whose list holds those it held before in another order told reordered. A call that changes nothing tells
// nothing. Nothing of the song's values is read.
struct SongValues::SendsKeeper
{
    SendsKeeper (SongValues&, Song&);

    // The keeper's callbacks, each for every element, whoever's it is, in an undo or a redo too: nothing, at once, but for
    // what is the sends'.
    void propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property);
    void childAdded (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child);
    void childOrderChanged (const juce::ValueTree& parent);

    SongValues& values;
    Song& song;
    SongSends told {}; // the song's sends as the listeners were last told them (at the start: as the song holds them)
    std::unordered_map<std::uint64_t, juce::ValueTree> elements {}; // each of told's sends' element, by its ID

    // What one notification changed, told in one call as it ends.
    struct Heard
    {
        std::vector<SendChange> changes {};
        std::vector<TrackID> reordered {};
    };

    void reread (const juce::ValueTree& send, Heard&);
    void settle (Heard&);
    void tell (Heard&) const;

    JUCE_DECLARE_NON_COPYABLE (SendsKeeper)
};

// What the unsaved check compares of the sends with the saved copy (model/project/Project.cpp) while the song's tree and the
// saved copy's hold them: a send's element in a song's tree (PHAZITE), by its ID, with the ID of the track it is on (the
// first that readSendsValue() reads; an invalid tree for none); and a track's sends' IDs in its element's order, those
// readSendsValue() reads, none for a track the tree hasn't. Reading only the trees given, no value whole.
struct FoundSendElement
{
    juce::ValueTree element {};
    TrackID track {};
};

FoundSendElement findSendElement (const juce::ValueTree& song, DeviceID);
std::vector<DeviceID> readTrackSendOrder (const juce::ValueTree& song, TrackID);
} // namespace phazite
