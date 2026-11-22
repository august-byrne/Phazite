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
// which automation's code (model/automation/internal/Automation.h) computes each edit on; and the song's sections
// (SectionsValue.h: each by its ID), which the sections' code (model/arrangement/internal/Sections.h) computes each edit on.
// All as plain values in Phazite's own types.
//
// The values are the storage: the song holds an area of each (SongValues) in its one undo history, and the song's tree holds
// no CLIP element (nor a clip's NOTE, CONTROL or WARP elements), no TEMPO or TIMESIGNATURE element, no CHORD or CHORDKEY
// element, no MIDILINK element, no CURVE or POINT element and no SECTION element: its TEMPOMAP element stays, empty, the
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
//      its value); its part of the keeper (struct SongValues::<Area>Keeper: childAdded() and, where it acts there,
//      childRemoved(), asked for every element that comes into the song's tree or leaves it, whoever's it is, each returning
//      at once for what is not the area's, and for what is, reading the area and setting it through its setter in the mode
//      of the moment; its refusal of a lone element of the area (step 13); and a static adopt() that takes what the loaded
//      tree holds of it in, outside the history); its setter, a member of SongValues defined there; its whole state's part
//      (leadsTo<Area>(), write<Area>In()); its comparison of two kept values (holdSame<Area>(), declared in WholeState.h) and
//      of two elements with their kept values (holdSame<Area>Of(), is<Area>Place()); and, if it holds IDs,
//      findLargestIDIn (const <Area>Value&) (IDsHeld.h). Each new file listed in CMakeLists.txt (phazite_core) and in
//      src/model/README.md. An area whose element's presence means something (the chord lane's CHORDLANE: a song without
//      one takes its keys from the key set last), or only is written into the song's file and compared with the saved copy
//      (the MIDI links' MIDILINKS, added with the first link and kept once the last goes): the element stays as an empty
//      placeholder where the song has one, and only the area's code adds or takes it away, in the step that changes the
//      value: the element first when adding, the value first when taking away, so that the value never says more than the element; isHeardAs<Area>() covers only
//      what is inside it, and its own adding and taking away stays the tree's (the listeners hear it as a child of the
//      root); holdSame<Area>() and the check (step 7) compare whether each has the element too; and whole state writes an
//      empty placeholder as an empty element. An area whose element is a place every song that holds its values has (the
//      tempo map's TEMPOMAP, the curves' AUTOMATION): its adopt() makes it where the loaded tree has none, nothing adds or
//      takes it away after, isHeardAs<Area>() covers it whole (the element itself added or taken out too), and the passes
//      over elements leave it alone (step 9); where files have held none of it while the area was empty (AUTOMATION, once
//      added with the first curve), whole state leaves it out while the area is empty (CurvesArea.h: writeCurvesIn()).
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
//      and, where a merge's choice never leaves the area's elements to the keeper (not the clips', which come in with a
//      track chosen whole), of NothingTakenInButClips::count() in model/versions/VersionsModel.cpp. An area with a
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
//        automation; nor modulation's, which moves nothing a render is made of).
//      - FreezeKeeper (model/mixer/Freeze.cpp, internal/Freeze.h): marks the frozen tracks whose file holds what changed
//        (all of them for the tempo map) and compares them at its later moment. What a frozen track's file holds of an area is
//        in its Holds (parts: copies of its elements, getWhatTheFreezeHolds(); values: ValuesKept; tempos; curves: its frozen
//        devices' automation, getFrozenCurves(); modulators: its modulators and its frozen containers' macros, as values,
//        getFrozenModulators()), compared in holdsWhatWasFrozen(): an area held in parts moves to its value there at its
//        switch. An area whose value is read to compare it (the curves, the modulators) is read only for a track it told
//        changed since its last comparison, at its first and as a version is applied (curvesAsFrozen, modulatorsAsFrozen).
//      - PlaybackSession (model/playback/internal/PlaybackSession.h): a new plan (triggerAsyncUpdate()); for an area no plan
//        holds and something else the session feeds does (the MIDI links: the MIDI tap's controllers), that marked and
//        brought up to date at a later moment of its own (followMidiLinks(), at the next turn of the message loop, or at once
//        where a writer that is no tree callback must have it so: MidiLinker::learn()), no plan made.
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
//        out, in that call (modulation's: a modulator or a macro by its ID); its tree callback asks only of the sources the
//        tree still holds (isHeardFromTree()), where an area's leaves that list for an override of its own.
//  11. What takes the song or a part of it for another thread or for later takes the area whole (WholeState.h:
//      copyWholeSong(), copyWhole(), keepValues(), or a part's own whole copy, as copyWholeClip()): the changes' worker
//      (model/versions/ChangesWorker.cpp: take(), makeSkeleton()); the saved copy (model/project/Project.cpp: savedState,
//      the tree with keepValues(), written with copyWhole()); the clipboard (model/arrangement/Clipboard.cpp); an export
//      (model/project/Export.cpp, DawProject.cpp). What reads the area on the message thread reads it in place, by
//      reference (Automation.h: getCurvesValue(), findCurve()), keeping no reference across a set; a song that holds no
//      values (a check's copy) reads as one whose area is empty.
//  12. The tree's listeners: each listener of step 10 leaves what the areas tell alone in its tree callbacks by asking
//      isHeardAsKept() (below) once, where it returns early for it (ClipRenders.cpp, Freeze.cpp, internal/Freeze.h,
//      PlaybackSession.cpp, SongPlayer.cpp, Project.cpp, internal/Project.h, ProjectContext.cpp, VersionHistory.cpp and
//      internal/VersionHistory.h); a new area adds its line there (its isHeardAs<Area>()), not a line in each listener. A
//      listener that takes one area's notifications differently from another's says so in a comment where it does (the
//      clip renders, which leave the chord lane's placeholder alone too). From the listener's piece (step 14) to the
//      switch, the tree is still the area's storage, and the keeper feeds <area>Changed() from the tree's callbacks under
//      the area's elements. At the switch the feed goes (the teller tells), and the area's line in isHeardAsKept() goes
//      with it where the tree then holds nothing of the area (the clips' went so); it stays where the area keeps a place in
//      the tree that the keeper's safety net writes (the tempo map's TEMPOMAP, the chord lane's CHORDLANE).
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
#include "model/base/internal/Immer.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/TempoMapValue.h"
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

