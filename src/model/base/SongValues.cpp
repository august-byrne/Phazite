// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongValues.h"
#include "Song.h"
#include "internal/Schema.h"
#include "internal/SongText.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
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
    TrackID getTrackIDOf (const juce::ValueTree& track)
    {
        return track.hasType (schema::TRACK) ? TrackID::fromRaw ((std::uint64_t) (juce::int64) track[schema::trackID]) : TrackID();
    }

    // Whether a track's element holds clips: a track's, not a folder's or a return's (Schema.h: trackKind).
    bool holdsClips (const juce::ValueTree& track)
    {
        const auto kind = track[schema::trackKind].toString();
        return track.hasType (schema::TRACK) && kind != schema::trackIsFolder && kind != schema::trackIsReturn;
    }

    // The tracks an element added to the song or taken out of it holds: itself, if a track; a folder's tracks; the track
    // list's. Only tracks hold clips (Schema.h).
    void collectTracks (const juce::ValueTree& element, std::vector<juce::ValueTree>& tracks)
    {
        if (element.hasType (schema::TRACK))
            tracks.push_back (element);

        if (element.hasType (schema::TRACK) || element.hasType (schema::TRACKS))
            for (const auto& child : element)
                if (child.hasType (schema::TRACK))
                    collectTracks (child, tracks);
    }

    // Every CLIP element in an element and what it holds, taken out (its notes with it): what the loader left of a file's in
    // a place no clip of the song's is (in an element of a kind Phazite doesn't know, whose insides it leaves as they are:
    // Project.cpp, makeConsistent()), once the clips directly on its tracks are the areas'. So the song's tree holds none.
    void dropClipElements (juce::ValueTree element)
    {
        for (auto i = element.getNumChildren(); --i >= 0;)
        {
            if (auto child = element.getChild (i); child.hasType (schema::CLIP))
                element.removeChild (i, nullptr);
            else
                dropClipElements (child);
        }
    }

    // Two entries of the notes' area alike without a look inside: one clip's, the same value (ValueArea.h).
    struct SameNotes
    {
        bool operator() (const std::pair<std::uint64_t, ClipNotes>& a, const std::pair<std::uint64_t, ClipNotes>& b) const noexcept
        {
            return a.first == b.first && a.second.isSameAs (b.second);
        }
    };

    // Two entries of the clips' area alike: one clip's, the same value. A clip's value has no identity of its own to compare,
    // and is small: compared (its markers by identity first: ClipSound's ==).
    struct SameClip
    {
        bool operator() (const std::pair<std::uint64_t, ClipValue>& a, const std::pair<std::uint64_t, ClipValue>& b) const
        {
            return a.first == b.first && a.second == b.second;
        }
    };

    struct SameList
    {
        bool operator() (const std::pair<std::uint64_t, immer::flex_vector<ClipID>>& a,
                         const std::pair<std::uint64_t, immer::flex_vector<ClipID>>& b) const noexcept
        {
            return a.first == b.first && a.second.identity() == b.second.identity();
        }
    };

    // The notes of an alias group a clip joins (by its link), where another clip of it holds some: none for a clip in none, or
    // a group none of whose other clips holds notes.
    const ClipNotes* findGroupNotes (const SongClips& clips, const NotesByClip& notes, std::uint64_t link, ClipID except)
    {
        if (link == 0)
            return nullptr;

        if (const auto* members = clips.groups.find (link))
            for (const auto member : *members)
                if (member != except)
                    if (const auto* held = notes.find (member))
                        return held;

        return nullptr;
    }

    // A clip read from its element into the areas' next values: its value, kept within what a clip holds as the clips' own
    // setters keep it (keepWithinClip(): what the file's properties are each within their ranges, but not with each other),
    // on its track, at a place in its list; and a MIDI clip's notes, those of the alias group it joins where another clip of
    // it holds some, else its own. An ID the area holds already is the same clip again, which this one replaces: a move.
    void takeClip (SongClips& clips, NotesByClip& notes, const juce::ValueTree& element, TrackID track, int index)
    {
        auto value = keepWithinClip (readClipValue (element));
        value.track = track;
        const auto id = value.id;

        if (! id.isValid())
            return;

        if (value.isMidi())
        {
            if (const auto* group = findGroupNotes (clips, notes, value.link, id))
                notes.clips = notes.clips.set (id.getRaw(), *group);
            else
                notes.clips = notes.clips.set (id.getRaw(), readClipNotes (element));
        }
        else
        {
            notes.clips = notes.clips.erase (id.getRaw());

            // The sound of the audio alias group it joins, where another clip of it holds one alike: the group holds one.
            if (value.link != 0)
                if (const auto* members = clips.groups.find (value.link))
                    for (const auto member : *members)
                        if (const auto* other = clips.find (member); other != nullptr && member != id && other->isAudio())
                        {
                            if (other->sound == value.sound)
                                value.sound = other->sound;

                            break;
                        }
        }

        clips = withClip (withoutClip (clips, id), std::move (value), index);
    }

    // The song's TEMPOMAP element once what it held is read into the song's map (holdValues() and the keeper's safety net
    // alike): emptied, the place a whole copy writes the map in, its properties and children Phazite doesn't know taken out
    // too (Schema.h, TEMPOMAP).
    void emptyTempoMapElement (juce::ValueTree& map, juce::UndoManager* undo)
    {
        map.removeAllChildren (undo);
        map.removeAllProperties (undo);
    }
} // namespace

