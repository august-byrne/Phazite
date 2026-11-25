// SPDX-License-Identifier: AGPL-3.0-or-later
// A MIDI clip's notes and its controllers' values as a value: each note (NoteValues) in
// the clip's order with each one's place by its ID, each controller's value (ControlValues) in theirs, as one value for a clip
// (ClipNotes), and every MIDI clip's of a song by the clip's ID (NotesByClip), with the helpers that change one in batches;
// and the conversion between a clip's NOTE and CONTROL children and the value, the one place that reads and writes those by
// name. The song holds the notes as these, in an area of its values (SongValues.h, its notes' parts in NotesArea.h), which
// the notes' code (model/notes/internal/Notes.h) computes each edit on, from one value to the next.
//
// Its own header, beside SongValues.h (one header for each area's values). immer is the model's alone
// (tools/check-architecture.sh: immer-outside-model): nothing outside src/model includes this.
#pragma once

#include "model/base/Beats.h"
#include "model/base/ControlKinds.h"
#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <vector>

namespace phazite
{
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
} // namespace phazite
