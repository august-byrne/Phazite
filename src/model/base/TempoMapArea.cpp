// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/TempoMapArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The song's TEMPOMAP element once what it held is read into the song's map (the adoption and the keeper's safety net
    // alike): emptied, the place a whole copy writes the map in, its properties and children Phazite doesn't know taken out
    // too (Schema.h, TEMPOMAP).
    void emptyTempoMapElement (juce::ValueTree& map, juce::UndoManager* undo)
    {
        map.removeAllChildren (undo);
        map.removeAllProperties (undo);
    }
} // namespace

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
SongValues::TempoMapKeeper::TempoMapKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

void SongValues::TempoMapKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (! isHeardAsTempoMap (parent, child))
        return;

    // The safety net (SongValues.h): a TEMPOMAP that comes into the song with entries in it has them read in as the song's
    // map, and is emptied as adopt() empties the song's own (emptyTempoMapElement(): what Phazite doesn't know of it goes
    // too); an entry added to the song's TEMPOMAP by itself is read into the map and taken out again while the song is
    // written outside the history, and refused in a step, whose Undo, taking the tree's change back first, would find it
    // gone (JUCE workaround W62 (docs/juce-workarounds.md)). Either in the mode of the moment.
    const auto isEntry = [] (const juce::ValueTree& element)
    { return element.hasType (schema::TEMPO) || element.hasType (schema::TIMESIGNATURE); };

    auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

    if (child.hasType (schema::TEMPOMAP) && parent == state)
    {
        std::vector<juce::ValueTree> entries;

        for (const auto& element : child)
            if (isEntry (element))
                entries.push_back (element);

        if (entries.empty())
            return;

        values.tempoMapAdopted += (int) entries.size();
        values.setTempoMap (song, readHeldTempoMap (child));
        emptyTempoMapElement (child, undo);
        return;
    }

    if (! isEntry (child) || ! parent.hasType (schema::TEMPOMAP) || parent.getParent() != state)
        return;

    if (undo != nullptr)
    {
        jassertfalse; // an entry added to the song's tempo map: the tempo map's functions change it (Tempo.h)
        return;
    }

    ++values.tempoMapAdopted;
    values.setTempoMap (song, withEntryTaken (values.tempoMap.get(), child));
    parent.removeChild (child, nullptr);
}

void SongValues::TempoMapKeeper::adopt (SongValues&, Song& loaded)
{
    // The song's TEMPOMAP, whose map its reader read into the area before (readHeldTempoMapIn(): SongValues.cpp, adopt()),
    // emptied (emptyTempoMapElement()); one made, empty, where the tree has none.
    auto root = loaded.getState();
    auto map = root.getChildWithName (schema::TEMPOMAP);

    if (! map.isValid())
    {
        map = juce::ValueTree (schema::TEMPOMAP);
        root.appendChild (map, nullptr);
    }

    emptyTempoMapElement (map, nullptr);
}

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

bool isHeardAsTempoMap (const juce::ValueTree& element)
{
    for (auto each = element; each.isValid(); each = each.getParent())
        if (each.hasType (schema::TEMPOMAP))
            return each.getParent().hasType (schema::PHAZITE);

    return false;
}

bool isHeardAsTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (child.hasType (schema::TEMPOMAP) && parent.hasType (schema::PHAZITE)) || isHeardAsTempoMap (parent);
}

//==============================================================================
bool leadsToTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return parent.hasType (schema::PHAZITE) && child.hasType (schema::TEMPOMAP);
}

void writeTempoMapIn (juce::ValueTree& element, const SongState& kept)
{
    // Kept of a song that holds its values, which holds at least one of each kind (none kept of one that keeps everything in
    // its tree, whose TEMPOMAP holds its entries).
    if (! element.hasType (schema::TEMPOMAP) || kept.tempoMap.tempos.empty())
        return;

    jassert (element.getNumChildren() == 0); // the song's tree holds none of its entries (SongValues.h)
    writeTempoMap (kept.tempoMap, element);
}

bool holdSameTempoMap (const juce::ValueTree& song, const SongState& kept, const juce::ValueTree& otherSong, const SongState& otherKept)
{
    // The values kept, each tree's TEMPOMAP the place of its map alone.
    if (song.getChildWithName (schema::TEMPOMAP).isValid() != otherSong.getChildWithName (schema::TEMPOMAP).isValid())
        return false;

    return kept.tempoMap.isSameAs (otherKept.tempoMap) || kept.tempoMap == otherKept.tempoMap;
}

bool holdSameTempoMapOf (const juce::ValueTree& element, const SongState& kept, const juce::ValueTree& otherElement,
                         const SongState& otherKept)
{
    const auto hasTempoMap = [] (const juce::ValueTree& each) { return each.getChildWithName (schema::TEMPOMAP).isValid(); };
    return ! (hasTempoMap (element) || hasTempoMap (otherElement)) || holdSameTempoMap (element, kept, otherElement, otherKept);
}

bool isTempoMapPlace (const juce::ValueTree&, const juce::ValueTree& child)
{
    return child.hasType (schema::TEMPOMAP);
}
} // namespace phazite
