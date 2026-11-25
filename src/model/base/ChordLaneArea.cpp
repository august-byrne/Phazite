// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ChordLaneArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <utility>

namespace phazite
{
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
    // Its entries, and whether the song has a lane (placed): a lane placed with no chord or key yet, or taken away, is a change
    // of it as any.
    if (! before.isSameAs (after))
        listeners.call ([] (SongValues::Listener& listener) { listener.chordLaneChanged(); });
}

//==============================================================================
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

//==============================================================================
bool leadsToChordLane (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::CHORDLANE);
}

void writeChordLaneIn (juce::ValueTree& element, const SongState& kept)
{
    // Nothing kept of an empty lane (a placed one is written as an empty CHORDLANE).
    if (! element.hasType (schema::CHORDLANE) || (kept.chordLane.chords.empty() && kept.chordLane.keys.empty()))
        return;

    writeChordLane (kept.chordLane, element);
}

bool holdSameChordLane (const SongState& kept, const SongState& otherKept)
{
    // The values, whether each has a lane among them (ChordLaneValue.h: placed, which the equality compares).
    return kept.chordLane.isSameAs (otherKept.chordLane) || kept.chordLane == otherKept.chordLane;
}
} // namespace phazite
