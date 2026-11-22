// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's values: what the song moves from its juce::ValueTree into immutable values held beside it (the immer library;
// roadmap step S5.20, model/base/internal/ValueArea.h), one area at a time. The areas are the song's clips (SongClips: each
// clip's properties and warp markers as a ClipValue, each track's clips in their order, each alias group's clips), which the
// clips' code (model/arrangement/internal/Clips.h, AudioClips.h) computes each edit on; a MIDI clip's notes and its
// controllers' values (ClipNotes), which the notes' code (model/notes/internal/Notes.h) computes each edit on, from one value
// to the next; and the song's tempo map (TempoMapValue.h: its tempos and time signatures), which the tempo map's code
// (model/transport/internal/Tempo.h) computes each edit on. All as plain values in Phazite's own types.
//
// The values are the storage: the song holds an area of each (SongValues) in its one undo history, and the song's tree holds
// no CLIP element (nor a clip's NOTE, CONTROL or WARP elements), and no TEMPO or TIMESIGNATURE element: its TEMPOMAP element
// stays, empty, the place a whole copy writes the map in. The conversion below (and TempoMapValue.h's, for the tempo map) is
// the one place that reads and writes those elements by their names: what a file's clip or tempo map (or any that comes into
// the song as an element: a version's track, a track pasted) is read into, and what a whole copy of the song is written from
// (WholeState.h).
//
// immer is the model's alone (tools/check-architecture.sh: immer-outside-model): nothing outside src/model includes this.
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/ControlKinds.h"
#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/ValueArea.h"

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

// A note as its clip holds it. Its start counts from its clip's content start, as every note's of the clip does: the song
// beat is that start plus this (model/arrangement/internal/Clips.h: getContentStart()).
struct NoteValues
{
    NoteID id {};
    int pitch = 60;
    BeatPosition start {}; // among the clip's notes
    BeatDuration length = BeatDuration::fromBeats (1.0);
    int velocity = 100;
    bool muted = false, slide = false, portamento = false;
    int channel = 1; // from 1 to 16

    bool operator== (const NoteValues&) const = default; // the beats compare as Beats.h says, never with a double's ==
};

// A controller's value as its clip holds it (ControlKinds.h). Its beat counts from the clip's content start, as a note's
// start does.
struct ControlValues
{
    ControlKind kind = ControlKind::controller;
    int number = 0; // a controller's and a note's pressure's; the other kinds have none
    double beat = 0.0;
    int value = 0;
    int channel = 1; // from 1 to 16

    // Written out, not defaulted: a defaulted one would compare the beat with a double's ==, which GCC's -Wfloat-equal refuses.
    bool operator== (const ControlValues& other) const noexcept
    {
        return kind == other.kind && number == other.number && juce::exactlyEqual (beat, other.beat) && value == other.value
               && channel == other.channel;
    }
};

// A MIDI channel kept within 1 to 16, as a note's or a controller's value holds it: what every place that gives a value its
// channel keeps it with (the conversion below, as it reads an element; the notes' edits, as they make a note or set its
// channel; keepWithinKind()), so that the value holds none beyond them, whatever writes or stores it.
int keepChannel (int channel) noexcept;

// A value as a clip would hold it, kept within what its kind takes: its number from 0 to 127 where its kind has one (0
// where it hasn't), its beat not before the clip's content start, its value within its kind's range, its channel from 1 to 16.
ControlValues keepWithinKind (ControlValues);

//==============================================================================
// One clip's notes and controllers' values: those of each clip of an alias group, which hold the same (Schema.h: clipLink).
// Copies are cheap and share what they hold.
struct ClipNotes
{
    immer::flex_vector<NoteValues> notes;            // in the clip's order
    immer::map<std::uint64_t, std::uint32_t> places; // a note's ID to its place in notes
    immer::flex_vector<ControlValues> controls;      // in their order

    // A note by its ID: none for one the clip hasn't.
    const NoteValues* find (NoteID) const;

    // The same value without a look inside: all three shared (ValueArea.h). And the same notes and values, compared.
    bool isSameAs (const ClipNotes&) const noexcept;
    bool operator== (const ClipNotes&) const;

