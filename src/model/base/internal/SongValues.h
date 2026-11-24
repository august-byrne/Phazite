// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's values: what the song moves from its juce::ValueTree into immutable values held beside it (the immer library;
// roadmap step S5.20, model/base/internal/ValueArea.h), one area at a time. The areas are the song's clips (ClipsValue.h:
// SongClips, each clip's properties and warp markers as a ClipValue, each track's clips in their order, each alias group's
// clips), which the clips' code (model/arrangement/internal/Clips.h, AudioClips.h) computes each edit on; a MIDI clip's notes
// and its controllers' values (ClipNotesValue.h: ClipNotes, every clip's in NotesByClip), which the notes' code
// (model/notes/internal/Notes.h) computes each edit on, from one value to the next; the song's tempo map
// (TempoMapValue.h: its tempos and time signatures), which the tempo map's code (model/transport/internal/Tempo.h) computes
// each edit on; the song's chord lane (ChordLaneValue.h: its chords and key changes), which the chord lane's code
// (model/chords/internal/Chords.h) computes each edit on; the song's MIDI links (MidiLinksValue.h: each controller linked
// to a parameter), which the MIDI links' code (model/midi/internal/MidiLinks.h) computes each edit on; the song's
// automation (CurveValues.h: SongCurves, each automated parameter's curve with its points, in the order the song made them),
// which automation's code (model/automation/internal/Automation.h) computes each edit on; the song's sections
// (SectionsValue.h: each by its ID), which the sections' code (model/arrangement/internal/Sections.h) computes each edit on;
// the song's modulation (ModulationValue.h: SongModulation, each modulator and macro by its ID, each owner's in its order),
// which modulation's code (model/modulation/internal/Modulation.h) computes each edit on; and the song's sends (SendsValue.h:
// SongSends, each send by its ID, each track's in its order), which the sends' code (model/mixer/internal/Mixer.h) computes
// each edit on; and the song's devices, containers and chains (DevicesValue.h: SongDevices, each by its ID with its place, each
// owner's list in its order), which the devices' setters (model/mixer/internal/Mixer.h) compute each edit on; and what the
// song's devices store in the history and outside it alike (DeviceStoreValue.h: DeviceStore, a
// plug-in's values set by hand, state, program, format ID and version, a chain's mute and solo, keyed: undone by entry), with
// each device's window's place outside the history alone (DevicesOutside.h, an OutsideValue), which the store's functions
// (model/plugins/internal/DeviceStore.h) read and write by IDs; and the tracks' switches (TrackSwitchesValue.h: each track's
// mute, solo and frozen switch and each track's and the master's automation mode, keyed: undone by entry), with each track's
// and the master's view outside the history alone (TracksOutside.h, an OutsideValue: its row's height, collapsed and pinned
// switches, drum mode and rows, and its automation lanes), which the tracks' functions by ID (model/arrangement/internal/
// Tracks.h, model/automation/internal/Automation.h, model/notes/internal/Notes.h) read and write; and what the song holds of
// its own (SongOwnValue.h: its root's properties and its LASTKEY, PIANOROLL and MERGING, in three values: SongOwnValue, plain;
// SongStanding, keyed; SongOutside, outside the history alone), which its getters and setters read and write. All as plain
// values in Phazite's own types.
//
// The values are the storage: the song holds an area of each (SongValues) in its one undo history, and the song's tree holds
// no CLIP element (nor a clip's NOTE, CONTROL or WARP elements), no TEMPO or TIMESIGNATURE element, no CHORD or CHORDKEY
// element, no MIDILINK element, no CURVE or POINT element, no SECTION element, no MODULATOR or MACRO element (nor a
// MODULATES, nor a modulator's VALUES), no SEND element, no DEVICE, CONTAINER or CHAIN element (nor a device's VALUES,
// SOUNDS or VALUE), no switch, view property or LANES of a TRACK or the MASTER, and no property of its root (PHAZITE) nor a
// LASTKEY, PIANOROLL or MERGING (the song's own; its count of IDs is the song's: Song.h, makeID()): its TEMPOMAP element
// stays, empty, the
// place a whole copy writes the map in, its AUTOMATION too (which a whole copy writes only while the song has a curve), its
// CHORDLANE, where it has one, empty too, the sign that it has a lane, and its MIDILINKS and its SECTIONS, where it has them
// (added with the first link and the first section), empty too. Each area's value header holds the conversion between its
// elements and its value, the one place that reads and writes those elements by their names: what a file's clip or tempo
// map (or any that comes into the song as an element: a version's track, a track pasted) is read into, and what a whole
// copy of the song is written from (WholeState.h).
//
// An area's parts. Each area has its own files, and each list that asks every area asks it in a line of its own; nothing
// generic joins them (no table, no registration): plain functions, called in a list in a fixed order, which each list states
// with its reason. What an area provides, the lists it joins, what reaches it and what it must not do, in the steps a builder
// of the next area follows (<Area> is its name, as TempoMap; <area> its member's, as tempoMap; its element <AREA>, as
// TEMPOMAP, the song's empty place for it where it keeps one):
//   1. Its value: internal/<Area>Value.h, with its .cpp in model/base/ (as TempoMapValue.h): the value's types, each with
//      isSameAs() (ValueArea.h) and ==, the helpers that alone write it, its invariants as a function the check calls
//      (TempoMapValue.h: isHeldAsTheSongHoldsIt(); ClipsValue.h: holdsItsLists()), and the conversion of its elements by
//      name (read<Area>Value(), write<Area>(), a merge's property set on the value). Two readers of its element:
//      read<Area>Value(), lenient (each property as the area's code always read it), for a song without values and for
//      tests; and readHeld<Area>(), as the song holds the area (each number by its description, within its range, and what
//      must hold together kept so, as ChordLaneValue.h's keepChordsApart()), which everything that enters the area reads:
//      the adoption, the keeper, the versions' pass and a merge's choice (steps 2, 8 and 9). Entries the selection names
//      that have no identity in the file (an automation point: CurveValues.h, PointValue) get a key in the value, never
//      saved nor compared as content, from a counter of SongValues' outside the history (givePointKey()): given to each as
//      it enters the song (the adoption, the keeper, an edit that makes one) and kept by each edit that keeps the entry, so
//      that the selection, holding keys, follows the entries through edits, undo and redo.
//   2. Its area: internal/<Area>Area.h and model/base/<Area>Area.cpp (as TempoMapArea.h), including this header: its teller
//      (struct SongValues::<Area>Teller, a ValueArea listener that adds itself to the area and tells Listener of a change of
//      its value); its part of the keeper (struct SongValues::<Area>Keeper: childAdded(), asked for every element that comes
//      into the song's tree, whoever's it is, returning at once for what is not the area's, and for what is, reading the area
//      and setting it through its setter in the mode of the moment; its refusal of a lone element of the area (step 13);
//      and a static adopt() that takes what the loaded tree holds of it in, outside the history); its setter, a member of
//      SongValues defined there; its whole state's part (leadsTo<Area>(), write<Area>In()); its comparison of two kept
//      values (holdSame<Area>(), declared in WholeState.h) and
//      of two elements with their kept values (holdSame<Area>Of(), is<Area>Place()); and, if it holds IDs,
//      findLargestIDIn (const <Area>Value&) (IDsHeld.h). Each new file listed in CMakeLists.txt (phazite_core) and in
//      src/model/README.md. An area whose element's presence means something (the chord lane's CHORDLANE: a song without
//      one takes its keys from the key set last), or only is written into the song's file and compared with the saved copy
//      (the MIDI links' MIDILINKS, added with the first link and kept once the last goes): the element stays as an empty
//      placeholder where the song has one, and only the area's code adds or takes it away, in the step that changes the
//      value: the element first when adding, the value first when taking away, so that the value never says more than the
//      element; isHeardAs<Area>() covers only
//      what is inside it, and its own adding and taking away stays the tree's (the listeners hear it as a child of the
//      root); holdSame<Area>() and the check (step 7) compare whether each has the element too; and whole state writes an
//      empty placeholder as an empty element. An area whose element is a place every song that holds its values has (the
//      tempo map's TEMPOMAP, the curves' AUTOMATION): its adopt() makes it where the loaded tree has none, nothing adds or
//      takes it away after, isHeardAs<Area>() covers it whole (the element itself added or taken out too), and the passes
//      over elements leave it alone (step 9); where files have held none of it while the area was empty (AUTOMATION, once
//      added with the first curve), whole state leaves it out while the area is empty (CurvesArea.h: writeCurvesIn()). An
//      area whose entries sit under an owner (the clips under tracks; modulation under tracks, the master and containers;
//      the devices under channels, containers and chains) names each owner by its ID in its entries, so that nothing follows
//      an owner as it moves, and what removes an owner erases its entries first (model/arrangement/internal/Tracks.h:
//      eraseTrackHoldings(); model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()); whole state writes an owner's
//      entries in its element (writeModulationIn(): after its other children but those another area writes after it, a
//      track's sends); and an entry keeps beside its owner each owner above it that a teller names (ModulatorPlace: a
//      container's macro keeps its channel), so that what is told names it without reading the tree, which a redo may not
//      have put back yet.
//   3. Here, in SongValues: its area as a member (ValueArea<<Area>Value> <area>, after the others); its setter (set<Area>());
//      a virtual <area>Changed() in Listener, empty by default; struct <Area>Teller and <Area>Keeper among the declarations
//      below, and a std::unique_ptr<<Area>Teller> after the others; a count of what its keeper took in, if it takes any in
//      (countTempoMapAdopted()); and isHeardAs<Area>(), below, from the listener's piece on, with its line in
//      isHeardAsKept() (steps 12 and 14).
//   4. SongValues.cpp: its area header included; its area in the constructor's initialiser list after the others
//      (<area> (song.getUndoManager(), its empty value)), and its teller after the others' (<area>Teller
//      (std::make_unique<<Area>Teller> (<area>, listeners))); the destructor lets go of its teller after the keeper and
//      before the others' (the reverse of their making); its keeper part a member of the Keeper, made in the Keeper's
//      list, and called in a line of each callback it acts in: every area's part is called, all of them, in the order the
//      callback states (a part never stops the others); its adopt() a line of SongValues::adopt().
//   5. WholeState.h and WholeState.cpp: a member of ValuesKept and a line of keepValues(); a line of writeIn() (its
//      write<Area>In()) and of leadsToKept() (its leadsTo<Area>()); a line of holdSameKeptOf() and of isPlaceOfKept(); its
//      holdSame<Area>() declared with the others.
//   6. IDsHeld.cpp: a line of findLargestIDHeld(), if it holds IDs.
//   7. tests/SongCheck.cpp: its rule as a function of its own (check<Area>Value(): no element of it in the song's tree, its
//      place there empty, its invariants), a line of findValueProblems(). tests/SchemaTests.cpp: its elements' type a line of
//      Changed's list of what the loader alone takes (is<Area>Entry()), and of findOpened(), which reads the opened song's
//      element back in its whole state; a branch of its own for a property whole state writes with its default where the
//      element had none (as the sections' energy).
//   8. model/versions/internal/AreaClashes.h: choose<Area>Clash(), declared there and defined with the area's model code
//      (as chooseTempoMapClash() in model/transport/Tempo.cpp), a line of chooseInAreas() in model/versions/VersionsModel.cpp.
//   9. model/versions/ApplySong.cpp: its pass (make<Area>Like(), through its setter) and a line of makeValuesLike(); its part
//      of each line's pass beside the whole song's, where that line holds it: a track's, makeTrackLikeIn() (as its
//      ClipPass), and the song's own, makeSongOwnLikeIn() (as its makeTempoMapLike()); its elements left out of every pass
//      over elements that names the areas' in its conditions: copyForSong(), isPlaced() (with isTempoMaps()),
//      findPlaces(), findHolder() and sendItemsHome(); and its keeper's count (step 3) a line of NothingTakenIn::count()
//      there and of the one in model/versions/VersionsModel.cpp (a merge's choice leaves nothing to the keeper: a track
//      chosen whole is given what it holds in each area by model/arrangement/Tracks.cpp, chooseTrackClash()). An area with a
//      placeholder (step 2): its pass owns the placeholder too, adding or taking it away through the area's function (as
//      Chords.h's makeChordLaneAs()), and isPlaced(), copyForSong() and makeSongOwnLikeIn()'s list of the song's own
//      elements leave the element and its children alone. What the other's version couldn't read (a stand-in): an area
//      whose entries have no identity leaves its value whole (the tempo map's, the chord lane's); one whose entries are
//      keyed keeps the song's entry of a stand-in's key in its place, as the passes over elements did (the MIDI links',
//      by a link's controller: ApplySong.cpp, makeMidiLinksLike()).
//  10. Each SongValues::Listener: its <area>Changed() override, which only marks (step 13); one that follows nothing of the
//      area says so in a comment beside its overrides. They are:
//      - ClipRenders (model/arrangement/ClipRenders.cpp, internal/ClipRenders.h): rendersMayChange() where the area holds
//        what a clip's render is made of (the clips', the tempo map's; not the notes', nor the curves': no render holds
//        automation; nor modulation's, which moves nothing a render is made of; nor the sends', which play after a clip).
//      - FreezeKeeper (model/mixer/Freeze.cpp, internal/Freeze.h): marks the frozen tracks whose file holds what changed
//        (all of them for the tempo map) and compares them at its later moment. What a frozen track's file holds of an area is
//        in its Holds (parts: its frozen devices' IDs, getFrozenDevices(); values: ValuesKept, the devices' and the store's
//        among them; tempos; curves: its frozen devices' automation, getFrozenCurves(); modulators: its modulators and its
//        frozen containers' macros, as values, getFrozenModulators()), compared in holdsWhatWasFrozen(), each area's value by
//        identity first. An area whose value is read to compare it (the curves, the modulators) is read only for a track it told
//        changed since its last comparison, at its first and as a version is applied (curvesAsFrozen, modulatorsAsFrozen).
//        An area no frozen file holds anything of follows nothing (the sends': a frozen file is its track before its fader).
//      - PlaybackSession (model/playback/internal/PlaybackSession.h): a new plan (triggerAsyncUpdate()); for an area no plan
//        holds and something else the session feeds does (the MIDI links: the MIDI tap's controllers), that marked and
//        brought up to date at a later moment of its own (followMidiLinks(), at the next turn of the message loop, or at once
//        where a writer that is no tree callback must have it so: MidiLinker::learn()), no plan made. A value set by hand
//        that plays at once is the one thing a listener sets from what it is told (the value after: modulation's
//        SongPlayer::takeModulator(), a send's level SongPlayer::takeSendLevel(); the values before and after: a device's
//        values set by hand, its sounds, a container's mix, a chain's fader and range, SongPlayer::takeDevice()), no plan
//        made; one an undo or a redo set
//        is marked (followAfterMove), and what writes automation follows it once the history's move is whole, from the
//        history's listener (Song.h: HistoryListener; PlaybackSession::historyMoved()), never while the area tells.
//      - SongPlayer (model/playback/SongPlayer.cpp, internal/SongPlayer.h): marks what its next compile reads again (its
//        changes: the tracks' notes and audio, the tempo, the curves by their parameters).
//      - Project (model/project/Project.cpp, internal/Project.h): counts the change and marks what to compare (a member of
//        its own, as tempoToCompare, cleared in clearToCompare()); its quick check, a line of compareWithSavedState()'s list
//        (as isTempoMapAsSaved()); and its whole comparison, holdTheSameProject(), through holdSameKeptOf() (step 5), which
//        the quick check agrees with.
//      - ProjectContext's SongWatcher (model/project/ProjectContext.cpp): names what changed and adds it to its group of
//        SongChanges (ProjectContext.h; a group of its own where the views need one).
//      - VersionHistory (model/versions/VersionHistory.cpp, internal/VersionHistory.h): counts the song's change
//        (songChanges, edited()), marks the lines it is in (markTrack(), markSongOwn()) and calls changesMayDiffer().
//      - MidiLinker (model/midi/MidiLinks.cpp, internal/MidiLinks.h): ends a learning whose parameter the area tells taken
//        out, in that call (modulation's: a modulator or a macro by its ID; the sends': a send's level by the send's ID; the
//        devices': a device's, a plug-in's or a chain's parameter by its device's or chain's ID; the tracks': a fader's by
//        the channel it remembers); it listens to the song's tree no more (the tracks' listener's piece took its last source).
//      - AudioRecorder (model/transport/internal/AudioRecorder.h): the tracks' alone, its inputs brought in line from the
//        tracks it is handed.
//  11. What takes the song or a part of it for another thread or for later takes the area whole (WholeState.h:
//      copyWholeSong(), copyWhole(), keepValues(), or a part's own whole copy, as copyWholeClip()): the changes' worker
//      (model/versions/ChangesWorker.cpp: take(), makeSkeleton(), copyTrack(), which writes a track's modulation in as
//      whole state does); the saved copy (model/project/Project.cpp: savedState, the tree with keepValues(), written with
//      copyWhole()); the clipboard (model/arrangement/Clipboard.cpp); an export
//      (model/project/Export.cpp, DawProject.cpp). What reads the area on the message thread reads it in place, by
//      reference (Automation.h: getCurvesValue(), findCurve()), keeping no reference across a set; a song that holds no
//      values (a check's copy) reads as one whose area is empty.
//  12. The tree's listeners: each listener of step 10 leaves what the areas tell alone in its tree callbacks by asking
//      isHeardAsKept() (below) once, where it returns early for it (ClipRenders.cpp, Freeze.cpp, internal/Freeze.h,
//      PlaybackSession.cpp, SongPlayer.cpp, Project.cpp, internal/Project.h, ProjectContext.cpp, VersionHistory.cpp and
//      internal/VersionHistory.h); a new area adds its line there (its isHeardAs<Area>()), not a line in each listener; a
//      property's callback asks it with the property (isHeardAsKept (tree, property)), as an area whose elements stay in
//      the tree for what another piece keeps of them (the devices': a plug-in's state, a chain's mute and solo, the store's
//      until its own switch) tells the two apart by the property's name. A
//      listener that takes one area's notifications differently from another's says so in a comment where it does (the
//      clip renders, which leave the chord lane's placeholder alone too). From the listener's piece (step 14) to the
//      switch, the tree is still the area's storage, and the keeper feeds <area>Changed() from the tree's callbacks under
//      the area's elements. At the switch the feed goes (the teller tells), and the area's line in isHeardAsKept() goes
//      with it where the tree then holds nothing of the area (the clips', modulation's and the sends' went so); it stays
//      where the area keeps a place in the tree that the keeper's safety net writes (the tempo map's TEMPOMAP, the chord
//      lane's CHORDLANE).
//  13. What an area must not do: no tree callback reads any area, directly or through what reads it (the root's listeners
//      have no order among them, and an Undo puts the tree back before the areas: below); a listener told of an area's
//      change only marks; a set equal to what is held makes no undo step (nothing, or ValueArea.h's setWithoutStep() where
//      only identity differs); a number an element holds as text is read through readValueNumber() or readDescribed()
//      (SongText.h), never a cast; what is written outside the undo history (view state, mute and solo) never lives in an
//      area's undoable value; entries without identity (a tempo map's) are never written into elements by what differs:
//      their writers move with the storage, in the switch; a lone element of the area added to a live song in an undo
//      step is refused by its keeper part (a JUCE assertion, as a lone CLIP is: JUCE workaround W62
//      (docs/juce-workarounds.md)).
//  14. The pieces an area is moved in, in this order, each with its tests: (a) its value and conversion (step 1), with the
//      readers on them, the tree still the storage: the round trip of every golden project's entries and of damaged ones
//      (tests/InconsistentProjectTests.cpp), and a test for every reader of the live tree that would silently find nothing
//      once the tree holds nothing (the changes' worker, an export: step 11); a test that puts the area's elements into a
//      song opens it through makeSong() (or reads a song without values), never appends them to a live song, whose keeper
//      refuses them from the switch on; (b) the listener (steps 10 and 12), fed from the tree: its case (as
//      tests/SongValuesTempoTests.cpp's: each edit tells once, its Undo and its Redo too) written to pass unchanged across
//      the switch; where it counts the version history's song changes for an edit that makes an ID, it counts the count of
//      IDs moving on (Song::makeID(): nextID) in the step alone, never in its Undo or its Redo, which leave the count as it
//      is; (c) the writers, through the setter, and the versions' pass and clash (steps 8
//      and 9), but where step 13 keeps them for the switch; (d) the switch (steps 2 to 7, the adoption, the tree emptied):
//      the keeper's cases with Undo and Redo (UndoCheck), the check's rule, whole state, the unsaved check agreeing with the
//      whole comparison (tests/ProjectTests.cpp), and a speed gate (tests/PerformanceGateTests.cpp) where a reader is on a
//      per-frame path.
//  15. The docs it updates: CLAUDE.md's paragraph on the song's values; src/model/README.md; tests/README.md's nets;
//      docs/design/engine.md; docs/project-format.md; docs/juce-workarounds.md's W62; docs/roadmap.md's sentence on where
//      S5.20 stands; and docs/performance.md after a measurement.
//
// immer is the model's alone (tools/check-architecture.sh: immer-outside-model): nothing outside src/model includes this.
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/ControlKinds.h"
#include "model/base/IDs.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/ClipNotesValue.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/OutsideValue.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/ValueArea.h"
#include "model/devices/ParameterRef.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
class Song;

