// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ApplySong.h"
#include "internal/SongDiff.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongText.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"

#include <functional>
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

    // A copy of an element of the other song, to be added to the song, without what in it the other's version couldn't read:
    // the stand-ins, and the elements that stand in for themselves (a clip a run of whose notes was lost). The song hasn't
    // them, so what they held stays as it is now: not there. A stand-in is never part of a song (Schema.h:
    // schema::versions::missing).
    juce::ValueTree copyWithoutStandIns (const juce::ValueTree& element)
    {
        auto copy = element.createCopy();
        const std::function<void (juce::ValueTree)> strip = [&] (juce::ValueTree tree)
        {
            for (int i = tree.getNumChildren(); --i >= 0;)
            {
                if (auto child = tree.getChild (i); isStandIn (child))
                    tree.removeChild (i, nullptr);
                else
                    strip (child);
            }
        };

        strip (copy);
        return copy;
    }

    // A clip's notes and controllers' values, which the notes' code makes like the other's (Notes.h: makeClipNotesLike()).
    bool isClipEvent (const juce::ValueTree& element, const juce::ValueTree& child)
    {
        return element.hasType (schema::CLIP) && (child.hasType (schema::NOTE) || child.hasType (schema::CONTROL));
    }

    // What the audio clips of an alias group share (AudioClips.h: shareTheSound(): file, tempo, pitch, direction, warp markers),
    // given by each audio clip of a group in an element made like the other song's, or added from it, to every clip of its
    // group in the song, as an edit through one of them gives it: those on other tracks too, which one track's line leaves as
    // they were. A clip's notes reach its group through the notes' code (Notes.h: makeClipNotesLike(), and the song's keeper
    // for a clip added, SongValues.h).
    void giveSharedSounds (Song& song, const juce::ValueTree& element, juce::UndoManager* undo)
    {
        if (! element.hasType (schema::CLIP))
        {
            for (const auto& child : element)
                giveSharedSounds (song, child, undo);

            return;
        }

        if (isAudioState (element) && element[schema::clipLink].toString().isNotEmpty())
            for (auto alias : getAliasStates (song, element))
                if (alias != element)
                    giveSharedSound (element, alias, undo);
    }

    // Whether makeLike() places a child among the element's: taken, a track only withTracks, never a clip's event.
    bool isPlaced (const juce::ValueTree& element, const juce::ValueTree& child, bool withTracks, Taken taken)
    {
        return isTaken (taken, element, child) && (withTracks || ! child.hasType (schema::TRACK)) && ! isClipEvent (element, child);
    }

    std::vector<Child> getCountedChildren (const juce::ValueTree& element, bool withTracks = true, Taken taken = Taken::counted)
    {
        std::vector<Child> children;
        std::map<juce::String, int> numUnkeyed;
        std::set<juce::String> names;

        for (const auto& child : element)
        {
            if (! isPlaced (element, child, withTracks, taken))
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
    // which are things of their own). A clip's notes and controllers' values by the notes' code, in the same history (the
    // song's, or none while the song is written outside it).
    void makeLike (Song& song, juce::ValueTree live, const juce::ValueTree& other, juce::UndoManager* undo, bool withTracks = true,
                   Taken taken = Taken::counted, const StandIns& standIns = noStandIns())
    {
        jassert ((undo == nullptr) == song.isWritingOutsideHistory());

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
            while (place < live.getNumChildren() && ! isPlaced (live, live.getChild (place), withTracks, taken))
                ++place;

            if (const auto found = kept.find (child.name); found != kept.end())
            {
                // What the other's version couldn't read stays as it is, in its place.
                auto tree = found->second;

                if (! isStandIn (child.tree))
                    makeLike (song, tree, child.tree, undo, true, taken, standIns);

                if (const auto index = live.indexOf (tree); index != place)
                    live.moveChild (index, place, undo);
            }
            else if (! isStandIn (child.tree))
            {
                auto added = copyWithoutStandIns (child.tree);
                live.addChild (added, place, undo);
                giveSharedSounds (song, added, undo);
            }
            else
            {
                continue; // a stand-in for what the song hasn't: nothing to put there
            }

            ++place;
        }

        if (live.hasType (schema::CLIP))
        {
            const auto id = readID (live[schema::clipID]);
            jassert (song.getIndex().findClip (id) == live); // the song's one clip of its ID
            makeClipNotesLike (song, ClipID::fromRaw (id), other);
            giveSharedSounds (song, live, undo);
        }
    }

    // Where each item is in a song, or in a part of it (its parent's type and key), by its own type and ID: notes aside,
    // whose IDs are their clips' own, and, while withTracks is off, the tracks in it (a folder's, things of their own).
    using Places = std::map<juce::String, juce::String>;

    Places findPlaces (const juce::ValueTree& tree, bool withTracks = true)
    {
        Places places;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (! withTracks && child.hasType (schema::TRACK))
                    continue;

                if (! child.hasType (schema::NOTE) && schema::holdsID (child, schema::trackID))
                    places[child.getType().toString() + ":" + child[schema::trackID].toString()] =
                        element.getType().toString() + ":" + schema::getElementKey (element);

                visit (child);
            }
        };

        visit (tree);
        return places;
    }

    // The song's items that are somewhere else in the other song, or in the part of it about to be taken (a track moved
    // into a folder, a clip onto another track, a device into a container), taken away first, to be put where the other
    // has them: no ID is in the song twice, meanwhile or after.
    void removeMovedItems (juce::ValueTree live, const Places& otherPlaces, juce::UndoManager* undo,
                           const StandIns& standIns = noStandIns())
    {
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

    // What holds items in a song, by its type and key as findPlaces() names it ("TRACK:12", "CHAIN:40"): a track, the
    // master, a chain; invalid if the song has none.
    juce::ValueTree findHolder (const juce::ValueTree& song, const juce::String& place)
    {
        for (const auto& child : song)
        {
            if (child.hasType (schema::NOTE))
                continue;

            if (schema::holdsID (child, schema::trackID) && nameOf (child) == place)
                return child;

            if (const auto found = findHolder (child, place); found.isValid())
                return found;
        }

        return {};
    }

    // Where an item goes among a holder's children to be where the other song has it among its holder's there: after the
    // one before it there that this holder has; else before the others of its order (a chain's devices), or last.
    int findIndexLike (const juce::ValueTree& home, const juce::ValueTree& item, const juce::ValueTree& homeThere)
    {
        std::map<juce::String, int> held;

        for (int i = 0; i < home.getNumChildren(); ++i)
            if (const auto child = home.getChild (i); schema::getElementKey (child).isNotEmpty())
                held.emplace (nameOf (child), i);

        juce::ValueTree there;

        for (const auto& child : homeThere)
            if (nameOf (child) == nameOf (item))
                there = child;

        for (auto before = there.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            if (const auto found = held.find (nameOf (before)); found != held.end())
                return found->second + 1;

        if (const auto group = schema::getOrderGroup (item); group.isNotEmpty())
            for (int i = 0; i < home.getNumChildren(); ++i)
                if (schema::getOrderGroup (home.getChild (i)) == group)
                    return i;

        return -1;
    }

    // What a track holds now that the other song holds somewhere else (a clip moved onto it since, from another track),
    // sent there, as it is now, before the track is made like the other's or taken away: it would be on neither track
    // otherwise. Where the song has nothing to hold it now (the track it was on deleted since), it goes with the rest, and
    // that track's own line brings it back.
    void sendItemsHome (juce::ValueTree state, const juce::ValueTree& track, const juce::ValueTree& other, juce::UndoManager* undo)
    {
        const auto otherPlaces = findPlaces (other);
        std::vector<std::pair<juce::ValueTree, juce::ValueTree>> going; // each with what holds it there, in the song
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::TRACK) || child.hasType (schema::NOTE))
                    continue;

                if (schema::holdsID (child, schema::trackID))
                {
                    if (const auto found = otherPlaces.find (nameOf (child));
                        found != otherPlaces.end() && found->second != nameOf (element))
                    {
                        if (const auto home = findHolder (state, found->second); home.isValid() && ! home.isAChildOf (child))
                            going.emplace_back (child, home);

                        continue;
                    }
                }

                visit (child);
            }
        };

        visit (track);

        for (auto& [item, home] : going)
        {
            const auto index = findIndexLike (home, item, findHolder (other, nameOf (home)));
            item.getParent().removeChild (item, undo);
            home.addChild (item, index, undo);
        }
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

    // A copy of a track of the other song's, a folder's, without the tracks in it, however deep, that the song has
    // somewhere now: each track is in the song once.
    void removeTracksHeld (juce::ValueTree copy, const juce::ValueTree& song)
    {
        for (int i = copy.getNumChildren(); --i >= 0;)
        {
            auto child = copy.getChild (i);

            if (! child.hasType (schema::TRACK))
                continue;

            if (findTrack (song, readID (child[schema::trackID])).isValid())
                copy.removeChild (i, nullptr);
            else
                removeTracksHeld (child, song);
        }
    }

    // The song's count of IDs brought past every ID it holds now and past the other's count: what it took from the other
    // can hold IDs past its own count (a song opened again as it was saved before they were given), and the next item it
    // makes gets an ID that nothing of either has. Not an undo step, as the count never is.
    void countIDsPast (Song& song, const juce::ValueTree& other)
    {
        auto state = song.getState();
        const auto next = std::max (findNextID (state), findNextID (other));

        if ((std::uint64_t) (juce::int64) state.getProperty (schema::nextID, (juce::int64) 0) != next)
            state.setProperty (schema::nextID, (juce::int64) next, nullptr);
    }

    // The curves of these devices like the other's.
    void makeCurvesLike (Song& song, juce::ValueTree state, juce::UndoManager* undo, const std::set<std::uint64_t>& devices,
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
                    makeLike (song, curve, found->second, undo);

                wanted.erase (found);
            }
            else
            {
                automation.removeChild (curve, undo);
            }
        }

        for (const auto& [key, curve] : wanted)
            if (! isStandIn (curve))
                automation.appendChild (copyWithoutStandIns (curve), undo);
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
    removeMovedItems (song.getState(), findPlaces (other), undo, standIns);
    makeLike (song, song.getState(), other, undo, true, Taken::counted, standIns);
    countIDsPast (song, other);
}

