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
#include "internal/SongState.h"
#include "internal/TempoMapArea.h"
#include "internal/TracksArea.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace phazite
{
Song::Song (SongState state)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // By steps alone: JUCE weighs each change by the size of its record, not by what it holds, so a budget of units kept as
    // few as 30 steps after a drag of many notes.
    undoManager.stepsKept = getUndoSteps();
    undoManager.keepMoreSteps (0);

    values = std::make_unique<SongValues> (*this);
    values->takeIn (*this, std::move (state));
}

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
    // Each area with its teller, in the order of the areas; their values come from the state the song is made of (takeIn()).
    // macro's statement ended again here, so that the formatter reads a body that holds nothing else.
    JUCE_ASSERT_MESSAGE_THREAD;
}

SongValues::~SongValues()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Each area's teller, in the reverse of their making.
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

void SongValues::takeIn (Song& song, SongState state)
{
    // The next point key past every key the state's curves hold (a state read from a file's tree gives its points keys from 1,
    // one taken from another song keeps that song's), so that no point the song makes takes one of them.
    for (const auto& entry : state.curves.curves)
        for (const auto& point : entry.second.points)
            nextPointKey = std::max (nextPointKey, point.key + 1);

    // Each area's value, a line each, outside the history: the song's own and its count of IDs first, the tracks last.
    songOwn.setOutsideHistory (std::move (state.songOwn));
    songStanding.setOutsideHistory (std::move (state.songStanding));
    songOutside.set (std::move (state.songOutside));
    song.setIDCount (state.nextID);
    clips.setOutsideHistory (std::move (state.clips));
    notes.setOutsideHistory (std::move (state.notes));
    tempoMap.setOutsideHistory (std::move (state.tempoMap));
    chordLane.setOutsideHistory (std::move (state.chordLane));
    midiLinks.setOutsideHistory (std::move (state.midiLinks));
    curves.setOutsideHistory (std::move (state.curves));
    sections.setOutsideHistory (std::move (state.sections));
    modulation.setOutsideHistory (std::move (state.modulation));
    sends.setOutsideHistory (std::move (state.sends));
    deviceStore.setOutsideHistory (std::move (state.deviceStore));
    devicesOutside.set (std::move (state.devicesOutside));
    devices.setOutsideHistory (std::move (state.devices));
    trackSwitches.setOutsideHistory (std::move (state.trackSwitches));
    tracksOutside.set (std::move (state.tracksOutside));
    tracks.setOutsideHistory (std::move (state.tracks));
}
} // namespace phazite
