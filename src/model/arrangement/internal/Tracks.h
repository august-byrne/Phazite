// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks: which ones it shows, their colors, and the track commands. The song holds them (schema::TRACKS):
// what each is, its name and color, mute and solo, and how it shows, by their IDs. What plays them is made from the
// song.
#pragma once

#include "model/arrangement/PlaylistValues.h"
#include "model/arrangement/TrackColours.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
class CommandRegistry;

//==============================================================================
// The song's tracks as values, by their IDs (base/internal/TracksValue.h; roadmap step S5.20, the tracks' first piece). The
// song's tree is still their storage: each reads it, through the conversion (a track's element found through the song's
// index, SongIndex.h), and at the tracks' switch reads the tracks' area in its place, with the same results and the same
// types.

// A track, the master too, found by its ID: what findTrack() returns, used as `if (const auto track = findTrack (song, id))
// track->name`. While the tree is the storage it holds the track's value read from its element (readTrackValue(), one whole
// read, countTracksReadAsValues()); at the switch it holds a pointer into the tracks' area, valid until the area's next
// change. So it is kept to the statement or the block that asked for it: it can be neither copied nor moved (a function
// returns it as it is made, which C++ makes no copy of), nor made on the heap, and what it holds is reached through a named
// lookup alone (its -> and * are refused on the lookup a call returns, so that `const auto& name = findTrack (...)->name`,
// which would outlive it, doesn't compile). Cheap to make: one lookup in the index and the element's properties. What still
// compiles and must not be written, since the language can't refuse it: a named lookup captured by reference in a lambda that
// is kept (a callback, a std::function stored), a `static` lookup (`static const auto track = findTrack (...)`), and a
// reference or a pointer to what a named one holds kept past it (`*track` or `&*track` returned by reference, `track->name`
// bound to a reference that outlives the block); read the fields you need into values of their own instead.
class TrackLookup
{
public:
    TrackLookup (const TrackLookup&) = delete;
    TrackLookup (TrackLookup&&) = delete;
    TrackLookup& operator= (const TrackLookup&) = delete;
    TrackLookup& operator= (TrackLookup&&) = delete;
    ~TrackLookup() = default;

    static void* operator new (std::size_t) = delete;
    static void* operator new[] (std::size_t) = delete;

    // Whether the song holds the track.
    explicit operator bool() const noexcept { return value.has_value(); }
    bool has_value() const noexcept { return value.has_value(); }

    // The track's value, of a lookup that found one.
    const TrackValue* operator->() const& noexcept;
    const TrackValue& operator*() const& noexcept;
    const TrackValue* operator->() const&& = delete;
    const TrackValue& operator*() const&& = delete;

private:
    friend TrackLookup findTrack (const Song&, TrackID);
    explicit TrackLookup (std::optional<TrackValue> found) noexcept;

    std::optional<TrackValue> value {};
};

// A track by its ID, or the master by its: a lookup that found it or none (above). One whole read of its element, counted.
TrackLookup findTrack (const Song&, TrackID);

// Whether the song holds a track, or the master, by its ID: one lookup in the index, no value read.
bool hasTrack (const Song&, TrackID);

// Every track of the song and the master as values, read whole (readTracksValue(): a walk of the track list, each track
// counted): for a pass, a comparison and the tests, never for a path that runs at every paint or tick.
SongTracks getTracksValue (const Song&);

// The tracks in the song's order and each one's place, from the song's index, as getTrackStates() orders them and
// getTrackName() numbers them (the order the tracks' value makes: makeTrackOrder()).
TrackOrder getTrackOrder (const Song&);

// A channel's fader (Schema.h: channelFader: a track's, a return's, a bus's, the master's) and its VCA (channelVCA: a plain
// folder's), and whether a device is its fader or its VCA, by the channel's ChannelRef (model/base/IDs.h;
// model/mixer/internal/Mixer.h: findChannelOfFader()): one lookup each, the master's by its flag or its ID; none and false for
// a channel the song hasn't. What a fader's parameter found asks at each tick (model/devices/internal/Parameters.h:
// FoundParameter::channel), and the devices automation's lanes show (model/automation/Automation.cpp: getLaneDevices()).
DeviceID getChannelFader (const Song&, ChannelRef);
DeviceID getChannelVCA (const Song&, ChannelRef);
bool isFaderOf (const Song&, ChannelRef, DeviceID);

