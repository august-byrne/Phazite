// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ClipsArea.h"
#include "Song.h"
#include "internal/IDsHeld.h"
#include "internal/NotesArea.h"
#include "internal/Schema.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
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

    // A clip read from its element into the areas' next values: its value, kept within what a clip holds as the clips' own
    // setters keep it (keepWithinClip(): what the file's properties are each within their ranges, but not with each other),
    // on its track, at a place in its list; and a MIDI clip's notes, those of the alias group it joins where another clip of
    // it holds some, else its own (NotesArea.h: takeClipNotes()). An ID the area holds already is the same clip again, which
    // this one replaces: a move.
    void takeClip (SongClips& clips, NotesByClip& notes, const juce::ValueTree& element, TrackID track, int index)
    {
        auto value = keepWithinClip (readClipValue (element));
        value.track = track;
        const auto id = value.id;

        if (! id.isValid())
            return;

        takeClipNotes (notes, clips, value, element);

        // The sound of the audio alias group it joins, where another clip of it holds one alike: the group holds one.
        if (value.isAudio() && value.link != 0)
            if (const auto* members = clips.groups.find (value.link))
                for (const auto member : *members)
                    if (const auto* other = clips.find (member); other != nullptr && member != id && other->isAudio())
                    {
                        if (other->sound == value.sound)
                            value.sound = other->sound;

                        break;
                    }

        clips = withClip (withoutClip (clips, id), std::move (value), index);
    }
} // namespace

//==============================================================================
SongValues::ClipsTeller::ClipsTeller (ValueArea<SongClips>& a, juce::ListenerList<SongValues::Listener>& l)
    : area (a),
      listeners (l)
{
    area.addListener (this);
}

SongValues::ClipsTeller::~ClipsTeller()
{
    area.removeListener (this);
}

void SongValues::ClipsTeller::valueAreaChanged (const SongClips& before, const SongClips& after)
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

//==============================================================================
SongValues::ClipsKeeper::ClipsKeeper (SongValues& v, Song& s)
    : values (v),
      song (s),
      state (s.getState())
{
}

std::vector<SongValues::ClipsKeeper::Remembered>::iterator SongValues::ClipsKeeper::findRemembered (const juce::ValueTree& track)
{
    return std::find_if (remembered.begin(), remembered.end(), [&track] (const Remembered& each) { return each.track == track; });
}

void SongValues::ClipsKeeper::sweep()
{
    if (remembered.size() < sweepAt)
        return;

    std::erase_if (remembered,
                   [this] (const Remembered& each) { return each.track.getReferenceCount() <= 1 || each.track.isAChildOf (state); });
    sweepAt = std::max<size_t> (64, remembered.size() * 2);
}

bool SongValues::ClipsKeeper::isInTrackList (const juce::ValueTree& parent, const juce::ValueTree& child) const
{
    if (child.hasType (schema::TRACKS))
        return parent == state;

    for (auto element = parent; element.isValid(); element = element.getParent())
        if (element.hasType (schema::TRACKS))
            return element.getParent() == state;

    return false;
}

void SongValues::ClipsKeeper::childAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
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

