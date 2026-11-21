// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongValues.h"
#include "internal/Schema.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>

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
int readChannel (const juce::ValueTree& element, const juce::Identifier& property)
{
    return keepChannel ((int) element.getProperty (property, 1));
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

        return ControlValues { *kind, (int) control[schema::controlNumber], (double) control[schema::controlBeat],
                               (int) control[schema::controlValue], readChannel (control, schema::controlChannel) };
    }

    const juce::Identifier& getFlagName (int flag) noexcept
    {
        return flag == 0 ? schema::noteMute : flag == 1 ? schema::noteSlide : schema::notePortamento;
    }

    void writeFlag (juce::ValueTree& note, int flag, bool on, juce::UndoManager* undo)
    {
        if (on)
            note.setProperty (getFlagName (flag), true, undo);
        else
            note.removeProperty (getFlagName (flag), undo);
    }

    // A note's element, read as `read`, given the values `next`: only the properties whose values differ are set (or, for a
    // flag turned off or the first channel, removed), so that the others stay as the element holds them: a property that is
    // absent stays absent while its value is the one it reads as, and one written as text (a file's) stays text, though JUCE
    // would see the number it reads as a value of its own and set it again.
    void writeNote (juce::ValueTree& note, const NoteValues& read, const NoteValues& next, juce::UndoManager* undo)
    {
        if (next.id != read.id)
            note.setProperty (schema::noteID, (juce::int64) next.id.getRaw(), undo);

        if (next.pitch != read.pitch)
            note.setProperty (schema::notePitch, next.pitch, undo);

        if (next.start != read.start)
            note.setProperty (schema::noteStart, next.start.inBeats(), undo);

        if (next.length != read.length)
            note.setProperty (schema::noteLength, next.length.inBeats(), undo);

        if (next.velocity != read.velocity)
            note.setProperty (schema::noteVelocity, next.velocity, undo);

        if (next.muted != read.muted)
            writeFlag (note, 0, next.muted, undo);

        if (next.slide != read.slide)
            writeFlag (note, 1, next.slide, undo);

        if (next.portamento != read.portamento)
            writeFlag (note, 2, next.portamento, undo);

        if (next.channel != read.channel)
            writeChannel (note, schema::noteChannel, next.channel, undo);
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

        for (const auto& [flag, on] : { std::pair { 0, values.muted }, std::pair { 1, values.slide }, std::pair { 2, values.portamento } })
            if (on)
                writeFlag (note, flag, true, nullptr);

        writeChannel (note, schema::noteChannel, values.channel, nullptr);
        return note;
    }

    // A controller's value's element, read as `read`, given the values `next`, as writeNote() gives a note's: only what differs.
    // Its number is written only where its kind has one.
    void writeControl (juce::ValueTree& control, const ControlValues& read, const ControlValues& next, juce::UndoManager* undo)
    {
        if (next.kind != read.kind)
            control.setProperty (schema::controlKind, getSchemaKind (next.kind), undo);

        if (! hasNumber (next.kind))
        {
            if (hasNumber (read.kind) || next.number != read.number)
                control.removeProperty (schema::controlNumber, undo);
        }
        else if (next.number != read.number)
        {
            control.setProperty (schema::controlNumber, next.number, undo);
        }

        if (! juce::exactlyEqual (next.beat, read.beat))
            control.setProperty (schema::controlBeat, next.beat, undo);

        if (next.value != read.value)
            control.setProperty (schema::controlValue, next.value, undo);

        if (next.channel != read.channel)
            writeChannel (control, schema::controlChannel, next.channel, undo);
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

    // How many of a controller's value's properties another's would set.
    int countDifferences (const ControlValues& a, const ControlValues& b)
    {
        return (a.kind != b.kind ? 1 : 0) + (a.number != b.number ? 1 : 0) + (juce::exactlyEqual (a.beat, b.beat) ? 0 : 1)
               + (a.value != b.value ? 1 : 0) + (a.channel != b.channel ? 1 : 0);
    }

    // Of the notes of the next value that the clip held as it was read, those that keep their elements: as many as can, in
    // the order of the value and of the clip both (the longest run of them whose places as read go up). The others are
    // written anew, where the value puts them.
    std::vector<bool> findNotesKept (const std::vector<int>& readPlaces)
    {
        // readPlaces: for each note of the next value, its place in the value read, or -1 for a note that is new.
        std::vector<bool> kept (readPlaces.size(), false);
        auto increasing = true;
        auto last = -1;

        for (const auto place : readPlaces)
        {
            if (place < 0)
                continue;

            increasing = increasing && place > last;
            last = place;
        }

        if (increasing)
        {
            for (size_t i = 0; i < readPlaces.size(); ++i)
                kept[i] = readPlaces[i] >= 0;

            return kept;
        }

        // The longest increasing run, by patience: tails[k] is the note (by its index in the value) that ends the best run of
        // k + 1 found so far.
        std::vector<size_t> tails, before (readPlaces.size(), readPlaces.size());

        for (size_t i = 0; i < readPlaces.size(); ++i)
        {
            if (readPlaces[i] < 0)
                continue;

            const auto at = std::lower_bound (tails.begin(), tails.end(), readPlaces[i],
                                              [&readPlaces] (size_t tail, int place) { return readPlaces[tail] < place; });

            if (at != tails.begin())
                before[i] = *(at - 1);

            if (at == tails.end())
                tails.push_back (i);
            else
                *at = i;
        }

        for (auto i = tails.empty() ? readPlaces.size() : tails.back(); i < readPlaces.size(); i = before[i])
            kept[i] = true;

        return kept;
    }
} // namespace

