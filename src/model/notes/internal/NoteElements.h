// SPDX-License-Identifier: AGPL-3.0-or-later
// Typed access to what a MIDI clip holds: its notes (schema::NOTE) and its controllers' values (schema::CONTROL), in
// Phazite's own types. The one place that reads and writes their properties by name: the notes' code (Notes.h), what
// plays them (model/playback/internal/SongPlayer.h) and the playlist's read of their shapes go through these, so that a
// value read as another type than it is saved as doesn't build.
//
// A view (NoteElement, NoteEdit, ControlElement) is for the length of a call: made by the functions below, used and let go,
// never kept in a member or across a change of the song, so that none outlives its element's place in the song. Roadmap
// step S5.20's first area, written by hand: a clip's own element is still found by the clips' functions
// (model/arrangement/internal/Clips.h), whose turn comes next, and what puts a clip's elements in and takes them out by
// their places (Notes.cpp: the controller lanes) still works on that element.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/notes/NoteValues.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace phazite
{
// A MIDI channel as a note's or a controller's element holds it (schema::noteChannel, controlChannel), from 1 to 16: 1 where
// none is written, and one beyond them kept within them. And written so, as nothing for the first.
int readChannel (const juce::ValueTree& element, const juce::Identifier& property);
void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager*);

// What a note is besides its place, pitch and velocity, each written on its element only while it is (Schema.h).
enum class NoteFlag
{
    muted,
    slide,
    portamento
};

// A note as its clip holds it. Its start counts from its clip's content start, as every note's of the clip does: the song
// beat is that start plus this (Clips.h: getContentStart()).
struct NoteValues
{
    NoteID id {};
    int pitch = 60;
    BeatPosition start {}; // among the clip's notes
    BeatDuration length = BeatDuration::fromBeats (1.0);
    int velocity = 100;
    bool muted = false, slide = false, portamento = false;
    int channel = 1; // from 1 to 16
};

// What makes a view of the element a call gives, where the view keeps it (NoteElement's and ControlElement's constructors).
struct InPlace
{
};

//==============================================================================
// A note of a clip, read. One of nothing (exists() is false) reads as the values a note without them has.
class NoteElement
{
public:
    NoteElement() = default;

    explicit NoteElement (juce::ValueTree noteState) noexcept
        : state (std::move (noteState))
    {
    }

    // Of the element a call gives, made where the view keeps it, with no copy on the way: for the functions below.
    template <typename Make>
    NoteElement (InPlace, Make&& make)
        : state (make())
    {
    }

    bool exists() const noexcept { return state.isValid(); }
    bool isNote() const noexcept { return state.hasType (schema::NOTE); }

    NoteID getID() const { return NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) state[schema::noteID])); }
    int getPitch() const { return (int) state[schema::notePitch]; }
    BeatPosition getStart() const { return BeatPosition::fromBeats ((double) state[schema::noteStart]); } // among its clip's notes
    BeatDuration getLength() const { return BeatDuration::fromBeats ((double) state[schema::noteLength]); }
    int getVelocity() const { return (int) state[schema::noteVelocity]; }
    bool has (NoteFlag flag) const { return (bool) state[getName (flag)]; }
    int getChannel() const { return readChannel (state, schema::noteChannel); }

    NoteValues getValues() const
    {
        return { getID(),
                 getPitch(),
                 getStart(),
                 getLength(),
                 getVelocity(),
                 has (NoteFlag::muted),
                 has (NoteFlag::slide),
                 has (NoteFlag::portamento),
                 getChannel() };
    }

    // Whether two views are of one note of the song.
    bool operator== (const NoteElement& other) const noexcept { return state == other.state; }

protected:
    static const juce::Identifier& getName (NoteFlag flag) noexcept
    {
        return flag == NoteFlag::muted ? schema::noteMute : flag == NoteFlag::slide ? schema::noteSlide : schema::notePortamento;
    }

    juce::ValueTree state;

    friend void removeNote (juce::ValueTree& clipState, const NoteElement&, juce::UndoManager*);
};

// A note of a clip, changed: each change in the undo history it was made with (none: outside any).
class NoteEdit final : public NoteElement
{
public:
    NoteEdit() = default;

    NoteEdit (juce::ValueTree noteState, juce::UndoManager* history) noexcept
        : NoteElement (std::move (noteState)),
          undo (history)
    {
    }

    template <typename Make>
    NoteEdit (InPlace, Make&& make, juce::UndoManager* history)
        : NoteElement (InPlace(), std::forward<Make> (make)),
          undo (history)
    {
    }