//==============================================================================
// What the values' listeners are told of the notes (Listener::clipNotesChanged()): the clips whose notes changed, found by
// comparing the two maps, by identity clip by clip (immer's diff, which skips what the two share, as a change leaves most of
// the map shared).
struct SongValues::NotesTeller final : ValueArea<NotesByClip>::Listener
{
    explicit NotesTeller (juce::ListenerList<SongValues::Listener>& l)
        : listeners (l)
    {
    }

    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const NotesByClip& before, const NotesByClip& after) override
    {
        if (listeners.isEmpty())
            return;

        std::vector<ClipID> changed;
        const auto note = [&changed] (const std::pair<std::uint64_t, ClipNotes>& entry)
        { changed.push_back (ClipID::fromRaw (entry.first)); };

        before.clips.impl().template diff<SameNotes> (
            after.clips.impl(),
            immer::make_differ (note, note,
                                [&changed] (const std::pair<std::uint64_t, ClipNotes>&, const std::pair<std::uint64_t, ClipNotes>& entry)
                                { changed.push_back (ClipID::fromRaw (entry.first)); }));

        if (changed.empty())
            return;

        std::sort (changed.begin(), changed.end());
        listeners.call ([&changed] (SongValues::Listener& listener) { listener.clipNotesChanged (changed); });
    }
};

//==============================================================================
// What the values' listeners are told of the clips (Listener::clipsChanged()): each clip whose value differs between the two
// (immer's diff, which skips what the two share), with its track before and after; and each track whose list holds the same
// clips in another order, from the lists that differ by identity.
struct SongValues::ClipsTeller final : ValueArea<SongClips>::Listener
{
    explicit ClipsTeller (juce::ListenerList<SongValues::Listener>& l)
        : listeners (l)
    {
    }

    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const SongClips& before, const SongClips& after) override
    {
        if (listeners.isEmpty())
            return;

        std::vector<ClipChange> changed;

        before.clips.impl().template diff<SameClip> (
            after.clips.impl(),
            immer::make_differ ([&changed] (const std::pair<std::uint64_t, ClipValue>& added)
                                { changed.push_back ({ ClipID::fromRaw (added.first), {}, added.second.track }); },
                                [&changed] (const std::pair<std::uint64_t, ClipValue>& removed)
                                { changed.push_back ({ ClipID::fromRaw (removed.first), removed.second.track, {} }); },
                                [&changed] (const std::pair<std::uint64_t, ClipValue>& was, const std::pair<std::uint64_t, ClipValue>& is)
                                { changed.push_back ({ ClipID::fromRaw (is.first), was.second.track, is.second.track }); }));

        std::vector<TrackID> reordered;
        const auto ignore = [] (const std::pair<std::uint64_t, immer::flex_vector<ClipID>>&) {};

        before.tracks.impl().template diff<SameList> (
            after.tracks.impl(), immer::make_differ (ignore, ignore,
                                                     [&reordered] (const std::pair<std::uint64_t, immer::flex_vector<ClipID>>& was,
                                                                   const std::pair<std::uint64_t, immer::flex_vector<ClipID>>& is)
                                                     {
                                                         if (was.second.size() != is.second.size())
                                                             return;

                                                         std::vector<ClipID> a (was.second.begin(), was.second.end()),
                                                             b (is.second.begin(), is.second.end());

                                                         if (a == b)
                                                             return;

                                                         std::sort (a.begin(), a.end());
                                                         std::sort (b.begin(), b.end());

                                                         if (a == b)
                                                             reordered.push_back (TrackID::fromRaw (is.first));
                                                     }));

        if (changed.empty() && reordered.empty())
            return;

        std::sort (changed.begin(), changed.end(), [] (const ClipChange& a, const ClipChange& b) { return a.id < b.id; });
        std::sort (reordered.begin(), reordered.end());
        listeners.call ([&changed, &reordered] (SongValues::Listener& listener) { listener.clipsChanged (changed, reordered); });
    }
};

//==============================================================================
// What the values' listeners are told of the tempo map (Listener::tempoMapChanged()): once for each change of its value.
struct SongValues::TempoMapTeller final : ValueArea<TempoMapValue>::Listener
{
    explicit TempoMapTeller (juce::ListenerList<SongValues::Listener>& l)
        : listeners (l)
    {
    }