NoteValues readNoteValues (const juce::ValueTree& note)
{
    return { NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) note[schema::noteID])),
             (int) note[schema::notePitch],
             BeatPosition::fromBeats ((double) note[schema::noteStart]),
             BeatDuration::fromBeats ((double) note[schema::noteLength]),
             (int) note[schema::noteVelocity],
             (bool) note[schema::noteMute],
             (bool) note[schema::noteSlide],
             (bool) note[schema::notePortamento],
             readChannel (note, schema::noteChannel) };
}

ClipNotes readClipNotes (const juce::ValueTree& clipState, ClipPlaces& where)
{
    auto notes = immer::flex_vector<NoteValues>().transient();
    auto places = immer::map<std::uint64_t, std::uint32_t>().transient();
    auto controls = immer::flex_vector<ControlValues>().transient();
    where.notes.clear();
    where.controls.clear();
    auto index = 0;

    for (const auto& child : clipState)
    {
        if (child.hasType (schema::NOTE))
        {
            const auto note = readNoteValues (child);
            places.set (note.id.getRaw(), (std::uint32_t) notes.size());
            notes.push_back (note);
            where.notes.push_back (index);
        }
        else if (child.hasType (schema::CONTROL))
        {
            if (const auto control = readControlValues (child))
            {
                controls.push_back (*control);
                where.controls.push_back (index);
            }
        }

        ++index;
    }

    return { notes.persistent(), places.persistent(), controls.persistent() };
}

ClipNotes readClipNotes (const juce::ValueTree& clipState)
{
    ClipPlaces places;
    return readClipNotes (clipState, places);
}