    // Whether places holds the place of each note, and nothing else: what the helpers below keep, for the tests and the
    // assertions where it is cheap.
    bool holdsItsPlaces() const;
};

// A clip's notes and values made from these, in their order: each note's ID once.
ClipNotes makeClipNotes (const std::vector<NoteValues>& notes, const std::vector<ControlValues>& controls = {});

// The notes with these IDs (those the clip has, each once) each changed by change (NoteValues&), in one batch. A change keeps
// the note's ID: the places stay as they are.
template <typename Change>
ClipNotes withNotesChanged (const ClipNotes& clip, const std::vector<NoteID>& ids, Change&& change)
{
    auto notes = clip.notes.transient();

    for (const auto id : ids)
    {
        if (const auto* place = clip.places.find (id.getRaw()))
        {
            notes.update (*place,
                          [&change, id] (NoteValues note)
                          {
                              change (note);
                              jassert (note.id == id);
                              juce::ignoreUnused (id);
                              return note;
                          });
        }
    }

    return { notes.persistent(), clip.places, clip.controls };
}

// Each note changed by change (NoteValues&), and each controller's value by changeControl (ControlValues&): a note keeps its ID.
template <typename ChangeNote, typename ChangeControl>
ClipNotes withEachChanged (const ClipNotes& clip, ChangeNote&& changeNote, ChangeControl&& changeControl)
{
    auto notes = immer::flex_vector<NoteValues>().transient();
    auto controls = immer::flex_vector<ControlValues>().transient();

    immer::for_each (clip.notes,
                     [&] (NoteValues note)
                     {
                         [[maybe_unused]] const auto id = note.id;
                         changeNote (note);
                         jassert (note.id == id);
                         notes.push_back (note);
                     });

    immer::for_each (clip.controls,
                     [&] (ControlValues control)
                     {
                         changeControl (control);
                         controls.push_back (control);
                     });

    return { notes.persistent(), clip.places, controls.persistent() };
}

// The notes for which gone (const NoteValues&) is true taken out, the places of those after them made anew.
template <typename Gone>
ClipNotes withoutNotes (const ClipNotes& clip, Gone&& gone)
{
    auto notes = immer::flex_vector<NoteValues>().transient();
    auto places = immer::map<std::uint64_t, std::uint32_t>().transient();
    auto any = false;

    immer::for_each (clip.notes,
                     [&] (const NoteValues& note)
                     {
                         if (gone (note))
                         {
                             any = true;
                             return;
                         }

                         places.set (note.id.getRaw(), (std::uint32_t) notes.size());
                         notes.push_back (note);
                     });

    if (! any)
        return clip;

    ClipNotes result { notes.persistent(), places.persistent(), clip.controls };
    jassert (result.places.size() == result.notes.size());
    return result;
}

// A note added after the clip's others: its ID none of them has.
ClipNotes withNoteAdded (const ClipNotes&, const NoteValues&);

// The controllers' values for which gone (size_t place, const ControlValues&) is true taken out; and values added after the
// clip's others, in their order.
template <typename Gone>
ClipNotes withoutControls (const ClipNotes& clip, Gone&& gone)
{
    auto controls = immer::flex_vector<ControlValues>().transient();
    size_t place = 0;
    auto any = false;

    immer::for_each (clip.controls,
                     [&] (const ControlValues& control)
                     {
                         if (gone (place++, control))
                             any = true;
                         else
                             controls.push_back (control);
                     });

    if (! any)
        return clip;

    return { clip.notes, clip.places, controls.persistent() };
}

ClipNotes withControlsAdded (const ClipNotes&, const std::vector<ControlValues>&);

//==============================================================================
// Every MIDI clip's notes and values in a song, by the clip's ID: the clips of an alias group (Schema.h: clipLink) hold ONE
// value, the same ClipNotes under each one's ID. An area of the song (ValueArea.h): the same map without a look inside.
struct NotesByClip
{
    immer::map<std::uint64_t, ClipNotes> clips;

