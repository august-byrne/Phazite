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
#include "internal/TempoMapArea.h"

#include <memory>
#include <vector>

namespace phazite
{
//==============================================================================
// What keeps the areas and the tree together for every change of the tree (SongValues.h), by asking every area's part of it
// in turn, each doing nothing for what is not its own (ClipsArea.h, TempoMapArea.h, ChordLaneArea.h, CurvesArea.h,
// MidiLinksArea.h, SectionsArea.h, ModulationArea.h, SendsArea.h; and DevicesArea.h, which tells the devices' listener from
// the tree while the tree holds them, and changes nothing). It changes the areas and the tree through the song's undo manager
// from inside the tree's callbacks, which run inside the perform() of the tree's own action: JUCE refuses that only while it
// undoes or redoes (when the keeper does nothing), and adds the inner actions to the step before the outer one, so that Undo
// takes the tree's change back before the areas'. JUCE workaround W62 (docs/juce-workarounds.md).
struct SongValues::Keeper final : juce::ValueTree::Listener
{
    Keeper (SongValues& v, Song& s)
        : song (s),
          state (s.getState()),
          clips (v, s),
          tempoMap (v, s),
          chordLane (v, s),
          midiLinks (v, s),
          curves (v, s),
          sections (v, s),
          modulation (v, s),
          sends (v, s),
          deviceStore (v, s),
          devices (v, s)
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
    MidiLinksKeeper midiLinks;
    CurvesKeeper curves;
    SectionsKeeper sections;
    ModulationKeeper modulation;
    SendsKeeper sends;
    DeviceStoreKeeper deviceStore;
    DevicesKeeper devices; // tells the devices' listener from the tree, in an undo and a redo too, until the devices' switch

    bool isUndoingOrRedoing() const { return song.getUndoManager().isPerformingUndoRedo(); }

    // A property changed, and children in another order: the devices' alone, while the tree holds them (DevicesArea.h).
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        devices.propertyChanged (tree, property);
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { devices.childOrderChanged (parent); }

    // A child added: every area asked in turn, each doing nothing (and returning at once) for what is not its own, so that an
    // area whose elements sit inside another's (a track's, say) sees them come in with it. The tempo map's first (the song's
    // TEMPOMAP, or one in it, which holds nothing of a clip's: the clips' part finds no track there), then the chord lane's
    // (the song's CHORDLANE, or one in it, which holds nothing of the tempo map's or a clip's), then the MIDI links' (the
    // song's MIDILINKS, or one in it, which holds nothing of another area's), then the curves' (the song's AUTOMATION, or one
    // in it, which holds nothing of another area's), then the sections' (the song's SECTIONS, or one in it, which holds
    // nothing of another area's), then the clips' (a track, what holds tracks, a CLIP, in the song's track list: none of them
    // the tempo map's, the chord lane's, the MIDI links', the curves' or the sections'), then modulation's (a track, what holds
    // tracks, the master, a container, a MODULATOR or a MACRO, of the song's: a track's modulators are neither its clips nor
    // anything in them, so the clips' and modulation's each take their own out of a track that came in with both), then the
    // sends' (a track, what holds tracks, a SEND, in the song's track list: a track's sends are neither its clips nor its
    // modulators, so each takes its own out of a track that came in with all three), then the store's (what a DEVICE or a
    // CHAIN that came holds of the store, which no other area holds anything of). Before them the devices', in an undo and
    // a redo too, which tells what came, takes nothing in and changes nothing of the tree: so it may come first (a track that
    // comes in with devices, clips and modulators has its devices told as it comes, read alone, its clips and its modulators
    // left to the parts after it).
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        devices.childAdded (parent, child);

        if (isUndoingOrRedoing())
            return;

        tempoMap.childAdded (parent, child);
        chordLane.childAdded (parent, child);
        midiLinks.childAdded (parent, child);
        curves.childAdded (parent, child);
        sections.childAdded (parent, child);
        clips.childAdded (parent, child);
        modulation.childAdded (parent, child);
        sends.childAdded (parent, child);
        deviceStore.childAdded (parent, child);
    }

    // A child taken out: every area that acts there asked in turn, each doing nothing for what is not its own. The clips' (a
    // track, or what holds tracks), then modulation's (a track, what holds tracks, the master, a container), then the sends'
    // (a track, or what holds tracks): the tempo map, the chord lane, the MIDI links, the curves and the sections do nothing
    // as their elements leave. Before them the devices', in an undo and a redo too, which tells what went.
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        devices.childRemoved (parent, child);

        if (isUndoingOrRedoing())
            return;

