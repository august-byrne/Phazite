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

void SongValues::eraseClips (Song& song, const std::vector<ClipID>& gone)
{
    // Each area's next value worked out whole, then set once (each tells once), as the keeper erased a track's clips.
    auto nextClips = clips.get();
    auto nextNotes = notes.get();
    auto anyClip = false, anyNotes = false;

    for (const auto id : gone)
    {
        if (nextClips.find (id) == nullptr)
            continue;

        nextClips = withoutClip (nextClips, id);
        anyClip = true;

        if (nextNotes.find (id) != nullptr)
        {
            nextNotes.clips = nextNotes.clips.erase (id.getRaw());
            anyNotes = true;
        }
    }

    if (anyClip)
        setAll (song, std::move (nextClips));

    if (anyNotes)
        setAll (song, std::move (nextNotes));
}

//==============================================================================
int takeTrackClipsIn (SongClips& clips, NotesByClip& notes, const juce::ValueTree& track, TrackID id)
{
    if (! holdsClips (track) || ! id.isValid())
        return 0;

    auto taken = 0;

    for (const auto& element : track)
    {
        if (element.hasType (schema::CLIP))
        {
            takeClip (clips, notes, element, id, -1);
            ++taken;
        }
    }

    return taken;
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
