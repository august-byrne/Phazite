// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tracks as values (roadmap step S5.20, the tracks' area; its first piece): each track, folder and return
// (Schema.h: TRACK) and the master (MASTER) as a TrackValue, its fields that are written in the undo history alone; every
// one of a song (SongTracks) by its ID, with each folder's list and the top list that order them, the master a member, and
// the helpers that alone write it; the song's order made of those lists (TrackOrder); and the conversion between those
// elements and the values, the one place that reads and writes a TRACK's and the MASTER's own properties by name. The
// song's tree is still the storage: the area's readers read the tree through this conversion (model/arrangement/internal/
// Tracks.h: getTracksValue(), findTrack(), and for one field of one track, the readers of one field below, on the element
// the song's index found), and the song holds no area of it yet (SongValues.h).
//
// What the value covers, and what it doesn't. A TRACK's trackID, trackKind, trackName, trackColour, folderBus,
// channelVolume, channelPan, channelFader, channelVCA, keyRoot, keyScale, trackInput, trackMonitoring, trackPadDevice and
// trackPad; the MASTER's trackID, channelFader, channelVCA, channelVolume and channelPan (and, as Phazite's code always
// read them, whatever else of those a MASTER holds: readTrackValue()); and where each track is (its folder, and its place in the
// folder's list or the top list). Not in it, and left alone by the conversion (a written element holds none of them): a
// track's mute, solo, frozen switch and automation mode, written in the undo history and outside it alike, which are a keyed
// value of their own (TrackSwitchesValue.h); its view (its height, collapsed, pinned, drum rows and lanes), written outside
// the history alone (TracksOutside.h); the elements in it (its clips, devices, modulators, sends: other areas'; a folder's
// tracks: the lists); and what Phazite doesn't know.
//
// Its own header, beside SongValues.h and the other areas' (one header for each area's values). It includes the notes'
// plain values (model/notes/NoteValues.h) for a track's key, a header that includes only base's. immer is the model's alone
// (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/notes/NoteValues.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace phazite
{
// A track, a folder, a return or the master as the song holds it: its fields written in the undo history alone. One type
// for the four, so that what reads "a track or the master" reads one (a fader, a volume, a pan).
struct TrackValue
{
    // What it is (Schema.h: trackKind; the master is its own element, MASTER). Not TrackKind: IDs.h names TrackID's tag so.
    enum class Kind : std::uint8_t
    {
        track,       // holds clips
        folder,      // holds tracks
        returnTrack, // takes in what tracks send to it
        master
    };

    // When its input is heard through it (Schema.h: trackMonitoring): while it is armed (none), always or never.
    enum class Monitoring : std::uint8_t
    {
        whileArmed,
        always,
        never
    };

    TrackID id {};           // trackID
    Kind kind = Kind::track; // trackKind; the master's kind master
    TrackID parent {};       // the folder it is in; none at the top, and for the master

    // trackName: none, or its own (empty too, as renaming a track to nothing writes it: no other reader tells the two apart,
    // but the versions compare the text, so it is held as written). None or empty: its number ("Track 3").
    std::optional<juce::String> name {};

    // trackColour as an ARGB value, none for its row's in the palette: what juce::Colour::fromString() reads of its text, but
    // none where that is transparent (which a text that is no colour reads as), as Phazite's code always took it.
    std::optional<juce::uint32> colour {};

    bool bus = false;    // folderBus: a folder made a bus with the bus switch
    float volume = 0.0f; // channelVolume, in decibels: defaultVolume (kind) where the element has none
    float pan = 0.0f;    // channelPan; a plain folder keeps it
    DeviceID fader {};   // channelFader: a track's, a return's, a bus's, the master's
    DeviceID vca {};     // channelVCA: a plain folder's

    std::optional<Key> key {};                      // keyRoot and keyScale: none without a root; major for a scale it hasn't
    juce::String input {};                          // trackInput: "1", "1+2"; empty for none
    Monitoring monitoring = Monitoring::whileArmed; // trackMonitoring
    DeviceID padDevice {};                          // trackPadDevice: the drum sampler a pad's own track plays a pad of
    std::optional<int> pad {};                      // trackPad: the pad's note

    bool isFolder() const noexcept { return kind == Kind::folder; }
    bool isReturn() const noexcept { return kind == Kind::returnTrack; }
    bool isMaster() const noexcept { return kind == Kind::master; }

    // Whether it holds clips: a track, not a folder, a return or the master.
    bool holdsClips() const noexcept { return kind == Kind::track; }

    // A folder whose volume a fader of its own sets, not a VCA (model/mixer/internal/Buses.h: isBus()).
    bool isBus() const noexcept { return kind == Kind::folder && fader.isValid(); }

    // The device that sets its volume: its VCA where it has one, else its fader (model/mixer/internal/Mixer.h: getFader()).
    DeviceID getVolumeDevice() const noexcept { return vca.isValid() ? vca : fader; }

    // Its name as its element holds it, empty for none.
    juce::String getOwnName() const { return name.value_or (juce::String()); }

    // The volume where its element has none: 0 dB, or for the master -3 dB, a new song's level.
    static float defaultVolume (Kind) noexcept;

    // The master as a song without a MASTER has it: kind master, at its default volume, without an ID.
    static TrackValue makeMaster();

    // Alike, compared (the floats exactly, as they are saved).
    bool operator== (const TrackValue&) const;
};

// Every track, folder and return of the song by its ID, each folder's list of the tracks in it, in their order, by the
// folder's ID, the top list under 0 (TrackID 0 names none: a list under it is no folder's), and the master, which is in no
// list. A list holds at least one track (an empty folder has none); the helpers below are the only writers of the three,
// and keep them in step: holdsItsLists() says whether they are.
struct SongTracks
{
    immer::map<std::uint64_t, TrackValue> tracks {};
    immer::map<std::uint64_t, immer::flex_vector<TrackID>> lists {};
    TrackValue master = TrackValue::makeMaster();

    // One by its ID (the master's for its ID), or nullptr: a pointer into this value, valid while it lives: keep the value in
    // a local of its own, never a temporary's.
    const TrackValue* find (TrackID) const noexcept;

    // A folder's list, the top list for none (TrackID()), or nullptr where it holds none.
    const immer::flex_vector<TrackID>* findList (TrackID folder) const noexcept;

    // The maps without a look inside (ValueArea.h), and the master alike; and alike, compared.
    bool isSameAs (const SongTracks&) const noexcept;
    bool operator== (const SongTracks&) const;
};

// A track added in its folder's list (its parent's, the top list for none) at a place (from 0, or -1 for after the others),
// changed in place where its folder stays, or moved to its new folder's list at the place given (or its end), the tracks in
// it with it; the master set for a value of kind master (which goes in no list). Refused, the value given back as it was
// (SongTracks::isSameAs() tells the caller): a track without an ID, or of the master's; one whose folder is none the value
// holds as a folder; a folder moved into itself or into a folder in it; and a folder made a track or a return while it holds
// tracks (their list names it).
SongTracks withTrack (const SongTracks&, TrackValue, int index = -1);

// One taken out of its list, with the tracks in it (a folder's) and their lists; nothing for the master or one it hasn't.
SongTracks withoutTrack (const SongTracks&, TrackID);

// A list in another order: the same IDs (else nothing changes). A folder's, the top list for none.
SongTracks withListOrder (const SongTracks&, TrackID folder, immer::flex_vector<TrackID> order);

// Whether the three agree: each track under its own ID, of a kind but master, once in one list, the list its folder's ID
// names; every folder a track names one the value holds as a folder; no folder in itself or in a folder in it; no list
// empty, nor without its folder (but the top list); the master of kind master, in no list, under no folder, its ID none of
// a track's; and no ID twice among the tracks', the faders' and the VCAs' (the master's too). For the tests, and the
// assertions where it is cheap.
bool holdsItsLists (const SongTracks&);

//==============================================================================
// The tracks in the song's order (each folder before the tracks in it, top to bottom, not the master) and each one's place
// there: its index, and its number among those that aren't folders, from 1, as a track without a name of its own is called
// ("Track 3"), 0 for a folder. Made from a value's lists: what the song's index keeps today (SongIndex.h: getTracks(),
// getTrackPlace()). Message thread.
struct TrackOrder
{
    struct Place
    {
        int index = -1, number = 0;
        bool operator== (const Place&) const = default;
    };

    std::vector<TrackID> order {};
    std::unordered_map<std::uint64_t, Place> places {};

    // A track's place, none (-1 and 0) for one that isn't in the order.
    Place getPlace (TrackID) const;
};

TrackOrder makeTrackOrder (const SongTracks&);

//==============================================================================
// The conversion: the one place that reads and writes TRACK's and MASTER's own properties by name (what else the elements
// hold, and whose it is: the top of this header). Numbers as the song reads one (SongText.h: readValueNumber(),
// readDescribed()), never a cast.

// A track or the master read from its element as Phazite's code always read it (model/arrangement/Tracks.cpp,
// model/mixer/Mixer.cpp's channels, Buses.cpp, model/notes/Notes.cpp's key, model/transport/RecordingModel.cpp before the
// value): its ID and its pad's device as a cast reads them (0 for what is none); its kind by its word (a folder, a return,
// else a track; the master by its element); its folder the TRACK it is in (none for the master, wherever it is); its name
// where it has one; its colour as juce::Colour::fromString() reads it, none for a transparent one; its bus switch, a cast's;
// its volume and pan as a number reads (juce::var's, a text's leading number), each held within a float's range, its
// default for none and for what is no finite number; its fader and VCA as its ID; a key where it has a root, the root as an
// int truncates it within 0 to 11, its scale by its saved name, else major; its input as text; its monitoring by its word;
// its pad, the number an int truncates, where it has one. Every property of these the element holds, wherever it is
// described (a MASTER's name too). None for an element that is no TRACK or MASTER. For a song whose tree is the storage, and
// for tests. Counted (countTracksReadAsValues()).
std::optional<TrackValue> readTrackValue (const juce::ValueTree& element);

// The same as the song holds it: what enters the tracks' area (from the tracks' switch: the adoption, a version's tracks and
// a merge's choice). Each property by its description where it is described on the element's type (Schema.h), nothing
// else: its ID one the song can count on from (more than 0, below schema::maxID), else none; its kind one of its words, or
// as the loader makes an unknown one (a folder where it holds a TRACK, else a track); a volume and a pan within their
// ranges, their defaults for none and for what is no finite number (readDescribed()); a key's root within its range, made
// whole, its scale one of its words (else major); a pad's note within its range, made whole; its monitoring one of its words.
// The rest as readTrackValue() reads it. Counted (countTracksReadAsValues()).
std::optional<TrackValue> readHeldTrack (const juce::ValueTree& element);

// A TRACK's element (the MASTER's for kind master), new, with its own properties as Phazite's writers write them
// (model/arrangement/Tracks.cpp, model/mixer/Mixer.cpp, Buses.cpp, model/notes/Notes.cpp, model/transport/RecordingModel.cpp,
// model/devices/DrumKit.cpp) and nothing else: its ID always; a TRACK's kind always; its name where it has one (empty too);
// its colour as juce::Colour::toString() writes it, where it has one; its bus switch only while on; its volume only off its
// default (defaultVolume()), its pan only off 0, each the float it is (SongText.h: writeValue()); its fader and its VCA
// where it has them; its key's root and scale (its saved name) where it has one; its input where it has one; its monitoring
// but while armed; its pad's device and its pad where it has them.
juce::ValueTree writeTrackElement (const TrackValue&);

// The master's own properties written into its element, as writeTrackElement() writes them: each set where it holds another
// text, each the writer leaves out taken away, the rest of the element (its devices, its lanes, its other properties) as
// it is.
void writeMasterInto (juce::ValueTree& master, const TrackValue&);

// A track's (or the master's) own properties written into its element by what differs between what the song holds of it
// (`held`, which the element holds as writeTrackElement() writes it) and its next value, as writeTrackElement() writes them,
// in the undo manager given (none: outside the history): first each property the writer writes of `held` and not of `next`
// taken away, then each it writes of `next` otherwise than of `held` set; nothing else read or written (not the element's
// other properties, nor its children). What the tracks' setters write by (model/arrangement/TrackWriters.cpp), the one rule
// with the writer and the loader's (keepTrackAsHeld()). Whether any was. Allocates nothing past what JUCE's writes do.
bool writeTrackChanges (juce::ValueTree& element, const TrackValue& held, const TrackValue& next, juce::UndoManager*);

// One of a track's own properties as writeTrackElement() writes it of a value, void where it writes none: what a narrow
// setter writes one property by (a volume's drag: model/mixer/Mixer.cpp, setChannelVolume()).
juce::var writeTrackProperty (const TrackValue&, const juce::Identifier& property);

// Every track of a song's tree (PHAZITE), read as readTrackValue() reads one: each TRACK in the track list and, in a folder,
// the TRACKs in it (a folder's before them), each list in its element's order; of two with one ID (which the loader leaves
// none of), the first, without what is in it; and the MASTER. What reads the tracks whole while the tree is their storage
// (model/arrangement/internal/Tracks.h: getTracksValue()), and the tests.
SongTracks readTracksValue (const juce::ValueTree& song);

// A track with one of its own properties given a value as readHeldTrack() reads one, or taken away (a void value: as if the
// element hadn't it): what a merge's clash on such a property sets; false, changing nothing, for a property the value
// doesn't cover (a switch, the view, one Phazite doesn't know) or one not described on its element (a MASTER's kind).
bool setTrackProperty (TrackValue&, const juce::Identifier& property, const juce::var& value);

// The element made to hold what the song holds of the track (writeTrackElement() of what readHeldTrack() reads of it), each
// property it covers compared as text (SongText.h: isSameValue()): set where it holds another text than the writer writes
// (a number in more digits than its float, "-6.0000001" for -6; a colour in capitals; a switch written otherwise), added
// where the writer writes one it hasn't (a key's scale for a root without one: major), taken away where the writer writes
// none (a volume at its default, a pan at 0, a bus switch that is off, an empty input, a scale without a root, a colour that
// is none). First, every property the tracks' three values cover (this value's, the switches' and the view's) that isn't
// described on the element's type taken away: a MASTER's kind, name, colour, bus, key, input, monitoring, pad, mute, solo,
// frozen, pinned and drum rows, which readTrackValue(), readSwitchesOf() and readViewOf() would read and the held readers
// don't (every one is described on TRACK). What the value doesn't cover stays as it is: the switches' and the view's
// (TrackSwitchesValue.h, TracksOutside.h: their own rules), the elements in it, and every property Phazite doesn't know.
// Its numbers are within their ranges already, and its words its descriptions'
// (Project.h: makeConsistent(), keepNumbers()). Nothing for an element readHeldTrack() reads none of. Whether it changed.
// Allocates nothing for an element that needs no change (every one Phazite's writers wrote). The loader's, for every track
// and the master (makeConsistent()), so that each of a song that went through it reads the same as readTrackValue() reads
// it and as readHeldTrack() does, and holds what writeTrackElement() writes of that.
bool keepTrackAsHeld (juce::ValueTree element);

// The IDs the song's tracks hold and name (IDsHeld.h): the largest a track holds below schema::maxID (its own, its fader's,
// its VCA's, and the master's), 0 for none; each of those (what a curve, a lane, a modulation and a MIDI link name a fader by,
// and a clip, a send, a sidechain and a modulator's place a track by); and each pad's device a pad's own track names
// (trackPadDevice).
std::uint64_t findLargestIDIn (const SongTracks&);
void forEachIDHeld (const SongTracks&, const std::function<void (std::uint64_t)>& each);
void forEachIDNamed (const SongTracks&, const std::function<void (std::uint64_t)>& each);

//==============================================================================
// One field of one element read as readTrackValue() reads it, without reading the rest (none counted:
// countTracksReadAsValues()): what the area's readers read one field of one track by, at every row of the playlist, every
// strip of the mixer and every paint of a clip, while the song's tree is the storage. Each of an element that is no TRACK or
// MASTER reads as one without it.
TrackID readTrackElementID (const juce::ValueTree& element);                    // trackID
std::optional<TrackValue::Kind> readTrackKind (const juce::ValueTree& element); // none for no TRACK or MASTER
juce::String readTrackName (const juce::ValueTree& element);                    // its own, empty for none
std::optional<juce::uint32> readTrackColour (const juce::ValueTree& element);   // TrackValue::colour
bool readFolderBus (const juce::ValueTree& element);                            // folderBus
float readChannelVolume (const juce::ValueTree& element);                       // TrackValue::volume
float readChannelPan (const juce::ValueTree& element);                          // TrackValue::pan
DeviceID readChannelFader (const juce::ValueTree& element);                     // channelFader
DeviceID readChannelVCA (const juce::ValueTree& element);                       // channelVCA
std::optional<Key> readTrackKey (const juce::ValueTree& element);               // TrackValue::key
juce::String readTrackInput (const juce::ValueTree& element);                   // trackInput
TrackValue::Monitoring readTrackMonitoring (const juce::ValueTree& element);    // trackMonitoring
DeviceID readPadDevice (const juce::ValueTree& element);                        // trackPadDevice
std::optional<int> readPad (const juce::ValueTree& element);                    // trackPad

// The device that sets the channel's volume (TrackValue::getVolumeDevice()), and whether it is a bus (TrackValue::isBus()),
// from the element alone.
DeviceID readVolumeDevice (const juce::ValueTree& element);
bool readIsBus (const juce::ValueTree& element);

// How many tracks were read into values whole (readTrackValue(), readHeldTrack(), keepTrackAsHeld() where it rewrites one)
// since Phazite started, on any thread: for the test that the playlist's rows read each track whole at most once and a
// paint, a tick, a mute's click and a rename none (tests/TracksValueTests.cpp).
std::uint64_t countTracksReadAsValues() noexcept;
} // namespace phazite
