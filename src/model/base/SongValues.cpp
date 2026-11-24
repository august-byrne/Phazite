// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongValues.h"
#include "Song.h"
#include "internal/ChordLaneArea.h"
#include "internal/ClipsArea.h"
#include "internal/CurvesArea.h"
#include "internal/DeviceStoreArea.h"
#include "internal/DevicesArea.h"
#include "internal/MidiLinksArea.h"
#include "internal/ModulationArea.h"
#include "internal/NotesArea.h"
#include "internal/SectionsArea.h"
#include "internal/SendsArea.h"
#include "internal/SongOwnArea.h"
#include "internal/TempoMapArea.h"
#include "internal/TracksArea.h"

#include <memory>

namespace phazite
{
//==============================================================================
// What keeps the areas and the tree together for every change of the tree (SongValues.h), by asking every area's part of it
// in turn, each doing nothing for what is not its own (TracksArea.h, TempoMapArea.h, ChordLaneArea.h, CurvesArea.h,
// MidiLinksArea.h, SectionsArea.h, ModulationArea.h, DeviceStoreArea.h, DevicesArea.h; and SongOwnArea.h, whose part takes
// in what is written to the song's root of its own: its safety net). The clips' and the sends' areas have no part: no track is
// in the tree for one of theirs to come into it on. The tracks' part first: what it takes (a TRACK added to the song's
// TRACKS, a TRACKS or a MASTER added to the song, a property of its MASTER: its safety net, which reads what a track holds
// through each area's reader) is asked of no other part. It changes the areas and the tree through the song's undo manager
// from inside the tree's callbacks, which run inside the perform() of the tree's own action: JUCE refuses that only while it
// undoes or redoes (when the keeper does nothing), and adds the inner actions to the step before the outer one, so that Undo
// takes the tree's change back before the areas'. JUCE workaround W62 (docs/juce-workarounds.md).
struct SongValues::Keeper final : juce::ValueTree::Listener
{
    Keeper (SongValues& v, Song& s)
        : song (s),
          state (s.getState()),
          tracks (v, s),
          tempoMap (v, s),
          chordLane (v, s),
          midiLinks (v, s),
          curves (v, s),
          sections (v, s),
          modulation (v, s),
          deviceStore (v, s),
          devices (v, s),
          trackLeaves (v, s),
          songOwn (v, s)
    {
        state.addListener (this);
    }

    ~Keeper() override { state.removeListener (this); }

    Song& song;
    juce::ValueTree state;

    // Each area's part that acts, a member each.
    TracksKeeper tracks;
    TempoMapKeeper tempoMap;
    ChordLaneKeeper chordLane;
    MidiLinksKeeper midiLinks;
    CurvesKeeper curves;
    SectionsKeeper sections;
    ModulationKeeper modulation;
    DeviceStoreKeeper deviceStore;
    DevicesKeeper devices;
    TrackLeavesKeeper trackLeaves; // the switches' and the view's: a lone LANES of the master's
    SongOwnKeeper songOwn;         // the song's own's safety net (SongOwnArea.h)

    bool isUndoingOrRedoing() const { return song.getUndoManager().isPerformingUndoRedo(); }

    // A property changed: the tracks' (a property of the song's MASTER, which holds none: its safety net), then the song's
    // own, a property written to the song's root (its safety net: SongOwnArea.h, which does nothing while the history undoes
    // or redoes).
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        tracks.propertyChanged (tree, property);
        songOwn.propertyChanged (tree, property);
    }

    // A child added: every area asked in turn, each doing nothing (and returning at once) for what is not its own, so that an
    // area whose elements sit inside another's sees them come in with it. The tracks' first (a TRACK added to the song's
    // TRACKS, a TRACKS or a MASTER added to the song: read in whole, what it holds with it, through each area's reader, and no
    // other part asked of it). Then the tempo map's (the song's TEMPOMAP, or one in it), then the chord lane's (the song's
    // CHORDLANE, or one in it), then the MIDI links' (the song's MIDILINKS, or one in it), then the curves' (the song's
    // AUTOMATION, or one in it), then the sections' (the song's SECTIONS, or one in it), then modulation's (a lone MODULATOR
    // or MACRO on the master, or the macros of a lone CONTAINER there), then the store's (what a lone DEVICE, CONTAINER or
    // CHAIN holds of the store), then the devices' (a lone DEVICE, CONTAINER, CHAIN, VALUES or SOUNDS), which take a DEVICE or
    // a CONTAINER out of the tree whole once modulation and the store took theirs out of it, then the switches' and the view's
    // (a lone LANES of the master's), then the song's own (a LASTKEY, a PIANOROLL or a MERGING added to the song's root, which
    // no other area holds). Nothing as a child is taken out: no area's part acts there.
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (isUndoingOrRedoing())
            return;