    juce::ListenerList<SongValues::Listener>& listeners;

    void valueAreaChanged (const TempoMapValue& before, const TempoMapValue& after) override
    {
        if (! before.isSameAs (after))
            listeners.call ([] (SongValues::Listener& listener) { listener.tempoMapChanged(); });
    }
};

//==============================================================================
// What keeps the areas and the tree together for every change of the tree (SongValues.h). It changes the areas and the tree
// through the song's undo manager from inside the tree's callbacks, which run inside the perform() of the tree's own action:
// JUCE refuses that only while it undoes or redoes (when the keeper does nothing), and adds the inner actions to the step
// before the outer one, so that Undo takes the tree's change back before the areas'. JUCE workaround W62
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

    // The tracks taken out of the song, each element with its clips as they went (their values in its order, and their
    // notes): given back if the same element comes back (a move), let go of once nothing else holds the element (the history
    // let go of the step that could bring it back), or it is back in the song by an undo, which brought its clips back itself.
    struct Remembered
    {
        juce::ValueTree track;
        std::vector<ClipValue> clips;
        std::vector<std::pair<ClipID, ClipNotes>> notes;
    };

    std::vector<Remembered> remembered;
    size_t sweepAt = 64;

    bool isUndoingOrRedoing() const { return song.getUndoManager().isPerformingUndoRedo(); }

    std::vector<Remembered>::iterator findRemembered (const juce::ValueTree& track)
    {
        return std::find_if (remembered.begin(), remembered.end(), [&track] (const Remembered& each) { return each.track == track; });
    }

    void sweep()
    {
        if (remembered.size() < sweepAt)
            return;

        std::erase_if (remembered,
                       [this] (const Remembered& each) { return each.track.getReferenceCount() <= 1 || each.track.isAChildOf (state); });
        sweepAt = std::max<size_t> (64, remembered.size() * 2);
    }

    // Whether a child of an element is in the song's track list (a track, a folder's tracks, or the track list itself): only
    // there do tracks hold the song's clips.
    bool isInTrackList (const juce::ValueTree& parent, const juce::ValueTree& child) const
    {
        if (child.hasType (schema::TRACKS))
            return parent == state;

        for (auto element = parent; element.isValid(); element = element.getParent())
            if (element.hasType (schema::TRACKS))
                return element.getParent() == state;

        return false;
    }

    //==============================================================================
    // The tempo map's safety net (SongValues.h): a TEMPOMAP that comes into the song with entries in it has them read in as the
    // song's map, and is emptied as holdValues() empties the song's own (emptyTempoMapElement(): what Phazite doesn't know of
    // it goes too); an entry added to the song's TEMPOMAP by itself is read into the map and taken out again
    // while the song is written outside the history, and refused in a step, whose Undo, taking the tree's change back first,
    // would find it gone (JUCE workaround W62 (docs/juce-workarounds.md)). Either in the mode of the moment.
    void takeTempoMapIn (juce::ValueTree& parent, juce::ValueTree& child)
    {
        const auto isEntry = [] (const juce::ValueTree& element)
        { return element.hasType (schema::TEMPO) || element.hasType (schema::TIMESIGNATURE); };

        auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

        if (child.hasType (schema::TEMPOMAP) && parent == state)
        {
            std::vector<juce::ValueTree> entries;

            for (const auto& element : child)
                if (isEntry (element))
                    entries.push_back (element);

            if (entries.empty())
                return;

            values.tempoMapAdopted += (int) entries.size();
            values.setTempoMap (song, readHeldTempoMap (child));
            emptyTempoMapElement (child, undo);
            return;
        }

        if (! isEntry (child) || ! parent.hasType (schema::TEMPOMAP) || parent.getParent() != state)
            return;

        if (undo != nullptr)
        {
            jassertfalse; // an entry added to the song's tempo map: the tempo map's functions change it (Tempo.h)
            return;
        }

        ++values.tempoMapAdopted;
        values.setTempoMap (song, withEntryTaken (values.tempoMap.get(), child));
        parent.removeChild (child, nullptr);
    }

    //==============================================================================
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (isUndoingOrRedoing())
            return;

        if (isHeardAsTempoMap (parent, child))
            return takeTempoMapIn (parent, child); // nothing of a clip in it

        // A track (or what holds tracks) put in the song's track list: each remembered one's clips back; and every CLIP element
        // on a track that came with its clips' elements, taken in (one on a folder or a return, which holds no clips, only
        // taken out). Not a CLIP element added to a track of the song by itself, which nothing in Phazite adds: the element
        // added can't be taken out again in the step that adds it, as the step's Undo, taking the tree's change back first,
        // would find it gone (JUCE workaround W62 (docs/juce-workarounds.md)).
        if (child.hasType (schema::CLIP) && isInTrackList (parent, child))
        {
            jassertfalse; // a clip's element on a track: the clips' functions add a clip (Clips.h)
            return;
        }

        std::vector<juce::ValueTree> tracks;
        collectTracks (child, tracks);

        if (tracks.empty() || ! isInTrackList (parent, child))
            return;

        auto nextClips = values.clips.get();
        auto nextNotes = values.notes.get();
        std::vector<juce::ValueTree> taken;
        auto changed = false;

        for (const auto& track : tracks)
        {
            if (const auto found = findRemembered (track); found != remembered.end())
            {
                for (const auto& clip : found->clips)
                    nextClips = withClip (withoutClip (nextClips, clip.id), clip);

                for (const auto& [id, value] : found->notes)
                    nextNotes.clips = nextNotes.clips.set (id.getRaw(), value);

                changed = changed || ! found->clips.empty();
                remembered.erase (found);
            }

            const auto id = getTrackIDOf (track);

            for (const auto& element : track)
            {
                if (! element.hasType (schema::CLIP))
                    continue;

                if (holdsClips (track))
                {
                    takeClip (nextClips, nextNotes, element, id, -1);
                    changed = true;
                    ++values.adopted;
                }

                taken.push_back (element);
            }
        }

        if (changed)
        {
            values.setAll (song, std::move (nextClips));
            values.setAll (song, std::move (nextNotes));
        }

        auto* undo = song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();

        for (auto& element : taken)
            element.getParent().removeChild (element, undo);

        sweep();
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        if (isUndoingOrRedoing() || ! isInTrackList (parent, child))
            return;

        // A track (or what holds tracks) taken out of the song's track list: its clips remembered with its element, and erased.
        std::vector<juce::ValueTree> tracks;
        collectTracks (child, tracks);
        auto nextClips = values.clips.get();
        auto nextNotes = values.notes.get();
        auto any = false;

        for (const auto& track : tracks)
        {
            const auto* list = nextClips.tracks.find (getTrackIDOf (track).getRaw());

            if (list == nullptr)
                continue;

            Remembered gone { track, {}, {} };
            const auto order = *list;

            for (const auto id : order)
            {
                if (const auto* clip = nextClips.find (id))
                    gone.clips.push_back (*clip);

                if (const auto* value = nextNotes.find (id))
                    gone.notes.emplace_back (id, *value);

                nextClips = withoutClip (nextClips, id);
                nextNotes.clips = nextNotes.clips.erase (id.getRaw());
            }

            if (const auto found = findRemembered (track); found != remembered.end())
                *found = std::move (gone);
            else
                remembered.push_back (std::move (gone));

            any = true;
        }

        if (! any)
            return;

        values.setAll (song, std::move (nextClips));
        values.setAll (song, std::move (nextNotes));
        sweep();
    }
};