// The tracks' (model/base/internal/TracksArea.h): what the listener is told of them.
struct TrackChange;
struct TrackSwitchChange;

// The song's own's (model/base/internal/SongOwnArea.h, SongOwnValue.h): what the listener is told of it.
struct SongOwnChange;

//==============================================================================
// The song's values, which the song holds once its tree is consistent (Song::holdValues(), as model/project/internal/
// Project.h's makeSong() makes a song): the clips' area, the notes' area, the tempo map's, the chord lane's, the MIDI links',
// the curves', the sections', modulation's and the sends', in the song's one undo history, and what keeps them and the tree
// together.
// Message thread only, as the song's tree is: nothing on the audio thread reads it (what plays gets what the model compiles
// of it), and what runs on another thread gets a copy (WholeState.h: copyWholeSong(), keepValues()).
//
// Who changes the values: the clips' code (Clips.h: addClip(), setClip(), eraseClip() and the rest, through setClip(),
// eraseClip() and setTrackClipOrder() below), the notes' code (Notes.h: setClipNotesValue(), which is setClipNotes() below),
// the tempo map's code (Tempo.h: every edit of the tempo map, through setTempoMap() below), the chord lane's code (Chords.h:
// every edit of the lane, through setChordLane() below), the MIDI links' code (MidiLinks.h: every edit of the links, through
// setMidiLinks() below), automation's code (Automation.h: every edit of the curves, through setCurves() and its setters
// below), the sections' code (Sections.h: every edit of the sections, through setSections() below), modulation's code
// (Modulation.h: through setModulator() and the three after it, below), the sends' code (Mixer.h: through setSend() and the
// three after it, below) and the devices' setters (Mixer.h: through setDevice() and the three after it, below). The song takes in
// what its file holds of each area as it holds its values (holdValues(): each area's adopt(), which reads it as the song
// holds it, as TempoMapValue.h's readHeldTempoMap(), ChordLaneValue.h's readHeldChordLane(), MidiLinksValue.h's
// readHeldMidiLinks(), CurveValues.h's readHeldCurves(), SectionsValue.h's readHeldSections(), ModulationValue.h's
// readHeldModulation() and SendsValue.h's readHeldSend() do; the modulators, macros and sends taken out of the tree), and
// empties its
// TEMPOMAP and AUTOMATION elements (making each where its file had none), and its CHORDLANE, MIDILINKS and SECTIONS where it
// has them, properties and children Phazite doesn't know too. A MIDI clip that joins an alias group takes the group's
// notes, and one that leaves it keeps those it holds (setClip()). Everything else that changes the tree is heard by the
// keeper, a listener of the song's root (each area's part of it in the area's file: TracksArea.h, TempoMapArea.h,
// ChordLaneArea.h, MidiLinksArea.h, CurvesArea.h, SectionsArea.h, ModulationArea.h, DeviceStoreArea.h, DevicesArea.h,
// SongOwnArea.h), which asks every area that has a part for every element that comes into the tree, each doing nothing for
// what is not its own. The tree holds no track
// since the tracks' switch (TracksArea.h): the tracks' part, asked first, takes them in as the song takes its values (each
// read once) and catches a TRACK added to the song's TRACKS or a property written to its MASTER: refused in a step (a JUCE
// assertion: the tracks' setters set a track), and outside the history read in with what it holds, through each area's
// reader (its clips with their notes, its devices and containers with the store's entries, its modulators and macros, its
// sends, its switches and view), and taken out (countTracksAdopted()). No other part has a case of a track: none follows
// a track as it comes, goes or moves (each area names its track by ID, and each remover erases what a track holds first:
// model/arrangement/internal/Tracks.h, eraseTrackHoldings()), and the clips' and the sends' areas have no part. The
// same safety net for the tempo map, which the versions don't use either (model/versions/ApplySong.cpp, VersionsModel.cpp):
// a TEMPOMAP that comes into the song with TEMPO or TIMESIGNATURE elements in it has them read into the tempo map's area,
// as its whole map, and is emptied as holdValues() empties the song's own (what Phazite doesn't know of it goes too); and
// a TEMPO or a TIMESIGNATURE added to the song's TEMPOMAP by itself while the song is written outside the history is read
// into the map (TempoMapValue.h: withEntryTaken()) and taken out again. One
// added by itself in a step is refused (a JUCE assertion: the tempo map's functions change the map), as the step that adds
// it can't take it out again (its Undo would find it gone). The chord lane's alike (ChordLaneArea.h): a CHORDLANE that comes
// in with CHORD or CHORDKEY elements has them read in as the song's lane and is emptied, a lone one added outside the
// history is read in and taken out, and one added in a step is refused; the MIDI links' alike (MidiLinksArea.h), for a
// MIDILINKS that comes in with MIDILINK
// elements and a lone MIDILINK, and the sections' (SectionsArea.h), for a SECTIONS that comes in with SECTION elements and
// a lone SECTION; and the curves' (CurvesArea.h) for a lone CURVE, each point read in given a key, while an AUTOMATION
// added to the song, a second beside the one it has from holdValues() on, is refused (a JUCE assertion), its curves unread,
// and taken out again outside the history. Each in the mode of the moment: in the history's step in progress, or outside
// the history while the song is written outside it (Song::OutsideHistory). Nothing while the history undoes or redoes a
// step: the areas' own actions move them. The keeper performs its changes from inside JUCE's action for the tree's, which
// JUCE adds to the step before
// the tree's own: so an undo puts the tree back first and the clips after (JUCE workaround W62 (docs/juce-workarounds.md)).
// Modulation's part (ModulationArea.h) for the modulators and macros of the master: a container added to it outside the
// history with MACRO elements has them read in and taken out (a safety net: a container's makers make its macros as values,
// which its inserter sets: Mixer.h, insertDevice()); and a lone one is refused in a step, read in outside the history. The
// devices' part (DevicesArea.h): a lone DEVICE, CONTAINER, CHAIN, VALUES or SOUNDS is refused in a step, read in (a device
// or a container on the master) and taken out outside the history (a safety net no path of Phazite's relies on). The
// tracks' switches' and view's part (TracksArea.h: TrackLeavesKeeper): a lone LANES on the master is refused in a step and
// read in outside the history; a track taken out leaves its switches' and view's entries where they are, for an Undo. The
// song's own part (SongOwnArea.h: SongOwnKeeper): a property of the
// root's own or its count of IDs written to the song's root, or a LASTKEY, PIANOROLL or MERGING added to it, is refused in a
// step and read in and taken out outside the history (a safety net no path of Phazite's relies on: countSongOwnAdopted());
// and as the song holds its values, the root's every property and those elements are read in and taken out, and what
// Phazite doesn't know there dropped.
//
// The rule for what listens to the song's tree, with no exception: a tree callback never reads the clips' area (nor the
// notes'), directly or through anything that reads them (the clips' functions, Clips.h, AudioClips.h; the notes', Notes.h;
// a whole copy, WholeState.h). Every listener of the song's root listens on a handle of its own, and JUCE tells a tree's
// listeners handle by handle in the order of the handles' addresses, so there is no order among them: the keeper may hear a
// change after the listener that reads; and during an undo the tree is back before the clips are. What reads the clips
// does it at its listener's later moment: a plan's next compile, the next turn of the message loop (Song::PendingWriter), or
// as the values' listener (Listener) is told. What the keeper can't see: a TRACK added with no undo manager outside an
// OutsideHistory scope is refused as one added in a step, and so is a tempo map's entry added so; nothing in Phazite does
// either to a song that holds its values (the loader does it before: makeSong()).
class SongValues final
{
public:
    explicit SongValues (Song&);
    ~SongValues();