void writeClipNotes (const ClipNotes& read, const ClipPlaces& where, const ClipNotes& next, juce::ValueTree& clip, juce::UndoManager* undo)
{
    jassert (where.notes.size() == read.notes.size() && where.controls.size() == read.controls.size());

    std::vector<int> removed;                                      // children, by their indices as read
    std::vector<std::pair<juce::ValueTree, NoteValues>> putBefore; // new notes that go before a note kept: its element, and the note
    std::vector<NoteValues> notesAppended;
    std::vector<ControlValues> controlsAppended;

    // First what changes in place, while the children are where they were read: a note kept (by its ID, where the value's
    // order and the clip's agree) is given its values, if they changed; the others are written anew where the value puts
    // them, and a note of the clip's that is no longer kept removed.
    if (read.notes.identity() != next.notes.identity())
    {
        if (read.places.identity() == next.places.identity() && read.notes.size() == next.notes.size())
        {
            size_t i = 0;
            auto a = read.notes.begin();

            for (auto b = next.notes.begin(); b != next.notes.end(); ++a, ++b, ++i)
            {
                if (! (*a == *b))
                {
                    auto element = clip.getChild (where.notes[i]);
                    jassert (element.hasType (schema::NOTE));
                    writeNote (element, *a, *b, undo);
                }
            }
        }
        else
        {
            std::vector<int> readPlaces;
            readPlaces.reserve (next.notes.size());

            immer::for_each (next.notes,
                             [&] (const NoteValues& note)
                             {
                                 const auto* place = read.places.find (note.id.getRaw());
                                 readPlaces.push_back (place != nullptr ? (int) *place : -1);
                             });

            const auto kept = findNotesKept (readPlaces);
            std::vector<bool> keeps (read.notes.size(), false);
            std::vector<NoteValues> waiting; // new notes that wait for the next note kept
            size_t i = 0;

            for (auto note = next.notes.begin(); note != next.notes.end(); ++note, ++i)
            {
                if (! kept[i])
                {
                    waiting.push_back (*note);
                    continue;
                }

                const auto place = (size_t) readPlaces[i];
                keeps[place] = true;
                auto element = clip.getChild (where.notes[place]);
                jassert (element.hasType (schema::NOTE));

                if (const auto& wasRead = read.notes[place]; ! (wasRead == *note))
                    writeNote (element, wasRead, *note, undo);

                for (const auto& each : waiting)
                    putBefore.emplace_back (element, each);

                waiting.clear();
            }

            notesAppended = std::move (waiting);

            for (size_t place = 0; place < keeps.size(); ++place)
                if (! keeps[place])
                    removed.push_back (where.notes[place]);
        }
    }

    // The controllers' values, which have no IDs: in their places (each that differs given the other's properties, those
    // past the shorter removed or added), or each that is the same kept, the others removed and those left added after them,
    // whichever sets fewer properties and children. A value removed and another added (a step of a draw in a lane) is so;
    // every value's beat changed (a tempo change that keeps the patterns' speed) is so in place.
    if (read.controls.identity() != next.controls.identity())
    {
        const auto numRead = read.controls.size(), numNext = next.controls.size(), numBoth = std::min (numRead, numNext);
        auto costInPlace = (int) (std::max (numRead, numNext) - numBoth);
        auto b = next.controls.begin();
        size_t i = 0;

        for (auto a = read.controls.begin(); i < numBoth; ++a, ++b, ++i)
            costInPlace += countDifferences (*a, *b);

        std::vector<int> notMatched;
        size_t matched = 0;
        i = 0;

        for (const auto& control : read.controls)
        {
            if (matched < numNext && next.controls[matched] == control)
                ++matched;
            else
                notMatched.push_back (where.controls[i]);

            ++i;
        }

        if (costInPlace < (int) (notMatched.size() + (numNext - matched)))
        {
            b = next.controls.begin();
            i = 0;

            for (auto a = read.controls.begin(); i < numBoth; ++a, ++b, ++i)
            {
                if (! (*a == *b))
                {
                    auto element = clip.getChild (where.controls[i]);
                    jassert (element.hasType (schema::CONTROL));
                    writeControl (element, *a, *b, undo);
                }
            }

            for (i = numBoth; i < numRead; ++i)
                removed.push_back (where.controls[i]);

            for (i = numBoth; i < numNext; ++i)
                controlsAppended.push_back (next.controls[i]);
        }
        else
        {
            removed.insert (removed.end(), notMatched.begin(), notMatched.end());

            for (i = matched; i < numNext; ++i)
                controlsAppended.push_back (next.controls[i]);
        }
    }

    // Then the children taken out (from the last, so that the indices read stay right), and those that come, the notes
    // before the values, after all the clip holds, as they always have.
    std::sort (removed.begin(), removed.end(), std::greater<>());

    for (const auto index : removed)
        clip.removeChild (index, undo);

    for (const auto& [element, note] : putBefore)
        clip.addChild (makeNote (note), clip.indexOf (element), undo);

    for (const auto& note : notesAppended)
        clip.appendChild (makeNote (note), undo);

    for (const auto& control : controlsAppended)
        clip.appendChild (makeControl (control), undo);
}