//==============================================================================
SongValues::SongValues (Song& song)
    : clips (song.getUndoManager(), {}),
      notes (song.getUndoManager(), {}),
      tempoMap (song.getUndoManager(), makeDefaultTempoMap()),
      notesTeller (std::make_unique<NotesTeller> (listeners)),
      clipsTeller (std::make_unique<ClipsTeller> (listeners)),
      tempoMapTeller (std::make_unique<TempoMapTeller> (listeners))
{
    JUCE_ASSERT_MESSAGE_THREAD

    // The areas' own listeners, which tell this one's (values of the song's, not elements of its tree: what
    // tools/check-architecture.sh's song-listeners counts by its name is no listener of the tree).
    auto& areaOfClips = clips;
    areaOfClips.addListener (clipsTeller.get());
    auto& areaOfNotes = notes;
    areaOfNotes.addListener (notesTeller.get());
    auto& areaOfTempos = tempoMap;
    areaOfTempos.addListener (tempoMapTeller.get());
}

SongValues::~SongValues()
{
    JUCE_ASSERT_MESSAGE_THREAD
    keeper = nullptr;
    auto& areaOfTempos = tempoMap;
    areaOfTempos.removeListener (tempoMapTeller.get());
    auto& areaOfNotes = notes;
    areaOfNotes.removeListener (notesTeller.get());
    auto& areaOfClips = clips;
    areaOfClips.removeListener (clipsTeller.get());
}

void SongValues::setTempoMap (Song& song, TempoMapValue next)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (song.getValues() == this);
    jassert (! next.tempos.empty() && ! next.signatures.empty()); // at least one of each (TempoMapValue.h)

    if (next.tempos.empty() || next.signatures.empty() || next == tempoMap.get())
        return;

    if (song.isWritingOutsideHistory())
        tempoMap.setOutsideHistory (std::move (next));
    else
        tempoMap.set (std::move (next));
}

