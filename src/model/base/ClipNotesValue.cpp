// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ClipNotesValue.h"
#include "internal/Schema.h"
#include "internal/SongText.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace phazite
{
int keepChannel (int channel) noexcept
{
    return juce::jlimit (1, 16, channel);
}

ControlValues keepWithinKind (ControlValues values)
{
    values.number = hasNumber (values.kind) ? juce::jlimit (0, 127, values.number) : 0;
    values.beat = std::max (0.0, values.beat);
    values.value = juce::jlimit (0, getHighestValue (values.kind), values.value);
    values.channel = keepChannel (values.channel);
    return values;
}

//==============================================================================
const NoteValues* ClipNotes::find (NoteID id) const
{
    if (const auto* place = places.find (id.getRaw()))
        return &notes[*place];

    return nullptr;
}

bool ClipNotes::isSameAs (const ClipNotes& other) const noexcept
{
    return notes.identity() == other.notes.identity() && places.identity() == other.places.identity()
           && controls.identity() == other.controls.identity();
}

bool ClipNotes::operator== (const ClipNotes& other) const
{
    return isSameAs (other) || (notes == other.notes && controls == other.controls);
}

bool ClipNotes::holdsItsPlaces() const
{
    if (places.size() != notes.size())
        return false;

    std::uint32_t place = 0;
    auto right = true;

    immer::for_each (notes,
                     [&] (const NoteValues& note)
                     {
                         const auto* found = places.find (note.id.getRaw());
                         right = right && found != nullptr && *found == place;
                         ++place;
                     });

    return right;
}

ClipNotes makeClipNotes (const std::vector<NoteValues>& notes, const std::vector<ControlValues>& controls)
{
    auto order = immer::flex_vector<NoteValues>().transient();
    auto places = immer::map<std::uint64_t, std::uint32_t>().transient();

    for (const auto& note : notes)
    {
        jassert (places.find (note.id.getRaw()) == nullptr); // each note's ID once
        places.set (note.id.getRaw(), (std::uint32_t) order.size());
        order.push_back (note);
    }

    return { order.persistent(), places.persistent(), immer::flex_vector<ControlValues> (controls.begin(), controls.end()) };
}

ClipNotes withNoteAdded (const ClipNotes& clip, const NoteValues& note)
{
    jassert (clip.find (note.id) == nullptr);
    return { clip.notes.push_back (note), clip.places.set (note.id.getRaw(), (std::uint32_t) clip.notes.size()), clip.controls };
}

ClipNotes withControlsAdded (const ClipNotes& clip, const std::vector<ControlValues>& added)
{
    if (added.empty())
        return clip;

    auto controls = clip.controls.transient();

    for (const auto& control : added)
        controls.push_back (control);

    return { clip.notes, clip.places, controls.persistent() };
}

//==============================================================================
namespace
{
    // A channel as a value holds it: read as readDescribed() reads a number, whole (as the loader makes it), 1 for none.
    int readChannelNumber (const juce::var& held)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? keepChannel ((int) juce::jlimit (1.0, 16.0, std::round (*number))) : 1;
    }
} // namespace

int readChannel (const juce::ValueTree& element, const juce::Identifier& property)
{
    return readChannelNumber (element[property]);
}

void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager* undo)
{
    jassert (channel == keepChannel (channel)); // kept within 1 to 16 where the value was made

    if (channel = keepChannel (channel); channel == 1)
        element.removeProperty (property, undo);
    else
        element.setProperty (property, channel, undo);
}

namespace
{
    // The kinds of controller events, by their names in the song (schema::controlKind).
    const char* getSchemaKind (ControlKind kind) noexcept
    {
        switch (kind)
        {
            case ControlKind::controller:
                return schema::controlIsController;
            case ControlKind::pitchWheel:
                return schema::controlIsPitchWheel;
            case ControlKind::channelPressure:
                return schema::controlIsChannelPressure;
            case ControlKind::aftertouch:
                return schema::controlIsAftertouch;
            case ControlKind::programChange:
                return schema::controlIsProgramChange;
        }

        return schema::controlIsController;
    }

    std::optional<ControlKind> readKind (const juce::ValueTree& control)
    {
        const auto kind = control[schema::controlKind].toString();

        for (const auto each : { ControlKind::controller, ControlKind::pitchWheel, ControlKind::channelPressure, ControlKind::aftertouch,
                                 ControlKind::programChange })
            if (kind == getSchemaKind (each))
                return each;

        return std::nullopt;
    }

    // A controller's value, read from its CONTROL element: none for a kind Phazite doesn't know.
    std::optional<ControlValues> readControlValues (const juce::ValueTree& control)
    {
        const auto kind = readKind (control);

        if (! kind.has_value())
            return std::nullopt;

        return ControlValues { *kind, (int) readDescribed (control, schema::controlNumber), readDescribed (control, schema::controlBeat),
                               (int) readDescribed (control, schema::controlValue), readChannel (control, schema::controlChannel) };
    }

    // A new note's element: its ID, pitch, start, length and velocity always, its flags while they are on and its channel if it
    // isn't the first, in that order.
    juce::ValueTree makeNote (const NoteValues& values)
    {
        juce::ValueTree note (schema::NOTE);
        note.setProperty (schema::noteID, (juce::int64) values.id.getRaw(), nullptr);
        note.setProperty (schema::notePitch, values.pitch, nullptr);
        note.setProperty (schema::noteStart, values.start.inBeats(), nullptr);
        note.setProperty (schema::noteLength, values.length.inBeats(), nullptr);
        note.setProperty (schema::noteVelocity, values.velocity, nullptr);

        if (values.muted)
            note.setProperty (schema::noteMute, true, nullptr);

        if (values.slide)
            note.setProperty (schema::noteSlide, true, nullptr);

        if (values.portamento)
            note.setProperty (schema::notePortamento, true, nullptr);

        writeChannel (note, schema::noteChannel, values.channel, nullptr);
        return note;
    }