// One track's place in that order (TrackOrder::Place: its index, and its number among those that aren't folders), from the
// song's index, without the order made; none (-1 and 0) for the master and an ID the song hasn't.
TrackOrder::Place getTrackPlace (const Song&, TrackID);

// The IDs of what a track's (or the master's, by its ID) parameters are of, which automation names (Schema.h: curveDevice):
// its channel's fader and VCA, its devices, containers and their chains (model/mixer/internal/Mixer.h: getDeviceOutline()),
// its sends (getSends()), and its modulators and macros, its containers' macros too (model/modulation/internal/Modulation.h),
// each read by its ID from where the song keeps it; not the tracks in it (a folder's), which are lines of their own. What a
// version's line of a track carries of the curves (model/versions/ApplySong.cpp: makeTrackLikeIn()), as
// model/versions/internal/SongDiff.h's getAutomatableIDs() reads it of a track's element in a whole copy.
std::set<std::uint64_t> getAutomatableIDs (const Song&, TrackID);

//==============================================================================
// The tracks' setters (model/arrangement/TrackWriters.cpp; roadmap step S5.20, the tracks' writers): with the versions'
// passes until they move, the conversion, the tracks' part of the keeper and the loader, the only code that changes a TRACK's
// or the MASTER's own properties or the track lists of a live song. Every writer of the tracks' own code (this file's,
// Buses.h, the channels' of Mixer.h, model/devices/internal/DrumKit.h's pads, model/transport/RecordingModel.h's input and
// monitoring, model/notes/internal/Notes.h's key) reads a track's value by its ID, makes the next value and sets it through
// them. While the song's tree is the tracks' storage each writes it by what differs between what the song holds (the tracks'
// keeper's value: SongValues::getTracksAsTold(), no element read) and the value given (TracksValue.h: writeTrackChanges(),
// the rule of the writer and the loader's): each property only where it differs, written as the song holds it; a track that
// comes to another folder as the same element, taken out and put in again, so that the other areas' keepers carry what it
// holds (its clips, devices, modulators, sends) and its switches, its view and its lanes go with it; a track in another place
// in its folder by JUCE's moveChild(); a new one from its value (writeTrackElement()). In the undo manager of the moment: the
// caller's step, or none while the song is written outside its history (Song::OutsideHistory). None begins a step. A value
// equal to what the song holds writes nothing; each refuses what the song can't hold, the song left as it was, and returns
// false: a track without an ID or with the master's, one whose folder is none the song holds as a folder, a folder put into
// itself or into a folder in it, a folder made another kind while it holds tracks (TracksValue.h: withTrack()), and a new
// track of an ID the song holds as something else (a fader, a VCA, a device). True where they wrote.
//
// A track's place in its folder's list (the top list for none) is its index there once it is there: from 0, before the
// track at that index now (0: before the first; past the list's end, after its last), or -1 for after the folder's last
// element (its lanes too: a track dropped into a folder). A folder's lanes (LANES, view state, which stays after the
// elements it was added after) are not in the list: a track put at an index goes right after the track before it.
//
// What the setters write otherwise than the code before them did (roadmap step S5.20, the tracks' writers):
//   - A track moved within its folder can come to another place among the folder's children relative to its LANES than the
//     element functions put it (it goes right after the track before it); the tracks' order is the same. LANES goes out of
//     the tree with the view's piece, and with it the difference.
//   - What writes through them writes in the mode of the moment: forgetWhatNamesTheGone()'s clean-up of a pad's track whose
//     sampler is gone (Mixer.h) takes its pad fields off outside the history while the song is written outside it, where the
//     code before always wrote them through the song's undo manager.

// What the song holds of a track (the master's by its ID), as the tracks' keeper keeps it, no element read (for a song that
// holds no values, its element read); none for an ID the song hasn't. And the song's tracks whole so (a song without values:
// its tree read whole). What a writer makes the next value from.
std::optional<TrackValue> findHeldTrack (const Song&, TrackID);
SongTracks getHeldTracks (const Song&);

