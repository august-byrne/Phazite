// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ApplySong.h"
#include "internal/SongDiff.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <map>
#include <set>

namespace phazite
{
namespace
{
    // An element's counted children, each named by its type and key, or, without a key, by its type and its place among
    // those of its type (a curve's points).
    struct Child
    {
        juce::ValueTree tree;
        juce::String name;
    };

    // Which of an element's parts making it like another takes: what is counted, or everything (switching in place).
    enum class Taken
    {
        counted,
        everything
    };

    bool isTaken (Taken taken, const juce::ValueTree& element, const juce::Identifier& property)
    {
        return taken == Taken::everything || ! schema::isUncountedProperty (element, property);
    }

    bool isTaken (Taken taken, const juce::ValueTree& element, const juce::ValueTree& child)
    {
        return taken == Taken::everything || ! schema::isUncountedElement (element, child);
    }

    // An element of a version's song whose piece, or part of it, couldn't be read (SongPieces.h: joinSong()): the song keeps
    // its own as it is now, wherever it is.
    bool isStandIn (const juce::ValueTree& element)
    {
        return element.hasProperty (schema::versions::missing);
    }

    juce::String nameOf (const juce::ValueTree& element)
    {
        return element.getType().toString() + ":" + schema::getElementKey (element);
    }

    // The stand-ins in the other song, by their names, as getCountedChildren() names them.
    using StandIns = std::set<juce::String>;

    StandIns findStandIns (const juce::ValueTree& other)
    {
        StandIns names;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (isStandIn (child))
                    names.insert (nameOf (child));
                else
                    visit (child);
            }
        };

