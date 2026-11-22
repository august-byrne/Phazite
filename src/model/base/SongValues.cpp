// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongValues.h"
#include "Song.h"
#include "internal/ChordLaneArea.h"
#include "internal/ClipsArea.h"
#include "internal/CurvesArea.h"
#include "internal/MidiLinksArea.h"
#include "internal/NotesArea.h"
#include "internal/TempoMapArea.h"

#include <memory>
#include <vector>

namespace phazite
{
//==============================================================================
// What keeps the areas and the tree together for every change of the tree (SongValues.h), by asking every area's part of it
// in turn, each doing nothing for what is not its own (ClipsArea.h, TempoMapArea.h, ChordLaneArea.h, CurvesArea.h,
// MidiLinksArea.h). It changes the areas and the tree through the song's undo manager from inside the tree's callbacks, which
// run inside the perform() of the tree's own action: JUCE refuses that only while it undoes or redoes (when the keeper does
// nothing), and adds the inner actions to the step before the outer one, so that Undo takes the tree's change back before the
// areas'. JUCE workaround W62 (docs/juce-workarounds.md).
struct SongValues::Keeper final : juce::ValueTree::Listener
{
    Keeper (SongValues& v, Song& s)
        : song (s),
          state (s.getState()),
          clips (v, s),
          tempoMap (v, s),
          chordLane (v, s),
          curves (v, s),
          midiLinks (v, s)
    {
        state.addListener (this);
    }

    ~Keeper() override { state.removeListener (this); }

    Song& song;
    juce::ValueTree state;

    // Each area's part, a member each (the notes' is the clips', which take each clip's notes in and out with the clip).
    ClipsKeeper clips;
    TempoMapKeeper tempoMap;
    ChordLaneKeeper chordLane;
    CurvesKeeper curves; // tells the curves' listener from the tree, in an undo and a redo too
    MidiLinksKeeper midiLinks;

    bool isUndoingOrRedoing() const { return song.getUndoManager().isPerformingUndoRedo(); }

    // A property changed, and children in another order: the curves' alone, while the tree holds them (CurvesArea.h).
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        curves.propertyChanged (tree, property);
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { curves.childOrderChanged (parent); }

    // A child added: every area asked in turn, each doing nothing (and returning at once) for what is not its own, so that an
    // area whose elements sit inside another's (a track's, say) sees them come in with it. The tempo map's first (the song's
    // TEMPOMAP, or one in it, which holds nothing of a clip's: the clips' part finds no track there), then the chord lane's
    // (the song's CHORDLANE, or one in it, which holds nothing of the tempo map's or a clip's), then the MIDI links' (the
    // song's MIDILINKS, or one in it, which holds nothing of another area's), then the clips' (a track, what holds tracks, a
    // CLIP, in the song's track list: none of them the tempo map's, the chord lane's or the MIDI links').
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        // Before them the curves', in an undo and a redo too (a CURVE, a POINT in one, the AUTOMATION: none another area's).
        curves.childAdded (parent, child);

        if (isUndoingOrRedoing())
            return;

        tempoMap.childAdded (parent, child);
        chordLane.childAdded (parent, child);
        midiLinks.childAdded (parent, child);
        clips.childAdded (parent, child);
    }

    // A child taken out: every area that acts there asked in turn, each doing nothing for what is not its own. The clips'
    // alone (a track, or what holds tracks): the tempo map, the chord lane and the MIDI links do nothing as their elements
    // leave.
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        // Before them the curves', in an undo and a redo too (a CURVE, or a POINT in one, taken out: none another area's).
        curves.childRemoved (parent, child);

        if (isUndoingOrRedoing())
            return;

        clips.childRemoved (parent, child);
    }
};

//==============================================================================
SongValues::SongValues (Song& song)
    : clips (song.getUndoManager(), {}),
      notes (song.getUndoManager(), {}),
      tempoMap (song.getUndoManager(), makeDefaultTempoMap()),
      chordLane (song.getUndoManager(), {}),
      midiLinks (song.getUndoManager(), {}),
      clipsTeller (std::make_unique<ClipsTeller> (clips, listeners)),
      notesTeller (std::make_unique<NotesTeller> (notes, listeners)),
      tempoMapTeller (std::make_unique<TempoMapTeller> (tempoMap, listeners)),
      chordLaneTeller (std::make_unique<ChordLaneTeller> (chordLane, listeners)),
      midiLinksTeller (std::make_unique<MidiLinksTeller> (midiLinks, listeners))
{
    // Each area with its teller, in the order of the areas; the keeper comes once the tree is consistent (adopt()). The
    // macro's statement ended again here, so that the formatter reads a body that holds nothing else.
    JUCE_ASSERT_MESSAGE_THREAD;
}

SongValues::~SongValues()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The keeper first, then each area's teller, in the reverse of their making.
    keeper = nullptr;
    midiLinksTeller = nullptr;
    chordLaneTeller = nullptr;
    tempoMapTeller = nullptr;
    notesTeller = nullptr;
    clipsTeller = nullptr;
}

void SongValues::addListener (Listener* listener) const
{
    JUCE_ASSERT_MESSAGE_THREAD
    listeners.add (listener);
}

void SongValues::removeListener (Listener* listener) const
{
    JUCE_ASSERT_MESSAGE_THREAD
    listeners.remove (listener);
}

bool SongValues::remembers (const juce::ValueTree& track) const
{
    return keeper != nullptr && keeper->clips.remembers (track);
}

std::vector<TrackID> SongValues::getRemembered() const
{
    return keeper != nullptr ? keeper->clips.getRemembered() : std::vector<TrackID>();
}

void SongValues::adopt (Song& song)
{
    // What the tree the loader made consistent holds of each area, into the area, outside the history, and out of the tree:
    // each area in turn, in this order (the clips with their notes, then the tempo map, then the chord lane, then the MIDI
    // links: none reads another's); then the keeper, from here on.
    ClipsKeeper::adopt (*this, song);
    TempoMapKeeper::adopt (*this, song);
    ChordLaneKeeper::adopt (*this, song);
    MidiLinksKeeper::adopt (*this, song);
    keeper = std::make_unique<Keeper> (*this, song);
}

void Song::holdValues()
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (values == nullptr);

    if (values != nullptr)
        return;

    values = std::make_unique<SongValues> (*this);
    values->adopt (*this);
}

//==============================================================================
// What of the tree's notifications the areas take: a line for each area, in the order of the areas.
bool isHeardAsKept (const juce::ValueTree& element)
{
    return isHeardAsTempoMap (element)      // the song's TEMPOMAP, or an element in it
           || isHeardAsChordLane (element)  // the song's CHORDLANE, or an element in it
           || isHeardAsCurves (element)     // a CURVE of the song's AUTOMATION, or an element in one
           || isHeardAsMidiLinks (element); // the song's MIDILINKS, or an element in it
}

bool isHeardAsKept (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return isHeardAsTempoMap (parent, child)      // the TEMPOMAP itself, or a child of an element in it
           || isHeardAsChordLane (parent, child)  // a child of the CHORDLANE or of an element in it (not the CHORDLANE itself)
           || isHeardAsCurves (parent, child)     // a CURVE added to the AUTOMATION or taken out, or a child of one's element
           || isHeardAsMidiLinks (parent, child); // a child of the MIDILINKS or of an element in it (not the MIDILINKS itself)
}
} // namespace phazite