// A track (a folder, a return, the master) set: added at its place in its folder's list where the song hasn't it, or changed
// (its own properties by what differs), and moved where its folder (TrackValue::parent) is another, as the same element, to
// its place there (`index`); one whose folder stays keeps its place, `index` ignored: a reorder within a folder is
// setTrackPlace()'s or setTrackListOrder()'s. The master's value (refused without an ID) is its own properties alone: what its
// channel's writers set by its narrow setters (Mixer.h: setChannelVolume(), setChannelPan()) and the versions' pass, a later
// piece, will set whole.
bool setTrack (Song&, TrackValue, int index = -1);

// Tracks made together, as one element in one action: the first at its place in its folder's list (above), each other in a
// folder made before it in the list, after the tracks put in it before (a folder duplicated with the tracks in it).
bool setNewTracks (Song&, const std::vector<TrackValue>&, int index = -1);

// A track taken out of the song, a folder with the tracks in it. What it holds in the song's areas is erased first by its
// caller (eraseTrackHoldings()), as what names it after (Mixer.h: forgetWhatNamesTheGone()).
bool eraseTrack (Song&, TrackID);

// A track moved to a place in a folder's list (the top list for none), as the same element: within its folder by JUCE's
// moveChild(), to another folder taken out and put in again. One in that folder at that index already writes nothing (no step
// that only moves an element Phazite doesn't know past it).
bool setTrackPlace (Song&, TrackID, TrackID folder, int index);

// A folder's list (the top list for none) in another order: the same IDs, each once, else nothing; each moved by moveChild().
bool setTrackListOrder (Song&, TrackID folder, const immer::flex_vector<TrackID>& order);

// A track's (or the master's) value as the song holds it, edited, and set (setTrack()): what the writers of one or a few fields
// write through (a name, a colour, a key, an input, a bus's fader).
bool setTrackFields (Song&, TrackID, const std::function<void (TrackValue&)>& edit);

// A track's copy made as values, a folder's with the tracks in it, nothing written: each with new IDs of the song's (its own,
// its fader's and VCA's, and its devices', in the order a copy of its element gave them: each track's own, then its devices',
// then the tracks in it), each given into `copied` with its original's; a copy plays no frozen file and no pad (no frozen
// switch, pad device or pad: those are the original's). The values in the order setNewTracks() takes them (the copy first,
// each folder's tracks after it), each with its original's ID beside its copy's; none for a track the song hasn't. What they
// hold in the other areas, and their switches and view, their caller copies by `copied` and by those pairs, and puts the
// values in with setNewTracks() (duplicateTracks()).
struct TrackCopies
{
    std::vector<TrackValue> values {};
    std::vector<std::pair<TrackID, TrackID>> originalsAndCopies {}; // each original with its copy, in the values' order
};

TrackCopies makeTrackCopies (Song&, TrackID original, CopiedIDs& copied);