        visit (other);
        return names;
    }

    const StandIns& noStandIns()
    {
        static const StandIns none;
        return none;
    }

    std::vector<Child> getCountedChildren (const juce::ValueTree& element, bool withTracks = true, Taken taken = Taken::counted)
    {
        std::vector<Child> children;
        std::map<juce::String, int> numUnkeyed;
        std::set<juce::String> names;

        for (const auto& child : element)
        {
            if (! isTaken (taken, element, child) || (! withTracks && child.hasType (schema::TRACK)))
                continue;

            const auto type = child.getType().toString();
            const auto key = schema::getElementKey (child);
            auto name = type + ":" + key;

            if (key.isEmpty() || names.count (name) > 0)
                name = type + "#" + juce::String (numUnkeyed[type]++);

            names.insert (name);
            children.push_back ({ child, name });
        }

        return children;
    }

    // The element made like the other, its children too, but for the tracks in it while withTracks is off (a folder's,
    // which are things of their own).
    void makeLike (juce::ValueTree live, const juce::ValueTree& other, juce::UndoManager* undo, bool withTracks = true,
                   Taken taken = Taken::counted, const StandIns& standIns = noStandIns())
    {
        for (int i = live.getNumProperties(); --i >= 0;)
            if (const auto name = live.getPropertyName (i); isTaken (taken, live, name) && ! other.hasProperty (name))
                live.removeProperty (name, undo);

        for (int i = 0; i < other.getNumProperties(); ++i)
        {
            const auto name = other.getPropertyName (i);

            if (! isTaken (taken, other, name))
                continue;

            if (const auto* value = live.getPropertyPointer (name); value == nullptr || ! isSameValue (*value, other[name]))
                live.setProperty (name, other[name], undo);
        }

        // The children it has that the other hasn't go; then, in the other's order, each it has made like the other's and
        // each it hasn't added, after the uncounted children before them (its lanes, which are view state).
        const auto wanted = getCountedChildren (other, withTracks, taken);
        std::set<juce::String> wantedNames;

        for (const auto& child : wanted)
            wantedNames.insert (child.name);

        std::map<juce::String, juce::ValueTree> kept;

        for (const auto& child : getCountedChildren (live, withTracks, taken))
        {
            if (wantedNames.count (child.name) == 0 && standIns.count (child.name) == 0)
                live.removeChild (child.tree, undo);
            else if (wantedNames.count (child.name) > 0)
                kept.emplace (child.name, child.tree);
        }

        int place = 0;

        for (const auto& child : wanted)
        {
            while (place < live.getNumChildren()
                   && (! isTaken (taken, live, live.getChild (place)) || (! withTracks && live.getChild (place).hasType (schema::TRACK))))
                ++place;

            if (const auto found = kept.find (child.name); found != kept.end())
            {
                // What the other's version couldn't read stays as it is, in its place.
                auto tree = found->second;

                if (! isStandIn (child.tree))
                    makeLike (tree, child.tree, undo, true, taken, standIns);

                if (const auto index = live.indexOf (tree); index != place)
                    live.moveChild (index, place, undo);
            }
            else if (! isStandIn (child.tree))
            {
                live.addChild (child.tree.createCopy(), place, undo);
            }
            else
            {
                continue; // a stand-in for what the song hasn't: nothing to put there
            }

            ++place;
        }
    }

    // Where each item is in a song (its parent's type and key), by its own type and ID: notes aside, whose IDs are their
    // clips' own.
    std::map<juce::String, juce::String> findPlaces (const juce::ValueTree& tree)
    {
        std::map<juce::String, juce::String> places;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (! child.hasType (schema::NOTE) && schema::holdsID (child, schema::trackID))
                    places[child.getType().toString() + ":" + child[schema::trackID].toString()] =
                        element.getType().toString() + ":" + schema::getElementKey (element);

                visit (child);
            }
        };

        visit (tree);
        return places;
    }

    // The items that are somewhere else in the other song (a track moved into a folder, a clip onto another track, a
    // device into a container), taken away first, to be put where the other has them: no ID is in the song twice meanwhile.
    void removeMovedItems (juce::ValueTree live, const juce::ValueTree& other, juce::UndoManager* undo,
                           const StandIns& standIns = noStandIns())
    {
        const auto otherPlaces = findPlaces (other);
        std::vector<juce::ValueTree> moved;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (! child.hasType (schema::NOTE) && schema::holdsID (child, schema::trackID) && standIns.count (nameOf (child)) == 0)
                {
                    const auto found = otherPlaces.find (child.getType().toString() + ":" + child[schema::trackID].toString());

                    if (found != otherPlaces.end() && found->second != element.getType().toString() + ":" + schema::getElementKey (element))
                    {
                        moved.push_back (child);
                        continue;
                    }
                }

                visit (child);
            }
        };

        visit (live);

        for (auto& tree : moved)
            tree.getParent().removeChild (tree, undo);
    }

    juce::ValueTree findTrack (const juce::ValueTree& song, std::uint64_t id)
    {
        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid() && readID (master[schema::trackID]) == id)
            return master;

        for (const auto& track : getTrackElements (song))
            if (readID (track[schema::trackID]) == id)
                return track;

        return {};
    }

    // The element that holds a track there: the song's track list, or a folder's element.
    juce::ValueTree findTrackParent (const juce::ValueTree& song, const juce::ValueTree& parentThere)
    {
        if (parentThere.hasType (schema::TRACK))
            if (const auto folder = findTrack (song, readID (parentThere[schema::trackID])); folder.isValid())
                return folder;

        return song.getChildWithName (schema::TRACKS);
    }

    // The curves of these devices like the other's.
    void makeCurvesLike (juce::ValueTree state, juce::UndoManager* undo, const std::set<std::uint64_t>& devices,
                         const juce::ValueTree& other)
    {
        auto automation = state.getChildWithName (schema::AUTOMATION);
        const auto otherAutomation = other.getChildWithName (schema::AUTOMATION);
        const auto isOurs = [&devices] (const juce::ValueTree& curve) { return devices.count (readID (curve[schema::curveDevice])) > 0; };

        std::map<juce::String, juce::ValueTree> wanted;

        for (const auto& curve : otherAutomation)
            if (curve.hasType (schema::CURVE) && isOurs (curve))
                wanted.emplace (schema::getElementKey (curve), curve);

        if (wanted.empty() && ! automation.isValid())
            return;

        if (! automation.isValid())
        {
            automation = juce::ValueTree (schema::AUTOMATION);
            state.appendChild (automation, undo);
        }

        for (int i = automation.getNumChildren(); --i >= 0;)
        {
            auto curve = automation.getChild (i);

            if (! curve.hasType (schema::CURVE) || ! isOurs (curve))
                continue;

            if (const auto found = wanted.find (schema::getElementKey (curve)); found != wanted.end())
            {
                if (! isStandIn (found->second)) // a curve the version couldn't read stays as it is
                    makeLike (curve, found->second, undo);

                wanted.erase (found);
            }
            else
            {
                automation.removeChild (curve, undo);
            }
        }

        for (const auto& [key, curve] : wanted)
            if (! isStandIn (curve))
                automation.appendChild (curve.createCopy(), undo);
    }

    // The tracks a song holds by their IDs, each with its parent's ID (0 for the track list) and its place there.
    struct TrackPlace
    {
        std::uint64_t parent = 0;
        int index = 0;
    };

    std::map<std::uint64_t, TrackPlace> findTrackPlaces (const juce::ValueTree& song)
    {
        std::map<std::uint64_t, TrackPlace> places;

        for (const auto& track : getTrackElements (song))
            places[readID (track[schema::trackID])] = { track.getParent().hasType (schema::TRACK)
                                                            ? readID (track.getParent()[schema::trackID])
                                                            : 0,
                                                        track.getParent().indexOf (track) };

        return places;
    }
} // namespace

