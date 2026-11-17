// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongIndex.h"
#include "internal/Schema.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

namespace phazite
{
namespace
{
    std::atomic<bool> checking { false };
    std::atomic<std::uint64_t> visited { 0 };
    // What the index keeps outside a song: whether checking is on (set as each test case starts), the count the speed gates
    // read (atomic: songs made on other threads count into it too), and, for each thread, whether its walks are counted
    // (off while a check walks) and its share of the checks: two songs on two threads meet nowhere else.
    thread_local bool countingWalks = true;

    void visit (std::uint64_t count = 1) noexcept
    {
        if (countingWalks)
            visited.fetch_add (count, std::memory_order_relaxed);
    }

    // Each element's ID is its "id" (Schema.h: trackID, clipID, noteID), saved as a number.
    std::uint64_t getID (const juce::ValueTree& state)
    {
        return (std::uint64_t) (juce::int64) state[schema::trackID];
    }

    bool isFolder (const juce::ValueTree& track)
    {
        return track[schema::trackKind].toString() == schema::trackIsFolder;
    }

    // While it lives, walks aren't counted: they are a check's.
    struct NotCounted
    {
        NotCounted() noexcept
            : was (std::exchange (countingWalks, false))
        {
        }

        ~NotCounted() { countingWalks = was; }

        bool was;
    };

    //==============================================================================
    // The walks the model made before it had an index, kept as what the index is checked against: the tracks through the
    // track list and its folders (only a folder holds tracks); a clip on any of them; a note among a clip's children; and an
    // alias group's clips, through every track of the list a clip is in.
    void collectTracks (const juce::ValueTree& parent, std::vector<juce::ValueTree>& found)
    {
        for (const auto& child : parent)
        {
            visit();

            if (child.hasType (schema::TRACK))
            {
                found.push_back (child);

                if (isFolder (child))
                    collectTracks (child, found);
            }
        }
    }

    juce::ValueTree walkToTrackIn (const juce::ValueTree& parent, std::uint64_t id)
    {
        for (const auto& child : parent)
        {
            visit();

            if (! child.hasType (schema::TRACK))
                continue;

            if (getID (child) == id)
                return child;

            if (isFolder (child))
                if (auto found = walkToTrackIn (child, id); found.isValid())
                    return found;
        }

        return {};
    }

    juce::ValueTree walkToNote (const juce::ValueTree& clipState, std::uint64_t id)
    {
        for (const auto& child : clipState)
        {
            visit();

            if (child.hasType (schema::NOTE) && getID (child) == id)
                return child;
        }

        return {};
    }

    void collectLinked (const juce::ValueTree& parent, const juce::String& link, const juce::ValueTree& except,
                        std::vector<juce::ValueTree>& found)
    {
        for (const auto& child : parent)
        {
            visit();

            if (child.hasType (schema::TRACK))
                collectLinked (child, link, except, found);
            else if (child.hasType (schema::CLIP) && child != except && child[schema::clipLink].toString() == link)
                found.push_back (child);
        }
    }

    std::vector<juce::ValueTree> walkLinked (const juce::ValueTree& clipState)
    {
        std::vector<juce::ValueTree> aliases { clipState };

        if (const auto link = clipState[schema::clipLink].toString(); link.isNotEmpty())
        {
            auto list = clipState.getParent();

            while (list.isValid() && ! list.hasType (schema::TRACKS))
                list = list.getParent();

            collectLinked (list, link, clipState, aliases);
        }

        return aliases;
    }

    // Where an element is under an ancestor: its index among its parent's children, after its parent's, and so on up. In
    // the order of these, elements are in the order a walk from the ancestor meets them.
    std::vector<int> getPlace (const juce::ValueTree& element, const juce::ValueTree& ancestor)
    {
        std::vector<int> place;

        for (auto e = element; e.isValid() && e != ancestor;)
        {
            const auto parent = e.getParent();
            place.push_back (parent.indexOf (e));
            e = parent;
        }

        std::reverse (place.begin(), place.end());
        return place;
    }

