// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongValues.h"
#include "Song.h"
#include "internal/Schema.h"
#include "internal/SongIndex.h"

#include <algorithm>
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
namespace
{
    std::uint64_t getClipIDOf (const juce::ValueTree& clip)
    {
        return (std::uint64_t) (juce::int64) clip[schema::clipID];
    }

    bool isMidiClip (const juce::ValueTree& element)
    {
        return element.hasType (schema::CLIP) && element[schema::clipKind].toString() != schema::clipIsAudio;
    }

    // A clip's NOTE and CONTROL children: what its value takes the place of in the tree of a song that holds its values.
    bool isEventElement (const juce::ValueTree& child)
    {
        return child.hasType (schema::NOTE) || child.hasType (schema::CONTROL);
    }

    // The clips an element added to the song or taken out of it holds: itself, if a clip; a track's, a folder's tracks', the
    // track list's. Only tracks hold clips (Schema.h), so nothing else is looked into.
    void collectClips (const juce::ValueTree& element, std::vector<juce::ValueTree>& clips)
    {
        if (element.hasType (schema::CLIP))
        {
            clips.push_back (element);
            return;
        }

        if (element.hasType (schema::TRACK) || element.hasType (schema::TRACKS))
            for (const auto& child : element)
                collectClips (child, clips);
    }

    // Two entries of the area alike without a look inside: one clip's, the same value (ValueArea.h).
    struct SameEntry
    {
        bool operator() (const std::pair<std::uint64_t, ClipNotes>& a, const std::pair<std::uint64_t, ClipNotes>& b) const noexcept
        {
            return a.first == b.first && a.second.isSameAs (b.second);
        }
    };
} // namespace

//==============================================================================
// What the area's listeners are told: the clips whose values changed, found by comparing the two maps, by identity clip by
// clip (immer's diff, which skips what the two share, as a change leaves most of the map shared).
struct SongValues::Teller final : ValueArea<NotesByClip>::Listener
{
    juce::ListenerList<NotesListener> listeners;

    void valueAreaChanged (const NotesByClip& before, const NotesByClip& after) override
    {
        if (listeners.isEmpty())
            return;

        std::vector<ClipID> changed;
        const auto note = [&changed] (const std::pair<std::uint64_t, ClipNotes>& entry)
        { changed.push_back (ClipID::fromRaw (entry.first)); };

        before.clips.impl().template diff<SameEntry> (
            after.clips.impl(),
            immer::make_differ (note, note,
                                [&changed] (const std::pair<std::uint64_t, ClipNotes>&, const std::pair<std::uint64_t, ClipNotes>& entry)
                                { changed.push_back (ClipID::fromRaw (entry.first)); }));

        if (changed.empty())
            return;

        std::sort (changed.begin(), changed.end());
        listeners.call ([&changed] (NotesListener& listener) { listener.clipNotesChanged (changed); });
    }
};

//==============================================================================
// What keeps the area and the tree together for every change of the tree that isn't the notes' own (SongValues.h). It changes
// the area and the tree through the song's undo manager from inside the tree's callbacks, which run inside the perform() of
// the tree's own action: JUCE refuses that only while it undoes or redoes (when the keeper does nothing), and adds the inner
// actions to the step before the outer one, so that Undo takes the tree's change back before the area's. JUCE workaround W62
// (docs/juce-workarounds.md).
struct SongValues::Keeper final : juce::ValueTree::Listener
{
    Keeper (SongValues& v, Song& s)
        : values (v),
          song (s),
          state (s.getState())
    {
        state.addListener (this);
    }

    ~Keeper() override { state.removeListener (this); }

    SongValues& values;
    Song& song;
    juce::ValueTree state;

    // The clips taken out of the song, each element with its value as it went, by the clip's ID: given back if the same
    // element comes back (a move), let go of once nothing else holds the element (the history let go of the step that could
    // bring it back), or it is back in the song by an undo, which brought its value back itself.
    struct Remembered
    {
        juce::ValueTree clip;
        ClipNotes value;
    };

    std::multimap<std::uint64_t, Remembered> remembered;
    size_t sweepAt = 64;

    bool isUndoingOrRedoing() const { return song.getUndoManager().isPerformingUndoRedo(); }

    std::multimap<std::uint64_t, Remembered>::iterator findRemembered (const juce::ValueTree& clip)
    {
        for (auto [found, end] = remembered.equal_range (getClipIDOf (clip)); found != end; ++found)
            if (found->second.clip == clip)
                return found;

        return remembered.end();
    }

