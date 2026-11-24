// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/WholeState.h"
#include "Song.h"
#include "internal/ChordLaneArea.h"
#include "internal/ClipsArea.h"
#include "internal/CurvesArea.h"
#include "internal/DeviceStoreArea.h"
#include "internal/DevicesArea.h"
#include "internal/MidiLinksArea.h"
#include "internal/ModulationArea.h"
#include "internal/Schema.h"
#include "internal/SectionsArea.h"
#include "internal/SendsArea.h"
#include "internal/SongOwnArea.h"
#include "internal/TempoMapArea.h"
#include "internal/TracksArea.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace phazite
{
namespace
{
    // Whether a child of an element in a copy of the song leads to what an area keeps beside the tree: each area asked in
    // turn, a line each (the song's TEMPOMAP, for the tempo map; the song's CHORDLANE, for the chord lane; the song's
    // MIDILINKS, for the MIDI links; the song's AUTOMATION, for the curves). Never a TRACK: each is written whole where it is
    // put in its TRACKS or its folder (writeTracksIn()).
    bool leadsToKept (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        if (leadsToTracks (parent, child)) // the song's TRACKS and MASTER, for the tracks
            return true;

        if (leadsToTempoMap (parent, child))
            return true;

        if (leadsToChordLane (parent, child))
            return true;

        if (leadsToMidiLinks (parent, child))
            return true;

        if (leadsToCurves (parent, child))
            return true;

        if (leadsToSections (parent, child)) // the song's SECTIONS, for the sections
            return true;

        if (leadsToModulation (parent, child)) // the master or a channel's container, for modulation
            return true;

        if (leadsToDeviceStore (parent, child)) // the master, a channel's devices, a container's chains, a chain's devices: the store
            return true;

        if (leadsToDevices (parent, child)) // the master, for the devices
            return true;

        return false;
    }

    void writeIn (juce::ValueTree element, const SongState& kept);

    // A track of the kept values as a TRACK that holds everything the song holds of it: its own properties first
    // (TracksValue.h: writeTrackElement()), a folder's tracks in it each written whole the same, in its list's order; then
    // what the areas keep of it written in around them (writeIn(): its clips first among its children, its devices and
    // containers after them, before a folder's tracks; its switches and view, its LANES after those; its modulators and
    // macros; its sends, the last).
    juce::ValueTree writeTrackWhole (const SongTracks& tracks, const TrackValue& track, const SongState& kept)
    {
        auto element = writeTrackElement (track);

        if (const auto* list = tracks.findList (track.id))
            for (const auto id : *list)
                if (const auto* inside = tracks.tracks.find (id.getRaw()))
                    element.appendChild (writeTrackWhole (tracks, *inside, kept), nullptr);

        writeIn (element, kept);
        return element;
    }

    // The tracks' part of whole state: a copy's TRACKS given every track of the top list, each written whole, in the list's
    // order; its MASTER the master's own properties (TracksValue.h: writeMasterInto()), which the other areas' parts then
    // write after (its mode and view, its devices, its LANES, its modulators). Nothing for another element, nor where the
    // values kept hold no tracks (a song without values: its tree holds them).
    void writeTracksIn (juce::ValueTree& element, const SongState& kept)
    {
        if (! kept.tracks.has_value())
            return;

        if (element.hasType (schema::MASTER))
        {
            writeMasterInto (element, kept.tracks->master);
            return;
        }

        if (! element.hasType (schema::TRACKS))
            return;

        if (const auto* list = kept.tracks->findList ({}))
            for (const auto id : *list)
                if (const auto* track = kept.tracks->tracks.find (id.getRaw()))
                    element.appendChild (writeTrackWhole (*kept.tracks, *track, kept), nullptr);
    }

    // What the areas keep of a copy's element written in, each area in turn, a line each (the tracks' own fields, first: the
    // TRACKs of a TRACKS, each whole, and the MASTER's own properties: writeTracksIn(); a track's clips, first among its
    // elements, in the track's order, each with its notes: ClipsArea.h; the tempo map in the song's TEMPOMAP, the place the
    // song keeps for it: TempoMapArea.h; the chord lane in the song's CHORDLANE: ChordLaneArea.h; the MIDI links in the song's
    // MIDILINKS: MidiLinksArea.h; the curves in the song's AUTOMATION, which a song without curves is written without:
    // CurvesArea.h), each of an element of its own, so that their order is no matter; then the same of each element in it
    // that leads to what an area keeps, found where the areas' elements are (the song's element, its MASTER, its devices,
    // its TEMPOMAP, its CHORDLANE, its MIDILINKS, its AUTOMATION).
    void writeIn (juce::ValueTree element, const SongState& kept)
    {
        writeTracksIn (element, kept);
        writeClipsIn (element, kept);
        writeDevicesIn (element, kept); // a TRACK's or the MASTER's devices and containers, right after its clips, each with
                                        // its chains and their devices, so that the parts below write into them: DevicesArea.h
        writeTempoMapIn (element, kept);
        writeChordLaneIn (element, kept);
        writeMidiLinksIn (element, kept);
        writeCurvesIn (element, kept);
        writeSectionsIn (element, kept);    // the sections in the song's SECTIONS: SectionsArea.h
        writeTrackLeavesIn (element, kept); // a TRACK's or the MASTER's switches and view, its LANES after its clips, devices
                                            // and a folder's tracks, before what the next two add: TracksArea.h
        writeModulationIn (element, kept);  // a TRACK's, the MASTER's or a CONTAINER's modulators and macros: ModulationArea.h
        writeSendsIn (element, kept);       // a TRACK's sends, after its modulators and macros (the last of its
                                            // children): SendsArea.h
        writeDeviceStoreIn (element, kept); // a plug-in's DEVICE's store (its VALUEs after its other children), a CHAIN's
                                            // mute and solo, a DEVICE's window's place: DeviceStoreArea.h
        writeSongOwnIn (element, kept);     // the song's root's own, its LASTKEY, PIANOROLL and MERGING after its other
                                            // children: SongOwnArea.h

        for (auto child : element)
            if (leadsToKept (element, child))
                writeIn (child, kept);
    }

    // The places a root holds, in its order, each once (its first of a type): what a state names (SongState::rootPlaces).
    std::array<RootPlace, 7> findPlaces (const juce::ValueTree& root)
    {
        std::array<RootPlace, 7> places {};
        size_t count = 0;

        for (const auto& child : root)
        {
            const auto place = findRootPlace (child.getType());

            if (place == RootPlace::none || count == places.size())
                continue;

            if (const auto end = places.begin() + (std::ptrdiff_t) count; std::find (places.begin(), end, place) == end)
                places[count++] = place;
        }

        return places;
    }

    // Whole songs written as trees (writeSongTree()), and those of them on the message thread: countSongTreesWritten().
    std::atomic<int> treesWritten { 0 }, treesWrittenOnMessageThread { 0 };

    // A place's element as whole state writes it, empty: what the areas' writers write into (writeIn()).
    juce::ValueTree makePlace (RootPlace place)
    {
        return juce::ValueTree (getRootPlaceType (place));
    }
} // namespace

//==============================================================================
const juce::Identifier& getRootPlaceType (RootPlace place)
{
    switch (place)
    {
        case RootPlace::tempoMap:
            return schema::TEMPOMAP;
        case RootPlace::tracks:
            return schema::TRACKS;
        case RootPlace::master:
            return schema::MASTER;
        case RootPlace::automation:
            return schema::AUTOMATION;
        case RootPlace::midiLinks:
            return schema::MIDILINKS;
        case RootPlace::sections:
            return schema::SECTIONS;
        case RootPlace::chordLane:
            return schema::CHORDLANE;
        case RootPlace::none:
            break;
    }

    static const juce::Identifier none;
    return none;
}

RootPlace findRootPlace (const juce::Identifier& type)
{
    for (const auto place : { RootPlace::tempoMap, RootPlace::tracks, RootPlace::master, RootPlace::automation, RootPlace::midiLinks,
                              RootPlace::sections, RootPlace::chordLane })
        if (type == getRootPlaceType (place))
            return place;

    return RootPlace::none;
}

bool SongState::holdsPlace (RootPlace place) const noexcept
{
    for (const auto each : rootPlaces)
        if (each == place)
            return place != RootPlace::none;

    return false;
}

//==============================================================================
juce::ValueTree copyWholeSong (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.getValues() == nullptr)
        return song.getState().createCopy(); // a song that keeps everything in its tree

    return writeSongTree (getSongState (song));
}