    // Whether to compare this answer with a walk: each while a walk is short, and in a big song a share of them, so that the
    // checks cost about as much again as a lookup of a hundred elements, not a walk through the song for each.
    bool shouldCheck (size_t walkLength)
    {
        if (! checking)
            return false;

        constexpr size_t alwaysUpTo = 512, perLookup = 128;
        thread_local std::uint64_t asked = 0;

        return walkLength <= alwaysUpTo || ++asked % (walkLength / perLookup) == 0;
    }
} // namespace

//==============================================================================
SongIndex::SongIndex (juce::ValueTree songState)
    : root (std::move (songState)),
      owner (juce::Thread::getCurrentThreadId())
{
    // On the song's track list from the start, so that no listener there comes before it; and on the root for another list.
    root.addListener (&listWatcher);
    listen();
}

SongIndex::~SongIndex()
{
    root.removeListener (&listWatcher);
    trackList.removeListener (const_cast<SongIndex*> (this));
}

void SongIndex::ListWatcher::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == index.root && child.hasType (schema::TRACKS))
        index.listen();
}

void SongIndex::ListWatcher::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == index.root && child.hasType (schema::TRACKS))
        index.listen(); // lets go of the list gone, and of what it held
}

void SongIndex::setChecking (bool on) noexcept
{
    checking = on;
}

bool SongIndex::isChecking() noexcept
{
    return checking;
}

std::uint64_t SongIndex::countElementsVisited() noexcept
{
    return visited.load (std::memory_order_relaxed);
}

void SongIndex::countVisited (std::uint64_t count) noexcept
{
    visit (count);
}

//==============================================================================
juce::ValueTree SongIndex::findTrack (std::uint64_t id) const
{
    auto found = findTrackIndexed (id);

    if (shouldCheck (getSize()))
    {
        const NotCounted notCounted;
        jassert (found == walkToTrack (id)); // the index and the song differ
    }

    return found;
}

juce::ValueTree SongIndex::findClip (std::uint64_t id) const
{
    auto found = findClipIndexed (id);

    if (shouldCheck (getSize()))
    {
        const NotCounted notCounted;
        jassert (found == walkToClip (id)); // the index and the song differ
    }

    return found;
}

juce::ValueTree SongIndex::findNote (const juce::ValueTree& clipState, std::uint64_t id) const
{
    auto found = findNoteIndexed (clipState, id);

    if (shouldCheck ((size_t) clipState.getNumChildren()))
    {
        const NotCounted notCounted;
        jassert (found == walkToNote (clipState, id)); // the index and the song differ
    }

    return found;
}

std::vector<juce::ValueTree> SongIndex::getTracks() const
{
    knowOrder();

    if (isBehind())
    {
        forget();
        knowOrder();
    }

    visit (order.size());

    if (shouldCheck (order.size()))
    {
        const NotCounted notCounted;
        jassert (order == walkTracks()); // the index and the song differ
    }

    return order;
}

SongIndex::TrackPlace SongIndex::getTrackPlace (const juce::ValueTree& trackState) const
{
    knowOrder();
    visit();

    const auto findPlace = [this, &trackState]() -> std::optional<TrackPlace>
    {
        if (const auto found = places.find (getID (trackState)); found != places.end() && order[(size_t) found->second.index] == trackState)
            return found->second;

        return std::nullopt;
    };

    TrackPlace place;

    if (const auto found = findPlace(); found.has_value() && isInSong (trackState, getID (trackState), false))
    {
        place = *found;
    }
    else if (isInSong (trackState, getID (trackState), false))
    {
        // One of the song's that the order doesn't hold where it should (a change not heard yet, or two tracks with its ID):
        // the order made again from the tree, and the track found in it.
        forget();
        knowOrder();

        for (size_t i = 0, number = 0; i < order.size(); ++i)
        {
            visit();
            number += isFolder (order[i]) ? size_t { 0 } : size_t { 1 };

            if (order[i] == trackState)
            {
                place = { (int) i, isFolder (order[i]) ? 0 : (int) number };
                break;
            }
        }
    }

    if (shouldCheck (order.size()))
    {
        const NotCounted notCounted;
        const auto walked = walkTracks();
        TrackPlace expected;

        for (size_t i = 0, number = 0; i < walked.size() && expected.index < 0; ++i)
        {
            number += isFolder (walked[i]) ? size_t { 0 } : size_t { 1 };

            if (walked[i] == trackState)
                expected = { (int) i, isFolder (walked[i]) ? 0 : (int) number };
        }

        jassert (place == expected); // the index and the song differ
    }

    return place;
}