    void sweep()
    {
        if (remembered.size() < sweepAt)
            return;

        for (auto it = remembered.begin(); it != remembered.end();)
        {
            if (it->second.clip.getReferenceCount() <= 1 || it->second.clip.isAChildOf (state))
                it = remembered.erase (it);
            else
                ++it;
        }

        sweepAt = std::max<size_t> (64, remembered.size() * 2);
    }

    // A clip's value given to it and to the clips of its alias group in the song: the group's own, where another of its clips
    // holds one alike, so that the group holds ONE value.
    void give (NotesByClip& next, const juce::ValueTree& clip, ClipNotes value)
    {
        const auto members = song.getIndex().findLinked (clip);

        for (const auto& member : members)
        {
            if (member == clip)
                continue;

            if (const auto* held = next.find (ClipID::fromRaw (getClipIDOf (member))))
            {
                if (*held == value)
                    value = *held;

                break;
            }
        }

        for (const auto& member : members)
            if (isMidiClip (member))
                next.clips = next.clips.set (getClipIDOf (member), value);
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override
    {
        if (isUndoingOrRedoing())
            return;

        std::vector<juce::ValueTree> clips;
        collectClips (child, clips);

        if (clips.empty())
            return;

        auto next = values.notes.get();
        std::vector<juce::ValueTree> taken;

        for (const auto& clip : clips)
        {
            // The very element taken out (a move): its value back.
            if (const auto found = findRemembered (clip); found != remembered.end())
            {
                next.clips = next.clips.set (getClipIDOf (clip), found->second.value);
                remembered.erase (found);
                continue;
            }

            auto hasEvents = false;

            for (const auto& each : clip)
            {
                if (isEventElement (each))
                {
                    hasEvents = true;
                    taken.push_back (each);
                }
            }

            // Without them: the value a copy gave it first, or an empty one. An audio clip holds none.
            if (! hasEvents && (! isMidiClip (clip) || next.find (ClipID::fromRaw (getClipIDOf (clip))) != nullptr))
                continue;

            give (next, clip, hasEvents ? readClipNotes (clip) : ClipNotes());
        }

        values.setAll (song, next);
        auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

        for (auto& event : taken)
            event.getParent().removeChild (event, undo);

        sweep();
    }

    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override
    {
        if (isUndoingOrRedoing())
            return;

        std::vector<juce::ValueTree> clips;
        collectClips (child, clips);

        if (clips.empty())
            return;

        auto next = values.notes.get();

        for (const auto& clip : clips)
        {
            const auto id = getClipIDOf (clip);

            if (const auto* held = next.find (ClipID::fromRaw (id)))
            {
                if (const auto found = findRemembered (clip); found != remembered.end())
                    found->second.value = *held;
                else
                    remembered.emplace (id, Remembered { clip, *held });

                next.clips = next.clips.erase (id);
            }
        }

        values.setAll (song, next);
        sweep();
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        // Nothing in Phazite gives a clip of the song another ID (a clip is given its ID before it joins the song), which would
        // leave its value under its old one: the check of the song's values finds that (tests/SongCheck.h).
        if (! tree.hasType (schema::CLIP) || isUndoingOrRedoing())
            return;

        // A clip that joins an alias group (Clips.h: copyClips(), a version applied) takes the group's value; one that leaves
        // it (Make Unique, the last of a group) keeps a copy of it, the value it holds.
        if (property != schema::clipLink || tree[schema::clipLink].toString().isEmpty() || ! isMidiClip (tree))
            return;

        const auto id = ClipID::fromRaw (getClipIDOf (tree));

        for (const auto& member : song.getIndex().findLinked (tree))
        {
            if (member == tree)
                continue;

            if (const auto* held = values.notes.get().find (ClipID::fromRaw (getClipIDOf (member))))
            {
                if (const auto* own = values.findClipNotes (id); own == nullptr || ! own->isSameAs (*held))
                {
                    auto next = values.notes.get();
                    next.clips = next.clips.set (id.getRaw(), *held);
                    values.setAll (song, next);
                }

                return;
            }
        }
    }
};

//==============================================================================
SongValues::SongValues (Song& song)
    : notes (song.getUndoManager(), {}),
      teller (std::make_unique<Teller>())
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The area's own listener, which tells this one's (a value of the song's, not an element of its tree: what
    // tools/check-architecture.sh's song-listeners counts by its name is no listener of the tree).
    auto& area = notes;
    area.addListener (teller.get());
}

SongValues::~SongValues()
{
    JUCE_ASSERT_MESSAGE_THREAD
    keeper = nullptr;
    auto& area = notes;
    area.removeListener (teller.get());
}

void SongValues::setAll (Song& song, NotesByClip next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        notes.setOutsideHistory (std::move (next));
    else
        notes.set (std::move (next));
}

void SongValues::setClipNotes (Song& song, ClipID clip, ClipNotes value)
{
    jassert (song.getValues() == this);
    auto next = notes.get();
    const auto clipState = song.getIndex().findClip (clip.getRaw());

    if (! clipState.isValid())
    {
        next.clips = next.clips.set (clip.getRaw(), std::move (value)); // a clip not in the song yet: itself alone
    }
    else
    {
        jassert (isMidiClip (clipState));

        for (const auto& member : song.getIndex().findLinked (clipState))
            if (isMidiClip (member))
                next.clips = next.clips.set (getClipIDOf (member), value);
    }

    setAll (song, std::move (next));
}

void SongValues::eraseClipNotes (Song& song, ClipID clip)
{
    if (notes.get().find (clip) == nullptr)
        return;

    auto next = notes.get();
    next.clips = next.clips.erase (clip.getRaw());
    setAll (song, std::move (next));
}

void SongValues::copyClipNotes (Song& song, ClipID from, ClipID to)
{
    if (const auto* value = notes.get().find (from))
    {
        jassert (! song.getIndex().findClip (to.getRaw()).isValid()); // a copy, not in the song yet
        auto next = notes.get();
        next.clips = next.clips.set (to.getRaw(), *value);
        setAll (song, std::move (next));
    }
}

void SongValues::addNotesListener (NotesListener* listener)
{
    JUCE_ASSERT_MESSAGE_THREAD
    teller->listeners.add (listener);
}

void SongValues::removeNotesListener (NotesListener* listener)
{
    JUCE_ASSERT_MESSAGE_THREAD
    teller->listeners.remove (listener);
}

bool SongValues::remembers (const juce::ValueTree& clip) const
{
    return keeper != nullptr && keeper->findRemembered (clip) != keeper->remembered.end();
}

void SongValues::adopt (Song& song)
{
    // Every clip's NOTE and CONTROL children into the area, outside the history, the clips of an alias group sharing the
    // first's value (the loader made them hold the same: Project.cpp, makeConsistent()); then the keeper, from here on.
    std::vector<juce::ValueTree> clips;
    collectClips (song.getState().getChildWithName (schema::TRACKS), clips);
    auto next = notes.get();
    std::map<juce::String, ClipNotes> groups;

    for (auto& clip : clips)
    {
        if (! isMidiClip (clip))
            continue;

        auto value = readClipNotes (clip);

        if (const auto link = clip[schema::clipLink].toString(); link.isNotEmpty())
        {
            if (const auto found = groups.find (link); found != groups.end())
            {
                jassert (found->second == value); // the loader makes an alias group's clips hold the same
                value = found->second;
            }
            else
            {
                groups.emplace (link, value);
            }
        }

        next.clips = next.clips.set (getClipIDOf (clip), value);

        for (int i = clip.getNumChildren(); --i >= 0;)
            if (isEventElement (clip.getChild (i)))
                clip.removeChild (i, nullptr);
    }

    notes.setOutsideHistory (std::move (next));
    keeper = std::make_unique<Keeper> (*this, song);
}

void Song::holdValues()
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (values == nullptr);