//==============================================================================
// The song's tracks as it holds them: a TRACK element for each (Schema.h), folders holding theirs, and the master's own
// element, MASTER. What reads tracks outside the tracks' own code reads them by ID (above, and the getters below), never
// these elements (roadmap step S5.20, the tracks' second piece), and what writes a track's own fields or the track lists
// writes them through the setters above (the writers' piece: model/arrangement/TrackWriters.cpp). The element functions below
// stay for the tracks' readers (this file's, model/arrangement/PlaylistModel.cpp, model/mixer/MixerModel.cpp, Buses.cpp and
// the channels' functions of model/mixer/Mixer.cpp), the setters, and what the later pieces move. What still reads a
// TRACK's or the MASTER's element of a live song outside this file, TrackWriters.cpp, PlaylistModel.cpp, MixerModel.cpp,
// Buses.cpp and model/base/ (through these functions, the conversion's readers of one field, TracksValue.h, or the schema's
// names), by file and function, each with whose it is:
//   - model/mixer/Mixer.cpp, the channels' readers: canHoldInstrument() (insertDevice()'s, setInstrument()'s and
//     placeDevice()'s check), needsInstrument(), getDeviceChannel() (a device's TRACK by its parents), the master's and each
//     channel's element in getDeviceOutline() and getSongDeviceOutline(), insertDevice() (a plain folder's chain),
//     copyDevices() (both tracks' kinds), removeDevice() (a bus's switch), placeDevice(), getFader(), getVolumeParameterID(),
//     getPanParameterID(), findChannelOfFader(), getChannelVolume(), getChannelPan(), getReturnTracks(), and
//     setChannelVolume() and setChannelPan(), the tracks' narrow setters, which write the one property a drag's event sets by
//     the setters' rule (TracksValue.h: writeTrackProperty()); the sends' there: getSendValue(), getSendValues(), addSend(),
//     takeSendsOf() (the keeper's) and chooseSendClash() (a merge's); the devices' there: findChainState() (a channel's chain
//     by its TRACK or MASTER) and what reads a chain through it (getDevices(), getInstrument(), hasInstrument(),
//     getDeviceSoundFiles(), getChannelDeviceValues(), getDeviceChannelPlace()), and getItemIDs()' walk of the IDs the tree
//     holds.
//   - model/notes/Notes.cpp, the tracks' key read there: getTrackKey(); and chooseNoteClash() (a merge's path).
//   - model/automation/Automation.cpp: isParameterValue() (a tree callback's test).
//   - A merge's choice of a whole track or the master (the versions' piece): this file's chooseTrackClash().
//   - Walks that read one field of each track from the elements the song's index gives: model/mixer/Sidechain.cpp:
//     getSoundTracks(), getTracksFedBy(), describeTracksToListenTo().
// The tracks' switches and view are areas of the song's values, read and written by ID (getTrackSwitches(), getTrackView()
// and the functions below them), never on an element.
//   - The devices' code: model/mixer/ContainerModel.cpp: addContainer() (whether the channel's element is there). The
//     devices themselves are values (model/base/internal/DevicesValue.h), read by ID; a device's channel is its value's
//     place, and model/mixer/DeviceWriters.cpp asks the song's index whether it holds a track (holdsOwner(): hasTrack()).
//   - A merge's clash's path (an element the path names): model/modulation/Modulation.cpp: readOwnerPlace();
//     model/arrangement/Clips.cpp: findClipOnPath().
//   - The plan's compile (model/playback/SongPlayer.cpp), each channel's fields from the element the index gives it, through
//     the readers of one field: compile()'s master's channel, getChannels(), updateAudibility() (a pad's sampler's track, a
//     soloed return), setChannel(), addFaderAutomation(), addModulation(), getNotes() and compileNotes(), makeAudio(),
//     checkAudio(), getHeardInput(), addFrozenFile().
//   - The tracks' part of the keeper (the listener's piece, model/base/internal/TracksArea.h), which tells the values'
//     listeners what the tree tells of the tracks until the tracks' switch: no listener's tree callback reads a track's
//     element any more (they hear tracksChanged(), trackSwitchesChanged(), tracksViewChanged()); what is left in them is
//     the TRACK or the MASTER above what no area takes (the song's tree holds no device, container, chain or store entry: an
//     element or a property Phazite doesn't know): SongPlayer.cpp: markChange() (that channel); model/project/
//     ProjectContext.cpp: mark() (the same); Freeze.cpp's keeper: check() (the track a change is in),
//     holdsWhatWasFrozen(); model/versions/VersionHistory.cpp: markChanged() and markTrack() (the line of the TRACK or the
//     MASTER above a change, by its ID).
//   - The loader, the keepers and the check for unsaved changes: model/project/Project.cpp (makeSong(), makeNewSong(),
//     keepPlaces(), belongsIn(), keepIDs(), keepNames(), keepModulators(), keepSendsAsHeld(), keepDevicesAsHeld(),
//     keepTracksAsHeld(), collectLinkedClips(), getCountedChild(); holdSameClip(), holdSameTrackClips()); Freeze.cpp:
//     forgetMissingFrozenFiles(); Clips.cpp: takeClipsOf().
//   - The versions: model/versions/ApplySong.cpp's passes; ChangesWorker.cpp: copyTrack() (whole state of one track) and
//     makeSkeleton() (each track's ID, kind and name, from the index's elements, through the readers of one field).
// The track list, which every song has (a new song starts with it, the master's element too: model/project/internal/
// Project.h, makeNewSong()): added if the song has none yet, without an undo step. And the master's ID, which its channel's
// functions take (MixerModel.h: an empty ID names the master there).
juce::ValueTree getTrackList (Song&);
TrackID getMasterID (const Song&);

