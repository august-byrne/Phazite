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

    // Each track's ID is its "id" (Schema.h: trackID), saved as a number.
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
    // track list and its folders (only a folder holds tracks).
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

    if (const auto found = findPlace(); found.has_value() && isInSong (trackState, getID (trackState)))
    {
        place = *found;
    }
    else if (isInSong (trackState, getID (trackState)))
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
}

bool SongIndex::isExact() const
{
    const NotCounted notCounted;
    const auto walked = walkTracks();

    if (getTracks() != walked)
        return false;

    int number = 0;

    for (size_t i = 0; i < walked.size(); ++i)
    {
        const auto& track = walked[i];
        number += isFolder (track) ? 0 : 1;

        if (findTrackIndexed (getID (track)) != walkToTrack (getID (track))
            || getTrackPlace (track) != TrackPlace { (int) i, isFolder (track) ? 0 : number })
            return false;
    }

    // Nothing more than that: no track the song doesn't hold.
    size_t indexedTracks = 0;

    for (const auto& [id, entry] : tracks)
        indexedTracks += (size_t) entry.count;

    return indexedTracks == walked.size();
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

    if (! isFolder (track))
        return;

    for (const auto& child : track)
    {
        visit();

        if (child.hasType (schema::TRACK))
            addTrack (child);
    }
}

void SongIndex::removeTrack (const juce::ValueTree& track) const
{
    const auto entry = tracks.find (getID (track));

    if (entry == tracks.end() || entry->second.element != track)
        return; // not one of the song's tracks: none of what it holds is

    tracks.erase (entry);

    if (! isFolder (track))
        return;

    for (const auto& child : track)
        if (child.hasType (schema::TRACK))
            removeTrack (child);
}

bool SongIndex::isIndexedTrack (const juce::ValueTree& track) const
{
    const auto entry = tracks.find (getID (track));
    return entry != tracks.end() && entry->second.element == track;
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
}

//==============================================================================
bool SongIndex::isInSong (const juce::ValueTree& element, std::uint64_t id) const
{
    if (! element.isValid() || getID (element) != id)
        return false;

    auto parent = element.getParent();

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

std::vector<juce::ValueTree> SongIndex::walkTracks() const
{
    std::vector<juce::ValueTree> found;
    collectTracks (root.getChildWithName (schema::TRACKS), found);
    return found;
}

size_t SongIndex::getSize() const
{
    return tracks.size();
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

    if (isInSong (entry->second.element, id))
        return entry->second.element;

    // Gone, or moved, before the index heard of it: the tree's answer, and the index made again as it is next asked.
    forget();
    return walkToTrack (id);
}

//==============================================================================
void SongIndex::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! built)
        return;

    // A track's ID changed, or what a track is (a folder holds tracks): made again as it is next asked for.
    if ((property == schema::trackID || property == schema::trackKind) && tree.hasType (schema::TRACK))
        forget();
}

void SongIndex::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (! built)
        return;

    if (hasDuplicates && child.hasType (schema::TRACK))
    {
        forget();
        return;
    }

    if (child.hasType (schema::TRACK)
        && (parent == trackList || (parent.hasType (schema::TRACK) && isFolder (parent) && isIndexedTrack (parent))))
    {
        orderChanged();
        addTrack (child);
    }

    seeChildren (parent);
}

void SongIndex::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (! built)
        return;

    if (hasDuplicates && child.hasType (schema::TRACK))
    {
        forget();
        return;
    }

    if (child.hasType (schema::TRACK) && isIndexedTrack (child))
    {
        orderChanged();
        removeTrack (child);
    }

    seeChildren (parent);
}

void SongIndex::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (built && (parent == trackList || parent.hasType (schema::TRACK)))
        orderChanged();
}
} // namespace phazite