void SongValues::setAll (Song& song, SongClips next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        clips.setOutsideHistory (std::move (next));
    else
        clips.set (std::move (next));
}

void SongValues::setAll (Song& song, NotesByClip next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        notes.setOutsideHistory (std::move (next));
    else
        notes.set (std::move (next));
}

void SongValues::setWithoutStep (Song& song, SongClips next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        clips.setOutsideHistory (std::move (next));
    else
        clips.setWithoutStep (std::move (next));
}

void SongValues::setWithoutStep (Song& song, NotesByClip next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        notes.setOutsideHistory (std::move (next));
    else
        notes.setWithoutStep (std::move (next));
}

void SongValues::setClip (Song& song, ClipValue clip, int index, const ClipNotes* given)
{
    jassert (song.getValues() == this);
    jassert (clip.id.isValid());
    const auto id = clip.id;
    const auto* held = clips.get().find (id);

    // The value it holds: no change of the song's. Its sound alike but another value of it, so that an alias group's clips
    // hold one (ClipSound), is given without a step of its own: a "Bring Back" of the version the song is, which gives a
    // group's clips the one sound, would leave a step that changes nothing.
    if (held != nullptr && given == nullptr && *held == clip)
    {
        if (! held->sound.isSameAs (clip.sound))
            setWithoutStep (song, withClip (clips.get(), std::move (clip), index));

        return;
    }

    const auto joins = held == nullptr || held->link != clip.link;
    auto nextNotes = notes.get();

    // Its notes: none for an audio clip; given; the group's it joins; its own, kept; or a new clip's, empty.
    if (clip.isAudio())
        nextNotes.clips = nextNotes.clips.erase (id.getRaw());
    else if (given != nullptr)
        nextNotes.clips = nextNotes.clips.set (id.getRaw(), *given);
    else if (const auto* group = joins ? findGroupNotes (clips.get(), nextNotes, clip.link, id) : nullptr)
        nextNotes.clips = nextNotes.clips.set (id.getRaw(), *group);
    else if (nextNotes.find (id) == nullptr)
        nextNotes.clips = nextNotes.clips.set (id.getRaw(), ClipNotes());

    // On another track, or added: at the place given, else at the end of the track's list.
    setAll (song, withClip (clips.get(), std::move (clip), index));
    setAll (song, std::move (nextNotes));
}

void SongValues::eraseClip (Song& song, ClipID clip)
{
    if (clips.get().find (clip) == nullptr)
        return;

    setAll (song, withoutClip (clips.get(), clip));

    if (notes.get().find (clip) != nullptr)
    {
        auto next = notes.get();
        next.clips = next.clips.erase (clip.getRaw());
        setAll (song, std::move (next));
    }
}

void SongValues::setTrackClipOrder (Song& song, TrackID track, immer::flex_vector<ClipID> order)
{
    setAll (song, withTrackOrder (clips.get(), track, std::move (order)));
}

void SongValues::setClipNotes (Song& song, ClipID clip, ClipNotes value)
{
    jassert (song.getValues() == this);
    const auto* clipValue = clips.get().find (clip);

    if (clipValue == nullptr || ! clipValue->isMidi())
    {
        jassertfalse; // a MIDI clip of the song's alone holds notes (the notes' code asks: Notes.h)
        return;
    }

    // It and its alias group's other MIDI clips, which hold one value.
    std::vector<ClipID> members;

    if (const auto* group = clipValue->link != 0 ? clips.get().groups.find (clipValue->link) : nullptr)
    {
        for (const auto member : *group)
            if (const auto* each = clips.get().find (member); each != nullptr && each->isMidi())
                members.push_back (member);
    }
    else
    {
        members.push_back (clip);
    }

    // The notes each holds already: no change of the song's. Held as one value, nothing; held as values alike, this one,
    // without a step of its own.
    const ClipNotes* first = nullptr;
    auto alike = true, one = true;

    for (const auto member : members)
    {
        const auto* held = notes.get().find (member);
        alike = alike && held != nullptr && *held == value;

        if (! alike)
            break;

        if (first == nullptr)
            first = held;
        else
            one = one && held->isSameAs (*first);
    }

    if (alike && one)
        return;

    auto next = notes.get();

    for (const auto member : members)
        next.clips = next.clips.set (member.getRaw(), value);

    if (alike)
        setWithoutStep (song, std::move (next));
    else
        setAll (song, std::move (next));
}

void SongValues::addListener (Listener* listener) const
{
    JUCE_ASSERT_MESSAGE_THREAD
    listeners.add (listener);
}

void SongValues::removeListener (Listener* listener) const
{
    JUCE_ASSERT_MESSAGE_THREAD
    listeners.remove (listener);
}

bool isHeardAsTempoMap (const juce::ValueTree& element)
{
    for (auto each = element; each.isValid(); each = each.getParent())
        if (each.hasType (schema::TEMPOMAP))
            return each.getParent().hasType (schema::PHAZITE);

    return false;
}