// A track's element, anywhere in the list, or the master's; invalid for one that is gone. And each track's, top to bottom,
// every folder before its tracks (not the master), and their IDs: the tracks the playlist shows, tracks, returns and
// folders, and the tracks inside folders.
juce::ValueTree findTrackState (const Song&, TrackID);
std::vector<juce::ValueTree> getTrackStates (const Song&);
std::vector<TrackID> getTrackIDs (const Song&);

TrackID getTrackID (const juce::ValueTree& trackState);
bool isFolderState (const juce::ValueTree& trackState);
bool isReturnState (const juce::ValueTree& trackState);

// What a track is: a folder, which holds tracks; a return (it takes in what tracks send to it, and no solo silences it);
// or the master. And its kind (TrackValue::Kind: a track, the one kind that holds clips, a folder, a return, or the master for
// the master's ID), none for an ID the song hasn't: one lookup in the index and one field read, for the readers outside the
// tracks' area that ask what a track is (roadmap step S5.20, the tracks' second piece).
std::optional<TrackValue::Kind> getTrackKind (const Song&, TrackID);

// The song's tracks of one kind (the tracks that hold clips, the folders, the returns), top to bottom, a folder before its
// tracks: one walk of the order, as getTrackIDs().
std::vector<TrackID> getTrackIDsOfKind (const Song&, TrackValue::Kind);
bool isFolder (const Song&, TrackID);
bool isReturnTrack (const Song&, TrackID);
bool isMaster (const Song&, TrackID);

// A track's name as the song shows it: its own, or for a track or a return without one, its number among them, "Track 3".
juce::String getTrackName (const Song&, TrackID);

// The audio input a track records from (Schema.h: trackInput, "1" or "1+2"; empty for none, model/transport/internal/
// AudioRecorder.h: getInputChannels()) and when it is heard through it: one lookup and one field read each, as
// getTrackKind().
juce::String getTrackInput (const Song&, TrackID);
TrackValue::Monitoring getTrackMonitoring (const Song&, TrackID);

// The folder a track is in, or none; and the tracks a folder holds, directly or in its folders, top to bottom: those folders
// among them, empty ones too, as a folder is a track (its effects are part of the folder's sound).
TrackID getParentFolder (const Song&, TrackID);
std::vector<TrackID> getTracksInFolder (const Song&, TrackID folder);

// Mute and solo, each track's own switch: muted with its folders' mutes too, or only its own.
bool isTrackMuted (const Song&, TrackID, bool orInMutedFolder);
bool isTrackSoloed (const Song&, TrackID);
bool areAnyTracksSoloed (const Song&);

// Whether a track plays its frozen file (Schema.h: trackFrozen; model/mixer/internal/Freeze.h): one lookup and one field
// read; none for the master.
bool isTrackFrozen (const Song&, TrackID);

// A drum sampler's pad played on a track of its own (model/devices/internal/DrumKit.h: givePadItsOwnTrack()): the track, the
// sampler (Schema.h: trackPadDevice) and the pad's note (trackPad). One track's, none for a track that plays no pad (one
// lookup); and every one of the song, in its order, one walk of the order as getTrackIDs().
struct TrackPad
{
    TrackID track {};
    DeviceID sampler {};
    int pad = 0;
};

std::optional<TrackPad> getTrackPad (const Song&, TrackID);
std::vector<TrackPad> getPadTracks (const Song&);

// The switches set, not as undo steps (as in many DAWs); a mute in the caller's undo step with its undo manager (a bounce
// mutes the track it rendered as part of its step).
void setTrackMuted (Song&, TrackID, bool, juce::UndoManager* = nullptr);
void setTrackSoloed (Song&, TrackID, bool);

// A track's name and color, in the caller's undo step (renameTrack() and setTrackColours() are steps of their own).
void setTrackName (Song&, TrackID, const juce::String&);
void setTrackColour (Song&, TrackID, juce::Colour);

// A track's color: its own, or else the palette's color for its row, so every track has one. And the same of a track's
// own color as its value holds it (TrackValue::colour), for a reader that holds the value.
juce::Colour getTrackColour (const Song&, TrackID, int row);
juce::Colour getTrackColour (std::optional<juce::uint32> own, int row);

