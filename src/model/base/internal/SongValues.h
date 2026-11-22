// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's values: what the song moves from its juce::ValueTree into immutable values held beside it (the immer library;
// roadmap step S5.20, model/base/internal/ValueArea.h), one area at a time. The first area is a MIDI clip's notes and its
// controllers' values (ClipNotes), as plain values in Phazite's own types, which the notes' code
// (model/notes/internal/Notes.h) computes each edit on, from one value to the next.
//
// The value is the storage: the song holds an area of them (SongValues, NotesByClip: each MIDI clip's value by its ID, one
// value for the clips of an alias group) in its one undo history, and a clip of the song holds no NOTE or CONTROL element in
// its tree. The conversion below is the one place that reads and writes those elements by their names: what a file's clip
// (or any clip that comes into the song with them: a paste, a version, an import) is read into, and what a whole copy of the
// song is written from (WholeState.h).
//
// immer is the model's alone (tools/check-architecture.sh: immer-outside-model): nothing outside src/model includes this.
#pragma once

#include "model/base/Beats.h"
#include "model/base/ControlKinds.h"
#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/ValueArea.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <memory>
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
// its WARP children hold, in Phazite's own types. The song doesn't hold these yet: its clips are still its tree's elements,
// which the clips' code (model/arrangement/internal/Clips.h, AudioClips.h) reads into these values (getClipValue()) for the
// code outside it, so that what reads a clip reads it by its ID, whatever stores it.

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
// the clips' code writes only while it is set) holds its default here.
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

// The shortest a MIDI clip is: what the clips' code keeps a clip's length to, and writeClipElement() too.
inline constexpr double shortestClipBeats = 1.0 / 64.0;

// Every clip of a song, as the clips' area will hold them (the song holds none of these yet): the clips by their IDs, each
// track's clips in the order its element holds them (a track without clips has no entry), and each alias group's clips (by
// its link) in the order they joined it (a clip in none is in none). The helpers below are the only writers of the three, and
// keep them in step: holdsItsLists() says whether they are.
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
// Project.h's makeSong() makes a song): the notes' area, in the song's one undo history, and what keeps it and the tree
// together. Message thread only, as the song's tree is: nothing on the audio thread reads it (what plays gets what the model
// compiles of it), and what runs on another thread gets a copy (WholeState.h: copyWholeSong(), keepValues()).
//
// Who changes a clip's value: the notes' code (Notes.h: setClipNotesValue(), which is setClipNotes() below) and the clips'
// code that copies a clip in the song (copyClipNotes()). Everything else that changes the tree is heard by the keeper, a
// listener of the song's root, added before any other but the song's index's: a clip that comes into the song with NOTE or
// CONTROL elements (a paste, a duplicate's copy, an import, a version applied, a merge, a track that holds clips) has them
// read into its value and taken out of the tree; a clip taken out of the song has its value remembered with its element
// (JUCE moves a clip by taking it out and putting it in again, the same element) and erased, and given back if the same
// element comes back; one that comes without them, and isn't remembered, keeps the value a copy gave it, or an empty one;
// a clip that joins an alias group (its clipLink set) takes the group's value. Each in the mode of the moment: in the
// history's step in progress, or outside the history while the song is written outside it (Song::OutsideHistory). Nothing
// while the history undoes or redoes a step: the area's own action moves it. The keeper is also what tells the clips'
// listeners (ClipsListener) what the tree says of the clips, a step's, an undo's and a redo's alike: it hears the tree after
// the song's index (which listens to the track list, below the root), and in no set order among the song's other listeners
// of the root (JUCE tells those of one element in the order of their handles' addresses), which needs none: the clips'
// listeners only mark what they are told, and the tree's own listeners hear nothing of a clip. What the keeper can't see: a clip added or
// taken out with no undo manager outside an OutsideHistory scope is written into the step in progress (if any) as if it had
// one; nothing in Phazite does that to a song that holds its values (the loader does it before: makeSong()).
class SongValues final
{
public:
    explicit SongValues (Song&);
    ~SongValues();

    ValueArea<NotesByClip> notes;

    // A clip's value: none for a clip that isn't a MIDI clip of the song's.
    const ClipNotes* findClipNotes (ClipID clip) const { return notes.get().find (clip); }