    if (values != nullptr)
        return;

    values = std::make_unique<SongValues> (*this);
    values->adopt (*this);
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

namespace
{
    // A number of a note's element as its value holds it: the property's own, kept within what its description gives
    // (model/base/internal/Schema.h), and where the element has none, what the description gives for one that is no number
    // (a velocity of 100, a pitch of 60, a length of a beat). In the tree a note without a velocity stayed without one; a
    // value always has one, which the loader left to this (a file from others: tests/FuzzTests.cpp found one).
    double readDescribed (const juce::var& held, const schema::PropertyDescription& property)
    {
        const auto& range = property.getRange();

        if (held.isVoid())
            return range.has_value() ? range->fallback.value_or (0.0) : 0.0;

        const auto number = (double) held;
        return range.has_value() ? juce::jlimit (range->low, range->high, number) : number;
    }

    double readDescribed (const juce::ValueTree& element, const schema::PropertyDescription& property)
    {
        return readDescribed (element[property], property);
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
        note.channel = keepChannel (value.isVoid() ? 1 : (int) value);
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

//==============================================================================
bool ClipSound::isSameAs (const ClipSound& other) const noexcept
{
    return warps.identity() == other.warps.identity() && file == other.file && juce::exactlyEqual (tempo, other.tempo)
           && juce::exactlyEqual (pitch, other.pitch) && reversed == other.reversed;
}

bool ClipSound::operator== (const ClipSound& other) const
{
    return file == other.file && juce::exactlyEqual (tempo, other.tempo) && juce::exactlyEqual (pitch, other.pitch)
           && reversed == other.reversed && (warps.identity() == other.warps.identity() || warps == other.warps);
}

bool ClipValue::operator== (const ClipValue& other) const
{
    // An audio clip's length is none of its value: it has none of its own (its range follows from its part).
    return id == other.id && kind == other.kind && track == other.track && name == other.name && start == other.start
           && (isAudio() || length == other.length) && offset == other.offset && muted == other.muted && link == other.link
           && sound == other.sound && juce::exactlyEqual (fileStart, other.fileStart) && juce::exactlyEqual (fileEnd, other.fileEnd)
           && juce::exactlyEqual (gain, other.gain) && fadeIn == other.fadeIn && fadeOut == other.fadeOut && takes == other.takes
           && take == other.take && juce::exactlyEqual (takeLength, other.takeLength);
}

//==============================================================================
namespace
{
    using ClipList = immer::flex_vector<ClipID>;
    using ClipLists = immer::map<std::uint64_t, ClipList>;

    // A list without a clip (the same list for one it hasn't); and the lists without it under a key, the key gone with the
    // last clip.
    ClipList without (const ClipList& list, ClipID clip)
    {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == clip)
                return list.erase (i);

        return list;
    }

    ClipLists withoutIn (const ClipLists& lists, std::uint64_t key, ClipID clip)
    {
        if (key == 0)
            return lists;

        if (const auto* list = lists.find (key))
        {
            const auto rest = without (*list, clip);
            return rest.empty() ? lists.erase (key) : lists.set (key, rest);
        }

        return lists;
    }

    // The lists with a clip under a key, at a place (or after the others).
    ClipLists withIn (const ClipLists& lists, std::uint64_t key, ClipID clip, int index)
    {
        if (key == 0)
            return lists;

        const auto* found = lists.find (key);
        auto list = found != nullptr ? *found : ClipList();
        const auto place = index >= 0 ? std::min ((size_t) index, list.size()) : list.size();
        return lists.set (key, list.insert (place, clip));
    }
} // namespace

SongClips withClip (const SongClips& clips, ClipValue clip, int index)
{
    jassert (clip.id.isValid());
    auto next = clips;
    const auto* old = clips.find (clip.id);

    if (old == nullptr || old->track != clip.track)
    {
        if (old != nullptr)
            next.tracks = withoutIn (next.tracks, old->track.getRaw(), clip.id);

        next.tracks = withIn (next.tracks, clip.track.getRaw(), clip.id, index);
    }

    if (old == nullptr || old->link != clip.link)
    {
        if (old != nullptr)
            next.groups = withoutIn (next.groups, old->link, clip.id);

        next.groups = withIn (next.groups, clip.link, clip.id, -1);
    }

    const auto key = clip.id.getRaw(); // read before the value is moved: a call's arguments come in no set order
    next.clips = next.clips.set (key, std::move (clip));
    return next;
}

SongClips withoutClip (const SongClips& clips, ClipID id)
{
    const auto* old = clips.find (id);

    if (old == nullptr)
        return clips;

    auto next = clips;
    next.tracks = withoutIn (next.tracks, old->track.getRaw(), id);
    next.groups = withoutIn (next.groups, old->link, id);
    next.clips = next.clips.erase (id.getRaw());
    return next;
}

SongClips withTrackOrder (const SongClips& clips, TrackID track, immer::flex_vector<ClipID> order)
{
    const auto* list = clips.tracks.find (track.getRaw());
    const auto now = list != nullptr ? *list : ClipList();
    std::vector<ClipID> held (now.begin(), now.end()), given (order.begin(), order.end());
    std::sort (held.begin(), held.end());
    std::sort (given.begin(), given.end());

    if (held != given)
    {
        jassertfalse; // the same clips, in another order
        return clips;
    }

    if (order.empty())
        return clips;

    auto next = clips;
    next.tracks = next.tracks.set (track.getRaw(), std::move (order));
    return next;
}

bool holdsItsLists (const SongClips& clips)
{
    // Each list's clips: of its track or group, each once in it.
    const auto listsHold = [&clips] (const ClipLists& lists, bool byTrack, size_t& count)
    {
        for (const auto& [key, list] : lists)
        {
            if (list.empty())
                return false;

            std::vector<ClipID> seen (list.begin(), list.end());
            std::sort (seen.begin(), seen.end());

            if (std::adjacent_find (seen.begin(), seen.end()) != seen.end())
                return false;

            for (const auto& id : seen)
            {
                const auto* clip = clips.find (id);

                if (clip == nullptr || (byTrack ? clip->track.getRaw() : clip->link) != key)
                    return false;
            }

            count += list.size();
        }

        return true;
    };

    size_t onTracks = 0, inGroups = 0, expectedOnTracks = 0, expectedInGroups = 0;

    if (! listsHold (clips.tracks, true, onTracks) || ! listsHold (clips.groups, false, inGroups))
        return false;

    for (const auto& [id, clip] : clips.clips)
    {
        if (id != clip.id.getRaw())
            return false;

        expectedOnTracks += clip.track.isValid() ? size_t { 1 } : size_t { 0 };
        expectedInGroups += clip.link != 0 ? size_t { 1 } : size_t { 0 };
    }

    return onTracks == expectedOnTracks && inGroups == expectedInGroups;
}

bool hasGroupsOfOne (const SongClips& clips)
{
    return std::any_of (clips.groups.begin(), clips.groups.end(), [] (const auto& group) { return group.second.size() == 1; });
}

//==============================================================================
namespace
{
    // One property of a clip's element read into its value, as the clips' code reads it (a void value: as if the element
    // hadn't it, which reads as 0, false or empty, but the take, 1): whether it is a clip's property.
    bool readInto (ClipValue& value, const juce::Identifier& property, const juce::var& held)
    {
        const auto bend = [&held] { return juce::jlimit (-1.0f, 1.0f, (float) held); };

        if (property == schema::clipID)
            value.id = ClipID::fromRaw ((std::uint64_t) (juce::int64) held);
        else if (property == schema::clipKind)
            value.kind = held.toString() == schema::clipIsAudio ? ClipValue::Kind::audio : ClipValue::Kind::midi;
        else if (property == schema::clipName)
            value.name = held.toString();
        else if (property == schema::clipStart)
            value.start = BeatPosition::fromBeats ((double) held);
        else if (property == schema::clipLength)
            value.length = BeatDuration::fromBeats ((double) held);
        else if (property == schema::clipOffset)
            value.offset = BeatDuration::fromBeats ((double) held);
        else if (property == schema::clipMute)
            value.muted = (bool) held;
        else if (property == schema::clipLink)
            value.link = (std::uint64_t) (juce::int64) held;
        else if (property == schema::clipFile)
            value.sound.file = held.toString();
        else if (property == schema::clipFileStart)
            value.fileStart = (double) held;
        else if (property == schema::clipFileEnd)
            value.fileEnd = (double) held;
        else if (property == schema::clipTempo)
            value.sound.tempo = (double) held;
        else if (property == schema::clipPitch)
            value.sound.pitch = (float) held;
        else if (property == schema::clipReversed)
            value.sound.reversed = (bool) held;
        else if (property == schema::clipGain)
            value.gain = (float) held;
        else if (property == schema::clipFadeIn)
            value.fadeIn.seconds = (double) held;
        else if (property == schema::clipFadeOut)
            value.fadeOut.seconds = (double) held;
        else if (property == schema::clipFadeInShape)
            value.fadeIn.sCurve = held.toString() == schema::fadeIsSCurve;
        else if (property == schema::clipFadeOutShape)
            value.fadeOut.sCurve = held.toString() == schema::fadeIsSCurve;
        else if (property == schema::clipFadeInBend)
            value.fadeIn.bend = bend();
        else if (property == schema::clipFadeOutBend)
            value.fadeOut.bend = bend();
        else if (property == schema::clipTakes)
            value.takes = (int) held;
        else if (property == schema::clipTake)
            value.take = held.isVoid() ? 1 : (int) held;
        else if (property == schema::clipTakeLength)
            value.takeLength = (double) held;
        else
            return false;

        return true;
    }