    const ClipNotes* find (ClipID clip) const { return clips.find (clip.getRaw()); }
    bool isSameAs (const NotesByClip& other) const noexcept { return clips.identity() == other.clips.identity(); }
};

//==============================================================================
// A clip as a value (roadmap step S5.20, the clips' area): what each property of its CLIP element (Schema.h, "On CLIP") and
// its WARP children hold, in Phazite's own types. The song holds its clips as these (SongClips, below), which the clips' code
// (model/arrangement/internal/Clips.h, AudioClips.h) reads (getClipValue()) and sets for the code outside it, by their IDs.

// A warp marker as an audio clip holds it (Schema.h: WARP): a time of its file as recorded, and where that time plays, as a
// time of the file played straight at the clip's tempo, both in seconds.
struct WarpValue
{
    double time = 0.0, playsAt = 0.0;

    bool operator== (const WarpValue& other) const noexcept
    {
        return juce::exactlyEqual (time, other.time) && juce::exactlyEqual (playsAt, other.playsAt);
    }
};

// A fade as an audio clip holds it (Schema.h: clipFadeIn, clipFadeInShape, clipFadeInBend and the fade-out's): how long it
// is, in seconds; whether its shape is an S-curve (Schema.h: fadeIsSCurve) or a curve; and how far it bends, from -1 to 1.
struct ClipFade
{
    double seconds = 0.0;
    bool sCurve = false;
    float bend = 0.0f;

    bool operator== (const ClipFade& other) const noexcept
    {
        return juce::exactlyEqual (seconds, other.seconds) && sCurve == other.sCurve && juce::exactlyEqual (bend, other.bend);
    }
};

// What the audio clips of an alias group share (Schema.h: clipLink): the file, its tempo while the clip follows the song's,
// its pitch, its direction and its warp markers, in their order. The clips of a group are to hold the same value, its
// markers one vector, shared, as a group's notes are one ClipNotes under each clip's ID.
struct ClipSound
{
    juce::String file;                   // clipFile, as the song holds it: a path relative to the project, or a full one
    double tempo = 0.0;                  // clipTempo: 0 for none (it plays at the file's own speed)
    float pitch = 0.0f;                  // clipPitch, in semitones
    bool reversed = false;               // clipReversed
    immer::flex_vector<WarpValue> warps; // WARP, in the order the clip holds them (the song holds none while tempo is 0)

    // The same markers without a look inside (one vector, shared) and the rest alike; and alike, compared.
    bool isSameAs (const ClipSound&) const noexcept;
    bool operator== (const ClipSound&) const;
};

// A clip as the song holds it: each property of its CLIP element as a plain value. What a clip's element hasn't (a property
// written only while it is set, or one a damaged file left out) holds its default here: its description's (Schema.h).
struct ClipValue
{
    // What it holds (Schema.h: clipKind): notes (clipIsMidi) or a part of an audio file (clipIsAudio). Its own name, as the
    // ClipID's tag (model/base/IDs.h) is phazite::ClipKind.
    enum class Kind : std::uint8_t
    {
        midi,
        audio
    };

    ClipID id;              // clipID
    Kind kind = Kind::midi; // clipKind
    TrackID track;          // the track it is on: none for a clip on none (a copy)
    juce::String name;      // clipName: empty for none (it is called after its track)
    BeatPosition start;     // clipStart

    // clipLength: a MIDI clip's. An audio clip has none (its range follows from its part of its file and its tempo:
    // AudioClips.h), so readClipValue() gives it 0, and == leaves it out of an audio clip's value, whatever it holds.
    // A MIDI clip whose element has none holds the description's 4 beats.
    BeatDuration length = BeatDuration::fromBeats (4.0);

    BeatDuration offset;                   // clipOffset: how far into its notes a MIDI clip starts
    bool muted = false;                    // clipMute
    std::uint64_t link = 0;                // clipLink: its alias group, 0 for none (the loader takes a link of 0 away: Project.cpp)
    ClipSound sound;                       // an audio clip's file and how it plays it
    double fileStart = 0.0, fileEnd = 0.0; // clipFileStart, clipFileEnd: the part of its file, in seconds
    float gain = 0.0f;                     // clipGain, in decibels
    ClipFade fadeIn, fadeOut;              // clipFadeIn, clipFadeInShape, clipFadeInBend and the fade-out's
    int takes = 0;                         // clipTakes: 0 for none (a clip recorded round a loop has 2 or more)
    int take = 1;                          // clipTake: the one it plays, from 1
    double takeLength = 0.0;               // clipTakeLength, in seconds