void writeClipNotes (const ClipNotes& next, juce::ValueTree& clip, juce::UndoManager* undo)
{
    ClipPlaces where;
    const auto read = readClipNotes (clip, where);
    writeClipNotes (read, where, next, clip, undo);
}

bool setNoteProperty (NoteValues& note, const juce::Identifier& property, const juce::var& value)
{
    if (property == schema::noteID)
        note.id = NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) value));
    else if (property == schema::notePitch)
        note.pitch = (int) value;
    else if (property == schema::noteStart)
        note.start = BeatPosition::fromBeats ((double) value);
    else if (property == schema::noteLength)
        note.length = BeatDuration::fromBeats ((double) value);
    else if (property == schema::noteVelocity)
        note.velocity = (int) value;
    else if (property == schema::noteMute)
        note.muted = (bool) value;
    else if (property == schema::noteSlide)
        note.slide = (bool) value;
    else if (property == schema::notePortamento)
        note.portamento = (bool) value;
    else if (property == schema::noteChannel)
        note.channel = keepChannel (value.isVoid() ? 1 : (int) value);
    else
        return false;

    return true;
}

namespace
{
    // A clip's events (its notes, and its controllers' values of a kind Phazite knows) in its order, each by what names it:
    // a note by its ID, a value by its place among the values.
    struct NamedEvent
    {
        bool note = false;
        std::uint64_t name = 0;

        bool operator== (const NamedEvent&) const = default;
    };

    std::vector<NamedEvent> getNamedEvents (const juce::ValueTree& clip)
    {
        std::vector<NamedEvent> events;
        std::uint64_t controls = 0;

        for (const auto& child : clip)
        {
            if (child.hasType (schema::NOTE))
                events.push_back ({ true, readNoteValues (child).id.getRaw() });
            else if (child.hasType (schema::CONTROL) && readKind (child).has_value())
                events.push_back ({ false, controls++ });
        }

        return events;
    }

    bool isEvent (const juce::ValueTree& child)
    {
        return child.hasType (schema::NOTE) || (child.hasType (schema::CONTROL) && readKind (child).has_value());
    }
} // namespace

void followOrderOf (juce::ValueTree& clip, const juce::ValueTree& other, juce::UndoManager* undo)
{
    const auto wanted = getNamedEvents (other);

    if (getNamedEvents (clip) == wanted)
        return;

    std::map<std::uint64_t, juce::ValueTree> notes;
    std::vector<juce::ValueTree> controls;

    for (const auto& child : clip)
    {
        if (child.hasType (schema::NOTE))
            notes.emplace (readNoteValues (child).id.getRaw(), child);
        else if (isEvent (child))
            controls.push_back (child);
    }

    // In the other's order, each moved to its place: from the first child on, past those that aren't events.
    int place = 0;

    for (const auto& event : wanted)
    {
        while (place < clip.getNumChildren() && ! isEvent (clip.getChild (place)))
            ++place;

        juce::ValueTree element;

        if (event.note)
        {
            if (const auto found = notes.find (event.name); found != notes.end())
                element = found->second;
        }
        else if (event.name < controls.size())
        {
            element = controls[(size_t) event.name];
        }

        if (! element.isValid())
            continue;

        if (const auto index = clip.indexOf (element); index != place)
            clip.moveChild (index, place, undo);

        ++place;
    }
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