void SongIndex::knowOrder() const
{
    prepare();

    if (orderKnown)
        return;

    order = walkTracks();
    places.clear();
    int number = 0;

    for (size_t i = 0; i < order.size(); ++i)
    {
        const auto folder = isFolder (order[i]);
        number += folder ? 0 : 1;
        places.try_emplace (getID (order[i]), TrackPlace { (int) i, folder ? 0 : number });
    }

    orderKnown = true;
}

void SongIndex::orderChanged() const
{
    // What the order held let go of at once: a removed track's handle would keep it and all it holds.
    orderKnown = false;
    order.clear();
    places.clear();
    ++layout;
}

std::vector<juce::ValueTree> SongIndex::findLinked (const juce::ValueTree& clipState) const
{
    auto found = findLinkedIndexed (clipState);

    if (clipState[schema::clipLink].toString().isNotEmpty() && shouldCheck (getSize()))
    {
        const NotCounted notCounted;
        jassert (found == walkLinked (clipState)); // the index and the song differ
    }

    return found;
}

bool SongIndex::isExact() const
{
    const NotCounted notCounted;
    const auto walked = walkTracks();

    if (getTracks() != walked)
        return false;

    size_t numClips = 0, numLinked = 0;
    int number = 0;

    for (size_t i = 0; i < walked.size(); ++i)
    {
        const auto& track = walked[i];
        number += isFolder (track) ? 0 : 1;

        if (findTrackIndexed (getID (track)) != walkToTrack (getID (track))
            || getTrackPlace (track) != TrackPlace { (int) i, isFolder (track) ? 0 : number })
            return false;

        for (const auto& clip : track)
        {
            if (! clip.hasType (schema::CLIP))
                continue;

            ++numClips;

            if (findClipIndexed (getID (clip)) != walkToClip (getID (clip)) || findLinkedIndexed (clip) != walkLinked (clip))
                return false;

            // As the index groups them: by a link that names a group (an empty one names none).
            if (clip[schema::clipLink].toString().isNotEmpty())
                ++numLinked;
        }
    }

    // Nothing more than that: no track, clip or group the song doesn't hold.
    size_t indexedTracks = 0, indexedClips = 0, indexedLinked = 0;

    for (const auto& [id, entry] : tracks)
        indexedTracks += (size_t) entry.count;

    for (const auto& [id, clip] : clips)
    {
        indexedClips += (size_t) clip.count;

        // The notes of a clip whose notes were asked for: each, and no others.
        if (clip.notes != nullptr && clip.count == 1)
        {
            size_t numNotes = 0, indexedNotes = 0;

            for (const auto& note : clip.element)
            {
                if (note.hasType (schema::NOTE))
                {
                    ++numNotes;

                    if (findNoteIndexed (clip.element, getID (note)) != walkToNote (clip.element, getID (note)))
                        return false;
                }
            }

            if (clip.notes != nullptr)
                for (const auto& [noteID, entry] : *clip.notes)
                    indexedNotes += (size_t) entry.count;

            if (numNotes != indexedNotes)
                return false;
        }
    }

    for (const auto& [link, group] : groups)
        indexedLinked += group.clips.size();

    return indexedTracks == walked.size() && indexedClips == numClips && indexedLinked == numLinked;
}

//==============================================================================
void SongIndex::listen() const
{
    // The track list it listens to is the song's: a merge or a Bring Back may have put another in its place, or taken it.
    if (const auto current = root.getChildWithName (schema::TRACKS); current != trackList)
    {
        auto* listener = const_cast<SongIndex*> (this);
        trackList.removeListener (listener);
        trackList = current;
        trackList.addListener (listener);
        forget();
    }
}

void SongIndex::prepare() const
{
    // The thread that made the song is the one that may look into it (for the live song, the message thread; a song a thread
    // makes for itself from a copy, its own): a lookup may make the index, or a part of it, and the song's tree is no more
    // thread-safe than that.
    jassert (juce::Thread::getCurrentThreadId() == owner);
    listen();

    if (! built)
        build();
}