//==============================================================================
// The song's values, which the song holds once its tree is consistent (Song::holdValues(), as model/project/internal/
// Project.h's makeSong() makes a song): the clips' area, the notes' area, the tempo map's, the chord lane's, the MIDI links',
// the curves' and the sections', in the song's one undo history, and what keeps them and the tree together. Message thread
// only, as the song's tree is: nothing on the audio thread reads it (what plays gets what the model compiles of it), and
// what runs on another thread gets a copy (WholeState.h: copyWholeSong(), keepValues()).
//
// Who changes the values: the clips' code (Clips.h: addClip(), setClip(), eraseClip() and the rest, through setClip(),
// eraseClip() and setTrackClipOrder() below), the notes' code (Notes.h: setClipNotesValue(), which is setClipNotes() below),
// the tempo map's code (Tempo.h: every edit of the tempo map, through setTempoMap() below), the chord lane's code (Chords.h:
// every edit of the lane, through setChordLane() below), the MIDI links' code (MidiLinks.h: every edit of the links, through
// setMidiLinks() below), automation's code (Automation.h: every edit of the curves, through setCurves() and its setters
// below) and the sections' code (Sections.h: every edit of the sections, through setSections() below). The song takes in
// what its file holds of each area as it holds its values (holdValues(): each area's adopt(), which reads it as the song
// holds it, as TempoMapValue.h's readHeldTempoMap(), ChordLaneValue.h's readHeldChordLane(), MidiLinksValue.h's
// readHeldMidiLinks(), CurveValues.h's readHeldCurves() and SectionsValue.h's readHeldSections() do), and empties its
// TEMPOMAP and AUTOMATION elements (making each where its file had none), and its CHORDLANE, MIDILINKS and SECTIONS where it
// has them, properties and children Phazite doesn't know too. A MIDI clip that joins an alias group takes the group's
// notes, and one that leaves it keeps those it holds (setClip()). Everything else that changes the tree is heard by the
// keeper, a listener of the song's root (each area's part of it in the area's file: ClipsArea.h, TempoMapArea.h,
// ChordLaneArea.h, MidiLinksArea.h, CurvesArea.h, SectionsArea.h), which asks every area for
// every element that comes into the tree or leaves it, each doing nothing for what is not its own: a track taken
// out of the song (alone, in a folder, the track list) has its clips (their values, their order and their notes) remembered
// with its element and erased from both areas; the same element put back (JUCE moves a track by taking it out and putting it
// in again) gets them back; and a track that comes into the song's tree with its clips' elements in it (a track pasted or
// applied so: a safety net, which the versions don't use: model/versions/ApplySong.cpp) has them read into both areas and
// taken out of the tree. A CLIP element added to a track of the song by itself is no clip of the song's (a JUCE assertion:
// the clips' functions add a clip), as the step that adds it can't take it out again (its Undo would find it gone). The
// same safety net for the tempo map, which the versions don't use either (model/versions/ApplySong.cpp, VersionsModel.cpp):
// a TEMPOMAP that comes into the song with TEMPO or TIMESIGNATURE elements in it has them read into the tempo map's area,
// as its whole map, and is emptied as holdValues() empties the song's own (what Phazite doesn't know of it goes too); and
// a TEMPO or a TIMESIGNATURE added to the song's TEMPOMAP by itself while the song is written outside the history is read
// into the map (TempoMapValue.h: withEntryTaken()) and taken out again. One
// added by itself in a step is refused as a lone CLIP is, for the same reason (a JUCE assertion: the tempo map's functions
// change the map). The chord lane's alike (ChordLaneArea.h): a CHORDLANE that comes in with CHORD or CHORDKEY elements has
// them read in as the song's lane and is emptied, a lone one added outside the history is read in and taken out, and one
// added in a step is refused; the MIDI links' alike (MidiLinksArea.h), for a MIDILINKS that comes in with MIDILINK
// elements and a lone MIDILINK, and the sections' (SectionsArea.h), for a SECTIONS that comes in with SECTION elements and
// a lone SECTION; and the curves' (CurvesArea.h) for a lone CURVE, each point read in given a key, while an AUTOMATION
// added to the song, a second beside the one it has from holdValues() on, is refused (a JUCE assertion), its curves unread,
// and taken out again outside the history. Each in the mode of the moment: in the history's step in progress, or outside
// the history while the song is written outside it (Song::OutsideHistory). Nothing while the history undoes or redoes a
// step: the areas' own actions move them. The keeper performs its changes from inside JUCE's action for the tree's, which
// JUCE adds to the step before
// the tree's own: so an undo puts the tree back first and the clips after (JUCE workaround W62 (docs/juce-workarounds.md)).
// Modulation's part of the keeper (ModulationArea.h) changes nothing: while the tree still holds the song's modulators and
// macros (roadmap step S5.20, modulation's second piece), it tells Listener::modulationChanged() from the tree's
// notifications, at once, in an undo and a redo too.
//
// The rule for what listens to the song's tree, with no exception: a tree callback never reads the clips' area (nor the
// notes'), directly or through anything that reads them (the clips' functions, Clips.h, AudioClips.h; the notes', Notes.h;
// a whole copy, WholeState.h). Every listener of the song's root listens on a handle of its own, and JUCE tells a tree's
// listeners handle by handle in the order of the handles' addresses, so there is no order among them: the keeper may hear a
// change after the listener that reads, which then finds a track moved (JUCE moves one by taking it out and putting it in
// again) without its clips; and during an undo the tree is back before the clips are. What reads the clips does it at its
// listener's later moment: a plan's next compile, the next turn of the message loop (Song::PendingWriter), or as the values'
// listener (Listener) is told. What the keeper can't see: a track added or taken out with no undo manager outside an
// OutsideHistory scope is written into the step in progress (if any) as if it had one, and a tempo map's entry added so is
// refused as one added in a step; nothing in Phazite does either to a song that holds its values (the loader does it
// before: makeSong()).
class SongValues final
{
public:
    explicit SongValues (Song&);
    ~SongValues();