bool isHeardAsTempoMap (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (child.hasType (schema::TEMPOMAP) && parent.hasType (schema::PHAZITE)) || isHeardAsTempoMap (parent);
}

bool SongValues::remembers (const juce::ValueTree& track) const
{
    return keeper != nullptr && keeper->findRemembered (track) != keeper->remembered.end();
}

std::vector<TrackID> SongValues::getRemembered() const
{
    std::vector<TrackID> tracks;

    if (keeper != nullptr)
        for (const auto& each : keeper->remembered)
            tracks.push_back (getTrackIDOf (each.track));

    return tracks;
}

void SongValues::adopt (Song& song)
{
    // Every CLIP of the tree the loader made consistent into both areas, outside the history: each track's in their order,
    // the clips of an alias group sharing the first's notes and, audio clips, its sound (the loader made them hold the same:
    // Project.cpp, makeConsistent()), each kept within what a clip holds (takeClip()); then every CLIP element out of the tree,
    // those anywhere else too, which are no clips of the song's (dropClipElements()); and the keeper, from here on.
    std::vector<juce::ValueTree> tracks;
    collectTracks (song.getState().getChildWithName (schema::TRACKS), tracks);
    auto nextClips = clips.get();
    auto nextNotes = notes.get();

    for (auto& track : tracks)
    {
        const auto id = getTrackIDOf (track);

        for (const auto& element : track)
            if (element.hasType (schema::CLIP) && holdsClips (track))
                takeClip (nextClips, nextNotes, element, id, -1);

        for (int i = track.getNumChildren(); --i >= 0;)
            if (track.getChild (i).hasType (schema::CLIP))
                track.removeChild (i, nullptr);
    }

    dropClipElements (song.getState());
    clips.setOutsideHistory (std::move (nextClips));
    notes.setOutsideHistory (std::move (nextNotes));

    // The tempo map the loader made consistent (and makeSong() made whole: Tempo.h, ensureTempoMap()) into its area, outside
    // the history, each entry as the song reads one (TempoMapValue.h: readHeldTempoMap()); then its element emptied
    // (emptyTempoMapElement()).
    auto state = song.getState();
    auto map = state.getChildWithName (schema::TEMPOMAP);

    if (! map.isValid())
    {
        map = juce::ValueTree (schema::TEMPOMAP);
        state.appendChild (map, nullptr);
    }

    tempoMap.setOutsideHistory (readHeldTempoMap (map));
    emptyTempoMapElement (map, nullptr);

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
namespace
{
    // A number of an element (a note's, a controller's value's, a clip's, a warp marker's) as its value holds it: the
    // property's own, read as the loader reads it (SongText.h: readValueNumber(), so that a text reads as the same number
    // wherever it is read: "1e-321" as that subnormal, never as 0), kept within what its description gives
    // (model/base/internal/Schema.h); and where the element has none, or what it has is no finite number, what the
    // description gives for one that is no number (a velocity of 100, a pitch of 60, a length of a beat), else 0. In the
    // tree a note without a velocity stayed without one; a value always has one, which the loader left to this (a file from
    // others: tests/FuzzTests.cpp found one).
    double readDescribed (const juce::var& held, const schema::PropertyDescription& property)
    {
        const auto& range = property.getRange();
        const auto number = readValueNumber (held);

        if (! number.has_value() || ! std::isfinite (*number))
            return range.has_value() ? range->fallback.value_or (0.0) : 0.0;

        return range.has_value() ? juce::jlimit (range->low, range->high, *number) : *number;
    }

    double readDescribed (const juce::ValueTree& element, const schema::PropertyDescription& property)
    {
        return readDescribed (element[property], property);
    }

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

    if (order.empty() || std::equal (order.begin(), order.end(), now.begin(), now.end()))
        return clips; // the order it holds: the same value

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
    // One property of a clip's element read into its value, as its description reads it (readDescribed(): a void value, as
    // if the element hadn't it, reads as the description's fallback, else as none: 0, false, empty; but the take, 1; and a
    // number outside its range is kept within it): whether it is a clip's property.
    bool readInto (ClipValue& value, const juce::Identifier& property, const juce::var& held)
    {
        if (property == schema::clipID)
            value.id = ClipID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) held));
        else if (property == schema::clipKind)
            value.kind = held.toString() == schema::clipIsAudio ? ClipValue::Kind::audio : ClipValue::Kind::midi;
        else if (property == schema::clipName)
            value.name = held.toString();
        else if (property == schema::clipStart)
            value.start = BeatPosition::fromBeats (readDescribed (held, schema::clipStart));
        else if (property == schema::clipLength)
            value.length = BeatDuration::fromBeats (readDescribed (held, schema::clipLength));
        else if (property == schema::clipOffset)
            value.offset = BeatDuration::fromBeats (readDescribed (held, schema::clipOffset));
        else if (property == schema::clipMute)
            value.muted = (bool) held;
        else if (property == schema::clipLink)
            value.link = (std::uint64_t) std::max<juce::int64> (0, (juce::int64) held);
        else if (property == schema::clipFile)
            value.sound.file = held.toString();
        else if (property == schema::clipFileStart)
            value.fileStart = readDescribed (held, schema::clipFileStart);
        else if (property == schema::clipFileEnd)
            value.fileEnd = readDescribed (held, schema::clipFileEnd);
        else if (property == schema::clipTempo)
            value.sound.tempo = held.isVoid() ? 0.0 : readDescribed (held, schema::clipTempo);
        else if (property == schema::clipPitch)
            value.sound.pitch = (float) readDescribed (held, schema::clipPitch);
        else if (property == schema::clipReversed)
            value.sound.reversed = (bool) held;
        else if (property == schema::clipGain)
            value.gain = (float) readDescribed (held, schema::clipGain);
        else if (property == schema::clipFadeIn)
            value.fadeIn.seconds = readDescribed (held, schema::clipFadeIn);
        else if (property == schema::clipFadeOut)
            value.fadeOut.seconds = readDescribed (held, schema::clipFadeOut);
        else if (property == schema::clipFadeInShape)
            value.fadeIn.sCurve = held.toString() == schema::fadeIsSCurve;
        else if (property == schema::clipFadeOutShape)
            value.fadeOut.sCurve = held.toString() == schema::fadeIsSCurve;
        else if (property == schema::clipFadeInBend)
            value.fadeIn.bend = (float) readDescribed (held, schema::clipFadeInBend);
        else if (property == schema::clipFadeOutBend)
            value.fadeOut.bend = (float) readDescribed (held, schema::clipFadeOutBend);
        else if (property == schema::clipTakes)
            value.takes = held.isVoid() ? 0 : (int) readDescribed (held, schema::clipTakes);
        else if (property == schema::clipTake)
            value.take = held.isVoid() ? 1 : (int) readDescribed (held, schema::clipTake);
        else if (property == schema::clipTakeLength)
            value.takeLength = held.isVoid() ? 0.0 : readDescribed (held, schema::clipTakeLength);
        else
            return false;

        return true;
    }

    // A clip's value with nothing of the other kind of clip's properties (each at its default): a MIDI clip has no sound, part of
    // a file, gain, fades or takes; an audio clip no length or offset of its own.
    ClipValue ofItsKind (ClipValue value)
    {
        if (value.isAudio())
        {
            value.length = {};
            value.offset = {};
            return value;
        }

        const ClipValue none;
        value.sound = none.sound;
        value.fileStart = none.fileStart;
        value.fileEnd = none.fileEnd;
        value.gain = none.gain;
        value.fadeIn = none.fadeIn;
        value.fadeOut = none.fadeOut;
        value.takes = none.takes;
        value.take = none.take;
        value.takeLength = none.takeLength;
        return value;
    }
} // namespace