void makeSongTheSame (Song& song, const juce::ValueTree& other)
{
    // Outside the history, as what the song keeps beside its tree is written too (Song::OutsideHistory).
    const Song::OutsideHistory outside (song);

    // Its count of IDs the larger of the two: what either branch made keeps an ID of its own.
    const auto next = std::max (findNextID (song.getState()), findNextID (other));
    removeMovedItems (song.getState(), findPlaces (other), nullptr);
    makeLike (song, song.getState(), other, nullptr, true, Taken::everything);
    song.getState().setProperty (schema::nextID, (juce::int64) next, nullptr);
    song.getUndoManager().clearUndoHistory();
}

void makeSongAsCopied (Song& song, const juce::ValueTree& copy)
{
    const Song::OutsideHistory outside (song);

    // Everything, outside the history, and its count of IDs as the copy's: what the history holds is the copy's already, so
    // that only what changed without it differs (a property set without a step, view state, the count of IDs).
    makeLike (song, song.getState(), copy, nullptr, true, Taken::everything);
    song.getState().setProperty (schema::nextID, copy[schema::nextID], nullptr);
}

namespace
{
    bool makeTrackLikeIn (Song& song, juce::ValueTree state, TrackID id, const juce::ValueTree& other, juce::UndoManager* undo)
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
            // What it held then that is somewhere else now (a clip moved onto another track) comes back from there, and what
            // it holds now that the other song holds somewhere else goes there.
            const auto standIns = findStandIns (then);
            removeMovedItems (state, findPlaces (then, false), undo, standIns);
            sendItemsHome (state, live, other, undo);
            makeLike (song, live, then, undo, false, Taken::counted, standIns);
        }
        else if (live.isValid())
        {
            // Added since: taken away. The tracks in it (a folder's) are things of their own, which stay: where it was, in
            // their order, as ungrouping a folder leaves them. And what it holds that the other song holds somewhere else (a
            // clip moved onto it) goes there.
            sendItemsHome (state, live, other, undo);
            auto parent = live.getParent();
            auto place = parent.indexOf (live) + 1;

            for (int i = 0; i < live.getNumChildren();)
            {
                if (auto child = live.getChild (i); child.hasType (schema::TRACK))
                {
                    live.removeChild (child, undo);
                    parent.addChild (child, place++, undo);
                }
                else
                {
                    ++i;
                }
            }

            parent.removeChild (live, undo);
        }
        else
        {
            // Gone since: back where it was, after the track before it there, with the tracks it held that aren't elsewhere
            // now, and what they held that is elsewhere now back from there; without what the other's version couldn't read
            // (a clip of it), which isn't in the song now either.
            auto parent = findTrackParent (state, then.getParent());
            auto copy = copyWithoutStandIns (then);
            removeTracksHeld (copy, state);
            removeMovedItems (state, findPlaces (copy), undo);

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
            giveSharedSounds (song, copy, undo);
        }

        makeCurvesLike (song, state, undo, devices, other);
        return true;
    }

    void makeSongOwnLikeIn (Song& song, juce::ValueTree state, const juce::ValueTree& other, juce::UndoManager* undo)
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

            // One the other's version couldn't read (a stand-in, which only a piece that isn't Phazite's can make of these)
            // stays as it is.
            if (then.isValid() && isStandIn (then))
                continue;

            if (live.isValid() && then.isValid())
                makeLike (song, live, then, undo, true, Taken::counted, findStandIns (then));
            else if (live.isValid())
                state.removeChild (live, undo);
            else if (then.isValid())
                state.appendChild (copyWithoutStandIns (then), undo);
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
    const auto done = makeTrackLikeIn (song, song.getState(), id, other, &song.getUndoManager());
    countIDsPast (song, other);
    return done;
}