// A track's view (base/internal/TracksOutside.h: its height, collapsed, pinned, drum mode and drum rows, lanes), the master's
// by its ID, as the song's view holds it (SongValues.h: tracksOutside): one lookup and a look in a map; an empty view for an
// ID the song hasn't. And the same of the element the song's index gave a reader already (a playlist's row), with no second
// lookup.
TrackView getTrackView (const Song&, TrackID);
TrackView getViewOf (const Song&, const juce::ValueTree& trackState);

// The tracks' switches and view as the song holds them (base/internal/SongValues.h: trackSwitches, tracksOutside), each a
// copy of the area's value (cheap: it shares what it holds); of a song that holds no values (a check's copy), as its tree
// holds them (TrackSwitchesValue.h: readTrackSwitches(); TracksOutside.h: readTracksOutside() and readViewOf()). The tracks'
// and the master's entries of each by their keys: a track's ID, the master's 0 among the switches and its own member of the
// view (its ID is one a version gives anew). What the readers and the writers of the switches and the view read them by:
//   - findSwitchKey(): a channel's key among the switches (a track's ID, the master's 0), none for an ID the song hasn't;
//   - setTrackSwitches(): the switches given a whole value (a one-entry set: TrackSwitchesValue.h, withMute() and the three
//     after it), in the history (the step in progress, or outside it while the song is written outside it) or outside it;
//   - setTrackView(): a track's or the master's row given a view, outside the history (view state), nothing for an ID the
//     song hasn't.
TrackSwitches getTrackSwitches (const Song&);
TracksOutside getTracksOutside (const Song&);
std::optional<std::uint64_t> findSwitchKey (const Song&, TrackID);
void setTrackSwitches (Song&, TrackSwitches, bool inHistory);
void setTrackView (Song&, TrackID, TrackView);

// A track's frozen switch (Schema.h: trackFrozen; model/mixer/internal/Freeze.h), as its mute: outside the history (a
// freeze rendered, the freeze keeper's unfreezing), or in the caller's step with its undo manager (Bounce in Place).
void setTrackFrozen (Song&, TrackID, bool, juce::UndoManager* = nullptr);

// View settings the project keeps for each track, without undo steps; their on-screen meaning is the playlist's
// (ui/playlist/TrackHeights.h).
std::optional<int> getSavedTrackHeight (const Song&, TrackID); // schema::trackHeight as saved, nothing if never set
void setSavedTrackHeight (Song&, TrackID, int height);         // as given: the playlist clamps

// A collapsed track is a thin row in the playlist, with only its name, and a collapsed folder hides its tracks.
// A pinned track stays in view while the others scroll. Projects keep both, without undo steps, as they keep
// track heights.
bool isCollapsed (const Song&, TrackID);
void setCollapsed (Song&, TrackID, bool);
bool isPinned (const Song&, TrackID);
void setPinned (Song&, TrackID, bool);

// True if the track is inside a collapsed folder, so the playlist doesn't show it.
bool isHiddenInCollapsedFolder (const Song&, TrackID);

// Solo never changes a track's mute switch. Solos a track alone, unsoloing the others, as a right-click on FL Studio's mute
// switch does; or unsolos it if it is the only one.
void toggleSoloAlone (Song&, TrackID);

// True if a track is silent because other tracks are soloed, though it isn't muted itself.
bool isSilencedBySolo (const Song&, TrackID);

// An empty track after another (at the end if that is none), with no color or key of its own (its row's color in the
// palette), not selected: the track a bounce renders onto, and those a new song starts with.
TrackID insertPlainTrack (Song&, TrackID after);

//==============================================================================
// The selected tracks, top to bottom; and the same without the tracks in a selected folder (those go wherever their
// folder goes).
std::vector<TrackID> getSelectedTracks (const Selection&);
std::vector<TrackID> getSelectedOutermostTracks (const Selection&);

//==============================================================================
// Inserts an empty track after another one (at the end if that is none), in the palette's next color, and selects it
// alone if a selection is given. Returns none if it can't.
TrackID insertTrack (Song&, TrackID after, Selection*);