    ValueArea<SongClips> clips;
    ValueArea<NotesByClip> notes;
    ValueArea<TempoMapValue> tempoMap;           // each kind by beat, at least one of each (TempoMapValue.h)
    ValueArea<ChordLaneValue> chordLane;         // each kind in the lane's order, empty for a song without a lane (ChordLaneValue.h)
    ValueArea<MidiLinksValue> midiLinks;         // in the song's order, one of each controller and of each parameter (MidiLinksValue.h)
    ValueArea<SongCurves> curves;                // by their parameters, in the order the song made them, each point keyed (CurveValues.h)
    ValueArea<SectionsValue> sections;           // in the song's order, one of each ID (SectionsValue.h)
    ValueArea<SongModulation> modulation;        // by ID, each owner's in its order, held together (ModulationValue.h)
    ValueArea<SongSends> sends;                  // by ID, each track's in its order, held together (SendsValue.h)
    ValueArea<DeviceStore> deviceStore;          // keyed: undone by entry (DeviceStoreValue.h)
    OutsideValue<DevicesOutside> devicesOutside; // never in the history: each device's window's place (DevicesOutside.h)
    ValueArea<SongDevices> devices;              // by ID, each owner's in its order, held together (DevicesValue.h)
    ValueArea<TrackSwitches> trackSwitches;      // keyed: undone by entry; the master's mode under 0 (TrackSwitchesValue.h)
    OutsideValue<TracksOutside> tracksOutside;   // never in the history: each track's row, the master's (TracksOutside.h)
    ValueArea<SongTracks> tracks;                // by ID, each folder's list and the top list, the master a member (TracksValue.h)
    ValueArea<SongOwnValue> songOwn;             // the song's own that a step alone writes, a merge waiting too (SongOwnValue.h)
    ValueArea<SongStanding> songStanding;        // keyed: undone by entry; the key set last, the versions' names (SongOwnValue.h)
    OutsideValue<SongOutside> songOutside;       // never in the history: the loop, the play start, the grids... (SongOwnValue.h)

    // A parameter's curve: none for a parameter without one. A pointer into the area's value as it is now, valid until the
    // area's next change (as findClip()).
    const CurveValue* findCurve (const ParameterRef& parameter) const { return curves.get().find (parameter); }

    // A clip's value: none for a clip that isn't the song's. A pointer into the area's value as it is now, valid until the
    // area's next change; to read the same value on past changes, keep a copy of the area's value (cheap: it shares what it
    // holds) and find the clip there.
    const ClipValue* findClip (ClipID clip) const { return clips.get().find (clip); }

    // A clip's notes: none for a clip that isn't a MIDI clip of the song's.
    const ClipNotes* findClipNotes (ClipID clip) const { return notes.get().find (clip); }