    ValueArea<SongClips> clips;
    ValueArea<NotesByClip> notes;
    ValueArea<TempoMapValue> tempoMap;   // each kind by beat, at least one of each (TempoMapValue.h)
    ValueArea<ChordLaneValue> chordLane; // each kind in the lane's order, empty for a song without a lane (ChordLaneValue.h)
    ValueArea<MidiLinksValue> midiLinks; // in the song's order, one of each controller and of each parameter (MidiLinksValue.h)
    ValueArea<SongCurves> curves;        // by their parameters, in the order the song made them, each point keyed (CurveValues.h)
    ValueArea<SectionsValue> sections;   // in the song's order, one of each ID (SectionsValue.h)

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
    // members by name: later pieces of roadmap step S5.20 may add one.
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
        // or leaves it with modulators or macros in it tells each of them as added or taken out. While the song's tree is still
        // modulation's storage (roadmap step S5.20, modulation's second piece), the keeper tells it from the tree's
        // notifications under the song's modulators and macros (ModulationArea.h: ModulationKeeper), at once, one call for each
        // notification that changes any (an undo's and a redo's too, as they come back one by one), so that a step that writes
        // several of a modulator's properties tells several calls, as the tree did; from modulation's switch on, the area tells
        // it once for each change of its value. Either way, what a step, its Undo or its Redo told in all is what differs in the
        // song's modulation before and after. What listens to the song's root leaves what its tree tells of the song's
        // MODULATOR and MACRO elements (added, taken out or changed) and of what is in them alone (isHeardAsKept(), below,
        // through isHeardAsModulation()): a TRACK or a CONTAINER that comes or goes with modulators in it, and an owner's
        // children in another order (its modulators among them), are the tree's, heard as before, and its modulators are told
        // here, so the step is heard twice, and only marks twice.
        virtual void modulationChanged (const std::vector<ModulationChange>& modulators, const std::vector<ModulatorPlace>& reordered)
        {
            juce::ignoreUnused (modulators, reordered);
        }
    };

    void addListener (Listener*) const;
    void removeListener (Listener*) const;

    // How many CLIP elements the keeper took into the areas from the song's tree since the song held its values (the safety
    // net, above): what the versions, which give a track its clips through the clips' functions, check they never leave to it
    // (model/versions/ApplySong.cpp). And whether the keeper remembers a track's element with its clips, taken out of the
    // song, and the IDs of the tracks it remembers: for the keeper's tests, which check that it lets go of what nothing can
    // bring back.
    int countAdopted() const noexcept { return adopted; }
    bool remembers (const juce::ValueTree& track) const;
    std::vector<TrackID> getRemembered() const;

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

