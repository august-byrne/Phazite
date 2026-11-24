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
#include "model/base/internal/Selection.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
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
// The song's tracks as it holds them: a TRACK element for each (Schema.h), folders holding theirs, and the master's own
// element, MASTER. What reads tracks outside the tracks' own code reads them by ID (above, and the getters below), never
// these elements (roadmap step S5.20, the tracks' second piece). The element functions below stay for the tracks' own code
// (this file's, model/arrangement/PlaylistModel.cpp, model/mixer/MixerModel.cpp, Buses.cpp and the channels' functions of
// model/mixer/Mixer.cpp) and for what the later pieces move. What still reads a TRACK's or the MASTER's element of a live
// song outside this file, PlaylistModel.cpp, MixerModel.cpp, Buses.cpp and model/base/ (through these functions, the
// conversion's readers of one field, TracksValue.h, or the schema's names), by file and function, each with whose it is:
//   - model/mixer/Mixer.cpp, the channels' functions (the area's own, which the writers' piece moves): canHoldInstrument()
//     (insertDevice()'s, setInstrument()'s and placeDevice()'s check), needsInstrument(), getDeviceChannel() (a device's
//     TRACK by its parents), the master's and each channel's element in getDeviceOutline() and getSongDeviceOutline(),
//     insertDevice() (a plain folder's chain), copyDevices() (both tracks' kinds), removeDevice() (a bus's switch),
//     placeDevice(), setMixedAsBus(), getFader(), getVolumeParameterID(), getPanParameterID(), findChannelOfFader(),
//     setChannelVolume(), setChannelPan(), getChannelVolume(), getChannelPan(), getReturnTracks() and
//     forgetWhatNamesTheGone()'s walk of the pads' tracks; the sends' there: getSendValue(), getSendValues(), addSend(),
//     takeSendsOf() (the keeper's) and chooseSendClash() (a merge's); the devices' there: findChainState() (a channel's chain
//     by its TRACK or MASTER) and what reads a chain through it (getDevices(), getInstrument(), hasInstrument(),
//     getDeviceSoundFiles(), getChannelDeviceValues(), getDeviceChannelPlace()), and getItemIDs()' walk of the IDs the tree
//     holds.
//   - model/notes/Notes.cpp, the tracks' view and key kept there (the writers' and the view's pieces): getTrackKey(),
//     setTrackKey(), applyLastKey(); isDrumTrack()'s drum mode (its instrument by its ID), setDrumTrack(), getDrumRowOrder(),
//     setDrumRowOrder(), getHiddenDrumRows(), hideDrumRow(), showAllDrumRows(); and chooseNoteClash() (a merge's path).
//   - model/automation/Automation.cpp, the lanes and the write mode (the leaf switch): findLanesState(), keepLane() and the
//     lanes' readers and writers on them, getAutomationMode(), setAutomationMode(), isWritingAutomation(); and
//     isParameterValue() (a tree callback's test).
//   - The writers (the writers' piece): model/devices/DrumKit.cpp: givePadItsOwnTrack(); model/mixer/Freeze.cpp:
//     freezeFromFile(), unfreeze(); model/transport/RecordingModel.cpp: findTrack() (its helper), setTrackInput(),
//     setInputMonitoring(), addTrackForInput(); model/presets/Presets.cpp: addTrackFromPreset()'s drum view.
//   - Walks that read one field of each track from the elements the song's index gives: Freeze.cpp: getFrozenTracks();
//     model/mixer/Sidechain.cpp: getSoundTracks(), getTracksFedBy(), describeTracksToListenTo().
//   - The devices' code: model/mixer/ContainerModel.cpp: addContainer() (whether the channel's element is there). The
//     devices themselves are values (model/base/internal/DevicesValue.h), read by ID; a device's channel is its value's
//     place, and model/mixer/DeviceWriters.cpp asks the song's index whether it holds a track (holdsOwner(): hasTrack()).
//   - A merge's clash's path (an element the path names): model/modulation/Modulation.cpp: readOwnerPlace();
//     model/arrangement/Clips.cpp: findClipOnPath().
//   - The plan's compile (model/playback/SongPlayer.cpp), each channel's fields from the element the index gives it, through
//     the readers of one field: compile()'s master's channel, getChannels(), updateAudibility() (a pad's sampler's track, a
//     soloed return), setChannel(), addFaderAutomation(), addModulation(), getNotes() and compileNotes(), makeAudio(),
//     checkAudio(), getHeardInput(), addFrozenFile().
//   - The listeners' tree callbacks (the listener's piece): SongPlayer.cpp: markChange(), valueTreePropertyChanged(),
//     markTracksIn(), takeValue() (a plain folder's VCA turning its tracks); model/project/ProjectContext.cpp:
//     isTrackElement(), nameChannelOf(), mark(), valueTreePropertyChanged(); model/transport/AudioRecorder.cpp: update();
//     model/playback/PlaybackSession.cpp: isViewState(); Freeze.cpp's keeper: holdsWhatWasFrozen().
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
// by its ID, as readViewOf() reads it: one lookup, read whole, for what asks several of its fields once per command (a track's
// preset); an empty view for an ID the song hasn't.
TrackView getTrackView (const Song&, TrackID);

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
//   - Not what names them: a send to a return that goes, the modulations and MIDI links of what goes, the pad tracks of a
//     sampler that goes (forgetWhatNamesTheGone()'s). Not their curves (kept, as a deleted track's always were), nor the
//     view, the switches or the lanes (the TRACK element's own, which go and come back with it).
//   - Not their devices, containers and chains, nor what the devices' store holds of them (kept by ID): the song's tree still
//     holds them under the TRACK, and they go and come back with it until the devices' switch, which adds their line here.
//   - Not a move: a track JUCE moves to another parent (taken out and put in again, the same element) keeps what it holds by
//     the keeper, which remembers it as it goes and gives it back as it comes, in the same step: moveTrackState() and its
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