ClipValue readClipValue (const juce::ValueTree& clip)
{
    // Each property the element holds; those it hasn't, as their descriptions read none (a value's defaults are those: a
    // MIDI clip's length of 4 beats, a start of 0, a take of 1); and its track, where it is.
    ClipValue value;

    for (int i = 0; i < clip.getNumProperties(); ++i)
    {
        const auto name = clip.getPropertyName (i);
        readInto (value, name, clip.getProperty (name));
    }

    // Nothing of the other kind of clip's: an audio clip has no length of its own (its part of its file and its tempo give its
    // range), a MIDI clip no file.
    value = ofItsKind (std::move (value));

    if (const auto parent = clip.getParent(); parent.hasType (schema::TRACK))
        value.track = TrackID::fromRaw ((std::uint64_t) (juce::int64) parent[schema::trackID]);

    // An audio clip's markers; a clip without any shares the one empty vector.
    if (value.isAudio() && clip.getChildWithName (schema::WARP).isValid())
    {
        auto warps = immer::flex_vector<WarpValue>().transient();

        for (const auto& child : clip)
            if (child.hasType (schema::WARP))
                warps.push_back ({ readDescribed (child, schema::warpTime), readDescribed (child, schema::warpPlaysAt) });

        value.sound.warps = warps.persistent();
    }

    return value;
}