// The same for a return, at the end, named (model/mixer/internal/Mixer.h gives it what it takes in).
TrackID insertReturnTrack (Song&, const juce::String& name, Selection*);

// The song's tracks (not its folders or returns), top to bottom, after adding empty ones at the end until there are as many
// as asked for: what a new song starts with.
std::vector<TrackID> ensureTracks (Song&, int count);

//==============================================================================
// Folders group tracks, and every new one is a bus, which mixes them through its own devices and fader
// (model/mixer/internal/Buses.h; docs/design.md, section 5).

// Inserts an empty folder after a track (at the end if that is none), and selects it alone.
TrackID insertFolder (Song&, TrackID after, Selection*);

// Puts tracks into a new folder where the first of them was, in the order given, and selects the folder.
TrackID groupIntoFolder (Song&, const std::vector<TrackID>&, Selection*);

// Moves a track into a folder, after its last track.
void moveTrackIntoFolder (Song&, TrackID track, TrackID folder);

// Takes a folder's tracks out, to where the folder was, and deletes the folder.
void ungroupFolder (Song&, TrackID folder);

//==============================================================================
// What the track commands do. These don't start an undo step: the command does (Command::undoTarget).

bool hasSelectedTracks (const Selection&);
std::vector<TrackID> getSelectedFolders (const Selection&); // top to bottom
bool hasSelectedFolders (const Selection&);

// The selected tracks, or all of the playlist's if none is selected; none without a song.
std::vector<TrackID> getSelectedTracksOrAll (const Selection&);

// Inserts an empty track or folder after the last selected track, or at the end, and selects it alone. None without a
// song.
TrackID insertTrackAfterSelection (Selection&);
TrackID insertFolderAfterSelection (Selection&);

// Takes the tracks out of each folder, and deletes the folders.
void ungroupFolders (Song&, const std::vector<TrackID>& folders);

// Selects the track above (direction -1) or below (1) the selection; with nothing selected, the last or the first track.
// It stops at the first and the last track.
void selectNeighbourTrack (Selection&, int direction);

// Moves the selected tracks one place up (direction -1) or down (1) within their folders. A track swaps places with its
// neighbour, so selected neighbours move together.
void moveSelectedTracks (Selection&, int direction);

// Switches for lists of tracks (base/Toggles.h): on for all of them, or off if all are on. Mute and solo are each
// track's own switch, not a folder's that it is in. Collapsing and pinning change the view: they aren't undo steps.
bool areAllTracksMuted (const Song&, const std::vector<TrackID>&);
void toggleTracksMuted (Song&, const std::vector<TrackID>&);
bool areAllTracksSoloed (const Song&, const std::vector<TrackID>&);
void toggleTracksSoloed (Song&, const std::vector<TrackID>&);
bool areAllTracksCollapsed (const Song&, const std::vector<TrackID>&);
void toggleTracksCollapsed (Song&, const std::vector<TrackID>&);
bool areAllTracksPinned (const Song&, const std::vector<TrackID>&);
void toggleTracksPinned (Song&, const std::vector<TrackID>&);

// Every row of the playlist collapsed, or every one expanded, as Collapse All does (view state, no undo step): the tracks
// and folders (a collapsed folder hides its tracks), the master's and the returns' rows, and every automation lane; and
// whether any of them is expanded, which tells Collapse All which it does.
bool isAnyRowExpanded (const Song&);
void setAllRowsCollapsed (Song&, bool collapsed);
void toggleAllRowsCollapsed (Song&); // collapses them all while any is expanded, or else expands them all

// Copies each track right after its original, with its devices as they are now, its clips and its automation, and returns
// the copies. With a selection given, the copies are selected instead of what was; without one, what is selected stays as
// it was.
std::vector<TrackID> duplicateTracks (Song&, const std::vector<TrackID>&, Selection*);

// Renames a track, as one undo step; an empty name gives it its number back. The same name changes nothing.
void renameTrack (Song&, TrackID, const juce::String& name);

// Adds a track, or a folder, after the last one, as one undo step ("Add Track", "Add Folder"), and selects it if a selection
// is given. Returns it, or none.
TrackID addTrackAtEnd (Song&, bool folder, Selection*);