        if (tracks.childAdded (parent, child))
            return;

        tempoMap.childAdded (parent, child);
        chordLane.childAdded (parent, child);
        midiLinks.childAdded (parent, child);
        curves.childAdded (parent, child);
        sections.childAdded (parent, child);
        modulation.childAdded (parent, child);
        deviceStore.childAdded (parent, child);
        devices.childAdded (parent, child);
        trackLeaves.childAdded (parent, child);
        songOwn.childAdded (parent, child);
    }
};

//==============================================================================
SongValues::SongValues (Song& song)
    : clips (song.getUndoManager(), {}),
      notes (song.getUndoManager(), {}),
      tempoMap (song.getUndoManager(), makeDefaultTempoMap()),
      chordLane (song.getUndoManager(), {}),
      midiLinks (song.getUndoManager(), {}),
      curves (song.getUndoManager(), {}),
      sections (song.getUndoManager(), {}),
      modulation (song.getUndoManager(), {}),
      sends (song.getUndoManager(), {}),
      deviceStore (song.getUndoManager(), {}),
      devices (song.getUndoManager(), {}),
      trackSwitches (song.getUndoManager(), {}),
      tracks (song.getUndoManager(), {}),
      songOwn (song.getUndoManager(), {}),
      songStanding (song.getUndoManager(), {}),
      clipsTeller (std::make_unique<ClipsTeller> (clips, listeners)),
      notesTeller (std::make_unique<NotesTeller> (notes, listeners)),
      tempoMapTeller (std::make_unique<TempoMapTeller> (tempoMap, listeners)),
      chordLaneTeller (std::make_unique<ChordLaneTeller> (chordLane, listeners)),
      midiLinksTeller (std::make_unique<MidiLinksTeller> (midiLinks, listeners)),
      curvesTeller (std::make_unique<CurvesTeller> (curves, listeners)),
      sectionsTeller (std::make_unique<SectionsTeller> (sections, listeners)),
      modulationTeller (std::make_unique<ModulationTeller> (modulation, listeners)),
      sendsTeller (std::make_unique<SendsTeller> (sends, listeners)),
      deviceStoreTeller (std::make_unique<DeviceStoreTeller> (*this, listeners)),
      devicesTeller (std::make_unique<DevicesTeller> (devices, listeners)),
      trackSwitchesTeller (std::make_unique<TrackSwitchesTeller> (*this, listeners)),
      tracksViewTeller (std::make_unique<TracksViewTeller> (*this, listeners)),
      tracksTeller (std::make_unique<TracksTeller> (tracks, listeners)),
      songOwnTeller (std::make_unique<SongOwnTeller> (songOwn, listeners)),
      songStandingTeller (std::make_unique<SongStandingTeller> (songStanding, listeners)),
      songOutsideTeller (std::make_unique<SongOutsideTeller> (songOutside, listeners))
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
    songOutsideTeller = nullptr;
    songStandingTeller = nullptr;
    songOwnTeller = nullptr;
    tracksTeller = nullptr;
    tracksViewTeller = nullptr;
    trackSwitchesTeller = nullptr;
    devicesTeller = nullptr;
    deviceStoreTeller = nullptr;
    sendsTeller = nullptr;
    modulationTeller = nullptr;
    sectionsTeller = nullptr;
    curvesTeller = nullptr;
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