void makeSongLike (Song& song, const juce::ValueTree& other)
{
    auto* undo = &song.getUndoManager();
    const auto standIns = findStandIns (other);
    removeMovedItems (song.getState(), other, undo, standIns);
    makeLike (song.getState(), other, undo, true, Taken::counted, standIns);
}

void makeSongTheSame (Song& song, const juce::ValueTree& other)
{
    // Its count of IDs the larger of the two: what either branch made keeps an ID of its own.
    const auto next = std::max (findNextID (song.getState()), findNextID (other));
    removeMovedItems (song.getState(), other, nullptr);
    makeLike (song.getState(), other, nullptr, true, Taken::everything);
    song.getState().setProperty (schema::nextID, (juce::int64) next, nullptr);
    song.getUndoManager().clearUndoHistory();
}

namespace
{
    bool makeTrackLikeIn (juce::ValueTree state, TrackID id, const juce::ValueTree& other, juce::UndoManager* undo)
    {
        auto live = findTrack (state, id.getRaw());
        const auto then = findTrack (other, id.getRaw());

        if (! live.isValid() && ! then.isValid())
            return false;

        // A track the other's version couldn't read stays as it is.
        if (then.isValid() && isStandIn (then))
            return true;

        // Its devices' automation, of what either of them holds.
        auto devices = getAutomatableIDs (live);
        devices.merge (getAutomatableIDs (then));

        if (live.isValid() && then.isValid())
        {
            makeLike (live, then, undo, false, Taken::counted, findStandIns (then));
        }
        else if (live.isValid())
        {
            // Added since: taken away.
            live.getParent().removeChild (live, undo);
        }
        else
        {
            // Gone since: back where it was, after the track before it there, with the tracks it held that aren't elsewhere now.
            auto parent = findTrackParent (state, then.getParent());
            auto copy = then.createCopy();

            for (int i = copy.getNumChildren(); --i >= 0;)
                if (const auto child = copy.getChild (i);
                    child.hasType (schema::TRACK) && findTrack (state, readID (child[schema::trackID])).isValid())
                    copy.removeChild (i, nullptr);

            int index = 0;

            for (auto before = then.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto found = parent.indexOf (findTrack (state, readID (before[schema::trackID]))); found >= 0)
                {
                    index = found + 1;
                    break;
                }
            }

            parent.addChild (copy, index, undo);
        }

