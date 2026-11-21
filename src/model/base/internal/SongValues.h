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
#include "model/base/internal/ValueArea.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <immer/algorithm.hpp>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>

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
// while the history undoes or redoes a step: the area's own action moves it. What the keeper can't see: a clip added or
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
    // What the clips' code that copies a clip in the song does (model/arrangement/internal/Clips.h: copyClipState()), so that
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
    std::unique_ptr<Keeper> keeper;

    void setAll (Song&, NotesByClip next); // in the history, or outside it, as the song is written now
    void adopt (Song&);                    // Song::holdValues()

    JUCE_DECLARE_NON_COPYABLE (SongValues)
    JUCE_DECLARE_NON_MOVEABLE (SongValues)
};

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
} // namespace phazite