juce::ValueTree writeClipElement (const ClipValue& value, const ClipNotes* notes)
{
    // Each property of the clip's kind, only while it is set:
    //
    //   property                       kind   written
    //   clipID, clipKind, clipStart    both   always
    //   clipName                       both   while not empty
    //   clipLength                     MIDI   always
    //   clipOffset                     MIDI   while more than 0
    //   clipMute                       both   while on
    //   clipLink                       both   while not 0
    //   clipFile                       audio  always
    //   clipFileStart, clipFileEnd     audio  always
    //   clipTempo                      audio  while more than 0
    //   clipPitch                      audio  while not 0
    //   clipReversed                   audio  while on
    //   clipGain                       audio  while not 0
    //   clipFadeIn, clipFadeOut        audio  while more than 0
    //   clipFadeInShape, ...OutShape   audio  while an S-curve
    //   clipFadeInBend, ...OutBend     audio  while not 0
    //   clipTakes, clipTake            audio  while it has takes
    //   clipTakeLength                 audio  while it has takes, and is more than 0
    //   WARP children                  audio  each marker, in the value's order
    juce::ValueTree clip (schema::CLIP);
    const auto audio = value.isAudio();
    const auto set = [&clip] (bool held, const juce::Identifier& name, const juce::var& written)
    {
        if (held)
            clip.setProperty (name, written, nullptr);
    };

    clip.setProperty (schema::clipID, (juce::int64) value.id.getRaw(), nullptr);
    clip.setProperty (schema::clipKind, juce::String (audio ? schema::clipIsAudio : schema::clipIsMidi), nullptr);
    set (value.name.isNotEmpty(), schema::clipName, value.name);
    clip.setProperty (schema::clipStart, value.start.inBeats(), nullptr);
    set (! audio, schema::clipLength, value.length.inBeats());
    set (! audio && value.offset.inBeats() > 0.0, schema::clipOffset, value.offset.inBeats());
    set (value.muted, schema::clipMute, true);
    set (value.link != 0, schema::clipLink, (juce::int64) value.link);

    if (audio)
    {
        const auto writeFade =
            [&set] (const ClipFade& fade, const juce::Identifier& seconds, const juce::Identifier& shape, const juce::Identifier& bend)
        {
            set (fade.seconds > 0.0, seconds, fade.seconds);
            set (fade.sCurve, shape, juce::String (schema::fadeIsSCurve));
            set (! juce::exactlyEqual (fade.bend, 0.0f), bend, fade.bend);
        };

        clip.setProperty (schema::clipFile, value.sound.file, nullptr);
        clip.setProperty (schema::clipFileStart, value.fileStart, nullptr);
        clip.setProperty (schema::clipFileEnd, value.fileEnd, nullptr);
        set (value.sound.tempo > 0.0, schema::clipTempo, value.sound.tempo);
        set (! juce::exactlyEqual (value.sound.pitch, 0.0f), schema::clipPitch, value.sound.pitch);
        set (value.sound.reversed, schema::clipReversed, true);
        set (! juce::exactlyEqual (value.gain, 0.0f), schema::clipGain, value.gain);
        writeFade (value.fadeIn, schema::clipFadeIn, schema::clipFadeInShape, schema::clipFadeInBend);
        writeFade (value.fadeOut, schema::clipFadeOut, schema::clipFadeOutShape, schema::clipFadeOutBend);
        set (value.takes > 0, schema::clipTakes, value.takes);
        set (value.takes > 0, schema::clipTake, value.take);
        set (value.takes > 0 && value.takeLength > 0.0, schema::clipTakeLength, value.takeLength);

        for (const auto& warp : value.sound.warps)
        {
            juce::ValueTree marker (schema::WARP);
            marker.setProperty (schema::warpTime, warp.time, nullptr);
            marker.setProperty (schema::warpPlaysAt, warp.playsAt, nullptr);
            clip.appendChild (marker, nullptr);
        }
    }

    if (notes != nullptr && ! audio)
        writeClipNotes (*notes, clip);

    return clip;
}

ClipValue keepWithinClip (ClipValue value)
{
    value = ofItsKind (std::move (value));
    value.start = BeatPosition::fromBeats (std::max (0.0, value.start.inBeats()));

    if (value.isMidi())
        value.length = BeatDuration::fromBeats (std::max (value.length.inBeats(), shortestClipBeats));

    value.fileStart = std::max (0.0, value.fileStart);
    value.fileEnd = std::max (value.fileStart, value.fileEnd);
    value.fadeIn.bend = juce::jlimit (-1.0f, 1.0f, value.fadeIn.bend);
    value.fadeOut.bend = juce::jlimit (-1.0f, 1.0f, value.fadeOut.bend);

    // Its takes: none, or the one it plays among them (what a file holds of them is written only with a count: SongValues.h,
    // writeClipElement()).
    if (value.takes <= 0)
    {
        const ClipValue none;
        value.takes = none.takes;
        value.take = none.take;
        value.takeLength = none.takeLength;
    }
    else
    {
        value.take = juce::jlimit (1, value.takes, value.take);
    }

    return value;
}

bool setClipProperty (ClipValue& value, const juce::Identifier& property, const juce::var& given)
{
    return readInto (value, property, given);
}
} // namespace phazite