    // A value as an element without any of its properties reads.
    ClipValue readNothing()
    {
        ClipValue value;
        value.length = {};
        return value;
    }

    // Whether two values of a property read as the same.
    bool readAlike (const juce::Identifier& property, const juce::var& a, const juce::var& b)
    {
        auto first = readNothing(), second = readNothing();
        readInto (first, property, a);
        readInto (second, property, b);
        return first == second;
    }

    // A clip's element written a property at a time, changing only what differs: a property set, unless the element holds
    // it already as it would be read; and one held only while it is set (whileSet), removed where it isn't.
    struct ClipWriter
    {
        juce::ValueTree& clip;
        juce::UndoManager* undo;

        void set (const juce::Identifier& name, const juce::var& written) const
        {
            if (! clip.hasProperty (name) || ! readAlike (name, written, clip[name]))
                clip.setProperty (name, written, undo);
        }

        void setWhile (bool held, const juce::Identifier& name, const juce::var& written) const
        {
            if (held)
                set (name, written);
            else
                clip.removeProperty (name, undo); // which tells no one where the element hasn't it
        }

        // Each part of a fade, or (held given) each that differs from held's.
        void setFade (const ClipFade& fade, const ClipFade* held, const juce::Identifier& seconds, const juce::Identifier& shape,
                      const juce::Identifier& bend) const
        {
            if (held == nullptr || ! juce::exactlyEqual (fade.seconds, held->seconds))
                setWhile (fade.seconds > 0.0, seconds, fade.seconds);

            if (held == nullptr || fade.sCurve != held->sCurve)
                setWhile (fade.sCurve, shape, juce::String (schema::fadeIsSCurve));

            if (held == nullptr || ! juce::exactlyEqual (fade.bend, held->bend))
                setWhile (! juce::exactlyEqual (fade.bend, 0.0f), bend, fade.bend);
        }