    // A new value's element: its kind, its number where its kind has one, its beat and value, and its channel if it isn't the
    // first, in that order.
    juce::ValueTree makeControl (const ControlValues& values)
    {
        juce::ValueTree control (schema::CONTROL);
        control.setProperty (schema::controlKind, getSchemaKind (values.kind), nullptr);

        if (hasNumber (values.kind))
            control.setProperty (schema::controlNumber, values.number, nullptr);

        control.setProperty (schema::controlBeat, values.beat, nullptr);
        control.setProperty (schema::controlValue, values.value, nullptr);
        writeChannel (control, schema::controlChannel, values.channel, nullptr);
        return control;
    }
} // namespace

NoteValues readNoteValues (const juce::ValueTree& note)
{
    return { NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) note[schema::noteID])),
             (int) readDescribed (note, schema::notePitch),
             BeatPosition::fromBeats (readDescribed (note, schema::noteStart)),
             BeatDuration::fromBeats (readDescribed (note, schema::noteLength)),
             (int) readDescribed (note, schema::noteVelocity),
             (bool) note[schema::noteMute],
             (bool) note[schema::noteSlide],
             (bool) note[schema::notePortamento],
             readChannel (note, schema::noteChannel) };
}

namespace
{
    // NOTE and CONTROL elements read into a clip's value, each kind in its order; anything else left out.
    template <typename Elements>
    ClipNotes readEvents (const Elements& elements)
    {
        auto notes = immer::flex_vector<NoteValues>().transient();
        auto places = immer::map<std::uint64_t, std::uint32_t>().transient();
        auto controls = immer::flex_vector<ControlValues>().transient();

        for (const auto& child : elements)
        {
            if (child.hasType (schema::NOTE))
            {
                const auto note = readNoteValues (child);
                places.set (note.id.getRaw(), (std::uint32_t) notes.size());
                notes.push_back (note);
            }
            else if (child.hasType (schema::CONTROL))
            {
                if (const auto control = readControlValues (child))
                    controls.push_back (*control);
            }
        }

        return { notes.persistent(), places.persistent(), controls.persistent() };
    }
} // namespace

ClipNotes readClipNotes (const juce::ValueTree& clipState)
{
    return readEvents (clipState);
}

ClipNotes readClipNotes (const std::vector<juce::ValueTree>& events)
{
    return readEvents (events);
}

void writeClipNotes (const ClipNotes& value, juce::ValueTree& clip)
{
    immer::for_each (value.notes, [&clip] (const NoteValues& note) { clip.appendChild (makeNote (note), nullptr); });
    immer::for_each (value.controls, [&clip] (const ControlValues& control) { clip.appendChild (makeControl (control), nullptr); });
}

bool setNoteProperty (NoteValues& note, const juce::Identifier& property, const juce::var& value)
{
    if (property == schema::noteID)
        note.id = NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) value));
    else if (property == schema::notePitch)
        note.pitch = (int) readDescribed (value, schema::notePitch);
    else if (property == schema::noteStart)
        note.start = BeatPosition::fromBeats (readDescribed (value, schema::noteStart));
    else if (property == schema::noteLength)
        note.length = BeatDuration::fromBeats (readDescribed (value, schema::noteLength));
    else if (property == schema::noteVelocity)
        note.velocity = (int) readDescribed (value, schema::noteVelocity);
    else if (property == schema::noteMute)
        note.muted = (bool) value;
    else if (property == schema::noteSlide)
        note.slide = (bool) value;
    else if (property == schema::notePortamento)
        note.portamento = (bool) value;
    else if (property == schema::noteChannel)
        note.channel = readChannelNumber (value);
    else
        return false;

    return true;
}

void giveClipEvents (const juce::ValueTree& from, juce::ValueTree to, juce::UndoManager* undo)
{
    const auto eventsOf = [] (const juce::ValueTree& clip)
    {
        std::vector<juce::ValueTree> events;

        for (const auto& child : clip)
            if (child.hasType (schema::NOTE) || child.hasType (schema::CONTROL))
                events.push_back (child);

        return events;
    };

    const auto theirs = eventsOf (from);
    const auto mine = eventsOf (to);

    // The same kinds of event in the same order: each that differs takes the other's properties, in its place.
    if (theirs.size() == mine.size()
        && std::equal (theirs.begin(), theirs.end(), mine.begin(),
                       [] (const juce::ValueTree& a, const juce::ValueTree& b) { return a.getType() == b.getType(); }))
    {
        for (size_t i = 0; i < theirs.size(); ++i)
            if (! theirs[i].isEquivalentTo (mine[i]))
                juce::ValueTree (mine[i]).copyPropertiesAndChildrenFrom (theirs[i], undo);

        return;
    }

    // Otherwise all of them, copies, where its first event was (or after all it holds).
    auto place = mine.empty() ? to.getNumChildren() : to.indexOf (mine.front());

    for (const auto& event : mine)
        to.removeChild (event, undo);

    place = juce::jmin (place, to.getNumChildren());

    for (const auto& event : theirs)
        to.addChild (event.createCopy(), place++, undo);
}
} // namespace phazite