    // A clip set: added (an ID no clip of the song has, on its track's list at a place from 0, or after the others for -1),
    // or changed (on another track, at the end of that one's list, or at the place given). A MIDI clip added with notes holds
    // them alone (the caller gives its alias group's where it joins one); one added without them, and one whose alias group
    // changes, takes the notes of the group it joins where another clip of it holds some (one that leaves keeps those it
    // holds), else a new one an empty value; an audio clip holds none. That its track holds clips is the caller's to have
    // asked. In the song's undo history (the step in progress), or outside it while the song is written outside it
    // (Song::isWritingOutsideHistory()). A value equal to the one the clip holds is no change, and makes no action in the
    // history: nothing, or, where only its sound is another value alike (an alias group's clips share one: ClipSound), that
    // value without a step of its own (ValueArea.h: setWithoutStep()). In ClipsArea.cpp, as the next two are.
    void setClip (Song&, ClipValue, int index = -1, const ClipNotes* notes = nullptr);

    // A clip taken out of the song, its notes with it; and a track's clips put in another order (the same clips), the same way:
    // the order it holds already is no change.
    void eraseClip (Song&, ClipID);
    void setTrackClipOrder (Song&, TrackID, immer::flex_vector<ClipID> order);

    // Clips taken out of the song, each with its notes, in one set of each area (a clip the song hasn't left out): what goes
    // with the tracks a function removes (model/arrangement/internal/Tracks.h: eraseTrackHoldings(), through the clips' code,
    // Clips.h: eraseTrackClips()). Nothing more: an alias group left with one clip keeps it linked, as the keeper left it
    // (the remover's unlinkLoneClips() follows). In ClipsArea.cpp.
    void eraseClips (Song&, const std::vector<ClipID>&);

    // The same for modulators and macros (each with what it modulates, from its owner's list too) and for sends: those of
    // these IDs the song holds taken out in one set of the area, each list kept in step as each one's helper keeps it
    // (ModulationValue.h: withoutModulator(); SendsValue.h: withoutSend()), so that no walk of the whole area checks it: what
    // a track's removal takes (Modulation.h: eraseModulatorsOf(); Mixer.h: eraseTrackSends()), its cost the track's. Whether
    // it changed the area. In ModulationArea.cpp and SendsArea.cpp.
    bool eraseModulators (Song&, const std::vector<ModulatorID>&);
    bool eraseSends (Song&, const std::vector<DeviceID>&);

    // Gives a MIDI clip of the song and each MIDI clip of its alias group this value of notes, as setClip() sets: the one way
    // to change a clip's notes. Notes equal to those each holds are no change, and make no action in the history: nothing
    // where they hold one value, else this one without a step of its own (a group holds one value). In NotesArea.cpp.
    void setClipNotes (Song&, ClipID, ClipNotes);

    // Gives the song this tempo map, the tempo map's code's one way to change it (model/transport/internal/Tempo.h): in the
    // song's undo history (the step in progress), or outside it while the song is written outside it. A map equal to the one
    // the song holds is no change, and writes nothing: no step, the value the song holds kept (an entry has no identity of
    // its own to keep, nor a value of the map's anything else shares, so nothing is lost). In TempoMapArea.cpp.
    void setTempoMap (Song&, TempoMapValue);

    // Gives the song this chord lane, the chord lane's code's one way to change it (model/chords/internal/Chords.h): in the
    // song's undo history (the step in progress), or outside it while the song is written outside it. A lane equal to the one
    // the song holds is no change, and writes nothing: no step, the value the song holds kept (an entry has no identity of
    // its own to keep). Whether the song has a lane is its tree's CHORDLANE's to say, which the chord lane's code adds and
    // takes away in the same step. In ChordLaneArea.cpp.
    void setChordLane (Song&, ChordLaneValue);

    // Gives the song these MIDI links, the MIDI links' code's one way to change them (model/midi/internal/MidiLinks.h): in the
    // song's undo history (the step in progress), or outside it while the song is written outside it. Links equal to those
    // the song holds are no change, and write nothing: no step, the value the song holds kept (a link has no identity of its
    // own to keep but its controller, which its value holds). Whether the song has a MIDILINKS, the place a whole copy writes
    // them in, is its tree's to say, which the MIDI links' code adds, and takes away, in the same step. In MidiLinksArea.cpp.
    void setMidiLinks (Song&, MidiLinksValue);

    // Gives the song these curves, automation's code's one way to change them (model/automation/internal/Automation.h): in
    // the song's undo history (the step in progress), or outside it while the song is written outside it. Curves equal to
    // those the song holds (each alike, in the same order: their points' keys aside) are no change, and write nothing: no
    // step, the value the song holds kept, its keys with it. The value's points keep the keys they hold: what sets them gives
    // each point the song makes a new key (givePointKey()), and keeps the key of each it keeps. In CurvesArea.cpp, as the
    // next four, each through it:
    //   - setCurve(): a parameter's whole curve, changed in its place or added after the others, its points given the keys
    //     the song's curve of it holds by their places, and new keys past its last (a version's curve, a merge's choice, a
    //     copy's); whether it wrote.
    //   - eraseCurve(): a parameter's curve taken out; whether it was there.
    //   - renameCurve(): the curve of one parameter given to another, in its place among the song's curves, its points' keys
    //     kept (a folder's fader's curve given to the fader that takes its place), the listener told the parameter it had and
    //     the one it has in one call (Listener::curvesChanged()), in its Undo and its Redo too. Nothing for a parameter without
    //     a curve, for the same parameter, or for one that has a curve already (asserted).
    //   - setCurveOrder(): the song's curves of these parameters (each once) put first, in this order, the others after them
    //     in theirs; a parameter without a curve left out; whether the order changed.
    void setCurves (Song&, SongCurves);
    bool setCurve (Song&, const ParameterRef&, CurveValue);

    // A curve with the keys of the song's curve held (none for a new curve) by their places, and new keys past its last: a
    // curve set whole (setCurve(), the versions' pass: model/versions/ApplySong.cpp, makeCurvesLike()).
    CurveValue keyByPlace (const CurveValue* held, CurveValue);

    bool eraseCurve (Song&, const ParameterRef&);
    void renameCurve (Song&, const ParameterRef& from, const ParameterRef& to);
    bool setCurveOrder (Song&, const std::vector<ParameterRef>& first);

    // The song's next point key, a key no point of the song has had since the song held its values: from a count the song's
    // values keep outside the undo history, so that no Undo gives a key again, never saved (CurveValues.h: PointValue). And
    // the next to be given, which every point's key is below (for the check: tests/SongCheck.cpp).
    std::uint64_t givePointKey() noexcept { return nextPointKey++; }
    std::uint64_t getNextPointKey() const noexcept { return nextPointKey; }

    // Gives the song these sections, the sections' code's one way to change them (model/arrangement/internal/Sections.h): in
    // the song's undo history (the step in progress), or outside it while the song is written outside it. Sections equal to
    // those the song holds are no change, and write nothing: no step, the value the song holds kept (a section's identity is
    // its ID, which its value holds). Whether the song has a SECTIONS, the place a whole copy writes them in, is its tree's to
    // say, which the sections' code adds, and takes away, in the same step. In SectionsArea.cpp.
    void setSections (Song&, SectionsValue);

    // Give the song's modulation a modulator or a macro (ModulationValue.h: withHeldModulator(), at a place in its owner's
    // list, from 0, or after the others for -1; one it holds keeps its place where its owner stays), take one out, put an
    // owner's in another order (the same IDs), or give it a whole value: modulation's code's ways to change it
    // (model/modulation/internal/Modulation.h: setModulator(), eraseModulator(), setModulatorOrder(), setAllModulation(),
    // which find the owner a place names in the song), in the song's undo history (the step in progress), or outside it while
    // the song is written outside it. A value equal to what the song holds is no change, and writes nothing: no step, the
    // value held kept. Whether it changed the song's. In ModulationArea.cpp.
    bool setModulator (Song&, ModulatorValue, int index = -1);
    bool eraseModulator (Song&, ModulatorID);
    bool setModulatorOrder (Song&, const ModulatorPlace& owner, immer::flex_vector<ModulatorID> order);
    bool setAllModulation (Song&, SongModulation);

    // Give the song's sends a send (SendsValue.h: withSend(), at a place in its track's list, from 0, or after the others for
    // -1; one it holds keeps its place where its track stays), take one out, put a track's in another order (the same IDs), or
    // give them a whole value: the sends' code's ways to change them (model/mixer/internal/Mixer.h: addSend(),
    // setSendLevel(), setPreFader(), removeSend(), forgetWhatNamesTheGone(), copySends(), chooseSendClash(); the versions'
    // pass, model/versions/ApplySong.cpp, makeSendsLike()), in the song's undo history (the step in progress), or outside it
    // while the song is written outside it. A value equal to what the song holds is no change, and writes nothing: no step, the
    // value held kept. Whether it changed the song's. In SendsArea.cpp.
    bool setSend (Song&, SendValue, int index = -1);
    bool eraseSend (Song&, DeviceID);
    bool setTrackSendOrder (Song&, TrackID, immer::flex_vector<DeviceID> order);
    bool setAllSends (Song&, SongSends);

    // Give the song's devices' store a whole value (DeviceStoreValue.h, keyed: an Undo puts back only the entries its step
    // changed), its functions' one way to change it (model/plugins/internal/DeviceStore.h):
    //   - setDeviceStore(): in the song's undo history (the step in progress: a preset's state and program, a value set by
    //     hand, a chain's switches cleared with its last neighbour, a version applied in the history), or outside it while the
    //     song is written outside it. A value that holds the same of the song's as the area (==) makes no undo action: where
    //     only a plug-in's held values differ, it is kept outside the history (a held value is never a change of the song's).
    //   - storeOutsideHistory(): outside the history whatever is open (a plug-in's store, its own window, its late answers, a
    //     chain's mute and solo, a parameter's held value): compared with what the area holds by identity alone
    //     (isSameAs()), so that a value that differs only in a held value is kept, as no change of the song's.
    // Whether the song's entries changed (a held value aside). In DeviceStoreArea.cpp, as the next.
    bool setDeviceStore (Song&, DeviceStore);
    bool storeOutsideHistory (Song&, DeviceStore);

    // A device's window's place given, or taken away (none), as view state: never in the history, no change of the song's,
    // nothing told (OutsideValue.h).
    void setWindowPlace (DeviceID, std::optional<WindowPlace>);

    // Give the song's devices a device, a container or a chain (DevicesValue.h: withDevice(), at a place in its list, from 0,
    // or after the others for -1; one it holds keeps its place where its list stays, and moves with what it holds where its
    // place names another list), take one out with what it holds, put an owner's list in another order (the same IDs), or
    // give them a whole value: the devices' setters' ways to change them (model/mixer/internal/Mixer.h: setDevice(),
    // setNewDevices(), eraseDevice(), setDeviceListOrder() and the narrow setters beside them; the keeper; the versions' pass,
    // model/versions/ApplySong.cpp, makeDevicesLike()), in the song's undo history (the step in progress), or outside it while
    // the song is written outside it. A value equal to what the song holds is no change, and writes nothing: no step, the
    // value held kept. Whether it changed the song's. In DevicesArea.cpp.
    bool setDevice (Song&, DeviceValue, int index = -1);
    bool eraseDevice (Song&, DeviceID);
    bool setDeviceListOrder (Song&, const DevicePlace& owner, immer::flex_vector<DeviceID> order);
    bool setAllDevices (Song&, SongDevices);