void SongValues::adopt (Song& song)
{
    // What the tree the loader made consistent holds of each area, into the area, outside the history, and out of the tree:
    // each area in turn, in this order (the clips with their notes, then the tempo map, then the chord lane, then the MIDI
    // links, then the curves, then the sections, then modulation, then the sends, then the store, then the devices, which
    // take every DEVICE, CONTAINER and CHAIN element out of the tree once modulation took a container's macros and the store
    // what a device or a chain stores out of them, then the tracks' switches and view, then the tracks themselves, last,
    // which take every TRACK out of the tree and the MASTER's own: none reads another's), the song's own first, with its
    // count of IDs (SongOwnArea.h), so that anything an area's adoption made would count on from the count the file holds
    // (none makes one), and what Phazite doesn't know at the root dropped before any area reads it (the placeholders, the
    // TRACKS and the MASTER among them, are known and stay); then the keeper, from here on.
    SongOwnKeeper::adopt (*this, song);
    ClipsKeeper::adopt (*this, song);
    TempoMapKeeper::adopt (*this, song);
    ChordLaneKeeper::adopt (*this, song);
    MidiLinksKeeper::adopt (*this, song);
    CurvesKeeper::adopt (*this, song);
    SectionsKeeper::adopt (*this, song);
    ModulationKeeper::adopt (*this, song);
    SendsKeeper::adopt (*this, song);
    DeviceStoreKeeper::adopt (*this, song);
    DevicesKeeper::adopt (*this, song);
    TrackLeavesKeeper::adopt (*this, song);
    TracksKeeper::adopt (*this, song);
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
    return isHeardAsTempoMap (element)       // the song's TEMPOMAP, or an element in it
           || isHeardAsChordLane (element)   // the song's CHORDLANE, or an element in it
           || isHeardAsCurves (element)      // the song's AUTOMATION, or an element in it
           || isHeardAsMidiLinks (element)   // the song's MIDILINKS, or an element in it
           || isHeardAsSections (element)    // the song's SECTIONS, or an element in it
           || isHeardAsDevices (element)     // a DEVICE, a CONTAINER or a CHAIN, or an element in one (the safety net's)
           || isHeardAsTracks (element)      // the song's track list, a track, the master, a track's or the master's LANES
           || isHeardAsDeviceStore (element) // a plug-in's VALUE, or an element in it
           || isHeardAsSongOwn (element);    // the song's LASTKEY, PIANOROLL or MERGING
}

bool isHeardAsKept (const juce::ValueTree& element, const juce::Identifier& property)
{
    return isHeardAsTempoMap (element)                 // the song's TEMPOMAP, or an element in it
           || isHeardAsChordLane (element)             // the song's CHORDLANE, or an element in it
           || isHeardAsCurves (element)                // the song's AUTOMATION, or an element in it
           || isHeardAsMidiLinks (element)             // the song's MIDILINKS, or an element in it
           || isHeardAsSections (element)              // the song's SECTIONS, or an element in it
           || isHeardAsDevices (element, property)     // a DEVICE, a CONTAINER or a CHAIN, or an element in one
           || isHeardAsTracks (element, property)      // what the tracks' three values cover of a track, the master, a LANE
           || isHeardAsDeviceStore (element, property) // a store's property of a device or a chain, or one in a VALUE
           || isHeardAsSongOwn (element, property);    // the root's own or its count, or one of LASTKEY, PIANOROLL, MERGING
}

bool isHeardAsKept (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return isHeardAsTempoMap (parent, child)       // the TEMPOMAP itself, or a child of an element in it
           || isHeardAsChordLane (parent, child)   // a child of the CHORDLANE or of an element in it (not the CHORDLANE itself)
           || isHeardAsCurves (parent, child)      // the AUTOMATION itself, or a child of an element in it
           || isHeardAsMidiLinks (parent, child)   // a child of the MIDILINKS or of an element in it (not the MIDILINKS itself)
           || isHeardAsSections (parent, child)    // a child of the SECTIONS or of an element in it (not the SECTIONS itself)
           || isHeardAsDevices (parent, child)     // a device, a container, a chain, its VALUES or SOUNDS, or what is in one
           || isHeardAsTracks (parent, child)      // a track, the track list, the master, a LANES or a LANE added or taken out
           || isHeardAsDeviceStore (parent, child) // a plug-in's VALUE added or taken out, or what is in one
           || isHeardAsSongOwn (parent, child);    // the song's LASTKEY, PIANOROLL or MERGING added or taken out
}
} // namespace phazite