    bool isMidi() const noexcept { return kind == Kind::midi; }
    bool isAudio() const noexcept { return kind == Kind::audio; }

    // Written out, not defaulted: a defaulted one would compare doubles with ==, which GCC's -Wfloat-equal refuses.
    bool operator== (const ClipValue&) const;
};

// The shortest a MIDI clip is: what the clips' code keeps a clip's length to (keepWithinClip()).
inline constexpr double shortestClipBeats = 1.0 / 64.0;

// Every clip of a song, as the clips' area holds them: the clips by their IDs, each track's clips in their order (what a
// track's clips are written in, first among its elements, in a whole copy of the song: WholeState.h; a track without clips
// has no entry), and each alias group's clips (by its link) in the order they joined it (a clip in none is in none). The
// helpers below are the only writers of the three, and keep them in step: holdsItsLists() says whether they are.
struct SongClips
{
    immer::map<std::uint64_t, ClipValue> clips;
    immer::map<std::uint64_t, immer::flex_vector<ClipID>> tracks;
    immer::map<std::uint64_t, immer::flex_vector<ClipID>> groups;

    const ClipValue* find (ClipID id) const { return clips.find (id.getRaw()); }

    // The same three without a look inside: what an area compares (ValueArea.h).
    bool isSameAs (const SongClips& other) const noexcept
    {
        return clips.identity() == other.clips.identity() && tracks.identity() == other.tracks.identity()
               && groups.identity() == other.groups.identity();
    }
};

// A clip added, at a place in its track's list (from 0, or -1 for after the others there), or one of them changed: on another
// track, it leaves its old track's list for the new one's (at the place given, or at its end); in another alias group, it
// leaves its old group's list for the end of the new one's. A clip that stays on its track keeps its place there.
SongClips withClip (const SongClips&, ClipValue, int index = -1);

// A clip taken out, of its track's list and its group's too (nothing for one that isn't there).
SongClips withoutClip (const SongClips&, ClipID);

// A track's clips put in another order: the same clips (else nothing changes).
SongClips withTrackOrder (const SongClips&, TrackID, immer::flex_vector<ClipID> order);

// Whether the three agree: each clip once in its track's list and, in an alias group, once in its group's; each list
// holding clips of that track or group alone, and none of them empty. For the tests, and the assertions where it is cheap.
// A group of one clip is a group to these (the helpers leave what the caller makes): hasGroupsOfOne() says whether any is.
bool holdsItsLists (const SongClips&);

// Whether an alias group holds one clip alone, which the model never leaves (the last clip of a group leaves it as the
// next-to-last goes: Clips.h, eraseClip()); what a song's consistency check is to say there is none of.
bool hasGroupsOfOne (const SongClips&);