    // Give the tracks' switches a whole value (TrackSwitchesValue.h, keyed: an Undo puts back only the entries its step
    // changed, as the tree undid a property at a time), their writers' one way to change them (model/arrangement/internal/
    // Tracks.h: setTrackSwitches(), and the setters by ID through it; model/automation/internal/Automation.h:
    // setAutomationMode(); the versions' pass; a merge's choice):
    //   - setSwitches(): in the song's undo history (the step in progress: Bounce in Place mutes and freezes its source, a
    //     version applied in the history, a merge's choice), or outside it while the song is written outside it.
    //   - setSwitchesOutsideHistory(): outside the history whatever is open (a toggle, the freeze keeper's unfreezing, a freeze
    //     rendered, the automation writer's latch; a new track's, as a maker gives a new item's: a duplicate's copy, and a
    //     version's or a merge's track new to the song that the switches hold no entry of).
    // A value that holds what the area holds (the same value, or one alike: ==, which compares the maps by what differs) is no
    // change: no undo action, nothing told. The one-entry setters (withMute(), withSolo(), withFrozen(),
    // withAutomationMode()) give the very value where the entry is so already, a look in one map. Entries of a track the
    // song doesn't hold (one a step took away, which its Undo brings back with them) are kept, and never written, compared
    // or counted. Whether it changed. In TracksArea.cpp, as the next.
    bool setSwitches (Song&, TrackSwitches);
    bool setSwitchesOutsideHistory (Song&, TrackSwitches);

    // Give the tracks' view a whole value (TracksOutside.h), as view state: never in the history, no change of the song's
    // (OutsideValue.h), each track's row and the master's (withTrackView(), withMasterView(): the very value where the row is
    // so already). Told to the views alone (Listener::tracksViewChanged()). Whether it changed.
    bool setTracksOutside (TracksOutside);

    // Give the song's tracks a track (TracksValue.h: withTrack(), at a place in its folder's list, from 0, or after the others
    // for -1; one it holds changed in place where its folder stays, else moved to its new folder's list; the master for a value
    // of kind master), take one out with the tracks in it (a folder's) and their lists, put a list in another order (the same
    // IDs), or give them a whole value (a pass; the adoption sets the area outside the history itself): the tracks' setters'
    // ways to change them (model/arrangement/internal/Tracks.h: setTrack(), setNewTracks(), eraseTrack(), setTrackPlace(),
    // setTrackListOrder(), setMasterAnew()), in the song's undo history (the step in progress), or outside it while the song is
    // written outside it. A plain area, undone whole: what is written outside the history on purpose (the switches, the view)
    // lives in values of its own. A value the area refuses (withTrack()'s refusals) or equal to what it holds is no change,
    // and writes nothing: no step, the value held kept (identity first: never a walk of the whole value). Whether it changed
    // the song's. In TracksArea.cpp, as the next.
    bool setTrack (Song&, TrackValue, int index = -1);
    bool eraseTrack (Song&, TrackID);
    bool setTrackListOrder (Song&, TrackID folder, immer::flex_vector<TrackID> order);
    bool setAllTracks (Song&, SongTracks);

    // The tracks in the song's order and each one's place (TracksValue.h: makeTrackOrder()), made once for each value of the
    // tracks' lists and of which of them are folders: kept with the lists it was made of, compared by identity, and made again
    // where the lists differ or a track came to be a folder or stopped being one (immer's diff of the tracks' map since,
    // O(changed)), as playedTempoMaps is kept. Valid until the area's next change: what holds it longer copies it.
    const TrackOrder& getTrackOrder() const;

    // Whether each ask of getTrackOrder() is compared with an order made afresh of the tracks (makeTrackOrder()), a JUCE
    // assertion where they differ: every ask while the song has at most 512 tracks, a share of them in a bigger song, so that
    // the checks cost about as much again as the asks. Off unless set, as the tests set it (tests/TestMain.cpp); any thread.
    static void setCheckingTrackOrder (bool) noexcept;
    static bool isCheckingTrackOrder() noexcept;

    // Give the song's own its next values (SongOwnValue.h), their setters' one way to change them (SongOwnValue.h:
    // setSongOwn(), setSongStanding(), storeStandingOutsideHistory(), setSongOutside(), which call these):
    //   - setOwn(): the song's own value, in the song's undo history (the step in progress), or outside it while the song is
    //     written outside it.
    //   - setStanding(): its standing (keyed: an Undo puts back only the entries its step changed), the same.
    //   - setStandingOutsideHistory(): its standing outside the history whatever is open.
    //   - setOutside(): what it keeps of how it plays and shows, never in the history.
    // A value equal to what the area holds is no change: no undo action, nothing told. Whether it changed. In SongOwnArea.cpp.
    bool setOwn (Song&, SongOwnValue);
    bool setStanding (Song&, SongStanding);
    bool setStandingOutsideHistory (SongStanding);
    bool setOutside (SongOutside);

    // The tempo map as Phazite's engine plays it (engine/core/TempoMap.h), made of the tempo map's value at a sample rate,
    // kept for the tempo map's code (model/transport/Tempo.cpp: getTempoMap()), so that it is made once for each change of
    // the value and each sample rate asked, not at every call (each conversion of a beat to seconds, each audio clip read):
    // the last two asked, each with the value it was made of, compared by identity. Message thread only, as the area is: what
    // plays gets a copy (a plan's), and nothing on the audio thread reaches these.
    struct PlayedTempoMap
    {
        TempoMapValue madeOf;
        double sampleRate = 0.0;
        engine::TempoMap map;
    };

    mutable std::array<std::optional<PlayedTempoMap>, 2> playedTempoMaps;
    mutable size_t nextPlayedTempoMap = 0; // which of them is made next

    // The tracks' order kept (getTrackOrder()), with the tracks' map and lists it was made of.
    struct KeptTrackOrder
    {
        immer::map<std::uint64_t, TrackValue> tracks {};
        immer::map<std::uint64_t, immer::flex_vector<TrackID>> lists {};
        TrackOrder order {};
    };

    mutable std::optional<KeptTrackOrder> keptTrackOrder;

    // A clip that changed, with the track it was on before and the one it is on after: none before for a clip added, none
    // after for one taken out (erased), the same track twice for a change in place.
    struct ClipChange
    {
        ClipID id;
        TrackID before, after;
    };

    // A modulator or a macro that changed (ModulationValue.h): its ID; where it was and where it is (an empty place where it
    // wasn't, or isn't), whether it was there and whether it is (both, for one changed in place or come to another place); the
    // parts of it that changed; and its value after, for one that is there, valid during the call alone: what plays may set
    // its modulator's node from it at once (PlaybackSession.h), as nothing else of the song is read meanwhile. Set its
    // members by name.
    struct ModulationChange
    {
        // The parts, as bits of parts: each its own, as what plays takes each its own way (PlaybackSession.h).
        enum Part : std::uint16_t
        {
            made = 1 << 0,            // added, taken out, or come to another place (another channel, another container)
            kind = 1 << 1,            // a modulator's kind, or a macro become a modulator: another modulator
            name = 1 << 2,            // a macro's name
            off = 1 << 3,             // a modulator's switch
            valuesSetByHand = 1 << 4, // a modulator's values set by hand, any of them
            valuesAppeared = 1 << 5,  // and with them its first value set by hand, or its last set back to its default
            steps = 1 << 6,           // a step sequencer's steps
            macroValue = 1 << 7,      // a macro's value
            modulations = 1 << 8      // what it modulates: a modulation made, taken out, moved or changed in amount
        };

        ModulatorID id {};
        ModulatorPlace before {}, after {};
        bool wasThere = false, isThere = false;
        std::uint16_t parts = 0;
        const ModulatorValue* now = nullptr;

        bool has (std::uint16_t part) const noexcept { return (parts & part) != 0; }
    };

    // A send that changed (SendsValue.h): its ID; the track it was on and the one it is on (none before for one added, none
    // after for one taken out, the same for one changed in place); the return it went to and the one it goes to (none where
    // it wasn't, or isn't); the parts of it that changed; and its value after, for one that is there, valid during the call
    // alone: what plays may set its level from it at once (PlaybackSession.h), as nothing else of the song is read meanwhile.
    // Set its members by name.
    struct SendChange
    {
        // The parts, as bits of parts: each its own, as what plays takes each its own way (PlaybackSession.h).
        enum Part : std::uint8_t
        {
            made = 1 << 0,  // added, taken out, or come to another track
            route = 1 << 1, // the return it goes to, or whether it sends before the fader
            level = 1 << 2  // its level
        };

        DeviceID id {};
        TrackID fromBefore {}, fromAfter {};
        TrackID toBefore {}, toAfter {};
        std::uint8_t parts = 0;
        const SendValue* now = nullptr;

        bool has (std::uint8_t part) const noexcept { return (parts & part) != 0; }
        bool wasThere() const noexcept { return fromBefore.isValid(); }
        bool isThere() const noexcept { return fromAfter.isValid(); }
    };

    // A device, a container or a chain that changed (DevicesValue.h): its ID and its form; where it was and where it is (an
    // empty place where it wasn't, or isn't), whether it was there and whether it is (both, for one changed in place or come
    // to another place); the parts of it that changed; and its value before and after, for one that was and is there, valid
    // during the call alone: what plays may set its device's node from the two at once (PlaybackSession.h: the values that
    // differ between them), as nothing else of the song is read meanwhile. Set its members by name. Only what the devices'
    // value covers (DevicesValue.h): never a plug-in's VALUE, its state, program, format ID or version, a chain's mute or solo
    // (the store's: DeviceStoreChange), nor a window's place.
    struct DeviceChange
    {
        // The parts, as bits of parts: each its own, as what plays takes each its own way (PlaybackSession.h).
        enum Part : std::uint16_t
        {
            made = 1 << 0,              // added or taken out: alone, or with the track, the container or the chain it is in
            place = 1 << 1,             // come to another place (its channel's track given another ID, another container, chain)
            settings = 1 << 2,          // its switch, sidechain, type or plug-in, a container's or a chain's name, split, or kind
            mix = 1 << 3,               // a plug-in's or a container's mix
            values = 1 << 4,            // Phazite's own device's values set by hand, any of them
            valuesAppeared = 1 << 5,    // and with them its first value set by hand, or its last set back to its default
            sounds = 1 << 6,            // Phazite's own device's sounds, any of them
            soundsAppeared = 1 << 7,    // and with them its first sound, or its last taken away
            chainFader = 1 << 8,        // a chain's volume or pan
            chainRange = 1 << 9,        // a chain's crossover, or the keys or velocities it plays
            sidechainSwitched = 1 << 10 // a value set by hand that turns what its sidechain hears on or off (Sidechain.h)
        };