    // Gives a clip and each clip of its alias group that is in the song this value (a clip not in the song yet: itself alone),
    // in the song's undo history (the step in progress), or outside it while the song is written outside it
    // (Song::isWritingOutsideHistory()). The one way to change a clip's value.
    void setClipNotes (Song&, ClipID, ClipNotes);
    void eraseClipNotes (Song&, ClipID);

    // A copy of a clip (to, not in the song yet) given the clip's value: the same value, shared until one of them is edited.
    // What the clips' code that copies a clip in the song does (model/arrangement/Clips.cpp: addCopy()), so that
    // the copy, an alias or not, holds what the clip holds without a walk through its notes.
    void copyClipNotes (Song&, ClipID from, ClipID to);

    // The clips whose values changed, by a step, an undo, a redo or a write outside the history: compared by identity, clip by
    // clip. A listener only marks them, and reads nothing else of the song while it is told (ValueArea.h): during an undo the
    // rest of the step may not be back yet.
    struct NotesListener
    {
        virtual ~NotesListener() = default;
        virtual void clipNotesChanged (const std::vector<ClipID>& clips) = 0;
    };

    void addNotesListener (NotesListener*);
    void removeNotesListener (NotesListener*);

    // A clip that changed, with the track it was on before and the one it is on after: none before for a clip added, none
    // after for one taken out (erased), the same track twice for a change in place.
    struct ClipChange
    {
        ClipID id;
        TrackID before, after;
    };

    // What hears the song's clips change, from one place, by their IDs: a clip added, taken out, moved to another track or
    // changed in place (any of its properties, its warp markers), and the tracks whose order of clips changed; a track added
    // or taken out tells each of its clips (those of the tracks in it too) as added or taken out, in one call. A step, an undo,
    // a redo and a write outside the history alike. A clip's notes and controllers' values are no part of it: NotesListener's.
    // While the song's tree holds its clips, the keeper tells these from the tree's notifications as they come, in the order
    // they come (a clip moved to another track is taken out of one and added to the other: two calls), and what listens to the
    // song's root hears nothing of a clip from the tree (isHeardAsClips(), below), so that nothing hears a change twice. Once
    // the clips are an area of their own (roadmap step S5.20) the area tells them, as one call for each change of its value.
    // A listener only marks what changed (the clips, the tracks), and reads nothing of the song while it is told: during an
    // undo the rest of the step may not be back yet (ValueArea.h). It does its work later (a plan's next compile, the next turn
    // of the message loop), however many calls came before. Listening changes nothing of the values: a song held const may
    // be listened to.
    struct ClipsListener
    {
        virtual ~ClipsListener() = default;
        virtual void clipsChanged (const std::vector<ClipChange>& clips, const std::vector<TrackID>& reordered) = 0;
    };

    void addClipsListener (ClipsListener*) const;
    void removeClipsListener (ClipsListener*) const;

    // Whether the keeper remembers a clip's element with its value, taken out of the song (above): for the keeper's tests,
    // which check that it lets go of what nothing can bring back (tests/SongValuesTests.cpp). Not for the check of the song's
    // values (tests/SongCheck.h): a clip taken out leaves the area at once, a step still open or not, so no entry of the area
    // belongs to a clip outside the tree, remembered or not.
    bool remembers (const juce::ValueTree& clip) const;

private:
    friend class Song;

    struct Teller;
    struct Keeper;
    std::unique_ptr<Teller> teller;
    mutable juce::ListenerList<ClipsListener> clipsListeners; // told by the keeper (addClipsListener())
    std::unique_ptr<Keeper> keeper;

    void setAll (Song&, NotesByClip next); // in the history, or outside it, as the song is written now
    void adopt (Song&);                    // Song::holdValues()

    JUCE_DECLARE_NON_COPYABLE (SongValues)
    JUCE_DECLARE_NON_MOVEABLE (SongValues)
};

