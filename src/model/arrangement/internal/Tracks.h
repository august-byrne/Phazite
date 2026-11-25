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
// The song's tracks as values, by their IDs (base/internal/TracksValue.h; roadmap step S5.20): the tracks' area of the song's
// values (SongValues.h: tracks) holds them. Each reader reads the area's value in place.

// A track, the master too, found by its ID: what findTrack() returns, used as `if (const auto track = findTrack (song, id))
// track->name`. It holds a pointer into the tracks' area, valid until the area's next change. So it is
// kept to the statement or the block that asked for it: it can be neither copied nor moved (a function returns it as it is
// made, which C++ makes no copy of), nor made on the heap, and what it holds is reached through a named lookup alone (its ->
// and * are refused on the lookup a call returns, so that `const auto& name = findTrack (...)->name`, which would outlive
// it, doesn't compile). Cheap to make: one look in a map. What still compiles and must not be written, since the language
// can't refuse it: a named lookup captured by reference in a lambda that is kept (a callback, a std::function stored), a
// `static` lookup (`static const auto track = findTrack (...)`), a reference or a pointer to what a named one holds kept past
// it (`*track` or `&*track` returned by reference, `track->name` bound to a reference that outlives the block), and one held
// across a write of the tracks (a setter's call), which may free what it points at; read the fields you need into values of
// their own instead.
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
    explicit operator bool() const noexcept { return found != nullptr; }
    bool has_value() const noexcept { return found != nullptr; }

    // The track's value, of a lookup that found one.
    const TrackValue* operator->() const& noexcept;
    const TrackValue& operator*() const& noexcept;
    const TrackValue* operator->() const&& = delete;
    const TrackValue& operator*() const&& = delete;

private:
    friend TrackLookup findTrack (const Song&, TrackID);
    explicit TrackLookup (const TrackValue* inArea) noexcept;

    const TrackValue* found = nullptr; // into the area's value
};

// A track by its ID, or the master by its: a lookup that found it or none (above). One look in the area's map, counted.
TrackLookup findTrack (const Song&, TrackID);

// How many lookups findTrack() made since Phazite started, on any thread: for the tests that the playlist's rows look each
// row's track up once, as many however many tracks the song has (tests/PerformanceGateTests.cpp).
std::uint64_t countTrackLooks() noexcept;

// Whether the song holds a track, or the master, by its ID: one look in the area's map.
bool hasTrack (const Song&, TrackID);

// Every track of the song and the master as values: the area's value, by reference (valid until the area's next change: what
// keeps it longer copies it, cheaply, as it shares what it holds).
const SongTracks& getTracksValue (const Song&);

// The tracks in the song's order and each one's place (makeTrackOrder()): the order the song's values keep, made once for each
// value of the tracks' lists (SongValues::getTrackOrder()); valid until the area's next change.
const TrackOrder& getTrackOrder (const Song&);

// A channel's fader (Schema.h: channelFader: a track's, a return's, a bus's, the master's) and its VCA (channelVCA: a plain
// folder's), and whether a device is its fader or its VCA, by the channel's ChannelRef (model/base/IDs.h;
// model/mixer/internal/Mixer.h: findChannelOfFader()): one look each, the master's by its flag; none and false for a channel
// the song hasn't. What a fader's parameter found asks at each tick (model/devices/internal/Parameters.h:
// FoundParameter::channel), and the devices automation's lanes show (model/automation/Automation.cpp: getLaneDevices()).
DeviceID getChannelFader (const Song&, ChannelRef);
DeviceID getChannelVCA (const Song&, ChannelRef);
bool isFaderOf (const Song&, ChannelRef, DeviceID);

// One track's place in that order (TrackOrder::Place: its index, and its number among those that aren't folders); none (-1 and
// 0) for the master and an ID the song hasn't.
TrackOrder::Place getTrackPlace (const Song&, TrackID);

// The IDs of what a track's (or the master's, by its ID) parameters are of, which automation names (Schema.h: curveDevice):
// its channel's fader and VCA, its devices, containers and their chains (model/mixer/internal/Mixer.h: getDeviceOutline()),
// its sends (getSends()), and its modulators and macros, its containers' macros too (model/modulation/internal/Modulation.h),
// each read by its ID from where the song keeps it; not the tracks in it (a folder's), which are lines of their own. What a
// version's line of a track carries of the curves (model/versions/ApplySong.cpp: makeTrackLikeIn()), as
// model/versions/internal/SongDiff.h's getAutomatableIDs() reads it of a track's element in a whole copy.
std::set<std::uint64_t> getAutomatableIDs (const Song&, TrackID);