juce::ValueTree copyWhole (const Song& song, const juce::ValueTree& part)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto root = song.getState();

    if (song.getValues() != nullptr)
    {
        if (part == root)
            return copyWholeSong (song);

        // One of the root's places (the song's tree holds nothing else: each empty), written from the state.
        if (part.getParent() == root)
            if (const auto place = findRootPlace (part.getType());
                place != RootPlace::none && root.getChildWithName (part.getType()) == part)
                return writeSongPart (getSongState (song), place);

        jassert (! part.isAChildOf (root)); // nothing else of a song that holds its values is in its tree
    }

    return part.createCopy();
}

SongState getSongState (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD
    SongState state;

    // Each area's value as it is now, a line each: a copy that shares what it holds.
    if (const auto* values = song.getValues())
    {
        state.notes = values->notes.get();
        state.clips = values->clips.get();
        state.tempoMap = values->tempoMap.get();
        state.chordLane = values->chordLane.get();
        state.midiLinks = values->midiLinks.get();
        state.curves = values->curves.get();
        state.sections = values->sections.get();
        state.modulation = values->modulation.get();
        state.sends = values->sends.get();
        state.deviceStore = values->deviceStore.get();
        state.devicesOutside = values->devicesOutside.get();
        state.devices = values->devices.get();
        state.trackSwitches = values->trackSwitches.get();
        state.tracksOutside = values->tracksOutside.get();
        state.tracks = values->tracks.get();
        state.songOwn = values->songOwn.get();
        state.songStanding = values->songStanding.get();
        state.songOutside = values->songOutside.get();
        state.nextID = song.getIDCount();
    }

    // The places its root holds, in their order (a few children of the root, each empty), read from its tree.
    state.rootPlaces = findPlaces (song.getState());
    return state;
}