    void setPitch (int pitch) { state.setProperty (schema::notePitch, pitch, undo); }
    void setStart (BeatPosition start) { state.setProperty (schema::noteStart, start.inBeats(), undo); } // among its clip's notes
    void setLength (BeatDuration length) { state.setProperty (schema::noteLength, length.inBeats(), undo); }
    void setVelocity (int velocity) { state.setProperty (schema::noteVelocity, velocity, undo); }
    void setChannel (int channel) { writeChannel (state, schema::noteChannel, channel, undo); }

    void set (NoteFlag flag, bool on)
    {
        if (on)
            state.setProperty (getName (flag), true, undo);
        else
            state.removeProperty (getName (flag), undo);
    }

private:
    juce::UndoManager* undo = nullptr;
};

// A clip's note by its ID, by the song's index (model/base/internal/SongIndex.h), which looks through a clip outside the
// song: nothing for a note or a clip that is gone. To change, in the song's undo history.
NoteElement findNote (const Song&, const juce::ValueTree& clipState, NoteID);
NoteEdit editNote (Song&, const juce::ValueTree& clipState, NoteID);

// Each note of a clip, in the order the clip holds them.
template <typename Visit>
void forEachNote (const juce::ValueTree& clipState, Visit&& visit)
{
    for (auto child = clipState.begin(), end = clipState.end(); child != end; ++child)
        if (const NoteElement note (InPlace(), [&child] { return *child; }); note.isNote())
            visit (note);
}

// A note with these values put at the end of a clip's element, taken out of it, and all of a clip's taken out: in the undo
// history given, or outside any (a clip filled before it joins the song).
void appendNote (juce::ValueTree& clipState, const NoteValues&, juce::UndoManager*);
void removeNote (juce::ValueTree& clipState, const NoteElement&, juce::UndoManager*);
void removeAllNotes (juce::ValueTree& clipState, juce::UndoManager*);

//==============================================================================
// A controller's value as its clip holds it (NoteValues.h: ControlKind). Its beat counts from the clip's content start, as
// a note's start does.
struct ControlValues
{
    ControlKind kind = ControlKind::controller;
    int number = 0; // a controller's and a note's pressure's; the other kinds have none
    double beat = 0.0;
    int value = 0;
    int channel = 1; // from 1 to 16
};

// The kinds of controller events, by their names in the song (schema::controlKind).
const char* getSchemaKind (ControlKind) noexcept;

// A controller's value in a clip, read. One made from values (makeControl()) is in no clip yet.
class ControlElement
{
public:
    ControlElement() = default;

    explicit ControlElement (juce::ValueTree controlState) noexcept
        : state (std::move (controlState))
    {
    }

    template <typename Make>
    ControlElement (InPlace, Make&& make)
        : state (make())
    {
    }

    bool exists() const noexcept { return state.isValid(); }
    bool isControl() const noexcept { return state.hasType (schema::CONTROL); }

    std::optional<ControlKind> getKind() const; // none for a kind Phazite doesn't know
    int getNumber() const { return (int) state[schema::controlNumber]; }
    double getBeat() const { return (double) state[schema::controlBeat]; } // among its clip's notes
    int getValue() const { return (int) state[schema::controlValue]; }
    int getChannel() const { return readChannel (state, schema::controlChannel); }

private:
    juce::ValueTree state;

    friend void appendControl (juce::ValueTree& clipState, const ControlElement&, juce::UndoManager*);
};

// A value as a clip would hold it, kept within what its kind takes (its number only if its kind has one, its beat not
// before the clip's content start); and one put at the end of a clip's element.
ControlElement makeControl (const ControlValues&);
void appendControl (juce::ValueTree& clipState, const ControlElement&, juce::UndoManager*);

// Whether one of a clip's elements is a controller's value, and the value at a place among the clip's elements (nothing
// where another kind of element is); and each of a clip's, in order, with its place.
ControlElement getControlAt (const juce::ValueTree& clipState, int place);

template <typename Visit>
void forEachControl (const juce::ValueTree& clipState, Visit&& visit)
{
    auto place = 0;

    for (auto child = clipState.begin(), end = clipState.end(); child != end; ++child, ++place)
        if (const ControlElement control (InPlace(), [&child] { return *child; }); control.isControl())
            visit (place, control);
}

// Each note and each controller's value of a clip in one walk, in the order the clip holds them: what plays the song reads.
template <typename VisitNote, typename VisitControl>
void forEachEvent (const juce::ValueTree& clipState, VisitNote&& visitNote, VisitControl&& visitControl)
{
    for (auto child = clipState.begin(), end = clipState.end(); child != end; ++child)
    {
        if (const NoteElement note (InPlace(), [&child] { return *child; }); note.isNote())
            visitNote (note);
        else if (const ControlElement control (InPlace(), [&child] { return *child; }); control.isControl())
            visitControl (control);
    }
}
} // namespace phazite