// The channel a merge's clash's path names by its steps (model/versions/SongMerge.cpp: "TRACKS:TRACKS", then a "TRACK:12" for
// each folder the track is in and for itself; or "MASTER:MASTER"), all of them, where the song holds it there (each track in
// the folder the step before names, read from the tracks' area): a track's or the master's
// ChannelRef. None for a path that names none, or holds a step of anything else.
std::optional<ChannelRef> findChannelOnPath (const Song&, const juce::StringArray& path);

//==============================================================================
// The tracks' setters (model/arrangement/TrackWriters.cpp; roadmap step S5.20): the only code that
// changes the tracks' area of a live song. Every writer of the tracks' own code (this
// file's, the versions' passes and a merge's choice, Buses.h, the channels' of Mixer.h, model/devices/internal/DrumKit.h's
// pads, model/transport/RecordingModel.h's input and monitoring, model/notes/internal/Notes.h's key) reads a track's value by
// its ID, makes the next value and sets it through them. Each works out the next value of the area from what it holds
// (TracksValue.h: withTrack(), withoutTrack(), withListOrder()) and sets it once (SongValues.h: setTrack(), eraseTrack(),
// setTrackListOrder(), setAllTracks()), in the mode of the moment: the caller's step, or outside the history while the song is
// written outside it (Song::OutsideHistory). None begins a step. A value equal to what the song holds writes nothing; each
// refuses what the song can't hold, the song left as it was, and returns false: a track without an ID or with the master's,
// one whose folder is none the song holds as a folder, a folder put into itself or into a folder in it, a folder made another
// kind while it holds tracks (TracksValue.h: withTrack()), and a new track of an ID the song holds as something else (a
// fader, a VCA, a device; a device's but where setNewTracks() is told otherwise: IDsHeldElsewhere). True where they wrote.
// A track moved changes its place in the lists alone: every other area names its
// track by ID (a clip's track, a send's, a device's and a modulator's place), and none follows.
//
// A track's place in its folder's list (the top list for none) is its index there once it is there: from 0, before the
// track at that index now (0: before the first; past the list's end, after its last), or -1 for after its last.

// What the song holds of a track (the master's by its ID), and the song's tracks whole (a copy, which shares what it holds):
// what a writer makes the next value from.
std::optional<TrackValue> findHeldTrack (const Song&, TrackID);
SongTracks getHeldTracks (const Song&);

// A track (a folder, a return, the master) set: added at its place in its folder's list where the song hasn't it, or changed
// in place, and moved where its folder (TrackValue::parent) is another, to its place there (`index`); one whose folder stays
// keeps its place, `index` ignored: a reorder within a folder is setTrackPlace()'s or setTrackListOrder()'s. The master's value
// (refused without an ID) is its own fields alone: what its channel's writers set by its narrow setters (Mixer.h:
// setChannelVolume(), setChannelPan()), and the versions' passes set whole, its ID too (model/versions/ApplySong.cpp:
// makeTracksLike(), makeTrackLikeIn()).
bool setTrack (Song&, TrackValue, int index = -1);

// The master put in the song anew, with its value (refused without an ID, or for a value that isn't the master's): a merge's
// choice of the whole MASTER (model/arrangement/Tracks.cpp: chooseTrackClash()), what it held in the song's areas erased first
// by its caller (eraseTrackHoldings()). Told as the master made (TracksArea.h: TrackChange::made; SongTracks::masterPutIn), as
// its element put in anew was: what follows the master's channel starts again (MIDI learn of its fader ends:
// model/midi/internal/MidiLinks.h).
bool setMasterAnew (Song&, const TrackValue&);

// What setNewTracks() does with a new track whose ID the song holds as an item of another area's (a device's, a container's,
// a chain's): refused, the song left as it was (every caller but one); or put in all the same where the caller makes that
// area like a whole song's that holds none of that kind under that ID in the same step, or outside the history with the rest
// (model/versions/ApplySong.cpp: makeTracksLike(), the whole song's tracks pass, the first of the passes: the other holder goes
// in its own area's pass, after it). Meanwhile the song holds that ID twice. A new track of an ID a fader or a VCA of the
// song's holds, the tracks' own, is refused either way.
enum class IDsHeldElsewhere
{
    refused,
    goWithTheirAreas
};

// Tracks made together, in one set of the area: the first at its place in its folder's list (above), each other in a folder
// made before it in the list, after the tracks put in it before (a folder duplicated with the tracks in it).
bool setNewTracks (Song&, const std::vector<TrackValue>&, int index = -1, IDsHeldElsewhere = IDsHeldElsewhere::refused);

// A track taken out of the song, a folder with the tracks in it. What it holds in the song's areas is erased first by its
// caller (eraseTrackHoldings()), as what names it after (Mixer.h: forgetWhatNamesTheGone()).
bool eraseTrack (Song&, TrackID);

// A track moved to a place in a folder's list (the top list for none): its place alone, no other area following. One in that
// folder at that index already writes nothing (no step).
bool setTrackPlace (Song&, TrackID, TrackID folder, int index);