void SongIndex::build() const
{
    forget();
    built = true;
    listChildren = trackList.getNumChildren();

    for (const auto& child : trackList)
    {
        visit();

        if (child.hasType (schema::TRACK))
            addTrack (child);
    }
}

void SongIndex::forget() const
{
    built = false;
    tracks.clear();
    clips.clear();
    groups.clear();
    listChildren = 0;
    hasDuplicates = false;
    orderChanged();
}

void SongIndex::addTrack (const juce::ValueTree& track) const
{
    if (auto [entry, added] = tracks.try_emplace (getID (track), Entry { track, 1, track.getNumChildren() }); ! added)
    {
        // Held already: the index was made again from the tree, which held it, before it heard of it.
        if (entry->second.element == track)
            return;

        ++entry->second.count;
        hasDuplicates = true;
    }

    const auto folder = isFolder (track);

    for (const auto& child : track)
    {
        visit();

        if (child.hasType (schema::CLIP))
            addClip (child);
        else if (folder && child.hasType (schema::TRACK))
            addTrack (child);
    }
}

void SongIndex::addClip (const juce::ValueTree& clipState) const
{
    const auto link = clipState[schema::clipLink].toString();

    if (auto [entry, added] = clips.try_emplace (getID (clipState)); added)
    {
        entry->second.element = clipState;
        entry->second.link = link;
        entry->second.children = clipState.getNumChildren();
    }
    else if (entry->second.element == clipState)
    {
        return; // held already (above)
    }
    else
    {
        ++entry->second.count;
        hasDuplicates = true;
    }

    if (link.isNotEmpty())
    {
        auto& group = groups[link];
        group.clips.push_back (clipState);
        group.sortedAt = -1;
    }
}

void SongIndex::removeTrack (const juce::ValueTree& track) const
{
    const auto entry = tracks.find (getID (track));

    if (entry == tracks.end() || entry->second.element != track)
        return; // not one of the song's tracks: none of what it holds is

    tracks.erase (entry);
    const auto folder = isFolder (track);

    for (const auto& child : track)
    {
        if (child.hasType (schema::CLIP))
            removeClip (child);
        else if (folder && child.hasType (schema::TRACK))
            removeTrack (child);
    }
}

void SongIndex::removeClip (const juce::ValueTree& clipState) const
{
    const auto entry = clips.find (getID (clipState));

    if (entry == clips.end() || entry->second.element != clipState)
        return;

    if (const auto& link = entry->second.link; link.isNotEmpty())
    {
        if (const auto group = groups.find (link); group != groups.end())
        {
            auto& members = group->second.clips;
            members.erase (std::remove (members.begin(), members.end(), clipState), members.end());

            if (members.empty())
                groups.erase (group);
        }
    }

    clips.erase (entry);
}

bool SongIndex::isIndexedTrack (const juce::ValueTree& track) const
{
    const auto entry = tracks.find (getID (track));
    return entry != tracks.end() && entry->second.element == track;
}

SongIndex::Clip* SongIndex::findIndexedClip (const juce::ValueTree& clipState) const
{
    const auto entry = clips.find (getID (clipState));
    return entry != clips.end() && entry->second.count == 1 && entry->second.element == clipState ? &entry->second : nullptr;
}

const SongIndex::Entries& SongIndex::getNotes (Clip& clip) const
{
    // Made again where the clip holds another count of children than it last saw (a change not heard yet).
    if (clip.notes != nullptr && clip.element.getNumChildren() != clip.children)
        clip.notes = nullptr;

    if (clip.notes == nullptr)
    {
        clip.notes = std::make_unique<Entries>();
        clip.children = clip.element.getNumChildren();

        for (const auto& child : clip.element)
        {
            visit();

            if (child.hasType (schema::NOTE))
                if (auto [entry, added] = clip.notes->try_emplace (getID (child), Entry { child }); ! added)
                    ++entry->second.count;
        }
    }

    return *clip.notes;
}

void SongIndex::seeChildren (const juce::ValueTree& parent) const
{
    if (parent == trackList)
    {
        listChildren = parent.getNumChildren();
    }
    else if (parent.hasType (schema::TRACK))
    {
        if (const auto entry = tracks.find (getID (parent)); entry != tracks.end() && entry->second.element == parent)
            entry->second.children = parent.getNumChildren();
    }
    else if (parent.hasType (schema::CLIP))
    {
        if (auto* clip = findIndexedClip (parent))
            clip->children = parent.getNumChildren();
    }
}