juce::ValueTree writeSongTree (const SongState& state)
{
    // The root, its places in the state's order (its CHORDLANE as its lane is placed), then those every song holds the
    // areas' values in that the state names none of (its TEMPOMAP; its AUTOMATION while it holds a curve), and its lane where
    // the state's order names none for it: written as the adoption would have put them (each appended); then each area's
    // elements, as whole state wrote them into a copy of the song's tree (writeIn()). A state of a song that holds its values
    // (getSongState() of one, readSongTree()): one of a song without values names no tracks, and would write an empty song.
    jassert (state.tracks.has_value());
    ++treesWritten;

    if (juce::MessageManager::existsAndIsCurrentThread())
        ++treesWrittenOnMessageThread;

    juce::ValueTree root (schema::PHAZITE);

    for (const auto place : state.rootPlaces)
        if (place != RootPlace::none && (place != RootPlace::chordLane || state.chordLane.placed))
            root.appendChild (makePlace (place), nullptr);

    if (! state.holdsPlace (RootPlace::tempoMap))
        root.appendChild (makePlace (RootPlace::tempoMap), nullptr);

    if (! state.holdsPlace (RootPlace::automation) && ! state.curves.order.empty())
        root.appendChild (makePlace (RootPlace::automation), nullptr);

    if (! state.holdsPlace (RootPlace::chordLane) && state.chordLane.placed)
        root.appendChild (makePlace (RootPlace::chordLane), nullptr);

    writeIn (root, state);
    return root;
}

juce::ValueTree writeSongPart (const SongState& state, RootPlace place)
{
    if (place == RootPlace::none || ! state.holdsPlace (place) || (place == RootPlace::chordLane && ! state.chordLane.placed))
        return {};

    auto element = makePlace (place);
    writeIn (element, state);
    return element;
}

SongState readSongTree (const juce::ValueTree& consistent)
{
    std::uint64_t nextPointKey = 1;
    return readSongTree (consistent, nextPointKey);
}

SongState readSongTree (const juce::ValueTree& consistent, std::uint64_t& nextPointKey)
{
    // Each area's reader, a line each, in the order of the areas, the song's own (with its count of IDs) first: each reads its
    // elements as the song holds them, and changes no tree (none reads another's).
    SongState state;
    readHeldSongOwnIn (consistent, state);
    readHeldClipsIn (consistent, state);
    readHeldTempoMapIn (consistent, state);
    readHeldChordLaneIn (consistent, state);
    readHeldMidiLinksIn (consistent, state);
    readHeldCurvesIn (consistent, state, nextPointKey);
    readHeldSectionsIn (consistent, state);
    readHeldModulationIn (consistent, state);
    readHeldSendsIn (consistent, state);
    readHeldDeviceStoreIn (consistent, state);
    readHeldDevicesIn (consistent, state);
    readHeldTrackLeavesIn (consistent, state);
    readHeldTracksIn (consistent, state);
    state.rootPlaces = findPlaces (consistent);
    return state;
}

juce::ValueTree copyWholeClip (const Song& song, ClipID clip)
{
    if (const auto* values = song.getValues())
        if (const auto* value = values->findClip (clip))
            return writeClipElement (*value, values->findClipNotes (clip));

    return {};
}