// A folder's list (the top list for none) in another order: the same IDs, each once, else nothing.
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
// The master's ID, which its channel's functions take (MixerModel.h: an empty ID names the master there).
TrackID getMasterID (const Song&);

// The IDs of the song's tracks, top to bottom, every folder before its tracks (not the master): the tracks the playlist shows,
// tracks, returns and folders, and the tracks inside folders. A copy of the order's.
std::vector<TrackID> getTrackIDs (const Song&);

// What a track is: a folder, which holds tracks; a return (it takes in what tracks send to it, and no solo silences it);
// or the master. And its kind (TrackValue::Kind: a track, the one kind that holds clips, a folder, a return, or the master for
// the master's ID), none for an ID the song hasn't: one look in the tracks' area, for the readers outside the tracks' area
// that ask what a track is.
std::optional<TrackValue::Kind> getTrackKind (const Song&, TrackID);

// The song's tracks of one kind (the tracks that hold clips, the folders, the returns), top to bottom, a folder before its
// tracks: one walk of the order, as getTrackIDs().
std::vector<TrackID> getTrackIDsOfKind (const Song&, TrackValue::Kind);
bool isFolder (const Song&, TrackID);
bool isReturnTrack (const Song&, TrackID);
bool isMaster (const Song&, TrackID);

// A track's name as the song shows it: its own, or for a track or a return without one, its number among them, "Track 3". And
// the same of a track's value a reader found already (a playlist's row), with no second look.
juce::String getTrackName (const Song&, TrackID);
juce::String getTrackName (const Song&, const TrackValue&);

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
// ID the song hasn't.
TrackView getTrackView (const Song&, TrackID);

// The tracks' switches and view as the song holds them (base/internal/SongValues.h: trackSwitches, tracksOutside), each a copy of
// the area's value (cheap: it shares what it holds). The tracks' and the master's entries of each by their keys: a track's ID,
// the master's 0 among the switches and its own member of the view (its ID is one a version gives anew). What the readers and the
// writers of the switches and the view read them by:
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

// What these tracks hold in the song's areas (and the tracks in each folder among them), erased, each area through its own code,
// in one set of each area, in the step in progress (or outside the history while the song is written outside it): their clips
// with their notes (Clips.h: eraseTrackClips()); their channels' devices, containers and chains (Mixer.h: eraseChannelDevices();
// what the devices' store holds of them stays, kept by ID); their channels' modulators and macros and their containers' macros,
// by place (Modulation.h: eraseModulatorsOf()); their sends, by the track they are on (Mixer.h: eraseTrackSends()). The master's
// ID too: its devices, its modulators and its containers' macros (a merge's choice of the whole master). Begins no step. Every
// function that takes a track out of the song calls it first, before the track goes, as it calls forgetWhatNamesTheGone() after
// (Mixer.h): deleteTracks() (and through it the pad tracks forgetWhatNamesTheGone() deletes, a pad brought back, the DAWproject
// import), ungroupFolder() (the folder alone, its tracks moved out first), a version's tracks' passes
// (model/versions/ApplySong.cpp: makeTracksLike(), a line's track added since: makeTrackLikeIn()) and a merge's choice of a whole
// track or the master (chooseTrackClash()): nothing else erases what a track taken out held, so what isn't erased here stays in
// its area, naming a track the song hasn't (the tests' checks find it). Each area's own undo action brings its things back with
// the track's in an Undo, whatever order the history undoes them in, and no area's value is checked against another until the
// step is whole.
//   - Not what names them: a send to a return that goes, the modulations, MIDI links and curves of what goes, the pad tracks
//     of a sampler that goes (forgetWhatNamesTheGone()'s). Nor their switches and view, kept by their IDs (a deleted track's
//     come back with it).
//   - Not a move: a track moved changes its place in the tracks' lists alone, which no other area follows (each names its
//     track by ID): setTrackPlace() and its callers (moveTracksTo(), moveTracksAfter(), moveSelectedTracks(),
//     moveTrackIntoFolder(), groupIntoFolder(), the tracks ungroupFolder() takes out of its folder, the clipboard's track for a
//     paste, the mixer's setOutput()), a line's folder's tracks hoisted as it goes and the tracks a version puts in another
//     folder (ApplySong.cpp: makeTrackLikeIn(), makeTrackOrderLike()).
void eraseTrackHoldings (Song&, const std::vector<TrackID>&);

// The track commands that need no window: add, select the track above or below, move, mute and solo. They act
// on the tracks selected in the selection's song. Delete and duplicate are selection commands (Selection.h); the
// playlist adds rename and color, and the taller and shorter commands (TrackHeights.h).
void addTrackCommands (CommandRegistry&, Selection&);
} // namespace phazite