        DeviceID id {};
        DeviceValue::Form form = DeviceValue::Form::own;
        DevicePlace before {}, after {};
        bool wasThere = false, isThere = false;
        std::uint16_t parts = 0;
        const DeviceValue* was = nullptr;
        const DeviceValue* now = nullptr;

        bool has (std::uint16_t part) const noexcept { return (parts & part) != 0; }
    };

    // An entry of the devices' store that changed (DeviceStoreValue.h): its kind; its item (the plug-in, or the chain for a
    // switch) and, for a plug-in's value, its parameter (the plug-in's own ID for it); whether the song's entry of it came or
    // went (a plug-in's first value set by hand, its last set back to its default); whether the song's entries differ at all
    // (false for a plug-in's held value alone, written or taken back: DeviceStoreValue.h, PluginValueEntry); for a plug-in's
    // value, the value to give the plug-in (the entry after: the song's, or the held one an Undo of a first value comes back
    // to; none where none is left); and where the item is, as the devices' area holds it as the store tells (which an Undo
    // puts back before or after the store, in the step's order), with whether the song holds it at all (an answer of a
    // plug-in a step took away is written for a device the song doesn't hold), so that what is told names the item's channel
    // without reading the tree. Set its members by name.
    struct DeviceStoreChange
    {
        enum class Kind : std::uint8_t
        {
            pluginValue,
            state,
            program,
            formatID,
            version,
            chainMute,
            chainSolo
        };

        Kind kind = Kind::pluginValue;
        DeviceID item {};
        juce::String parameter {};
        bool appeared = false, went = false;
        bool changesSong = true;
        std::optional<float> valueToGive {};
        DevicePlace place {};
        bool isInSong = false;
    };

    // What hears the song's values change, from one place: one listener for every area, a function for each, which does
    // nothing unless a listener overrides it, so that an area added changes only the listeners that care. Each registers
    // once (addListener()). A listener only marks what changed, and reads nothing of the song while it is told: during an
    // undo the rest of the step may not be back yet (ValueArea.h). It does its work later (a plan's next compile, the next
    // turn of the message loop), however many calls came before. Listening changes nothing of the values: a song held const
    // may be listened to.
    struct Listener
    {
        virtual ~Listener() = default;

        // The clips whose notes and controllers' values changed, by a step, an undo, a redo or a write outside the history:
        // compared by identity, clip by clip.
        virtual void clipNotesChanged (const std::vector<ClipID>& clips) { juce::ignoreUnused (clips); }

        // The song's clips, by their IDs: the clips' area tells, in one call for each change of its value (each of a step's
        // changes, an undo's or a redo's whole step, a write outside the history), each clip added, taken out, moved to another
        // track or changed in place (any of its properties, its warp markers), found by comparing the two values by identity
        // clip by clip, and each track that holds the same clips in another order. A track added or taken out tells each of
        // its clips (those of the tracks in it too) as added or taken out, in one call. A clip's notes and controllers' values
        // are no part of it: clipNotesChanged()'s.
        virtual void clipsChanged (const std::vector<ClipChange>& clips, const std::vector<TrackID>& reordered)
        {
            juce::ignoreUnused (clips, reordered);
        }

        // The song's tempo map (TempoMapValue.h): its tempos or its time signatures, any of them. The tempo map's area tells it,
        // once for each change of its value: a step's, an undo's or a redo's whole step, a write outside the history. What
        // listens to the song's root leaves what its tree tells under its TEMPOMAP element alone (isHeardAsKept(), below,
        // through isHeardAsTempoMap()): what the keeper takes in there (its safety net) is told here, once.
        virtual void tempoMapChanged() {}

        // The song's chord lane (ChordLaneValue.h): its chords or its key changes, any of them. The chord lane's area tells it,
        // once for each change of its value: a step's, an undo's or a redo's whole step, a write outside the history. What
        // listens to the song's root leaves what its tree tells inside its CHORDLANE alone (isHeardAsKept(), below, through
        // isHeardAsChordLane()): what the keeper takes in there (its safety net) is told here, once. The CHORDLANE's own coming and going, which says
        // whether the song has a lane (a song without one takes its keys from the key set last), is the tree's, which it hears
        // as it always did: so a step that makes the lane (its first chord or key) is heard twice, its element added by the
        // tree and its first key here, and only marks twice.
        virtual void chordLaneChanged() {}

        // The song's automation (CurveValues.h): the parameters whose curves changed (a point added, taken out, moved or bent,
        // a curve added or taken out, given another parameter: the one it had and the one it has; those that came to other
        // places in the curves' order), each once, in one call. The curves' area tells it, once for each change of its value:
        // a step's, an undo's or a redo's whole step, a write outside the history (CurvesArea.h: CurvesTeller). What listens
        // to the song's root leaves what its tree tells of its AUTOMATION and in it alone (isHeardAsKept(), below, through
        // isHeardAsCurves()): what the keeper takes in there (its safety net) is told here, once.
        virtual void curvesChanged (const std::vector<ParameterRef>& parameters) { juce::ignoreUnused (parameters); }

        // The song's MIDI links (MidiLinksValue.h): a link made, moved or taken away, any of them. The links' area tells it, once
        // for each change of its value: a step's, an undo's or a redo's whole step, a write outside the history. What listens
        // to the song's root leaves what its tree tells inside its MIDILINKS alone (isHeardAsKept(), below, through
        // isHeardAsMidiLinks()): what the keeper takes in there (its safety net) is told here, once. The MIDILINKS's own coming
        // and going (added with the first link, written into the song's file) is the tree's, which it hears as it always did:
        // so a step that makes the first link is heard twice, its MIDILINKS added by the tree and its link here, and only
        // marks twice.
        virtual void midiLinksChanged() {}

        // The song's sections (SectionsValue.h), by their IDs: each section added, taken out or changed (any of its
        // properties), each once, in one call (SectionsArea.h: findSectionsChanged()). The sections' area tells it, once for each
        // change of its value: a step's, an undo's or a redo's whole step, a write outside the history; a change of their order
        // alone (a version's) names none. What listens to the song's root leaves what its tree tells inside its SECTIONS alone
        // (isHeardAsKept(), below, through isHeardAsSections()): what the keeper takes in there (its safety net) is told here,
        // once. The SECTIONS's own coming and going (added with the first section, written into the song's file) is the tree's,
        // which it hears as it always did: so a step that makes the first section is heard twice, its SECTIONS added by the tree
        // and its section here, and only marks twice.
        virtual void sectionsChanged (const std::vector<SectionID>& sections) { juce::ignoreUnused (sections); }

        // The song's modulators and macros (ModulationValue.h), by their IDs: each one added, taken out, come to another place
        // or changed, each once in a call, with where it was and is, the parts of it that changed (ModulationChange::Part) and
        // its value after (ModulationChange::now); and each owner (ModulatorPlace: a channel, or a container) that holds those
        // it held before in another order. A track, a folder's tracks, a container or the track list that comes into the song
        // or leaves it with modulators or macros in it tells each of them as added or taken out. Modulation's area tells it
        // (ModulationArea.h: ModulationTeller), once for each change of its value: a step's, an undo's or a redo's whole step,
        // a write outside the history; so what a step, its Undo or its Redo told in all is what differs in the song's
        // modulation before and after. The song's tree holds none of it: a TRACK or a CONTAINER that comes or goes with
        // modulators is the tree's, heard as before, and its modulators are told here, so the step is heard twice, and only
        // marks twice.
        virtual void modulationChanged (const std::vector<ModulationChange>& modulators, const std::vector<ModulatorPlace>& reordered)
        {
            juce::ignoreUnused (modulators, reordered);
        }

        // The song's sends (SendsValue.h), by their IDs: each one added, taken out, come to another track or changed, each once
        // in a call, with the track it was on and is on, the return it went to and goes to, the parts of it that changed
        // (SendChange::Part) and its value after (SendChange::now); each track that holds the sends it held before in another
        // order; and the song's sends whole after (now: a copy of the area's value, valid during the call, which what plays
        // keeps for what decides which send sends: SongPlayer.h). A track, a folder's tracks or the track list that comes into
        // the song or leaves it with sends on it tells each of them as added or taken out. The sends' area tells it
        // (SendsArea.h: SendsTeller), once for each change of its value: a step's, an undo's or a redo's whole step, a write
        // outside the history; so what a step, its Undo or its Redo told in all is what differs in the song's sends before and
        // after. The song's tree holds none of them: a TRACK that comes or goes with sends is the tree's, heard as before, and
        // its sends are told here, so the step is heard twice, and only marks twice.
        virtual void sendsChanged (const std::vector<SendChange>& sends, const std::vector<TrackID>& reordered, const SongSends& now)
        {
            juce::ignoreUnused (sends, reordered, now);
        }

        // The song's devices, containers and chains (DevicesValue.h), by their IDs: each one added, taken out, come to another
        // place or changed, each once in a call, with where it was and is, the parts of it that changed (DeviceChange::Part)
        // and its value before and after (DeviceChange::was, now); each owner (DevicePlace: a channel, a container, a chain)
        // that holds those it held before in another order; and the song's devices whole after (now: a copy of what the
        // keeper holds, valid during the call, the changes' values after in it), for what a change needs of the others: a
        // chain's place among its container's (its default crossover, whether it has one at all). A track, a folder's
        // tracks or the track list that comes into the song or leaves it with devices in it tells each of them as added or
        // taken out (a track JUCE moves, taken out and put in again, tells each twice). The devices' area tells it
        // (DevicesArea.h: DevicesTeller), once for each change of its value: a step's, an undo's or a redo's whole step, a
        // write outside the history; so what a step, its Undo or its Redo told in all is what differs in the song's devices
        // before and after. What the value doesn't cover is the store's (deviceStoreChanged(), below: a plug-in's values set
        // by hand, its state, program, format ID and version, a chain's mute and solo), or a window's place, which no one is
        // told. The song's tree holds none of them: a TRACK that comes or goes with devices is the tree's, heard as before,
        // and its devices are told here, so the step is heard twice, and only marks twice: the views' watcher raises the
        // devices' group for it (and any channel where one of them listens to a sidechain) beside what the tree raises for
        // the track (ProjectContext.cpp).
        virtual void devicesChanged (const std::vector<DeviceChange>& devices, const std::vector<DevicePlace>& reordered,
                                     const SongDevices& now)
        {
            juce::ignoreUnused (devices, reordered, now);
        }