void makeSongOwnLike (Song& song, const juce::ValueTree& other)
{
    makeSongOwnLikeIn (song, song.getState(), other, &song.getUndoManager());
    countIDsPast (song, other);
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

    // And what an edit takes away with what it names (model/mixer/internal/Mixer.h): a send to a return that is gone, with
    // what modulated it, and a pad's track whose drum sampler is gone; so that the song is as makeConsistent() leaves it.
    // Then a sidechain that would hear the device's own sound round a loop, which two lines' changes can make together (one
    // a sidechain, the other a send), as an edit that makes one lets it go (Sidechain.h: forgetSidechainLoops()).
    const auto count = [&state] (const std::function<bool (const juce::ValueTree&)>& counts)
    {
        auto found = 0;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                found += counts (child) ? 1 : 0;
                visit (child);
            }
        };

        visit (state);
        return found;
    };

    const auto isSend = [] (const juce::ValueTree& element) { return element.hasType (schema::SEND); };
    const auto hasSidechain = [] (const juce::ValueTree& element)
    { return element.hasType (schema::DEVICE) && element.hasProperty (schema::deviceSidechain); };

    const auto sendsBefore = count (isSend);
    forgetWhatNamesTheGone (song);
    const auto sidechainsBefore = count (hasSidechain);
    forgetSidechainLoops (song);

    return (int) (modulations.size() + sidechains.size()) + sendsBefore - count (isSend) + sidechainsBefore - count (hasSidechain);
}
} // namespace phazite