        // The markers: each in its place where there are as many, else all of them anew, after the clip's other children.
        void setMarkers (const immer::flex_vector<WarpValue>& warps) const
        {
            std::vector<juce::ValueTree> markers;

            for (const auto& child : clip)
                if (child.hasType (schema::WARP))
                    markers.push_back (child);

            if (markers.size() == warps.size())
            {
                for (size_t i = 0; i < markers.size(); ++i)
                {
                    const auto writeMarker = [&marker = markers[i], this] (const juce::Identifier& name, double written)
                    {
                        if (! marker.hasProperty (name) || ! juce::exactlyEqual ((double) marker[name], written))
                            marker.setProperty (name, written, undo);
                    };

                    writeMarker (schema::warpTime, warps[i].time);
                    writeMarker (schema::warpPlaysAt, warps[i].playsAt);
                }

                return;
            }

            for (auto& marker : markers)
                clip.removeChild (marker, undo);

            for (const auto& warp : warps)
            {
                juce::ValueTree marker (schema::WARP);
                marker.setProperty (schema::warpTime, warp.time, nullptr);
                marker.setProperty (schema::warpPlaysAt, warp.playsAt, nullptr);
                clip.appendChild (marker, undo);
            }
        }
    };
} // namespace

ClipValue readClipValue (const juce::ValueTree& clip)
{
    // Each property the element holds (those it hasn't read as nothing), and its track, where it is.
    auto value = readNothing();

    for (int i = 0; i < clip.getNumProperties(); ++i)
    {
        const auto name = clip.getPropertyName (i);
        readInto (value, name, clip.getProperty (name));
    }

    if (value.isAudio())
        value.length = {}; // an audio clip has no length of its own: its part of its file and its tempo give its range

    if (const auto parent = clip.getParent(); parent.hasType (schema::TRACK))
        value.track = TrackID::fromRaw ((std::uint64_t) (juce::int64) parent[schema::trackID]);

    // An audio clip's markers, as the clips' code reads them; a clip without any shares the one empty vector.
    if (clip.getChildWithName (schema::WARP).isValid())
    {
        auto warps = immer::flex_vector<WarpValue>().transient();

        for (const auto& child : clip)
            if (child.hasType (schema::WARP))
                warps.push_back ({ (double) child[schema::warpTime], (double) child[schema::warpPlaysAt] });

        value.sound.warps = warps.persistent();
    }

    return value;
}

void writeClipElement (const ClipValue& value, juce::ValueTree& clip, juce::UndoManager* undo)
{
    // Each property as the clips' code writes it, for the kind of clip that has it ("made": as the clip's maker writes it,
    // into an element without an ID yet; the properties in this order, its maker's):
    //
    //   property                       kind   written
    //   clipID, clipKind               both   always (the makers)
    //   clipName                       MIDI   while not empty (makeMidiClipState(); renameClips() removes it)
    //                                  audio  made: always, an empty one too (makeAudioClipState()); then while not empty,
    //                                         and an empty name over one removes it (renameClips()), but an empty one held,
    //                                         or none, stays as it is
    //   clipStart                      both   always, at 0 or later (the makers, setClipPlace(), setAudioPart())
    //   clipLength                     MIDI   always, at least shortestClipBeats (makeMidiClipState(), setClipPlace())
    //   clipOffset                     MIDI   while more than 0 (setClipPlace())
    //   clipMute                       both   while on (toggleClipsMuted())
    //   clipLink                       both   while not 0 (copyClips(), makeClipsUnique(), eraseClip()...)
    //   clipFile                       audio  always (makeAudioClipState(), giveSharedSound())
    //   clipFileStart, clipFileEnd     audio  always, the start at 0 or later, the end not before it (setAudioPart())
    //   clipTempo                      audio  while more than 0 (setClipTempo())
    //   clipPitch                      audio  while not 0 (setClipPitch())
    //   clipReversed                   audio  while on (setClipReversed())
    //   clipGain                       audio  while not 0 (setClipGain())
    //   clipFadeIn, clipFadeOut        audio  while more than 0 (setClipFade())
    //   clipFadeInShape, ...OutShape   audio  while an S-curve (setFadeShape())
    //   clipFadeInBend, ...OutBend     audio  while not 0, kept within -1 to 1 (setFadeShape())
    //   clipTakes, clipTake,           audio  while it has takes (AudioRecorder.cpp's placeTake(), playTake())
    //   clipTakeLength
    //   WARP children                  audio  each marker, in the value's order (writeWarpPoints())
    //
    // A property of the other kind's, never written by the clips' code, is left as it is.
    jassert (clip.hasType (schema::CLIP));
    const auto kept = keepWithinClip (value);
    const auto audio = kept.isAudio();
    const auto made = ! clip.hasProperty (schema::clipID);
    const ClipWriter writer { clip, undo };

    writer.set (schema::clipID, (juce::int64) kept.id.getRaw());
    writer.set (schema::clipKind, juce::String (audio ? schema::clipIsAudio : schema::clipIsMidi));

    if (kept.name.isNotEmpty() || (audio && made))
        writer.set (schema::clipName, kept.name);
    else if (! audio || clip[schema::clipName].toString().isNotEmpty())
        clip.removeProperty (schema::clipName, undo);

    writer.set (schema::clipStart, kept.start.inBeats());

    if (! audio)
    {
        writer.set (schema::clipLength, kept.length.inBeats());
        writer.setWhile (kept.offset.inBeats() > 0.0, schema::clipOffset, kept.offset.inBeats());
    }

    writer.setWhile (kept.muted, schema::clipMute, true);
    writer.setWhile (kept.link != 0, schema::clipLink, (juce::int64) kept.link);

    if (! audio)
        return;

    writer.set (schema::clipFile, kept.sound.file);
    writer.set (schema::clipFileStart, kept.fileStart);
    writer.set (schema::clipFileEnd, kept.fileEnd);
    writer.setWhile (kept.sound.tempo > 0.0, schema::clipTempo, kept.sound.tempo);
    writer.setWhile (! juce::exactlyEqual (kept.sound.pitch, 0.0f), schema::clipPitch, kept.sound.pitch);
    writer.setWhile (kept.sound.reversed, schema::clipReversed, true);
    writer.setWhile (! juce::exactlyEqual (kept.gain, 0.0f), schema::clipGain, kept.gain);
    writer.setFade (kept.fadeIn, nullptr, schema::clipFadeIn, schema::clipFadeInShape, schema::clipFadeInBend);
    writer.setFade (kept.fadeOut, nullptr, schema::clipFadeOut, schema::clipFadeOutShape, schema::clipFadeOutBend);
    writer.setWhile (kept.takes > 0, schema::clipTakes, kept.takes);
    writer.setWhile (kept.takes > 0, schema::clipTake, kept.take);
    writer.setWhile (kept.takes > 0, schema::clipTakeLength, kept.takeLength);
    writer.setMarkers (kept.sound.warps);
}

void writeClipElement (const ClipValue& value, const ClipValue& held, juce::ValueTree& clip, juce::UndoManager* undo)
{
    // Each property whose value differs, by its rule in writeClipElement()'s table above (a name, written by renameClips(),
    // set while it isn't empty and else removed), the value's as it is: the edits keep their values themselves.
    jassert (clip.hasType (schema::CLIP));
    const auto audio = value.isAudio();
    const ClipWriter writer { clip, undo };

    if (value.id != held.id)
        writer.set (schema::clipID, (juce::int64) value.id.getRaw());

    if (value.kind != held.kind)
        writer.set (schema::clipKind, juce::String (audio ? schema::clipIsAudio : schema::clipIsMidi));

    if (value.name != held.name)
        writer.setWhile (value.name.isNotEmpty(), schema::clipName, value.name);

    if (value.start != held.start)
        writer.set (schema::clipStart, value.start.inBeats());

    if (! audio)
    {
        if (value.length != held.length)
            writer.set (schema::clipLength, value.length.inBeats());

        if (value.offset != held.offset)
            writer.setWhile (value.offset.inBeats() > 0.0, schema::clipOffset, value.offset.inBeats());
    }

    if (value.muted != held.muted)
        writer.setWhile (value.muted, schema::clipMute, true);

    if (value.link != held.link)
        writer.setWhile (value.link != 0, schema::clipLink, (juce::int64) value.link);

    if (! audio)
        return;

    const auto& sound = value.sound;
    const auto& heldSound = held.sound;

    if (sound.file != heldSound.file)
        writer.set (schema::clipFile, sound.file);

    if (! juce::exactlyEqual (value.fileStart, held.fileStart))
        writer.set (schema::clipFileStart, value.fileStart);

    if (! juce::exactlyEqual (value.fileEnd, held.fileEnd))
        writer.set (schema::clipFileEnd, value.fileEnd);

    if (! juce::exactlyEqual (sound.tempo, heldSound.tempo))
        writer.setWhile (sound.tempo > 0.0, schema::clipTempo, sound.tempo);

    if (! juce::exactlyEqual (sound.pitch, heldSound.pitch))
        writer.setWhile (! juce::exactlyEqual (sound.pitch, 0.0f), schema::clipPitch, sound.pitch);

    if (sound.reversed != heldSound.reversed)
        writer.setWhile (sound.reversed, schema::clipReversed, true);

    if (! juce::exactlyEqual (value.gain, held.gain))
        writer.setWhile (! juce::exactlyEqual (value.gain, 0.0f), schema::clipGain, value.gain);

    writer.setFade (value.fadeIn, &held.fadeIn, schema::clipFadeIn, schema::clipFadeInShape, schema::clipFadeInBend);
    writer.setFade (value.fadeOut, &held.fadeOut, schema::clipFadeOut, schema::clipFadeOutShape, schema::clipFadeOutBend);

    if (value.takes != held.takes)
        writer.setWhile (value.takes > 0, schema::clipTakes, value.takes);

    if (value.take != held.take)
        writer.setWhile (value.takes > 0, schema::clipTake, value.take);

    if (! juce::exactlyEqual (value.takeLength, held.takeLength))
        writer.setWhile (value.takes > 0, schema::clipTakeLength, value.takeLength);

    if (! (sound.warps == heldSound.warps))
        writer.setMarkers (sound.warps);
}

ClipValue keepWithinClip (ClipValue value)
{
    value.start = BeatPosition::fromBeats (std::max (0.0, value.start.inBeats()));

    if (value.isMidi())
        value.length = BeatDuration::fromBeats (std::max (value.length.inBeats(), shortestClipBeats));

    value.fileStart = std::max (0.0, value.fileStart);
    value.fileEnd = std::max (value.fileStart, value.fileEnd);
    value.fadeIn.bend = juce::jlimit (-1.0f, 1.0f, value.fadeIn.bend);
    value.fadeOut.bend = juce::jlimit (-1.0f, 1.0f, value.fadeOut.bend);
    return value;
}

bool setClipProperty (ClipValue& value, const juce::Identifier& property, const juce::var& given)
{
    return readInto (value, property, given);
}
} // namespace phazite