        // The song's devices' store (DeviceStoreValue.h): each entry that changed (DeviceStoreChange), sorted by its item, its
        // kind and its parameter, in one call; and the store whole after (now: a copy of the area's value, valid during the
        // call, which what plays keeps for what decides which chain is heard: SongPlayer.h). The store's area tells it
        // (DeviceStoreArea.h: DeviceStoreTeller), at once, once for each change of its value: a step's, an undo's or a redo's
        // whole step, a write outside the history (a plug-in's instance hears its own writes, inside them). An Undo and a Redo
        // put back only the entries their step changed, so a value a plug-in's window set since survives the Undo of a step
        // that changed another (ValueArea.h: keyed). The song's tree holds none of it: a DEVICE or a CHAIN that comes or goes
        // is the tree's (and the devices'), heard as before, and its entries stay in the store, told nothing (a removal keeps
        // them, for its Undo).
        virtual void deviceStoreChanged (const std::vector<DeviceStoreChange>& entries, const DeviceStore& now)
        {
            juce::ignoreUnused (entries, now);
        }

        // The song's tracks, folders, returns and master (TracksValue.h), by their IDs: each one added, taken out or changed,
        // each once in a call, with its folder before and after and, for one that came, went or came to another folder, its
        // place in its list before and after (TrackChange::indexBefore, indexAfter: -1 for any other), the parts of it that
        // changed (TrackChange::Part, TracksArea.h) and its value after (TrackChange::now); each folder whose list holds
        // those it held before in another order (TrackID() for the top list); and the song's tracks whole after (now: a copy
        // of the area's value, valid during the call), which what plays keeps for what decides what is heard (SongPlayer.h:
        // heldTracks). A track, or a folder with the tracks in it, added or taken out tells each of them (its clips, devices,
        // modulators and sends are told by their own areas); a track moved to another folder is told once, its place
        // (TrackChange::place), the tracks in it nothing (their folder is the same); the master's ID given anew in place (a
        // version's) is told as the master's change, never as a track gone and another come, and the master put in anew whole
        // (a merge's choice) as the master made. The tracks' area tells it (TracksArea.h: TracksTeller), once for each change
        // of its value: a step's, an undo's or a redo's whole step, a write outside the history; so what a step, its Undo or
        // its Redo told in all is what differs in the song's tracks before and after. Its switches and its view are their
        // areas', told by the two below, its devices by devicesChanged().
        virtual void tracksChanged (const std::vector<TrackChange>& tracks, const std::vector<TrackID>& reordered, const SongTracks& now)
        {
            juce::ignoreUnused (tracks, reordered, now);
        }

        // The tracks' switches (TrackSwitchesValue.h): each entry that changed, a mute, a solo, a frozen switch or an automation
        // mode (the master's too), with whether it came or went (TrackSwitchChange, TracksArea.h), in one call; and the
        // switches whole after (now: a copy, valid during the call, which what plays keeps for what decides what is heard).
        // The switches' area tells it (TracksArea.h: TrackSwitchesTeller), once for each change of its value: a step's, an
        // undo's or a redo's whole step, a write outside the history (a mute toggled). An Undo and a Redo put back only the
        // entries their step changed (ValueArea.h: keyed). A track taken out tells none of its switches (the area keeps them
        // for an Undo, which brings the track back with them, telling none either).
        virtual void trackSwitchesChanged (const std::vector<TrackSwitchChange>& switches, const TrackSwitches& now)
        {
            juce::ignoreUnused (switches, now);
        }

        // How the tracks' rows show (TracksOutside.h: view state, no change of the song's and no undo step): the tracks whose
        // height, collapsed or pinned switch, drum mode or drum rows or automation lanes changed, and whether the master's
        // did. The view's value tells it (TracksArea.h: TracksViewTeller), once for each write that changes it, of the tracks
        // the song holds alone: not of a track a maker gives its view before it puts the track in the song (a duplicate's
        // copy, a merge's whole track: its row is new, told by tracksChanged()), nor of one a step took away (its entry kept
        // for an Undo). Only the views follow it (ProjectContext.cpp).
        virtual void tracksViewChanged (const std::vector<TrackID>& tracks, bool master) { juce::ignoreUnused (tracks, master); }

        // What the song holds of its own (SongOwnValue.h), told once for each change of each of its three values (a setter's
        // set, an Undo's or a Redo's move, a write outside the history), each only where it differs: the song's own value
        // (SongOwnValue: written in the history alone), with the parts of it that differ (SongOwnChange, SongOwnArea.h: the
        // outline, the notes, the numbering of IDs, patterns' speed, a merge waiting); its standing (SongStanding: written in
        // the history and outside it), with whether the key set last differs and whether where the song stands in its
        // versions does (versionsBranch, versionsBase); and what it keeps of how it plays and shows (SongOutside: never in
        // the history), with whether what plays differs (the loop's four, the metronome: what a plan holds of it), the play
        // start, punch, the input chosen last and the grids being none of it. Each area's teller tells it (SongOwnArea.h:
        // SongOwnTeller, SongStandingTeller, SongOutsideTeller), from the value before and after. The song's tree holds none
        // of them, nor its count of IDs (Song.h: makeID(), which tells no one), nor what Phazite doesn't know at its root
        // (dropped as the song opens).
        virtual void songOwnChanged (const SongOwnChange& change) { juce::ignoreUnused (change); }
        virtual void songStandingChanged (bool lastKey, bool versionsNames) { juce::ignoreUnused (lastKey, versionsNames); }
        virtual void songOutsideChanged (bool plays) { juce::ignoreUnused (plays); }
    };

    void addListener (Listener*) const;
    void removeListener (Listener*) const;

    // How many TEMPO and TIMESIGNATURE elements the keeper took into the tempo map's area from the song's tree since the song
    // held its values (the safety net, above): what the versions, which give the song its tempo map through setTempoMap(),
    // check they never leave to it.
    int countTempoMapAdopted() const noexcept { return tempoMapAdopted; }

    // How many CHORD and CHORDKEY elements the keeper took into the chord lane's area from the song's tree since the song held
    // its values (the safety net, above): what the versions, which give the song its lane through the chord lane's code
    // (Chords.h: makeChordLaneAs()), check they never leave to it.
    int countChordLaneAdopted() const noexcept { return chordLaneAdopted; }

    // How many MIDILINK elements the keeper took into the MIDI links' area from the song's tree since the song held its values
    // (the safety net, above): what the versions, which give the song its links through the MIDI links' code (MidiLinks.h:
    // makeMidiLinksAs(), chooseMidiLinkClash()), check they never leave to it.
    int countMidiLinksAdopted() const noexcept { return midiLinksAdopted; }

    // How many CURVE elements the keeper took into the curves' area from the song's tree since the song held its values (the
    // safety net, above): what the versions, which give the song its curves through automation's code (Automation.h:
    // setCurveValue(), eraseCurve(), setCurveOrder(), chooseCurveClash()), check they never leave to it.
    int countCurvesAdopted() const noexcept { return curvesAdopted; }

    // How many SECTION elements the keeper took into the sections' area from the song's tree since the song held its values (the
    // safety net, above): what the versions, which give the song its sections through the sections' code (Sections.h:
    // makeSectionsAs(), chooseSectionClash()), check they never leave to it.
    int countSectionsAdopted() const noexcept { return sectionsAdopted; }

    // How many MODULATOR and MACRO elements the keeper took into modulation's area from the song's tree since the song held
    // its values (the safety net, above: a container's makers make its macros as values, which its inserter sets, Mixer.h:
    // insertDevice(), so that a container made or loaded from a preset leaves it none): what the versions and the inserters,
    // which give the song its modulation through modulation's code (Modulation.h: makeModulationLike()'s setAllModulation(),
    // chooseModulationClash()), check they never leave to it.
    int countModulationAdopted() const noexcept { return modulationAdopted; }

    // How many DEVICE and CHAIN elements that came into the song with what the store holds (a store's property, a VALUE, a
    // window's place) the keeper took it out of since the song held its values, and how many lone VALUE elements it took in
    // (the safety net, DeviceStoreArea.h): what a track a merge's choice brings in whole holds; what the versions' passes and
    // the devices' writers, which give the store its entries through its setters (model/plugins/internal/DeviceStore.h:
    // setStoreEntries(); model/mixer/internal/Mixer.h: setNewDevices()), check they never leave to it.
    int countDeviceStoreAdopted() const noexcept { return deviceStoreAdopted; }

    // How many DEVICE, CONTAINER and CHAIN elements the keeper took into the devices' area from the song's tree since the song
    // held its values (the safety net, above: a channel that comes into the song with them, a lone one outside the history):
    // what the versions' passes, a merge's choice of a whole track, a duplicate and the inserters, which give the song its
    // devices through the devices' setters (model/mixer/internal/Mixer.h), check they never leave to it.
    int countDevicesAdopted() const noexcept { return devicesAdopted; }

    // How many TRACK and MASTER elements that came into the song with a switch, a view property or a LANES in it the keeper
    // took them out of since the song held its values, and how many lone LANES it read in outside the history (the safety
    // net, TracksArea.h: TrackLeavesKeeper): what a track a merge's choice brings in whole would hold, which the merge gives
    // through the areas (model/arrangement/Tracks.cpp: chooseTrackClash()); what the versions' passes and a duplicate, which
    // give the switches and the view through their setters, check they never leave to it.
    int countTrackLeavesAdopted() const noexcept { return trackLeavesAdopted; }

    // How many TRACK elements added to the song's TRACKS, MASTER elements added to the song and properties written to its
    // MASTER the tracks' part of the keeper took in since the song held its values (its safety net, TracksArea.h: TracksKeeper,
    // outside the history; one in a step is refused): what the versions' passes, a merge's choice, a duplicate, a preset, the
    // clipboard and the DAWproject import, which give the song its tracks through the tracks' setters, check they never leave
    // to it (model/versions/ApplySong.cpp, VersionsModel.cpp: NothingTakenIn).
    int countTracksAdopted() const noexcept { return tracksAdopted; }

    // How many of the song's own properties (its root's own, and its count of IDs) and LASTKEY, PIANOROLL and MERGING elements
    // written to the song's tree outside the history its part of the keeper took in since the song held its values (the
    // safety net, SongOwnArea.h: SongOwnKeeper): what the versions' passes, a merge's choice, a merge waiting set and the
    // loader, which give the song its own through its setters (SongOwnValue.h) and its count through Schema.h's
    // countIDsFrom(), check they never leave to it.
    int countSongOwnAdopted() const noexcept { return songOwnAdopted; }

private:
    friend class Song;

