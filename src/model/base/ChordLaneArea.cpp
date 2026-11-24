// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ChordLaneArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <utility>

namespace phazite
{
namespace
{
    // The song's CHORDLANE element once what it held is read into the song's lane (the adoption and the keeper's safety net
    // alike): emptied, the place a whole copy writes the lane in, its properties and children Phazite doesn't know taken out
    // too (Schema.h, CHORDLANE).
    void emptyChordLaneElement (juce::ValueTree& lane, juce::UndoManager* undo)
    {
        lane.removeAllChildren (undo);
        lane.removeAllProperties (undo);
    }

    bool isEntry (const juce::ValueTree& element)
    {
        return element.hasType (schema::CHORD) || element.hasType (schema::CHORDKEY);
    }
} // namespace

//==============================================================================
SongValues::ChordLaneTeller::ChordLaneTeller (ValueArea<ChordLaneValue>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::ChordLaneTeller::~ChordLaneTeller()
{
    area.removeListener (this);
}

void SongValues::ChordLaneTeller::valueAreaChanged (const ChordLaneValue& before, const ChordLaneValue& after)
{
    // Its entries alone: the lane placed or taken away, its CHORDLANE added or taken out, is the tree's to tell.
    if (! before.holdsSameEntriesAs (after))
        listeners.call ([] (SongValues::Listener& listener) { listener.chordLaneChanged(); });
}

//==============================================================================
SongValues::ChordLaneKeeper::ChordLaneKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

void SongValues::ChordLaneKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    const auto isLane = child.hasType (schema::CHORDLANE) && parent == state;

    if (! isLane && ! isHeardAsChordLane (parent, child))
        return;

    // The safety net (SongValues.h): a CHORDLANE that comes into the song with chords or keys in it has them read in as the
    // song's lane, and is emptied as adopt() empties the song's own (emptyChordLaneElement(): what Phazite doesn't know of it
    // goes too); a chord or a key added to the song's CHORDLANE by itself is read into the lane and taken out again while the
    // song is written outside the history, and refused in a step, whose Undo, taking the tree's change back first, would find
    // it gone (JUCE workaround W62 (docs/juce-workarounds.md)). Either in the mode of the moment.
    auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

    if (isLane)
    {
        auto entries = 0;

        for (const auto& element : child)
            entries += isEntry (element) ? 1 : 0;

        if (entries == 0)
            return;

        values.chordLaneAdopted += entries;
        values.setChordLane (song, readHeldChordLane (child));
        emptyChordLaneElement (child, undo);
        return;
    }

    if (! isEntry (child) || ! parent.hasType (schema::CHORDLANE) || parent.getParent() != state)
        return;

    if (undo != nullptr)
    {
        jassertfalse; // a chord or a key added to the song's chord lane: the chord lane's functions change it (Chords.h)
        return;
    }

    ++values.chordLaneAdopted;
    values.setChordLane (song, withEntryTaken (values.chordLane.get(), child));
    parent.removeChild (child, nullptr);
}

void SongValues::ChordLaneKeeper::adopt (SongValues&, Song& loaded)
{
    // The song's CHORDLANE, whose lane its reader read into the area before (readHeldChordLaneIn(): SongValues.cpp, adopt()),
    // emptied (emptyChordLaneElement()), and kept: the sign that the song has a lane, beside its value's (placed). Nothing for a
    // song without one.
    if (auto lane = loaded.getState().getChildWithName (schema::CHORDLANE); lane.isValid())
        emptyChordLaneElement (lane, nullptr);
}

void readHeldChordLaneIn (const juce::ValueTree& song, SongState& into)
{
    // The chord lane the loader made consistent, each entry as the song holds one (ChordLaneValue.h: readHeldChordLane(), placed
    // as the tree has a CHORDLANE); an empty lane, not placed, for a song without one.
    into.chordLane = readHeldChordLane (song.getChildWithName (schema::CHORDLANE));
}

//==============================================================================
void SongValues::setChordLane (Song& song, ChordLaneValue next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);

    if (next == chordLane.get())
        return;

    if (song.isWritingOutsideHistory())
        chordLane.setOutsideHistory (std::move (next));
    else
        chordLane.set (std::move (next));
}

bool isHeardAsChordLane (const juce::ValueTree& element)
{
    for (auto each = element; each.isValid(); each = each.getParent())
        if (each.hasType (schema::CHORDLANE))
            return each.getParent().hasType (schema::PHAZITE);

    return false;
}

bool isHeardAsChordLane (const juce::ValueTree& parent, const juce::ValueTree&)
{
    return isHeardAsChordLane (parent);
}

//==============================================================================
bool leadsToChordLane (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::CHORDLANE);
}

void writeChordLaneIn (juce::ValueTree& element, const SongState& kept)
{
    // Nothing kept of an empty lane, nor of a song that keeps everything in its tree (whose CHORDLANE holds its entries).
    if (! element.hasType (schema::CHORDLANE) || (kept.chordLane.chords.empty() && kept.chordLane.keys.empty()))
        return;

    jassert (element.getNumChildren() == 0); // the song's tree holds none of its chords or keys (SongValues.h)
    writeChordLane (kept.chordLane, element);
}

bool holdSameChordLane (const juce::ValueTree& song, const SongState& kept, const juce::ValueTree& otherSong, const SongState& otherKept)
{
    // Whether each has a lane (its tree's CHORDLANE, the sign of it), and the values kept.
    if (song.getChildWithName (schema::CHORDLANE).isValid() != otherSong.getChildWithName (schema::CHORDLANE).isValid())
        return false;

    return kept.chordLane.isSameAs (otherKept.chordLane) || kept.chordLane == otherKept.chordLane;
}

bool holdSameChordLaneOf (const juce::ValueTree& element, const SongState& kept, const juce::ValueTree& otherElement,
                          const SongState& otherKept)
{
    const auto hasChordLane = [] (const juce::ValueTree& each)
    { return each.hasType (schema::PHAZITE) && each.getChildWithName (schema::CHORDLANE).isValid(); };

    return ! (hasChordLane (element) || hasChordLane (otherElement)) || holdSameChordLane (element, kept, otherElement, otherKept);
}

bool isChordLanePlace (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return leadsToChordLane (parent, child);
}
} // namespace phazite