//==============================================================================
// The song's values, which the song holds once its tree is consistent (Song::holdValues(), as model/project/internal/
// Project.h's makeSong() makes a song): the clips' area, the notes' area and the tempo map's, in the song's one undo history,
// and what keeps them and the tree together. Message thread only, as the song's tree is: nothing on the audio thread reads it
// (what plays gets what the model compiles of it), and what runs on another thread gets a copy (WholeState.h:
// copyWholeSong(), keepValues()).
//
// Who changes the values: the clips' code (Clips.h: addClip(), setClip(), eraseClip() and the rest, through setClip(),
// eraseClip() and setTrackClipOrder() below), the notes' code (Notes.h: setClipNotesValue(), which is setClipNotes() below),
// and the tempo map's code (Tempo.h: every edit of the tempo map, through setTempoMap() below). The song takes in its file's
// tempo map as it holds its values (holdValues(): TempoMapValue.h, readHeldTempoMap()), and empties its TEMPOMAP element,
// properties and children Phazite doesn't know too. A MIDI clip that joins an alias group takes the group's notes, and one
// that leaves it keeps those it holds (setClip()). Everything else that changes the tree is heard by the keeper, a listener
// of the song's root: a track taken
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
// change the map). Each in the mode of the moment: in the history's step in progress, or outside the history while the song
// is written outside it (Song::OutsideHistory). Nothing while the history undoes or redoes a step: the areas' own actions
// move them. The keeper performs its changes from inside JUCE's action for the tree's, which JUCE adds to the step before
// the tree's own: so an undo puts the tree back first and the clips after (JUCE workaround W62 (docs/juce-workarounds.md)).
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
    ValueArea<TempoMapValue> tempoMap; // each kind by beat, at least one of each (TempoMapValue.h)

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
    // value without a step of its own (ValueArea.h: setWithoutStep()).
    void setClip (Song&, ClipValue, int index = -1, const ClipNotes* notes = nullptr);

    // A clip taken out of the song, its notes with it; and a track's clips put in another order (the same clips), the same way:
    // the order it holds already is no change.
    void eraseClip (Song&, ClipID);
    void setTrackClipOrder (Song&, TrackID, immer::flex_vector<ClipID> order);

    // Gives a MIDI clip of the song and each MIDI clip of its alias group this value of notes, as setClip() sets: the one way
    // to change a clip's notes. Notes equal to those each holds are no change, and make no action in the history: nothing
    // where they hold one value, else this one without a step of its own (a group holds one value).
    void setClipNotes (Song&, ClipID, ClipNotes);

    // Gives the song this tempo map, the tempo map's code's one way to change it (model/transport/internal/Tempo.h): in the
    // song's undo history (the step in progress), or outside it while the song is written outside it. A map equal to the one
    // the song holds is no change, and writes nothing: no step, the value the song holds kept (an entry has no identity of
    // its own to keep, nor a value of the map's anything else shares, so nothing is lost).
    void setTempoMap (Song&, TempoMapValue);

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
        // listens to the song's root leaves what its tree tells under its TEMPOMAP element alone (isHeardAsTempoMap(),
        // below): what the keeper takes in there (its safety net) is told here, once.
        virtual void tempoMapChanged() {}
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

private:
    friend class Song;

    struct NotesTeller;
    struct ClipsTeller;
    struct TempoMapTeller;
    struct Keeper;
    mutable juce::ListenerList<Listener> listeners; // told by the areas' tellers (addListener())
    std::unique_ptr<NotesTeller> notesTeller;
    std::unique_ptr<ClipsTeller> clipsTeller;
    std::unique_ptr<TempoMapTeller> tempoMapTeller;
    std::unique_ptr<Keeper> keeper;
    int adopted = 0, tempoMapAdopted = 0;

    // In the history, or outside it, as the song is written now; and a value alike to the area's, without a step of its own
    // (ValueArea.h: setWithoutStep()).
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
// in (its safety net, above), which the area tells.
bool isHeardAsTempoMap (const juce::ValueTree& element);
bool isHeardAsTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child);

//==============================================================================
// The conversion between a clip's element and its value: the one place that reads and writes NOTE and CONTROL by name.

// A MIDI channel as a note's or a controller's element holds it (schema::noteChannel, controlChannel), from 1 to 16: 1 where
// none is written, and one beyond them kept within them. And written so, as nothing for the first.
int readChannel (const juce::ValueTree& element, const juce::Identifier& property);
void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager*);

// A clip's notes and controllers' values, read from its NOTE and CONTROL children, each kind in its order, whatever the order
// of the two kinds among each other: each number read as the loader reads it (SongText.h: readValueNumber(), the same number
// for the same text wherever it is read) and kept within its description's range (Schema.h), one that is absent or no finite
// number as its description's fallback, else 0 (the channel, 1; the flags, off). A property Phazite doesn't know on a NOTE
// or a CONTROL is no part of the value, nor is a CONTROL of a kind Phazite doesn't know (which the loader takes out of a
// song: model/project/Project.cpp).
ClipNotes readClipNotes (const juce::ValueTree& clipState);

