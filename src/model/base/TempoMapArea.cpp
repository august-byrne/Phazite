// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/TempoMapArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <utility>

namespace phazite
{
//==============================================================================
SongValues::TempoMapTeller::TempoMapTeller (ValueArea<TempoMapValue>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::TempoMapTeller::~TempoMapTeller()
{
    area.removeListener (this);
}

void SongValues::TempoMapTeller::valueAreaChanged (const TempoMapValue& before, const TempoMapValue& after)
{
    if (! before.isSameAs (after))
        listeners.call ([] (SongValues::Listener& listener) { listener.tempoMapChanged(); });
}

//==============================================================================
void readHeldTempoMapIn (const juce::ValueTree& song, SongState& into)
{
    // The tempo map the loader made consistent (and makeSong() made whole: Tempo.h, ensureTempoMap()), each entry as the song
    // reads one (TempoMapValue.h: readHeldTempoMap()), an empty TEMPOMAP's where the tree has none.
    const auto map = song.getChildWithName (schema::TEMPOMAP);
    into.tempoMap = readHeldTempoMap (map.isValid() ? map : juce::ValueTree (schema::TEMPOMAP));
}

//==============================================================================
void SongValues::setTempoMap (Song& song, TempoMapValue next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);
    jassert (! next.tempos.empty() && ! next.signatures.empty()); // at least one of each (TempoMapValue.h)

    if (next.tempos.empty() || next.signatures.empty() || next == tempoMap.get())
        return;

    if (song.isWritingOutsideHistory())
        tempoMap.setOutsideHistory (std::move (next));
    else
        tempoMap.set (std::move (next));
}

//==============================================================================
bool leadsToTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::TEMPOMAP);
}

void writeTempoMapIn (juce::ValueTree& element, const SongState& kept)
{
    // A state holds at least one of each kind (none of one made empty, which has no map to write).
    if (! element.hasType (schema::TEMPOMAP) || kept.tempoMap.tempos.empty())
        return;

    writeTempoMap (kept.tempoMap, element);
}

bool holdSameTempoMap (const SongState& kept, const SongState& otherKept)
{
    return kept.tempoMap.isSameAs (otherKept.tempoMap) || kept.tempoMap == otherKept.tempoMap;
}
} // namespace phazite