juce::ValueTree writeDeviceTree (const SongState& state, DeviceID id)
{
    // Its element with what it holds (a container's chains, a chain's devices), then what the other areas hold of each written
    // in (writeIn(): its store's, its window's place, a container's macros), as whole state writes it into a song.
    auto element = writeDeviceWhole (state.devices, id);

    if (element.isValid())
        writeIn (element, state);

    return element;
}

SongTreesWritten countSongTreesWritten()
{
    return { .all = treesWritten.load(), .onMessageThread = treesWrittenOnMessageThread.load() };
}

namespace
{
    // An owner's modulators and macros (a channel's, a container's) in two states: the same IDs in the same order, each the
    // same (ModulationArea.h's comparisons), as the file holds them under their owner's element.
    bool holdSameOwned (const SongState& a, const SongState& b, const ModulatorPlace& owner)
    {
        if (! holdSameModulatorOrder (a, b, owner))
            return false;

        const auto* list = a.modulation.findList (owner);
        return list == nullptr
               || std::all_of (list->begin(), list->end(), [&a, &b] (ModulatorID id) { return holdSameModulator (a, b, id); });
    }

    // A track's sends in two states: the same IDs in the same order, each the same (SendsArea.h's comparisons).
    bool holdSameTrackSends (const SongState& a, const SongState& b, TrackID track)
    {
        if (! holdSameTrackSendOrder (a, b, track))
            return false;

        const auto* list = a.sends.tracks.find (track.getRaw());
        return list == nullptr || std::all_of (list->begin(), list->end(), [&a, &b] (DeviceID id) { return holdSameSend (a, b, id); });
    }
} // namespace

bool holdSameChannels (const SongState& a, const SongState& b)
{
    // The tracks whole first (their values, lists and the master's): the same, both hold the same tracks.
    if (! holdSameTracks (a, b))
        return false;

    if (! a.tracks.has_value())
        return true; // neither holds tracks (states of songs that hold no values)

    // Each area that shares its roots with the other state's the same without a look inside: none walked at all where every
    // one does (a session that changed none of them since the save).
    const auto sameClips = a.clips.isSameAs (b.clips) && a.notes.isSameAs (b.notes);
    const auto sameModulation = a.modulation.isSameAs (b.modulation);
    const auto sameSends = a.sends.isSameAs (b.sends);
    const auto sameDevices = a.devices.isSameAs (b.devices) && a.deviceStore.isSameAs (b.deviceStore);
    const auto sameSwitches = a.trackSwitches.isSameAs (b.trackSwitches);

    if (sameClips && sameModulation && sameSends && sameDevices && sameSwitches)
        return true;

    // A channel's (a track's, by its ID; the master's): what each area holds of it, a line each.
    const auto holdSameChannel = [&] (TrackID track, bool onMaster)
    {
        return (onMaster || sameClips || holdSameTrackClips (a, b, track))
               && (sameModulation || holdSameOwned (a, b, { .track = track, .onMaster = onMaster }))
               && (onMaster || sameSends || holdSameTrackSends (a, b, track))
               && (sameDevices || holdSameChannelDevices (a, b, makeChannelPlace (track, onMaster)))
               && (sameSwitches || holdSameSwitchesOf (a.trackSwitches, b.trackSwitches, onMaster ? 0 : track.getRaw()));
    };

    for (const auto& entry : a.tracks->tracks)
        if (! holdSameChannel (TrackID::fromRaw (entry.first), false))
            return false;

    if (! holdSameChannel ({}, true))
        return false;

    // Each container's macros (its owner's list: a channel's devices, the same in both, hold the same containers).
    if (! sameModulation)
        for (const auto& entry : a.devices.devices)
            if (const auto& device = entry.second; device.form == DeviceValue::Form::container)
                if (! holdSameOwned (a, b, { .track = device.place.track, .onMaster = device.place.onMaster, .container = device.id }))
                    return false;

    return true;
}

bool holdSameSong (const SongState& a, const SongState& b)
{
    // Each area in turn, a line each, in this order, the cheapest first: each by identity first.
    return holdSameSongOwn (a, b)      // the song's own value and its key set last
           && holdSameTempoMap (a, b)  // the tempo map, and its place
           && holdSameChordLane (a, b) // the chord lane, and whether the song has one
           && holdSameCurves (a, b)    // the curves, in their order
           && holdSameMidiLinks (a, b) // the MIDI links, and whether the song has their place
           && holdSameSections (a, b)  // the sections, and whether the song has their place
           && holdSameChannels (a, b); // the tracks and the master, with what each holds
}
} // namespace phazite