// Whether what the song's tree tells of an element (a property changed, a child added, taken out or moved in it) is the
// clips', which a listener of the song's root hears from the clips' listener alone (SongValues::ClipsListener) and leaves
// alone in its tree callbacks: a clip on a track, or an element in one (its warp markers; and its NOTE and CONTROL elements,
// which a song that holds its values holds only while the keeper takes them in, the notes' listener's). And whether a child
// added to an element, taken out of it or moved in it is: a clip on its track, or a child of an element that is. A clip
// elsewhere (a file that put one elsewhere, which the loader takes out: model/project/internal/Project.h, makeConsistent())
// is no clip of a track's, which the clips' listener isn't told of: the tree's listeners hear it as they always did.
bool isHeardAsClips (const juce::ValueTree& element);
bool isHeardAsClips (const juce::ValueTree& parent, const juce::ValueTree& child);

//==============================================================================
// The conversion between a clip's element and its value: the one place that reads and writes NOTE and CONTROL by name.

// A MIDI channel as a note's or a controller's element holds it (schema::noteChannel, controlChannel), from 1 to 16: 1 where
// none is written, and one beyond them kept within them. And written so, as nothing for the first.
int readChannel (const juce::ValueTree& element, const juce::Identifier& property);
void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager*);

// A clip's notes and controllers' values, read from its NOTE and CONTROL children, each kind in its order, whatever the order
// of the two kinds among each other: each property as the song has always read it (a property that is absent reads as 0,
// but the channel, 1, and the flags, off). A property Phazite doesn't know on a NOTE or a CONTROL is no part of the value,
// nor is a CONTROL of a kind Phazite doesn't know (which the loader takes out of a song: model/project/Project.cpp); any
// other child of the clip stays in its element.
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

// A clip's value, read from its CLIP element and its WARP children: each property as the clips' code has always read it (one
// that is absent reads as 0, but the take, 1; a fade's bend kept within -1 to 1; an audio clip's length 0, whatever its
// element holds), the markers in the order the element holds them. Its track is the TRACK the element is on (none for an
// element on none). Its NOTE and CONTROL children are its notes' value's (readClipNotes()), and a property or a child Phazite
// doesn't know is no part of it.
ClipValue readClipValue (const juce::ValueTree& clip);

// Makes a clip's element hold its value as the clips' code writes it, changing only what differs (in the undo history given,
// or outside any), so that what the clips' code writes on values writes each element as it does now: an element without an
// ID (one being made) as the clip's maker (Clips.cpp, makeMidiClipState(); AudioClips.cpp, makeAudioClipState()) writes it,
// in its order, and another as the edits leave it. Its writers' table is in SongValues.cpp. A property of the other kind's (a
// length on an audio clip, a file on a MIDI clip...) is left as it is, as are properties and children Phazite doesn't know,
// its NOTE and CONTROL children, and a MIDI clip's WARP children. Its track is where the element is, not written.
void writeClipElement (const ClipValue&, juce::ValueTree& clip, juce::UndoManager*);

// Makes a clip's element, which holds held (as readClipValue() reads it), hold a value instead, in the undo history given (or
// outside any): only the properties whose values differ from held's are written, each as the clips' edits write it (a
// property held only while it is set is removed at its default: writeClipElement()'s table), the markers anew where they
// differ; nothing else, so that a property the edit doesn't change stays as the element holds it (an empty name, an offset of
// 0 another file wrote). The value is written as it is: what keeps it within what a clip holds is the caller's
// (keepWithinClip()). Only the value's kind's properties are written (a MIDI clip's length and offset, an audio clip's sound,
// part, gain, fades and takes); its track is where the element is, not written. What the clips' code (model/arrangement/
// Clips.cpp) writes a clip of the song's tree with.
void writeClipElement (const ClipValue& value, const ClipValue& held, juce::ValueTree& clip, juce::UndoManager*);

// A clip's value kept within what a clip holds, as writeClipElement() keeps what it writes: its start at 0 or later, a MIDI
// clip's length at least shortestClipBeats, the part of its file starting at 0 or later and ending at its start or later, and
// its fades' bends within -1 to 1.
ClipValue keepWithinClip (ClipValue);

// A clip's value with one of its properties (by its name in the song: schema::clipStart, clipMute...) given a value as the
// value would read it from an element that held it, or taken away (a void value: as if the element hadn't it): a merge's
// clash on a clip's property, chosen. Whether the property is a clip's.
bool setClipProperty (ClipValue&, const juce::Identifier& property, const juce::var& value);
} // namespace phazite