// The same of NOTE and CONTROL elements on no clip (a MIDI file's read for an import), in their order.
ClipNotes readClipNotes (const std::vector<juce::ValueTree>& events);

// One note's values, read from its NOTE element, as readClipNotes() reads it.
NoteValues readNoteValues (const juce::ValueTree& noteState);

// Appends a clip's value to its element (a copy of the clip, outside any song: WholeState.h), its NOTE elements first, then
// its CONTROL elements, each kind in its order, each written as the song has always written one: a note's ID, pitch, start,
// length and velocity always, its flags only while they are on, and its channel only if it isn't the first; a value's kind,
// its number only where its kind has one, its beat and value, and its channel only if it isn't the first.
void writeClipNotes (const ClipNotes&, juce::ValueTree& clipCopy);

// A note's values with one of its properties (by its name in the song: schema::notePitch, noteMute...) given a value as the
// song would read it from the element, or taken away (a void value: as if the element hadn't it): a merge's clash on a note's
// value, chosen. Whether the property is a note's.
bool setNoteProperty (NoteValues&, const juce::Identifier& property, const juce::var& value);

// Gives a clip (to) another's notes and controllers' values (from's, its alias group's keeper), in the same order and with
// the same IDs, where its own were: an alias group's clips hold the same (Schema.h: clipLink). Only what differs is written.
// For a copy of the song, its elements whole (a merge's: model/versions/internal/SongMerge.h), in the undo history given, or
// outside any.
void giveClipEvents (const juce::ValueTree& from, juce::ValueTree to, juce::UndoManager*);

//==============================================================================
// The conversion between a clip's element and its value: the one place that reads and writes a CLIP's properties and its
// WARP children by name.

// A clip's value, read from its CLIP element and its WARP children: each number as its description reads it (Schema.h: one
// that is absent reads as its fallback, or else as none: a MIDI clip's length 4 beats, its start 0, its take 1, its tempo
// none; one outside its range kept within it), nothing of the other kind of clip's (an audio clip's length and offset 0, a
// MIDI clip's file, part, gain, fades and takes at their defaults, whatever its element holds), the markers in the order
// the element holds them. Its track is the TRACK the element is on (none for an element on none). Its NOTE and
// CONTROL children are its notes' value's (readClipNotes()), and a property or a child Phazite doesn't know is no part of
// it: the song keeps none of them (Schema.h, CLIP).
ClipValue readClipValue (const juce::ValueTree& clip);

// A clip's element as a whole copy of the song writes it (WholeState.h), new: each property of its kind only while it is set
// (its ID, kind and start always; a name while it isn't empty; a MIDI clip's length always and its offset while more than 0;
// its mute, link, an audio clip's tempo, pitch, direction, gain, fades and takes while they aren't their defaults; an audio
// clip's file and its part always), so that a property another program's file held at its default is not written again;
// then an audio clip's WARP elements, in their order; then its notes (writeClipNotes()), where given. Its value as it is:
// what keeps it within what a clip holds is the clips' code's (keepWithinClip()). Its track is where the element goes.
juce::ValueTree writeClipElement (const ClipValue&, const ClipNotes* notes = nullptr);

// A clip's value kept within what a clip holds, as the clips' code keeps what it sets, and as the song takes in a clip read
// from its element (Song::holdValues(), and the keeper's safety net): nothing of the other kind of clip's properties (each
// at its default, as readClipValue() reads them), its start at 0 or later, a MIDI clip's length at least shortestClipBeats,
// the part of its file starting at 0 or later and ending at its start or later, its fades' bends within -1 to 1, and its
// takes either none (0 takes, the first take, no take's length) or a count with the take it plays one of them.
ClipValue keepWithinClip (ClipValue);

// A clip's value with one of its properties (by its name in the song: schema::clipStart, clipMute...) given a value as the
// value would read it from an element that held it, or taken away (a void value: as if the element hadn't it): a merge's
// clash on a clip's property, chosen. Whether the property is a clip's.
bool setClipProperty (ClipValue&, const juce::Identifier& property, const juce::var& value);
} // namespace phazite
