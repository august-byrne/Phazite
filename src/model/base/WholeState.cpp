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

    void writeIn (juce::ValueTree element, const ValuesKept& kept);

    // A track of the kept values as a TRACK that holds everything the song holds of it: its own properties first
    // (TracksValue.h: writeTrackElement()), a folder's tracks in it each written whole the same, in its list's order; then
    // what the areas keep of it written in around them (writeIn(): its clips first among its children, its devices and
    // containers after them, before a folder's tracks; its switches and view, its LANES after those; its modulators and
    // macros; its sends, the last).
    juce::ValueTree writeTrackWhole (const SongTracks& tracks, const TrackValue& track, const ValuesKept& kept)
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
    void writeTracksIn (juce::ValueTree& element, const ValuesKept& kept)
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
    void writeIn (juce::ValueTree element, const ValuesKept& kept)
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
} // namespace

juce::ValueTree copyWholeSong (const Song& song)
{
    return copyWhole (song, song.getState());
}

juce::ValueTree copyWhole (const Song& song, const juce::ValueTree& part)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto copy = part.createCopy();

    if (song.getValues() != nullptr && (part == song.getState() || part.isAChildOf (song.getState())))
        writeIn (copy, keepValues (song));

    return copy;
}

juce::ValueTree copyWholeClip (const Song& song, ClipID clip)
{
    if (const auto* values = song.getValues())
        if (const auto* value = values->findClip (clip))
            return writeClipElement (*value, values->findClipNotes (clip));

    return {};
}

ValuesKept keepValues (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD
    ValuesKept kept;

    // Each area's value as it is now, a line each.
    if (const auto* values = song.getValues())
    {
        kept.notes = values->notes.get();
        kept.clips = values->clips.get();
        kept.tempoMap = values->tempoMap.get();
        kept.chordLane = values->chordLane.get();
        kept.midiLinks = values->midiLinks.get();
        kept.curves = values->curves.get();
        kept.sections = values->sections.get();
        kept.modulation = values->modulation.get();
        kept.sends = values->sends.get();
        kept.deviceStore = values->deviceStore.get();
        kept.devicesOutside = values->devicesOutside.get();
        kept.devices = values->devices.get();
        kept.trackSwitches = values->trackSwitches.get();
        kept.tracksOutside = values->tracksOutside.get();
        kept.tracks = values->tracks.get();
        kept.songOwn = values->songOwn.get();
        kept.songStanding = values->songStanding.get();
        kept.songOutside = values->songOutside.get();
        kept.nextID = song.getIDCount();
    }

    return kept;
}

juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept& kept)
{
    auto copy = treeCopy.createCopy();
    writeIn (copy, kept);
    return copy;
}

void makeWhole (juce::ValueTree& written, const ValuesKept& kept)
{
    writeIn (written, kept);
}

bool holdSameKeptOf (const juce::ValueTree& element, const ValuesKept& kept, const juce::ValueTree& otherElement,
                     const ValuesKept& otherKept)
{
    // Each area in turn, a line each, in this order (a track's clips, then the tempo map, then the chord lane, then the
    // curves, then the MIDI links), as each is compared once the element's own properties are.
    if (! holdSameClipsOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameTempoMapOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameChordLaneOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameCurvesOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameMidiLinksOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameSectionsOf (element, kept, otherElement, otherKept)) // then the sections
        return false;

    if (! holdSameModulationOf (element, kept, otherElement, otherKept)) // then an owner's modulators and macros
        return false;

    if (! holdSameSendsOf (element, kept, otherElement, otherKept)) // then a track's sends
        return false;

    if (! holdSameDevicesOf (element, kept, otherElement, otherKept)) // then a channel's devices, with what each holds
        return false;

    if (! holdSameDeviceStoreOf (element, kept, otherElement, otherKept)) // then what a device or a chain stores
        return false;

    if (! holdSameTrackSwitchesOf (element, kept, otherElement, otherKept)) // then a track's switches, the master's mode
        return false;

    if (! holdSameTracksOf (element, kept, otherElement, otherKept)) // then the tracks whole, the master's value and mode
        return false;

    if (! holdSameSongOwnOf (element, kept, otherElement, otherKept)) // then the song's own, of a song's root
        return false;

    return true;
}

bool isPlaceOfKept (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    // Each area whose place in the tree is an element of its own, a line each: the tempo map's TEMPOMAP, the chord lane's
    // CHORDLANE; the curves' AUTOMATION; the MIDI links' MIDILINKS. The clips' are none: the copy's tree holds none of them.
    if (isTempoMapPlace (parent, child))
        return true;

    if (isChordLanePlace (parent, child))
        return true;

    if (isCurvesPlace (parent, child))
        return true;

    if (isMidiLinksPlace (parent, child))
        return true;

    if (isSectionsPlace (parent, child)) // the sections' SECTIONS
        return true;

    return false;
}
} // namespace phazite