// Gives tracks a color, as one undo step ("Change Track Color", or "Colors" for several).
void setTrackColours (Song&, const std::vector<TrackID>&, juce::Colour);

// Moves tracks, in their order, to right after another (none: to the start), as one undo step ("Move Track(s)").
void moveTracksAfter (Song&, const std::vector<TrackID>& moving, TrackID previous);

// Moves tracks where a drag in the playlist drops them (TrackDrop, PlaylistValues.h), in their order in the playlist, a
// folder with the tracks inside it, as one undo step ("Move Track(s)"). Nothing moves where a folder would go into itself
// or into a folder inside it; canMoveTracksTo() says whether they can go there.
bool canMoveTracksTo (const Song&, const std::vector<TrackID>& moving, TrackID target, TrackDrop);
void moveTracksTo (Song&, const std::vector<TrackID>& moving, TrackID target, TrackDrop);

// Deletes tracks; a folder goes with the tracks inside it, and devices whose sidechain listened to one of them listen
// to their own sound again. With a selection, the track that takes the first one's place is selected, as in Ableton
// Live, so the keyboard can carry on from there.
void deleteTracks (Song&, const std::vector<TrackID>&, Selection*);

// What these tracks hold in the song's areas beside its tree (and the tracks in each folder among them), erased, each area
// through its own code, in one set of each area, in the step in progress (or outside the history while the song is written
// outside it): their clips with their notes (Clips.h: eraseTrackClips()); their channels' modulators and macros and their
// containers' macros, by place (Modulation.h: eraseModulatorsOf()); their sends, by the track they are on (Mixer.h:
// eraseTrackSends()). The master's ID too: its modulators and its containers' macros (a merge's choice of the whole master).
// Begins no step. Every function that takes a track out of the song calls it first, before the element goes, as it calls
// forgetWhatNamesTheGone() after (Mixer.h): deleteTracks() (and through it the pad tracks forgetWhatNamesTheGone() deletes,
// a pad brought back, the DAWproject import), ungroupFolder() (the folder alone, its tracks moved out first), a version's
// passes over elements (model/versions/ApplySong.cpp: makeLike(), removeMovedItems(), a line's track added since:
// makeTrackLikeIn()) and a merge's choice of a whole track (chooseTrackClash()), so that the keeper of the song's values,
// which still erases what a track taken out holds and remembers it with the element (model/base/internal/SongValues.h),
// finds nothing (SongValues::countHoldingsLeftWithKeepers(), which the tests' checks hold at 0). Each area's own undo action
// brings its things back with the track's in an Undo, whatever order JUCE undoes them in: the keeper does nothing while the
// history undoes or redoes, and no area's value is checked against the tree until the step is whole.
//   - Not what names them: a send to a return that goes, the modulations, MIDI links and curves of what goes, the pad tracks
//     of a sampler that goes (forgetWhatNamesTheGone()'s). Nor the view, the switches or the lanes (the TRACK element's own,
//     which go and come back with it).
//   - Not their devices, containers and chains, nor what the devices' store holds of them (kept by ID): the song's tree still
//     holds them under the TRACK, and they go and come back with it until the devices' switch, which adds their line here.
//   - Not a move: a track JUCE moves to another parent (taken out and put in again, the same element) keeps what it holds by
//     the keeper, which remembers it as it goes and gives it back as it comes, in the same step: setTrackPlace() and its
//     callers (moveTracksTo(), moveTracksAfter(), moveSelectedTracks(), moveTrackIntoFolder(), groupIntoFolder(), the tracks
//     ungroupFolder() takes out of its folder, the clipboard's track for a paste, the mixer's setOutput()), a line's folder's
//     tracks hoisted as it goes and the song's own line's tracks put in another folder (ApplySong.cpp: makeTrackLikeIn(),
//     makeSongOwnLikeIn()). The tracks' switch replaces each by a change of the track's place, which no area follows.
void eraseTrackHoldings (Song&, const std::vector<TrackID>&);

// The track commands that need no window: add, select the track above or below, move, mute and solo. They act
// on the tracks selected in the selection's song. Delete and duplicate are selection commands (Selection.h); the
// playlist adds rename and color, and the taller and shorter commands (TrackHeights.h).
void addTrackCommands (CommandRegistry&, Selection&);
} // namespace phazite