    // Each area's parts, each in its area's own file (the recipe, above), and the keeper, which asks them in turn.
    struct ClipsTeller;                             // ClipsArea.h
    struct NotesTeller;                             // NotesArea.h
    struct TempoMapTeller;                          // TempoMapArea.h
    struct ClipsKeeper;                             // ClipsArea.h: the clips' adoption, with their notes
    struct TempoMapKeeper;                          // TempoMapArea.h
    struct ChordLaneTeller;                         // ChordLaneArea.h
    struct ChordLaneKeeper;                         // ChordLaneArea.h
    struct MidiLinksTeller;                         // MidiLinksArea.h
    struct MidiLinksKeeper;                         // MidiLinksArea.h
    struct CurvesTeller;                            // CurvesArea.h
    struct CurvesKeeper;                            // CurvesArea.h
    struct SectionsTeller;                          // SectionsArea.h
    struct SectionsKeeper;                          // SectionsArea.h
    struct ModulationTeller;                        // ModulationArea.h
    struct ModulationKeeper;                        // ModulationArea.h
    struct SendsTeller;                             // SendsArea.h
    struct SendsKeeper;                             // SendsArea.h: the adoption
    struct DeviceStoreTeller;                       // DeviceStoreArea.h
    struct DeviceStoreKeeper;                       // DeviceStoreArea.h
    struct DevicesTeller;                           // DevicesArea.h
    struct DevicesKeeper;                           // DevicesArea.h
    struct TracksTeller;                            // TracksArea.h
    struct TracksKeeper;                            // TracksArea.h: the placeholders' safety net, and the adoption
    struct TrackSwitchesTeller;                     // TracksArea.h
    struct TracksViewTeller;                        // TracksArea.h
    struct TrackLeavesKeeper;                       // TracksArea.h: the switches' and the view's
    struct SongOwnTeller;                           // SongOwnArea.h
    struct SongStandingTeller;                      // SongOwnArea.h
    struct SongOutsideTeller;                       // SongOwnArea.h
    struct SongOwnKeeper;                           // SongOwnArea.h: the song's own's safety net, and its adoption
    struct Keeper;                                  // SongValues.cpp
    mutable juce::ListenerList<Listener> listeners; // told by the areas' tellers (addListener())
    std::unique_ptr<ClipsTeller> clipsTeller;
    std::unique_ptr<NotesTeller> notesTeller;
    std::unique_ptr<TempoMapTeller> tempoMapTeller;
    std::unique_ptr<ChordLaneTeller> chordLaneTeller;
    std::unique_ptr<MidiLinksTeller> midiLinksTeller;
    std::unique_ptr<CurvesTeller> curvesTeller;
    std::unique_ptr<SectionsTeller> sectionsTeller;
    std::unique_ptr<ModulationTeller> modulationTeller;
    std::unique_ptr<SendsTeller> sendsTeller;
    std::unique_ptr<DeviceStoreTeller> deviceStoreTeller;
    std::unique_ptr<DevicesTeller> devicesTeller;
    std::unique_ptr<TrackSwitchesTeller> trackSwitchesTeller;
    std::unique_ptr<TracksViewTeller> tracksViewTeller;
    std::unique_ptr<TracksTeller> tracksTeller;
    std::unique_ptr<SongOwnTeller> songOwnTeller;
    std::unique_ptr<SongStandingTeller> songStandingTeller;
    std::unique_ptr<SongOutsideTeller> songOutsideTeller;
    std::unique_ptr<Keeper> keeper;
    int tempoMapAdopted = 0;
    int chordLaneAdopted = 0;
    int midiLinksAdopted = 0;
    int curvesAdopted = 0;
    int sectionsAdopted = 0;
    int modulationAdopted = 0;
    int deviceStoreAdopted = 0;
    int devicesAdopted = 0;
    int trackLeavesAdopted = 0;
    int tracksAdopted = 0;
    int songOwnAdopted = 0;
    std::uint64_t nextPointKey = 1; // givePointKey(): outside the history, never undone, never saved

    // In the history, or outside it, as the song is written now; and a value alike to the area's, without a step of its own
    // (ValueArea.h: setWithoutStep()). Each with its area's setters.
    void setAll (Song&, SongClips next);
    void setAll (Song&, NotesByClip next);
    void setWithoutStep (Song&, SongClips next);
    void setWithoutStep (Song&, NotesByClip next);

    void adopt (Song&); // Song::holdValues()

    JUCE_DECLARE_NON_COPYABLE (SongValues)
    JUCE_DECLARE_NON_MOVEABLE (SongValues)
};

// Whether what the song's tree tells of an element (a property changed, a child added, taken out or moved in it) is the
// tempo map's, which a listener of the song's root hears from the values' listener alone (SongValues::Listener::
// tempoMapChanged()) and leaves alone in its tree callbacks: the song's TEMPOMAP element (the child of its PHAZITE element),
// or an element in it. And whether a child added to an element or taken out of it is: the TEMPOMAP itself, added to the
// song or taken out, or a child of an element that is. A child moved is the tempo map's where the element it moved in is.
// The song's TEMPOMAP is an empty placeholder (the map is its area's), so the tree tells of it only what the keeper takes
// in (its safety net, above), which the area tells. In TempoMapArea.cpp.
bool isHeardAsTempoMap (const juce::ValueTree& element);
bool isHeardAsTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells of an element (a property changed, a child moved in it) is the chord lane's, which a
// listener of the song's root hears from the values' listener alone (SongValues::Listener::chordLaneChanged()) and leaves
// alone in its tree callbacks: the song's CHORDLANE element (the child of its PHAZITE element), or an element in it. And
// whether a child added to an element or taken out of it is: a child of an element that is. Not the CHORDLANE itself added
// to the song or taken out, which says whether the song has a lane: the tree's, heard as any element's (Listener::
// chordLaneChanged()). The song's CHORDLANE is an empty placeholder (the lane is its area's), so the tree tells of what is in
// it only what the keeper takes in (its safety net, above), which the area tells. In ChordLaneArea.cpp.
bool isHeardAsChordLane (const juce::ValueTree& element);
bool isHeardAsChordLane (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells of an element (a property changed, a child added, taken out or moved in it) is the
// curves', which a listener of the song's root hears from the values' listener alone (SongValues::Listener::
// curvesChanged()) and leaves alone in its tree callbacks: the song's AUTOMATION element (the child of its PHAZITE element),
// or an element in it. And whether a child added to an element or taken out of it is: the AUTOMATION itself, added to the
// song or taken out, or a child of an element that is. The song's AUTOMATION is an empty place every song that holds its
// values has (the curves are their area's), so the tree tells of it only what the keeper takes in (its safety net, above),
// which the area tells. In CurvesArea.cpp.
bool isHeardAsCurves (const juce::ValueTree& element);
bool isHeardAsCurves (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells of an element (a property changed, a child moved in it) is the MIDI links', which a
// listener of the song's root hears from the values' listener alone (SongValues::Listener::midiLinksChanged()) and leaves
// alone in its tree callbacks: the song's MIDILINKS element (the child of its PHAZITE element), or an element in it. And
// whether a child added to an element or taken out of it is: a child of an element that is. Not the MIDILINKS itself added to
// the song or taken out (with the first link, written into the song's file): the tree's, heard as any element's. The song's
// MIDILINKS is an empty placeholder (the links are their area's), so the tree tells of what is in it only what the keeper
// takes in (its safety net, above), which the area tells. In MidiLinksArea.cpp.
bool isHeardAsMidiLinks (const juce::ValueTree& element);
bool isHeardAsMidiLinks (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells of an element (a property changed, a child moved in it) is the sections', which a listener
// of the song's root hears from the values' listener alone (SongValues::Listener::sectionsChanged()) and leaves alone in its
// tree callbacks: the song's SECTIONS element (the child of its PHAZITE element), or an element in it. And whether a child
// added to an element or taken out of it is: a child of an element that is. Not the SECTIONS itself added to the song or taken
// out (with the first section, written into the song's file): the tree's, heard as any element's. The song's SECTIONS is an
// empty placeholder (the sections are their area's), so the tree tells of what is in it only what the keeper takes in (its
// safety net, above), which the area tells. In SectionsArea.cpp.
bool isHeardAsSections (const juce::ValueTree& element);
bool isHeardAsSections (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells is the devices', which a listener of the song's root hears from the values' listener
// alone (SongValues::Listener::devicesChanged()) and leaves alone in its tree callbacks: a DEVICE, a CONTAINER or a CHAIN, or
// an element in one (a property changed, children in another order); a DEVICE, a CONTAINER, a CHAIN, a VALUES or a SOUNDS
// added or taken out, or a child of an element that is one. The song's tree holds none of them: what it tells of them is what
// the keeper takes out of a channel that came in with them, or a lone one it reads in (its safety net, DevicesArea.h), which
// the area tells. Not a TRACK, the MASTER or the track list that comes or goes with devices in it, which is the tree's, heard
// as before. In DevicesArea.cpp.
bool isHeardAsDevices (const juce::ValueTree& element);
bool isHeardAsDevices (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsDevices (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells is the devices' store's, which a listener of the song's root hears from the values'
// listener alone (SongValues::Listener::deviceStoreChanged()) and leaves alone in its tree callbacks: a VALUE under a DEVICE,
// or anything in one (an element of it, a child added to it or taken out, a property of it changed), added or taken out; a
// store's property of a DEVICE (a plug-in's state, program, format ID and version, a window's place) or of a CHAIN (its mute
// and solo) changed. The song's tree holds none of them: what it tells of them is what the keeper takes out of a DEVICE or a
// CHAIN that came in with them (its safety net, DeviceStoreArea.h), which the area tells. In DeviceStoreArea.cpp.
bool isHeardAsDeviceStore (const juce::ValueTree& element);
bool isHeardAsDeviceStore (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsDeviceStore (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells is any area's of the song's values, which a listener of the song's root hears from the
// values' listener alone (SongValues::Listener) and leaves alone in its tree callbacks: of an element (a child moved in it),
// of a property changed (the element and the property: a DEVICE's or a CHAIN's properties are the devices' but for the
// store's), and of a child added to an element or taken out of it. Each the OR of the areas' own functions above
// (isHeardAs<Area>()), a line each, in the order of the areas: the tempo map's, the chord lane's, the curves', the MIDI
// links', the sections', the devices', the tracks' (TracksArea.h: isHeardAsTracks()), then the devices' store's (modulation's
// and the sends' went at their switches: the tree holds nothing of them; the store's stays for what its keeper takes out of a
// DEVICE or a CHAIN that comes in with it), then the song's own (SongOwnArea.h: isHeardAsSongOwn(): its safety net's alone, a
// property of the root's own or its count of IDs, and a LASTKEY, PIANOROLL or MERGING, written to the song's tree).
// What every listener's tree callback asks, once, where it returns early for what the areas tell (the recipe's step 12,
// above): a new area adds its line here, not a line in each listener. In SongValues.cpp.
bool isHeardAsKept (const juce::ValueTree& element);
bool isHeardAsKept (const juce::ValueTree& element, const juce::Identifier& property);
bool isHeardAsKept (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