        clips.childRemoved (parent, child);
        modulation.childRemoved (parent, child);
        sends.childRemoved (parent, child);
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
      clipsTeller (std::make_unique<ClipsTeller> (clips, listeners)),
      notesTeller (std::make_unique<NotesTeller> (notes, listeners)),
      tempoMapTeller (std::make_unique<TempoMapTeller> (tempoMap, listeners)),
      chordLaneTeller (std::make_unique<ChordLaneTeller> (chordLane, listeners)),
      midiLinksTeller (std::make_unique<MidiLinksTeller> (midiLinks, listeners)),
      curvesTeller (std::make_unique<CurvesTeller> (curves, listeners)),
      sectionsTeller (std::make_unique<SectionsTeller> (sections, listeners)),
      modulationTeller (std::make_unique<ModulationTeller> (modulation, listeners)),
      sendsTeller (std::make_unique<SendsTeller> (sends, listeners)),
      deviceStoreTeller (std::make_unique<DeviceStoreTeller> (*this, listeners))
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

bool SongValues::remembers (const juce::ValueTree& track) const
{
    return keeper != nullptr && keeper->clips.remembers (track);
}

std::vector<TrackID> SongValues::getRemembered() const
{
    return keeper != nullptr ? keeper->clips.getRemembered() : std::vector<TrackID>();
}

const SongDevices* SongValues::getDevicesAsTold() const noexcept
{
    return keeper != nullptr ? &keeper->devices.told : nullptr;
}

void SongValues::adopt (Song& song)
{
    // What the tree the loader made consistent holds of each area, into the area, outside the history, and out of the tree:
    // each area in turn, in this order (the clips with their notes, then the tempo map, then the chord lane, then the MIDI
    // links, then the curves, then the sections, then modulation, then the sends: none reads another's); then the keeper, from
    // here on.
    ClipsKeeper::adopt (*this, song);
    TempoMapKeeper::adopt (*this, song);
    ChordLaneKeeper::adopt (*this, song);
    MidiLinksKeeper::adopt (*this, song);
    CurvesKeeper::adopt (*this, song);
    SectionsKeeper::adopt (*this, song);
    ModulationKeeper::adopt (*this, song);
    SendsKeeper::adopt (*this, song);
    DeviceStoreKeeper::adopt (*this, song);
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
    return isHeardAsTempoMap (element)        // the song's TEMPOMAP, or an element in it
           || isHeardAsChordLane (element)    // the song's CHORDLANE, or an element in it
           || isHeardAsCurves (element)       // the song's AUTOMATION, or an element in it
           || isHeardAsMidiLinks (element)    // the song's MIDILINKS, or an element in it
           || isHeardAsSections (element)     // the song's SECTIONS, or an element in it
           || isHeardAsDevices (element)      // a CONTAINER or a CHAIN of the song's, or what is in a device's VALUES or SOUNDS
           || isHeardAsDeviceStore (element); // a plug-in's VALUE, or an element in it
}

bool isHeardAsKept (const juce::ValueTree& element, const juce::Identifier& property)
{
    return isHeardAsTempoMap (element)                  // the song's TEMPOMAP, or an element in it
           || isHeardAsChordLane (element)              // the song's CHORDLANE, or an element in it
           || isHeardAsCurves (element)                 // the song's AUTOMATION, or an element in it
           || isHeardAsMidiLinks (element)              // the song's MIDILINKS, or an element in it
           || isHeardAsSections (element)               // the song's SECTIONS, or an element in it
           || isHeardAsDevices (element, property)      // what the devices' value covers of a device, a container or a chain
           || isHeardAsDeviceStore (element, property); // a store's property of a device or a chain, or one in a VALUE
}

bool isHeardAsKept (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return isHeardAsTempoMap (parent, child)        // the TEMPOMAP itself, or a child of an element in it
           || isHeardAsChordLane (parent, child)    // a child of the CHORDLANE or of an element in it (not the CHORDLANE itself)
           || isHeardAsCurves (parent, child)       // the AUTOMATION itself, or a child of an element in it
           || isHeardAsMidiLinks (parent, child)    // a child of the MIDILINKS or of an element in it (not the MIDILINKS itself)
           || isHeardAsSections (parent, child)     // a child of the SECTIONS or of an element in it (not the SECTIONS itself)
           || isHeardAsDevices (parent, child)      // a device, a container or a chain added or taken out, or what is in one
           || isHeardAsDeviceStore (parent, child); // a plug-in's VALUE added or taken out, or what is in one
}
} // namespace phazite
