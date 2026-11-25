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
#include <atomic>
#include <cstdint>
#include <vector>

namespace phazite
{
namespace
{
    // Whether a child of an element in a copy of the song leads to what an area writes in: each area asked in
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
    // write after (its mode and view, its devices, its LANES, its modulators). Nothing for another element.
    void writeTracksIn (juce::ValueTree& element, const SongState& kept)
    {
        if (element.hasType (schema::MASTER))
        {
            writeMasterInto (element, kept.tracks.master);
            return;
        }

        if (! element.hasType (schema::TRACKS))
            return;

        if (const auto* list = kept.tracks.findList ({}))
            for (const auto id : *list)
                if (const auto* track = kept.tracks.tracks.find (id.getRaw()))
                    element.appendChild (writeTrackWhole (kept.tracks, *track, kept), nullptr);
    }

    // What the areas keep of a copy's element written in, each area in turn, a line each (the tracks' own fields, first: the
    // TRACKs of a TRACKS, each whole, and the MASTER's own properties: writeTracksIn(); a track's clips, first among its
    // elements, in the track's order, each with its notes: ClipsArea.h; the tempo map in the song's TEMPOMAP: TempoMapArea.h;
    // the chord lane in the song's CHORDLANE: ChordLaneArea.h; the MIDI links in the song's MIDILINKS: MidiLinksArea.h; the
    // curves in the song's AUTOMATION: CurvesArea.h), each of an element of its own, so that their order is no matter; then
    // the same of each element in it that leads to what an area keeps, found where the areas' elements are (the song's
    // element, its MASTER, its devices, its TEMPOMAP, its CHORDLANE, its MIDILINKS, its AUTOMATION).
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
        writeSongOwnIn (element, kept);     // the song's root's own properties and its count of IDs: SongOwnArea.h

        for (auto child : element)
            if (leadsToKept (element, child))
                writeIn (child, kept);
    }

    // The root's elements in the order the description gives them (Schema.h: those that go in PHAZITE, in the order they are
    // described), made once.
    const std::vector<juce::Identifier>& getRootOrder()
    {
        static const std::vector<juce::Identifier> order = []
        {
            std::vector<juce::Identifier> types;

            for (const auto* element : schema::getElements())
                if (element->goesIn (schema::PHAZITE))
                    types.push_back (*element);

            return types;
        }();

        return order;
    }

    // Whether a state's root holds an element of the places the areas write in (SongState.h): its TEMPOMAP, TRACKS and MASTER
    // always; its AUTOMATION, MIDILINKS and SECTIONS while they hold an entry; its CHORDLANE while the lane is placed. None for
    // the song's own elements (LASTKEY, PIANOROLL, MERGING), which its own writer writes (SongOwnArea.h: writeSongOwnIn()).
    bool holdsPlace (const SongState& state, const juce::Identifier& type)
    {
        if (type == schema::TEMPOMAP || type == schema::TRACKS || type == schema::MASTER)
            return true;

        if (type == schema::AUTOMATION)
            return ! state.curves.order.empty();

        if (type == schema::MIDILINKS)
            return ! state.midiLinks.links.empty();

        if (type == schema::SECTIONS)
            return ! state.sections.sections.empty();

        if (type == schema::CHORDLANE)
            return state.chordLane.placed;

        return false;
    }

    // Whole songs written as trees (writeSongTree()), and those of them on the message thread: countSongTreesWritten().
    std::atomic<int> treesWritten { 0 }, treesWrittenOnMessageThread { 0 };
} // namespace

//==============================================================================
juce::ValueTree copyWholeSong (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD
    return writeSongTree (getSongState (song));
}

SongState getSongState (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD
    SongState state;

    // Each area's value as it is now, a line each: a copy that shares what it holds.
    const auto& values = *song.getValues();
    state.notes = values.notes.get();
    state.clips = values.clips.get();
    state.tempoMap = values.tempoMap.get();
    state.chordLane = values.chordLane.get();
    state.midiLinks = values.midiLinks.get();
    state.curves = values.curves.get();
    state.sections = values.sections.get();
    state.modulation = values.modulation.get();
    state.sends = values.sends.get();
    state.deviceStore = values.deviceStore.get();
    state.devicesOutside = values.devicesOutside.get();
    state.devices = values.devices.get();
    state.trackSwitches = values.trackSwitches.get();
    state.tracksOutside = values.tracksOutside.get();
    state.tracks = values.tracks.get();
    state.songOwn = values.songOwn.get();
    state.songStanding = values.songStanding.get();
    state.songOutside = values.songOutside.get();
    state.nextID = song.getIDCount();
    return state;
}

juce::ValueTree writeSongTree (const SongState& state)
{
    // The root with the places the state holds, in the description's order; each area's elements written in (writeIn(): the
    // song's own appends its LASTKEY, PIANOROLL and MERGING); then every child of the root brought to its place in the
    // description's order, moved where it isn't (nothing allocated).
    ++treesWritten;

    if (juce::MessageManager::existsAndIsCurrentThread())
        ++treesWrittenOnMessageThread;

    juce::ValueTree root (schema::PHAZITE);
    const auto& order = getRootOrder();

    for (const auto& type : order)
        if (holdsPlace (state, type))
            root.appendChild (juce::ValueTree (type), nullptr);

    writeIn (root, state);
    auto placed = 0;

    for (const auto& type : order)
    {
        for (auto i = placed; i < root.getNumChildren(); ++i)
        {
            if (root.getChild (i).hasType (type))
            {
                if (i != placed)
                    root.moveChild (i, placed, nullptr);

                ++placed;
                break;
            }
        }
    }

    return root;
}

juce::ValueTree writeSongPart (const SongState& state, const juce::Identifier& type)
{
    if (! holdsPlace (state, type))
        return {};

    juce::ValueTree element (type);
    writeIn (element, state);
    return element;
}

SongState readSongTree (const juce::ValueTree& consistent)
{
    // Each area's reader, a line each, in the order of the areas, the song's own (with its count of IDs) first: each reads its
    // elements as the song holds them, and changes no tree (none reads another's).
    std::uint64_t nextPointKey = 1;
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
    return state;
}

juce::ValueTree copyWholeClip (const Song& song, ClipID clip)
{
    const auto* values = song.getValues();

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

    for (const auto& entry : a.tracks.tracks)
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
