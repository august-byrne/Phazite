// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's values: the song held as immutable values (the immer library: ValueArea.h), an area for each part of it. The
// areas are the song's clips (ClipsValue.h: SongClips, each clip's properties and warp markers as a ClipValue, each track's
// clips in their order, each alias group's clips), which the clips' code (model/arrangement/internal/Clips.h, AudioClips.h)
// computes each edit on; a MIDI clip's notes and its controllers' values (ClipNotesValue.h: ClipNotes, every clip's in
// NotesByClip), which the notes' code (model/notes/internal/Notes.h) computes each edit on, from one value to the next; the
// song's tempo map (TempoMapValue.h: its tempos and time signatures), which the tempo map's code
// (model/transport/internal/Tempo.h) computes each edit on; the song's chord lane (ChordLaneValue.h: its chords and key
// changes, and whether the song has a lane), which the chord lane's code (model/chords/internal/Chords.h) computes each edit
// on; the song's MIDI links (MidiLinksValue.h: each controller linked to a parameter), which the MIDI links' code
// (model/midi/internal/MidiLinks.h) computes each edit on; the song's automation (CurveValues.h: SongCurves, each automated
// parameter's curve with its points, in the order the song made them), which automation's code
// (model/automation/internal/Automation.h) computes each edit on; the song's sections (SectionsValue.h: each by its ID),
// which the sections' code (model/arrangement/internal/Sections.h) computes each edit on; the song's modulation
// (ModulationValue.h: SongModulation, each modulator and macro by its ID, each owner's in its order), which modulation's
// code (model/modulation/internal/Modulation.h) computes each edit on; and the song's sends (SendsValue.h: SongSends, each
// send by its ID, each track's in its order), which the sends' code (model/mixer/internal/Mixer.h) computes each edit on; and
// the song's devices, containers and chains (DevicesValue.h: SongDevices, each by its ID with its place, each owner's list in
// its order), which the devices' setters (model/mixer/internal/Mixer.h) compute each edit on; and what the song's devices
// store in the history and outside it alike (DeviceStoreValue.h: DeviceStore, a plug-in's values set by hand, state,
// program, format ID and version, a chain's mute and solo, keyed: undone by entry), with each device's window's place
// outside the history alone (DevicesOutside.h, an OutsideValue), which the store's functions (model/plugins/internal/
// DeviceStore.h) read and write by IDs; and the tracks (TracksValue.h: SongTracks) with their switches (TrackSwitchesValue.h:
// each track's mute, solo and frozen switch and each track's and the master's automation mode, keyed: undone by entry), with
// each track's and the master's view outside the history alone (TracksOutside.h, an OutsideValue: its row's height,
// collapsed and pinned switches, drum mode and rows, and its automation lanes), which the tracks' functions by ID
// (model/arrangement/internal/Tracks.h, model/automation/internal/Automation.h, model/notes/internal/Notes.h) read and write;
// and what the song holds of its own (SongOwnValue.h: in three values: SongOwnValue, plain; SongStanding, keyed; SongOutside,
// outside the history alone), which its getters and setters read and write. All as plain values in Phazite's own types.
//
// The values are the song: it holds no juce::ValueTree. The form of a file's song is a tree of elements (Schema.h), which a
// song is made of once the loader made it consistent (SongState.h: readSongTree(); model/project/internal/Project.h:
// makeSong()), and which whole state writes from the values where a file's form is needed (SongState.h: writeSongTree();
// WholeState.h: copyWholeSong()). Each area's value header holds the conversion between its elements and its value, the one
// place that reads and writes those elements by their names: what a file's song, or any part of one that comes into the song
// as elements (a version's track, a track pasted), is read into, and what a whole copy of the song is written from.
//
// How to add an area to the song. Each area has its own files, and each list that asks every area asks it in a line of its
// own; nothing generic joins them (no table, no registration): plain functions, called in a list in a fixed order, which
// each list states with its reason. <Area> is the area's name (as TempoMap), <area> its member's (as tempoMap), <AREA> its
// element where it has one at the song's root (as TEMPOMAP).
//   1. The value and its conversion: internal/<Area>Value.h, with its .cpp in model/base/ (as TempoMapValue.h). The value's
//      types, each with isSameAs() (ValueArea.h) and ==; the helpers that alone write it; its invariants as a function the
//      check calls (TempoMapValue.h: isHeldAsTheSongHoldsIt(); ClipsValue.h: holdsItsLists()); and the conversion of its
//      elements by name: write<Area>(), and readHeld<Area>(), which reads the elements as the song holds the area (each
//      number by its description, within its range: SongText.h, readDescribed(); what must hold together kept so, as
//      ChordLaneValue.h's keepChordsApart()) and is what everything that enters the area reads: the song's making
//      (readSongTree()), the versions' pass and a merge's choice (step 7). The loader (model/project/internal/Project.h:
//      makeConsistent()) keeps a file's elements by the same rule, so that a file reads the same read whole or in part. A
//      reader of elements as they are written, no range kept, only where the loader's checks need one before the tree is
//      consistent (ModulationValue.h: readModulatorValueIn()). Entries the selection names that have no identity in the file
//      (an automation point: CurveValues.h, PointValue) get a key in the value, never saved nor compared as content, from a
//      count of SongValues' outside the history (givePointKey()): given to each as it enters the song and kept by each edit
//      that keeps the entry, so that the selection, holding keys, follows the entries through edits, undo and redo.
//   2. Its names: each element and property it writes described once in Schema.h (an Element with the elements it goes in, a
//      Property<T> with its range or its words, its default, its kind and whether it counts as a change), which the loader
//      and the check read; tests/golden/format/song-schema.json regenerated on purpose (PHAZITE_UPDATE_GOLDEN=1, the reason in
//      the commit); and each name in the representative golden project (tests/GoldenProjectTests.cpp), or in
//      getNamesNotInTheProject() there with why.
//   3. Plain, keyed or outside. A plain area (ValueArea<<Area>Value>) is undone whole: what only an undo step writes (the
//      clips, the tempo map, the devices). A keyed one, whose value type says undoesByEntry (ValueArea.h), is undone by
//      entry: what is written in steps and outside the history alike (the devices' store, the tracks' switches, the song's
//      standing), so that a write outside the history to an entry a step didn't change survives that step's Undo; what a
//      maker gives a new item there is written outside the history where the area holds no entry for it. What is never an
//      undo step (view state, how the song plays) is an OutsideValue (OutsideValue.h), which no step holds. What is written
//      outside the history never lives in a plain area: its Undo would put the value back whole.
//   4. The area: internal/<Area>Area.h and model/base/<Area>Area.cpp (as TempoMapArea.h), including this header; here its
//      member (ValueArea<<Area>Value> <area>, after the others) and its setter (set<Area>(), a member of SongValues defined
//      there: one set of the next value, in the step in progress or, while the song is written outside the history
//      (Song::OutsideHistory), outside it; a value equal to the one held is nothing, or ValueArea.h's setWithoutStep() where
//      only identity differs); in SongValues.cpp its area header included, its place in the constructor's initialiser list
//      (<area> (song.getUndoManager(), its empty value)) and its teller's after the others', the destructor letting go of its
//      teller before the others' (the reverse of their making), and a line of takeIn(), which sets each area to the state a
//      song is made of. Each new file listed in CMakeLists.txt (phazite_core) and in src/model/README.md. Its code, outside
//      model/base (as model/transport/internal/Tempo.h), reads it by ID, by reference into the area's value where it asks
//      on the message thread (Automation.h: getCurvesValue(), findCurve()), keeping no reference across a set, and is the
//      only code that changes it, through the setter. An area whose entries sit under an owner (the clips under tracks;
//      modulation under tracks, the master and containers; the devices under channels, containers and chains) names each
//      owner by its ID in its entries, so that nothing follows an owner as it moves, and what removes an owner erases its
//      entries first (model/arrangement/internal/Tracks.h: eraseTrackHoldings(); model/mixer/internal/Mixer.h:
//      forgetWhatNamesTheGone()); an entry keeps beside its owner each owner above it that a teller names (ModulatorPlace: a
//      container's macro keeps its channel), so that what is told names it without reading another area, which an undo may
//      not have put back yet.
//   5. Its teller and its listener function: struct SongValues::<Area>Teller among the declarations below (a ValueArea
//      listener that adds itself to the area and tells Listener of each change of its value, with the list of what changed
//      it works out from the value before and after), a std::unique_ptr<<Area>Teller> after the others, and a virtual
//      <area>Changed() in Listener, empty by default. Each SongValues::Listener gets its override, which only marks (step 9),
//      or says in a comment beside its overrides that it follows nothing of the area. They are:
//      - ClipRenders (model/arrangement/ClipRenders.cpp, internal/ClipRenders.h): rendersMayChange() where the area holds
//        what a clip's render is made of (the clips', the tempo map's; not the notes', nor the curves': no render holds
//        automation; nor modulation's, which moves nothing a render is made of; nor the sends', which play after a clip).
//      - FreezeKeeper (model/mixer/Freeze.cpp, internal/Freeze.h): marks the frozen tracks whose file holds what changed
//        (all of them for the tempo map) and compares them at its later moment. What a frozen track's file holds of an area is
//        in its Holds (parts: its frozen devices' IDs, getFrozenDevices(); values: SongState, the devices' and the store's
//        among them; tempos; curves: its frozen devices' automation, getFrozenCurves(); modulators: its modulators and its
//        frozen containers' macros, as values, getFrozenModulators()), compared in holdsWhatWasFrozen(), each area's value by
//        identity first. An area whose value is read to compare it (the curves, the modulators) is read only for a track it told
//        changed since its last comparison, at its first and as a version is applied (curvesAsFrozen, modulatorsAsFrozen).
//        An area no frozen file holds anything of follows nothing (the sends': a frozen file is its track before its fader).
//      - PlaybackSession (model/playback/internal/PlaybackSession.h): a new plan (triggerAsyncUpdate()); for an area no plan
//        holds and something else the session feeds does (the MIDI links: the MIDI tap's controllers), that marked and
//        brought up to date at a later moment of its own (followMidiLinks(), at the next turn of the message loop, or at once
//        where a writer must have it so: MidiLinker::learn()), no plan made. A value set by hand that plays at once is the one
//        thing a listener sets from what it is told (the value after: modulation's SongPlayer::takeModulator(), a send's level
//        SongPlayer::takeSendLevel(); the values before and after: a device's values set by hand, its sounds, a container's
//        mix, a chain's fader and range, SongPlayer::takeDevice()), no plan made; one an undo or a redo set is marked
//        (followAfterMove), and what writes automation follows it once the history's move is whole, from the history's
//        listener (Song.h: HistoryListener; PlaybackSession::historyMoved()), never while the area tells.
//      - SongPlayer (model/playback/SongPlayer.cpp, internal/SongPlayer.h): marks what its next compile reads again (its
//        changes: the tracks' notes and audio, the tempo, the curves by their parameters).
//      - Project (model/project/Project.cpp, internal/Project.h): counts the change and marks what to compare (a member of
//        its own, as tempoToCompare, cleared in clearToCompare()); its quick check, a line of compareWithSavedState()'s list
//        (as isTempoMapAsSaved()); and its whole comparison, WholeState.h's holdSameSong() (step 6), which the quick check
//        agrees with (tests/SavedCopyTests.cpp).
//      - ProjectContext's SongWatcher (model/project/ProjectContext.cpp): names what changed and adds it to its group of
//        SongChanges (ProjectContext.h; a group of its own where the views need one).
//      - VersionHistory (model/versions/VersionHistory.cpp, internal/VersionHistory.h): counts the song's change
//        (songChanges, edited()), marks the lines it is in (markTrack(), markSongOwn()) and calls changesMayDiffer().
//      - MidiLinker (model/midi/MidiLinks.cpp, internal/MidiLinks.h): ends a learning whose parameter the area tells taken
//        out, in that call (modulation's: a modulator or a macro by its ID; the sends': a send's level by the send's ID; the
//        devices': a device's, a plug-in's or a chain's parameter by its device's or chain's ID; the tracks': a fader's by
//        the channel it remembers).
//      - AudioRecorder (model/transport/internal/AudioRecorder.h): the tracks' alone, its inputs brought in line from the
//        tracks it is handed.
//   6. The song whole: a member of SongState (SongState.h) and a line of getSongState() and of readSongTree() (its
//      readHeld<Area>In(), in its area's file); in WholeState.cpp a line of writeIn() (its write<Area>In()) and of
//      leadsToKept() (its leadsTo<Area>()), its element, where it has one at the song's root, in the root's order, which is
//      its description's (Schema.h: PHAZITE's elements in the order they are described, which writeSongTree() follows); a
//      line of holdSameSong() (or of holdSameChannels(), for what a track or a device holds), its holdSame<Area>() declared
//      with the others in WholeState.h, comparing two states by identity first. An area whose element's presence means
//      something (the chord lane's CHORDLANE: a song without one takes its keys from the key set last) says it in its value
//      (ChordLaneValue.h: placed), which its code sets in the step that changes the value, its teller tells as any change,
//      and whole state writes as an empty element. An area whose element only holds its entries (the MIDI links' MIDILINKS,
//      the sections' SECTIONS) has none while it holds none: whole state writes no empty element, and a file's empty one
//      reads as none. An element that is a place every song has (the tempo map's TEMPOMAP), or one written while the area
//      holds entries (the curves' AUTOMATION: CurvesArea.h, writeCurvesIn()), is written so by whole state alone. An owner's
//      entries are written in its element (writeModulationIn(): after its other children but those another area writes
//      after it, a track's sends). If it holds IDs: findLargestIDIn (const <Area>Value&) (IDsHeld.h) and a line of
//      IDsHeld.cpp's findLargestIDHeld() (and of forEachNameableIDHeld() where something can name its entries).
//   7. The versions: choose<Area>Clash(), declared in model/versions/internal/AreaClashes.h and defined with the area's
//      model code (as chooseTempoMapClash() in model/transport/Tempo.cpp), a line of chooseInAreas() in
//      model/versions/VersionsModel.cpp; its pass in model/versions/ApplySong.cpp (make<Area>Like(), through its setter) and
//      a line of makeValuesLike(); its part of each line's pass beside the whole song's, where that line holds it: a
//      track's, makeTrackLikeIn() (as its ClipPass), and the song's own, makeSongOwnLikeIn() (as its makeTempoMapLike()).
//      What the other's version couldn't read (a stand-in): an area whose entries have no identity leaves its value whole
//      (the tempo map's, the chord lane's); one whose entries are keyed keeps the song's entry of a stand-in's key in its
//      place (the MIDI links', by a link's controller: ApplySong.cpp, makeMidiLinksLike()).
//   8. What takes the song or a part of it for another thread or for later takes a state (getSongState()) and writes the
//      area's elements from it where they are used: the changes' worker (model/versions/ChangesWorker.cpp: take(),
//      makeSkeleton(), copyTrack(), on the history's thread); the saved copy (model/project/Project.cpp: savedState, written
//      on the writer's thread with writeSongTree()); the clipboard (model/arrangement/Clipboard.cpp; a part's own whole copy,
//      as copyWholeClip()); an export (model/project/Export.cpp, DawProject.cpp).
//   9. What an area must not do: a listener told of an area's change only marks, and reads the song at a later moment
//      (during an undo the step's areas move back one after another: a listener told by the first may find the second not
//      back yet); a set equal to what is held makes no undo step; a number an element holds as text is read through
//      readValueNumber() or readDescribed() (SongText.h), never a cast; what is written outside the undo history never lives
//      in a plain area (step 3).
//  10. Its checks: tests/SongCheck.cpp, its rule as a function of its own (check<Area>Value(): its invariants), a line of
//      findValueProblems() and of findStateDifferences(); tests/SchemaTests.cpp, its elements' type a line of Changed's list
//      of what the loader alone takes (is<Area>Entry()), and of findOpened(), which reads the opened song's element back in
//      its whole state, with a branch of its own for a property whole state writes with its default where the element had
//      none (as the sections' energy).
//  11. Its tests: its value and conversion, the round trip of every golden project's entries and of damaged ones
//      (tests/InconsistentProjectTests.cpp), a song with its elements opened through makeSong(), and its state read back from
//      its own file's form (SongCheck.cpp: findReopenedProblems()); its listener's case (as tests/SongValuesTempoTests.cpp's:
//      each edit tells once, its Undo and its Redo too; where it counts the version history's song changes for an edit that
//      makes an ID, it counts the count of IDs moving on, Song::makeID(), in the step alone, never in its Undo or its Redo);
//      its writers, through the setter, and the versions' pass and clash (step 7); Undo and Redo (UndoCheck), the unsaved
//      check agreeing with the whole comparison (tests/SavedCopyTests.cpp), and a speed gate (tests/PerformanceGateTests.cpp)
//      where a reader is on a per-frame path.
//  12. The docs it updates: CLAUDE.md's bullet on the song's values; src/model/README.md; tests/README.md's nets;
//      docs/design/engine.md; docs/project-format.md; and docs/performance.md after a measurement.
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
struct SongState; // SongState.h