//==============================================================================
bool SongIndex::isInSong (const juce::ValueTree& element, std::uint64_t id, bool clip) const
{
    if (! element.isValid() || getID (element) != id)
        return false;

    auto parent = element.getParent();

    if (clip)
    {
        if (! parent.hasType (schema::TRACK))
            return false;

        parent = parent.getParent();
    }

    // Up through folders to the track list: only a folder holds tracks.
    for (;; parent = parent.getParent())
    {
        if (parent == trackList)
            return trackList.isValid();

        if (! parent.hasType (schema::TRACK) || ! isFolder (parent))
            return false;
    }
}

bool SongIndex::isBehind() const
{
    visit (tracks.size() + 1);

    if (trackList.getNumChildren() != listChildren)
        return true;

    for (const auto& [id, entry] : tracks)
        if (entry.element.getNumChildren() != entry.children)
            return true;

    return false;
}

juce::ValueTree SongIndex::walkToTrack (std::uint64_t id) const
{
    return walkToTrackIn (root.getChildWithName (schema::TRACKS), id);
}

juce::ValueTree SongIndex::walkToClip (std::uint64_t id) const
{
    for (const auto& track : walkTracks())
    {
        for (const auto& child : track)
        {
            visit();

            if (child.hasType (schema::CLIP) && getID (child) == id)
                return child;
        }
    }

    return {};
}

std::vector<juce::ValueTree> SongIndex::walkTracks() const
{
    std::vector<juce::ValueTree> found;
    collectTracks (root.getChildWithName (schema::TRACKS), found);
    return found;
}

size_t SongIndex::getSize() const
{
    return tracks.size() + clips.size();
}

juce::ValueTree SongIndex::findTrackIndexed (std::uint64_t id) const
{
    prepare();
    visit();
    auto entry = tracks.find (id);

    // None: unless the tree holds what the index hasn't heard of yet, it holds none.
    if (entry == tracks.end())
    {
        if (! isBehind())
            return {};

        forget();
        prepare();

        if (entry = tracks.find (id); entry == tracks.end())
            return {};
    }

    if (entry->second.count != 1)
        return walkToTrack (id);

    if (isInSong (entry->second.element, id, false))
        return entry->second.element;

    // Gone, or moved, before the index heard of it: the tree's answer, and the index made again as it is next asked.
    forget();
    return walkToTrack (id);
}

juce::ValueTree SongIndex::findClipIndexed (std::uint64_t id) const
{
    prepare();
    visit();
    auto entry = clips.find (id);

    if (entry == clips.end())
    {
        if (! isBehind())
            return {};

        forget();
        prepare();

        if (entry = clips.find (id); entry == clips.end())
            return {};
    }

    if (entry->second.count != 1)
        return walkToClip (id);

    if (isInSong (entry->second.element, id, true))
        return entry->second.element;

    forget();
    return walkToClip (id);
}

juce::ValueTree SongIndex::findNoteIndexed (const juce::ValueTree& clipState, std::uint64_t id) const
{
    if (! clipState.isValid())
        return {};

    prepare();
    auto* clip = findIndexedClip (clipState);

    // A clip outside the song (a copy), or one whose ID another clip shares: its notes looked through.
    if (clip == nullptr)
        return walkToNote (clipState, id);

    const auto& notes = getNotes (*clip); // made again first if the clip holds another count of children than it saw
    visit();
    const auto entry = notes.find (id);

    if (entry == notes.end())
        return {};

    if (entry->second.count == 1)
        if (const auto& note = entry->second.element; getID (note) == id && note.getParent() == clipState)
            return note;

    // Shared by two of its notes, or changed before the index heard of it: the clip's notes looked through.
    if (entry->second.count == 1)
        clip->notes = nullptr;

    return walkToNote (clipState, id);
}