private:
    friend class Song;

    // Each area's parts, each in its area's own file (the recipe, above), and the keeper, which asks them in turn.
    struct ClipsTeller;                             // ClipsArea.h
    struct NotesTeller;                             // NotesArea.h
    struct TempoMapTeller;                          // TempoMapArea.h
    struct ClipsKeeper;                             // ClipsArea.h: the clips', with their notes
    struct TempoMapKeeper;                          // TempoMapArea.h
    struct ChordLaneTeller;                         // ChordLaneArea.h
    struct ChordLaneKeeper;                         // ChordLaneArea.h
    struct MidiLinksTeller;                         // MidiLinksArea.h
    struct MidiLinksKeeper;                         // MidiLinksArea.h
    struct CurvesTeller;                            // CurvesArea.h
    struct CurvesKeeper;                            // CurvesArea.h
    struct SectionsTeller;                          // SectionsArea.h
    struct SectionsKeeper;                          // SectionsArea.h
    struct ModulationKeeper;                        // ModulationArea.h: the tree's feed of modulationChanged(), until the switch
    struct Keeper;                                  // SongValues.cpp
    mutable juce::ListenerList<Listener> listeners; // told by the areas' tellers (addListener())
    std::unique_ptr<ClipsTeller> clipsTeller;
    std::unique_ptr<NotesTeller> notesTeller;
    std::unique_ptr<TempoMapTeller> tempoMapTeller;
    std::unique_ptr<ChordLaneTeller> chordLaneTeller;
    std::unique_ptr<MidiLinksTeller> midiLinksTeller;
    std::unique_ptr<CurvesTeller> curvesTeller;
    std::unique_ptr<SectionsTeller> sectionsTeller;
    std::unique_ptr<Keeper> keeper;
    int adopted = 0, tempoMapAdopted = 0;
    int chordLaneAdopted = 0;
    int midiLinksAdopted = 0;
    int curvesAdopted = 0;
    int sectionsAdopted = 0;
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

// Whether what the song's tree tells of an element (a property changed, a child added, taken out or moved in it) is
// modulation's, which a listener of the song's root hears from the values' listener alone (SongValues::Listener::
// modulationChanged()) and leaves alone in its tree callbacks: a MODULATOR or a MACRO of the song's (one a channel of the
// song holds, a track's or the master's, or a container on one: readModulationValue() reads it), or an element in one. And
// whether a child added to an element or taken out of it is: a MODULATOR or a MACRO added to such a channel or container, or
// taken out of it, or a child of an element that is. Not a TRACK, a CONTAINER or the track list added or taken out with
// modulators in it: the tree's, heard as any element's, whose modulators the keeper tells (ModulationArea.h). While the tree
// is modulation's storage (roadmap step S5.20, its second piece), the keeper tells what the tree tells here. In
// ModulationArea.cpp.
bool isHeardAsModulation (const juce::ValueTree& element);
bool isHeardAsModulation (const juce::ValueTree& parent, const juce::ValueTree& child);

// Whether what the song's tree tells is any area's of the song's values, which a listener of the song's root hears from the
// values' listener alone (SongValues::Listener) and leaves alone in its tree callbacks: of an element (a property changed in
// it, a child moved in it), and of a child added to an element or taken out of it. Each the OR of the areas' own functions
// above (isHeardAs<Area>()), a line each, in the order of the areas: the tempo map's, the chord lane's, the curves', the
// MIDI links', the sections', then modulation's.
// What every listener's tree callback asks, once, where it returns early for what the areas tell (the recipe's step 12,
// above): a new area adds its line here, not a line in each listener. In SongValues.cpp.
bool isHeardAsKept (const juce::ValueTree& element);
bool isHeardAsKept (const juce::ValueTree& parent, const juce::ValueTree& child);
} // namespace phazite