// The tracks' (model/base/internal/TracksArea.h): what the listener is told of them.
struct TrackChange;
struct TrackSwitchChange;

// The song's own's (model/base/internal/SongOwnArea.h, SongOwnValue.h): what the listener is told of it.
struct SongOwnChange;

//==============================================================================
// The song's values, which the song holds from its making (Song.h: Song (SongState)): each area in the song's one undo
// history. Message thread only: nothing on the audio thread reads it (what plays gets what the model compiles of it), and
// what runs on another thread gets a copy (SongState.h: getSongState(); WholeState.h: copyWholeSong()).
//
// Who changes the values: the clips' code (Clips.h: addClip(), setClip(), eraseClip() and the rest, through setClip(),
// eraseClip() and setTrackClipOrder() below), the notes' code (Notes.h: setClipNotesValue(), which is setClipNotes() below),
// the tempo map's code (Tempo.h: every edit of the tempo map, through setTempoMap() below), the chord lane's code (Chords.h:
// every edit of the lane, through setChordLane() below), the MIDI links' code (MidiLinks.h: every edit of the links, through
// setMidiLinks() below), automation's code (Automation.h: every edit of the curves, through setCurves() and its setters
// below), the sections' code (Sections.h: every edit of the sections, through setSections() below), modulation's code
// (Modulation.h: through setModulator() and the three after it, below), the sends' code (Mixer.h: through setSend() and the
// three after it, below), the devices' setters (Mixer.h: through setDevice() and the three after it, below), the store's
// functions, the tracks' setters and the song's own's setters (each through its setters below). Each in the mode of the
// moment: in the history's step in progress, or outside the history while the song is written outside it
// (Song::OutsideHistory). A MIDI clip that joins an alias group takes the group's notes, and one that leaves it keeps those
// it holds (setClip()).
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
    // Clips.h: eraseTrackClips()). Nothing more: an alias group left with one clip keeps it linked
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
    // its own to keep). Whether the song has a lane is the value's to say (ChordLaneValue.h: placed), which the chord lane's
    // code sets in the step that places or takes away the lane. In ChordLaneArea.cpp.
    void setChordLane (Song&, ChordLaneValue);

    // Gives the song these MIDI links, the MIDI links' code's one way to change them (model/midi/internal/MidiLinks.h): in the
    // song's undo history (the step in progress), or outside it while the song is written outside it. Links equal to those
    // the song holds are no change, and write nothing: no step, the value the song holds kept (a link has no identity of its
    // own to keep but its controller, which its value holds). A song without links has none: whole state writes no empty
    // MIDILINKS (SongState.h). In MidiLinksArea.cpp.
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
    // its ID, which its value holds). A song without sections has none: whole state writes no empty SECTIONS (SongState.h).
    // In SectionsArea.cpp.
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
    // setNewDevices(), eraseDevice(), setDeviceListOrder() and the narrow setters beside them; the versions' pass,
    // model/versions/ApplySong.cpp, makeDevicesLike()), in the song's undo history (the step in progress), or outside it while
    // the song is written outside it. A value equal to what the song holds is no change, and writes nothing: no step, the
    // value held kept. Whether it changed the song's. In DevicesArea.cpp.
    bool setDevice (Song&, DeviceValue, int index = -1);
    bool eraseDevice (Song&, DeviceID);
    bool setDeviceListOrder (Song&, const DevicePlace& owner, immer::flex_vector<DeviceID> order);
    bool setAllDevices (Song&, SongDevices);

    // Give the tracks' switches a whole value (TrackSwitchesValue.h, keyed: an Undo puts back only the entries its step
    // changed), their writers' one way to change them (model/arrangement/internal/
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
    // IDs), or give them a whole value (a pass): the tracks' setters'
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
    // without reading another area. Set its members by name.
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
        // once for each change of its value: a step's, an undo's or a redo's whole step, a write outside the history.
        virtual void tempoMapChanged() {}

        // The song's chord lane (ChordLaneValue.h): its chords or its key changes, any of them, and whether the song has a lane
        // (placed: a song without one takes its keys from the key set last). The chord lane's area tells it, once for each
        // change of its value: a step's, an undo's or a redo's whole step, a write outside the history. A step that makes the
        // lane (its first chord or key) places it first and then sets its entries (Chords.h), so it is told twice, and only
        // marks twice.
        virtual void chordLaneChanged() {}

        // The song's automation (CurveValues.h): the parameters whose curves changed (a point added, taken out, moved or bent,
        // a curve added or taken out, given another parameter: the one it had and the one it has; those that came to other
        // places in the curves' order), each once, in one call. The curves' area tells it, once for each change of its value:
        // a step's, an undo's or a redo's whole step, a write outside the history (CurvesArea.h: CurvesTeller).
        virtual void curvesChanged (const std::vector<ParameterRef>& parameters) { juce::ignoreUnused (parameters); }

        // The song's MIDI links (MidiLinksValue.h): a link made, moved or taken away, any of them. The links' area tells it, once
        // for each change of its value: a step's, an undo's or a redo's whole step, a write outside the history.
        virtual void midiLinksChanged() {}

        // The song's sections (SectionsValue.h), by their IDs: each section added, taken out or changed (any of its
        // properties), each once, in one call (SectionsArea.h: findSectionsChanged()). The sections' area tells it, once for each
        // change of its value: a step's, an undo's or a redo's whole step, a write outside the history; a change of their order
        // alone (a version's) names none.
        virtual void sectionsChanged (const std::vector<SectionID>& sections) { juce::ignoreUnused (sections); }

        // The song's modulators and macros (ModulationValue.h), by their IDs: each one added, taken out, come to another place
        // or changed, each once in a call, with where it was and is, the parts of it that changed (ModulationChange::Part) and
        // its value after (ModulationChange::now); and each owner (ModulatorPlace: a channel, or a container) that holds those
        // it held before in another order. A track, a folder's tracks, a container or the track list that comes into the song
        // or leaves it with modulators or macros in it tells each of them as added or taken out. Modulation's area tells it
        // (ModulationArea.h: ModulationTeller), once for each change of its value: a step's, an undo's or a redo's whole step,
        // a write outside the history; so what a step, its Undo or its Redo told in all is what differs in the song's
        // modulation before and after.
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
        // after.
        virtual void sendsChanged (const std::vector<SendChange>& sends, const std::vector<TrackID>& reordered, const SongSends& now)
        {
            juce::ignoreUnused (sends, reordered, now);
        }

        // The song's devices, containers and chains (DevicesValue.h), by their IDs: each one added, taken out, come to another
        // place or changed, each once in a call, with where it was and is, the parts of it that changed (DeviceChange::Part)
        // and its value before and after (DeviceChange::was, now); each owner (DevicePlace: a channel, a container, a chain)
        // that holds those it held before in another order; and the song's devices whole after (now: a copy of the area's
        // value, valid during the call, the changes' values after in it), for what a change needs of the others: a chain's
        // place among its container's (its default crossover, whether it has one at all). A track, a folder's tracks or the
        // track list that comes into the song or leaves it with devices in it tells each of them as added or taken out. The
        // devices' area tells it (DevicesArea.h: DevicesTeller), once for each change of its value: a step's, an undo's or a
        // redo's whole step, a write outside the history; so what a step, its Undo or its Redo told in all is what differs in
        // the song's devices before and after. What the value doesn't cover is the store's (deviceStoreChanged(), below: a
        // plug-in's values set by hand, its state, program, format ID and version, a chain's mute and solo), or a window's
        // place, which no one is told.
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
        // that changed another (ValueArea.h: keyed). A device or a chain that goes leaves its entries in the store, told
        // nothing (a removal keeps them, for its Undo).
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
        // SongOwnTeller, SongStandingTeller, SongOutsideTeller), from the value before and after. Not its count of IDs
        // (Song.h: makeID(), which tells no one), nor what Phazite doesn't know at its root
        // (dropped as the song opens).
        virtual void songOwnChanged (const SongOwnChange& change) { juce::ignoreUnused (change); }
        virtual void songStandingChanged (bool lastKey, bool versionsNames) { juce::ignoreUnused (lastKey, versionsNames); }
        virtual void songOutsideChanged (bool plays) { juce::ignoreUnused (plays); }
    };

    void addListener (Listener*) const;
    void removeListener (Listener*) const;