std::vector<juce::ValueTree> SongIndex::findLinkedIndexed (const juce::ValueTree& clipState) const
{
    const auto link = clipState[schema::clipLink].toString();

    if (link.isEmpty())
        return { clipState };

    prepare();
    const auto* clip = findIndexedClip (clipState);
    const auto group = groups.find (link);

    if (clip == nullptr || clip->link != link || group == groups.end())
        return walkLinked (clipState);

    // In the song's order, put in it again only after tracks or clips were added, removed or moved.
    auto& members = group->second;

    if (members.sortedAt != layout)
    {
        std::vector<std::pair<std::vector<int>, juce::ValueTree>> placed;

        for (const auto& member : members.clips)
            placed.emplace_back (getPlace (member, trackList), member);

        std::sort (placed.begin(), placed.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        members.clips.clear();

        for (const auto& [place, member] : placed)
            members.clips.push_back (member);

        members.sortedAt = layout;
    }

    std::vector<juce::ValueTree> aliases { clipState };
    visit (members.clips.size());

    for (const auto& member : members.clips)
    {
        // Each still in the group and in the song, unless a change came before the index heard of it.
        if (member[schema::clipLink].toString() != link || ! isInSong (member, getID (member), true))
        {
            forget();
            return walkLinked (clipState);
        }

        if (member != clipState)
            aliases.push_back (member);
    }

    return aliases;
}

//==============================================================================
void SongIndex::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! built)
        return;

    // An element's ID changed, or what a track is (a folder holds tracks): made again as it is next asked for.
    if (property == schema::trackID)
    {
        if (tree.hasType (schema::TRACK) || tree.hasType (schema::CLIP))
            forget();
        else if (tree.hasType (schema::NOTE))
            if (auto* clip = findIndexedClip (tree.getParent()))
                clip->notes = nullptr;

        return;
    }

    if (property == schema::trackKind && tree.hasType (schema::TRACK))
    {
        forget();
        return;
    }

    // A clip linked to others, or no longer: from one group to the other.
    if (property == schema::clipLink && tree.hasType (schema::CLIP))
    {
        if (hasDuplicates)
        {
            forget();
            return;
        }

        if (findIndexedClip (tree) == nullptr)
            return; // not the song's

        removeClip (tree);
        addClip (tree);
        ++layout;
    }
}

void SongIndex::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (! built)
        return;

    if (hasDuplicates && (child.hasType (schema::TRACK) || child.hasType (schema::CLIP)))
    {
        forget();
        return;
    }

    if (child.hasType (schema::TRACK))
    {
        if (parent == trackList || (parent.hasType (schema::TRACK) && isFolder (parent) && isIndexedTrack (parent)))
        {
            orderChanged();
            addTrack (child);
        }
    }
    else if (child.hasType (schema::CLIP))
    {
        if (parent.hasType (schema::TRACK) && isIndexedTrack (parent))
        {
            ++layout;
            addClip (child);
        }
    }
    else if (child.hasType (schema::NOTE))
    {
        if (auto* clip = findIndexedClip (parent); clip != nullptr && clip->notes != nullptr)
        {
            if (auto [entry, added] = clip->notes->try_emplace (getID (child), Entry { child }); ! added && entry->second.element != child)
                ++entry->second.count;
        }
    }

    seeChildren (parent);
}

void SongIndex::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (! built)
        return;

    if (hasDuplicates && (child.hasType (schema::TRACK) || child.hasType (schema::CLIP)))
    {
        forget();
        return;
    }

    if (child.hasType (schema::TRACK))
    {
        if (isIndexedTrack (child))
        {
            orderChanged();
            removeTrack (child);
        }
    }
    else if (child.hasType (schema::CLIP))
    {
        ++layout;
        removeClip (child);
    }
    else if (child.hasType (schema::NOTE))
    {
        if (auto* clip = findIndexedClip (parent); clip != nullptr && clip->notes != nullptr)
        {
            if (const auto entry = clip->notes->find (getID (child)); entry != clip->notes->end())
            {
                if (entry->second.count == 1 && entry->second.element == child)
                    clip->notes->erase (entry);
                else
                    clip->notes = nullptr; // one of two with its ID: made again as it is next asked for
            }
        }
    }

    seeChildren (parent);
}

void SongIndex::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (built && (parent == trackList || parent.hasType (schema::TRACK)))
        orderChanged();
}
} // namespace phazite