        makeCurvesLike (state, undo, devices, other);
        return true;
    }

    void makeSongOwnLikeIn (juce::ValueTree state, const juce::ValueTree& other, juce::UndoManager* undo)
    {
        // Its own properties.
        for (int i = state.getNumProperties(); --i >= 0;)
            if (const auto name = state.getPropertyName (i); ! schema::isUncountedProperty (state, name) && ! other.hasProperty (name))
                state.removeProperty (name, undo);

        for (int i = 0; i < other.getNumProperties(); ++i)
            if (const auto name = other.getPropertyName (i); ! schema::isUncountedProperty (other, name))
                if (const auto* value = state.getPropertyPointer (name); value == nullptr || ! isSameValue (*value, other[name]))
                    state.setProperty (name, other[name], undo);

        // Its tempo map, sections, chord lane and the key set last: each as the other has it, or none.
        for (const auto* type : { &schema::TEMPOMAP, &schema::SECTIONS, &schema::CHORDLANE, &schema::LASTKEY })
        {
            auto live = state.getChildWithName (*type);
            const auto then = other.getChildWithName (*type);

            if (live.isValid() && then.isValid())
                makeLike (live, then, undo);
            else if (live.isValid())
                state.removeChild (live, undo);
            else if (then.isValid())
                state.appendChild (then.createCopy(), undo);
        }

        // The tracks both have, in the other's order: in its folders, and in its order there.
        const auto wanted = findTrackPlaces (other);

        for (const auto& track : getTrackElements (other))
        {
            const auto id = readID (track[schema::trackID]);
            auto live = findTrack (state, id);

            if (! live.isValid())
                continue;

            auto parent = findTrackParent (state, track.getParent());

            if (live.getParent() != parent)
            {
                live.getParent().removeChild (live, undo);
                parent.addChild (live, -1, undo);
            }

            // After the track before it there that both have.
            int index = 0;

            for (auto before = track.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto found = parent.indexOf (findTrack (state, readID (before[schema::trackID]))); found >= 0)
                {
                    index = found + 1;
                    break;
                }
            }

            if (const auto current = parent.indexOf (live); current != index && current != index - 1)
                parent.moveChild (current, current < index ? index - 1 : index, undo);
        }

        juce::ignoreUnused (wanted);
    }
} // namespace

bool makeTrackLike (Song& song, TrackID id, const juce::ValueTree& other)
{
    return makeTrackLikeIn (song.getState(), id, other, &song.getUndoManager());
}

bool makeTrackLike (juce::ValueTree song, TrackID id, const juce::ValueTree& other)
{
    return makeTrackLikeIn (std::move (song), id, other, nullptr);
}

void makeSongOwnLike (Song& song, const juce::ValueTree& other)
{
    makeSongOwnLikeIn (song.getState(), other, &song.getUndoManager());
}

void makeSongOwnLike (juce::ValueTree song, const juce::ValueTree& other)
{
    makeSongOwnLikeIn (std::move (song), other, nullptr);
}

int removeBrokenReferences (Song& song)
{
    auto* undo = &song.getUndoManager();
    const auto state = song.getState();
    std::set<std::uint64_t> items, tracks;
    const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (schema::holdsID (child, schema::trackID) && ! child.hasType (schema::NOTE))
                items.insert (readID (child[schema::trackID]));

            for (const auto* property : { &schema::channelFader, &schema::channelVCA })
                if (child.hasProperty (*property))
                    items.insert (readID (child[*property]));

            if (child.hasType (schema::TRACK))
                tracks.insert (readID (child[schema::trackID]));

            collect (child);
        }
    };

    collect (state);
    std::vector<juce::ValueTree> modulations;
    std::vector<juce::ValueTree> sidechains;
    const std::function<void (const juce::ValueTree&)> find = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::MODULATES) && items.count (readID (child[schema::modulatesDevice])) == 0)
                modulations.push_back (child);

            if (child.hasType (schema::DEVICE) && child.hasProperty (schema::deviceSidechain)
                && tracks.count (readID (child[schema::deviceSidechain])) == 0)
                sidechains.push_back (child);

            find (child);
        }
    };

    find (state);

    for (auto& modulation : modulations)
        modulation.getParent().removeChild (modulation, undo);

    for (auto& device : sidechains)
        device.removeProperty (schema::deviceSidechain, undo);

    return (int) (modulations.size() + sidechains.size());
}
} // namespace phazite