private:
    friend class Song;

    // Each area's teller, each in its area's own file (the recipe, above).
    struct ClipsTeller;                             // ClipsArea.h
    struct NotesTeller;                             // NotesArea.h
    struct TempoMapTeller;                          // TempoMapArea.h
    struct ChordLaneTeller;                         // ChordLaneArea.h
    struct MidiLinksTeller;                         // MidiLinksArea.h
    struct CurvesTeller;                            // CurvesArea.h
    struct SectionsTeller;                          // SectionsArea.h
    struct ModulationTeller;                        // ModulationArea.h
    struct SendsTeller;                             // SendsArea.h
    struct DeviceStoreTeller;                       // DeviceStoreArea.h
    struct DevicesTeller;                           // DevicesArea.h
    struct TracksTeller;                            // TracksArea.h
    struct TrackSwitchesTeller;                     // TracksArea.h
    struct TracksViewTeller;                        // TracksArea.h
    struct SongOwnTeller;                           // SongOwnArea.h
    struct SongStandingTeller;                      // SongOwnArea.h
    struct SongOutsideTeller;                       // SongOwnArea.h
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
    std::uint64_t nextPointKey = 1; // givePointKey(): outside the history, never undone, never saved

    // In the history, or outside it, as the song is written now; and a value alike to the area's, without a step of its own
    // (ValueArea.h: setWithoutStep()). Each with its area's setters.
    void setAll (Song&, SongClips next);
    void setAll (Song&, NotesByClip next);
    void setWithoutStep (Song&, SongClips next);
    void setWithoutStep (Song&, NotesByClip next);

    // The song made of a state (Song (SongState)): each area set to it, outside the history, a line each; the song's count of
    // IDs; and the next point key past every key its curves hold.
    void takeIn (Song&, SongState);

    JUCE_DECLARE_NON_COPYABLE (SongValues)
    JUCE_DECLARE_NON_MOVEABLE (SongValues)
};
} // namespace phazite
