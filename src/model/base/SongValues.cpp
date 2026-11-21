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

ClipNotes readClipNotes (const juce::ValueTree& clipState)
{
    auto notes = immer::flex_vector<NoteValues>().transient();
    auto places = immer::map<std::uint64_t, std::uint32_t>().transient();
    auto controls = immer::flex_vector<ControlValues>().transient();

    for (const auto& child : clipState)
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