void SongValues::ClipsKeeper::childRemoved (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (! isInTrackList (parent, child))
        return;

    // A track (or what holds tracks) taken out of the song's track list: its clips remembered with its element, and erased.
    // What it may have been remembered with before (taken out, and brought back by an undo since, which gave its clips back
    // itself) is replaced, or forgotten where it holds none now: else the next time it comes back, it would bring back
    // clips deleted since.
    std::vector<juce::ValueTree> tracks;
    collectTracks (child, tracks);
    auto nextClips = values.clips.get();
    auto nextNotes = values.notes.get();
    auto any = false;

    for (const auto& track : tracks)
    {
        const auto* list = nextClips.tracks.find (getTrackIDOf (track).getRaw());

        if (list == nullptr)
        {
            if (const auto found = findRemembered (track); found != remembered.end())
                remembered.erase (found);

            continue;
        }

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

void SongValues::ClipsKeeper::adopt (SongValues& into, Song& loaded)
{
    // Every CLIP of the tree the loader made consistent into both areas, outside the history: each track's in their order,
    // the clips of an alias group sharing the first's notes and, audio clips, its sound (the loader made them hold the same:
    // Project.cpp, makeConsistent()), each kept within what a clip holds (takeClip()); then every CLIP element out of the tree,
    // those anywhere else too, which are no clips of the song's (dropClipElements()).
    std::vector<juce::ValueTree> tracks;
    collectTracks (loaded.getState().getChildWithName (schema::TRACKS), tracks);
    auto nextClips = into.clips.get();
    auto nextNotes = into.notes.get();

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

    dropClipElements (loaded.getState());
    into.clips.setOutsideHistory (std::move (nextClips));
    into.notes.setOutsideHistory (std::move (nextNotes));
}

bool SongValues::ClipsKeeper::remembers (const juce::ValueTree& track) const
{
    return std::any_of (remembered.begin(), remembered.end(), [&track] (const Remembered& each) { return each.track == track; });
}

std::vector<TrackID> SongValues::ClipsKeeper::getRemembered() const
{
    std::vector<TrackID> tracks;

    for (const auto& each : remembered)
        tracks.push_back (getTrackIDOf (each.track));

    return tracks;
}

//==============================================================================
void SongValues::setAll (Song& song, SongClips next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        clips.setOutsideHistory (std::move (next));
    else
        clips.set (std::move (next));
}

void SongValues::setWithoutStep (Song& song, SongClips next)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (song.isWritingOutsideHistory())
        clips.setOutsideHistory (std::move (next));
    else
        clips.setWithoutStep (std::move (next));
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

//==============================================================================
bool leadsToClips (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (parent.hasType (schema::PHAZITE) || parent.hasType (schema::TRACKS) || parent.hasType (schema::TRACK))
           && (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK));
}

void writeClipsIn (juce::ValueTree& element, const ValuesKept& kept)
{
    if (! element.hasType (schema::TRACK))
        return;

    const auto track = (std::uint64_t) (juce::int64) element[schema::trackID];

    if (const auto* list = kept.clips.tracks.find (track))
    {
        int place = 0;

        for (const auto id : *list)
            if (const auto* clip = kept.clips.find (id))
                element.addChild (writeClipElement (*clip, kept.notes.find (id)), place++, nullptr);
    }
}

bool holdSameClip (const ValuesKept& a, const ValuesKept& b, ClipID clip)
{
    const auto* first = a.clips.find (clip);
    const auto* second = b.clips.find (clip);

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second && holdSameNotes (a, b, clip);
}

bool holdSameTrackClips (const ValuesKept& a, const ValuesKept& b, TrackID track)
{
    const auto* first = a.clips.tracks.find (track.getRaw());
    const auto* second = b.clips.tracks.find (track.getRaw());

    if (first == nullptr || second == nullptr)
        return first == second;

    if (first->identity() != second->identity() && ! (*first == *second))
        return false;

    for (const auto clip : *first)
        if (! holdSameClip (a, b, clip))
            return false;

    return true;
}

bool holdSameClipsOf (const juce::ValueTree& element, const ValuesKept& kept, const juce::ValueTree&, const ValuesKept& otherKept)
{
    return ! element.hasType (schema::TRACK)
           || holdSameTrackClips (kept, otherKept, TrackID::fromRaw ((std::uint64_t) (juce::int64) element[schema::trackID]));
}

std::uint64_t findLargestIDIn (const SongClips& clips)
{
    std::uint64_t largest = 0;

    for (const auto& entry : clips.clips)
    {
        largest = countHeldID (largest, entry.second.id.getRaw());
        largest = countHeldID (largest, entry.second.link);
    }

    return largest;
}
} // namespace phazite
