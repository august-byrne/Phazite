// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks' setters (internal/Tracks.h: setTrack() and those beside it), while the song's tree is the tracks' storage: the
// one place, with the conversion, the tracks' part of the keeper and the loader, that writes a TRACK's or the MASTER's own
// properties or the track lists of a live song (the versions' passes and a merge's choice write through it too). Each takes
// what the song holds from the
// tracks' keeper (SongValues::getTracksAsTold(): no element read), finds the elements it writes through the song's index, and
// writes them by what differs through the conversion (model/base/internal/TracksValue.h: writeTrackChanges(),
// writeTrackElement()), in the mode of the moment.
#include "internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getTreeUndo (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // The element whose TRACK children are a folder's list: the folder's, or the track list for none (made if the song has
    // none yet, as every song has one: getTrackList()). Invalid for a folder the song hasn't.
    juce::ValueTree findListElement (Song& song, TrackID folder)
    {
        if (! folder.isValid())
            return getTrackList (song);

        const auto element = findTrackState (song, folder);
        return element.hasType (schema::TRACK) ? element : juce::ValueTree();
    }

    // The child index at which a track goes among a list's element's children, for its place in the list once it is there
    // (internal/Tracks.h: from 0, right after the track before it, before the first for 0; -1 for after the element's last
    // child), the track that moves (`moving`, still among them) left out of the list.
    int findChildIndex (const juce::ValueTree& parent, int index, const juce::ValueTree& moving)
    {
        const auto numChildren = parent.getNumChildren();

        if (index < 0)
            return numChildren;

        auto count = 0, first = -1, last = -1, previous = -1;

        for (int i = 0; i < numChildren; ++i)
        {
            const auto child = parent.getChild (i);

            if (! child.hasType (schema::TRACK) || child == moving)
                continue;

            if (first < 0)
                first = i;

            if (count == index - 1)
                previous = i;

            last = i;
            ++count;
        }

        if (index == 0)
            return first >= 0 ? first : numChildren;

        if (previous >= 0)
            return previous + 1;

        return last >= 0 ? last + 1 : numChildren; // past the list's end: after its last
    }

    // Whether an ID is one the song holds as something other than a track: a fader's or a VCA's (a track's, the master's), or
    // a device's, a container's or a chain's (but where the caller lets it go with the devices' area: IDsHeldElsewhere).
    bool isHeldAsOther (const Song& song, const SongTracks& tracks, TrackID id, IDsHeldElsewhere elsewhere)
    {
        const auto raw = id.getRaw();
        auto held = tracks.master.fader.getRaw() == raw || tracks.master.vca.getRaw() == raw;

        for (const auto& [key, value] : tracks.tracks)
            held = held || value.fader.getRaw() == raw || value.vca.getRaw() == raw;

        return held || (elsewhere == IDsHeldElsewhere::refused && findDevice (song, DeviceID::fromRaw (raw)) != nullptr);
    }

    // A track's index in its folder's list (the top list for none) as the song holds it; -1 for one it hasn't there.
    int findPlaceInList (const SongTracks& tracks, TrackID folder, TrackID id)
    {
        if (const auto* list = tracks.findList (folder))
            for (size_t i = 0; i < list->size(); ++i)
                if ((*list)[i] == id)
                    return (int) i;

        return -1;
    }

    // Whether a track's value (not the master's: setTrack() checks the master's on its own path, and setNewTracks() refuses it)
    // can be set as the song holds its tracks: what withTrack() refuses (TracksValue.h), and a new track of an ID the song
    // holds as something else.
    bool canSet (const Song& song, const SongTracks& tracks, const TrackValue& value, int index,
                 IDsHeldElsewhere elsewhere = IDsHeldElsewhere::refused)
    {
        jassert (! value.isMaster());

        if (withTrack (tracks, value, index).isSameAs (tracks))
            return false;

        return tracks.tracks.find (value.id.getRaw()) != nullptr || ! isHeldAsOther (song, tracks, value.id, elsewhere);
    }

    // A track's element written from its value (writeTrackElement()), its properties in the order of another element's where
    // it has them, the rest after in the writer's order: a version's track put in the song as the version's whole copy writes
    // it, to the letter, while the song's tree is the tracks' storage (an element's text holds its properties in its order).
    // The writer's own element for none. It and writeChangesLike() (the writers' writtenLike) serve only while the tree stores
    // a TRACK's properties in an order (the versions' list of changes names a track's properties in its element's order), and
    // go with the tracks' switch (roadmap step S5.20).
    juce::ValueTree writeElementLike (const TrackValue& value, const juce::ValueTree& like)
    {
        auto written = writeTrackElement (value);

        if (! like.isValid())
            return written;

        juce::ValueTree ordered (written.getType());

        for (int i = 0; i < like.getNumProperties(); ++i)
            if (const auto name = like.getPropertyName (i); written.hasProperty (name))
                ordered.setProperty (name, written[name], nullptr);

        for (int i = 0; i < written.getNumProperties(); ++i)
            if (const auto name = written.getPropertyName (i); ! ordered.hasProperty (name))
                ordered.setProperty (name, written[name], nullptr);

        return ordered;
    }

    // A track's (or the master's) element written by what differs (writeTrackChanges()); given another element (a version's),
    // what it takes away taken away first, from the element's last property back (so that an Undo puts each back where it
    // was), and what it adds added in that element's order, as the passes over elements wrote them, so that the song's text
    // holds the version's to the letter. Without one, writeTrackChanges() alone.
    bool writeChangesLike (juce::ValueTree& element, const TrackValue& held, const TrackValue& next, juce::UndoManager* undo,
                           const juce::ValueTree& like)
    {
        auto changed = false;

        if (like.isValid() && ! (held == next))
        {
            const auto was = writeTrackElement (held), will = writeTrackElement (next);

            for (int i = element.getNumProperties(); --i >= 0;)
            {
                if (const auto name = element.getPropertyName (i); was.hasProperty (name) && ! will.hasProperty (name))
                {
                    element.removeProperty (name, undo);
                    changed = true;
                }
            }

            for (int i = 0; i < like.getNumProperties(); ++i)
            {
                if (const auto name = like.getPropertyName (i); will.hasProperty (name) && ! element.hasProperty (name))
                {
                    element.setProperty (name, will[name], undo);
                    changed = true;
                }
            }
        }

        return writeTrackChanges (element, held, next, undo) || changed;
    }

    // A track's element moved to a place in a list's element, as the same element.
    bool moveElement (Song& song, juce::ValueTree element, juce::ValueTree target, int index)
    {
        auto* undo = getTreeUndo (song);

        if (element.getParent() == target)
        {
            const auto from = target.indexOf (element);
            const auto before = findChildIndex (target, index, element);
            const auto to = juce::jlimit (0, target.getNumChildren() - 1, before > from ? before - 1 : before);

            if (to == from)
                return false;

            target.moveChild (from, to, undo);
            return true;
        }

        const auto at = findChildIndex (target, index, {});
        element.getParent().removeChild (element, undo);
        target.addChild (element, at, undo);
        return true;
    }
} // namespace

//==============================================================================
std::optional<TrackValue> findHeldTrack (const Song& song, TrackID id)
{
    if (const auto* values = song.getValues())
    {
        if (const auto* told = values->getTracksAsTold())
        {
            if (const auto* value = told->find (id))
                return *value;

            return std::nullopt;
        }
    }

    return readTrackValue (findTrackState (song, id));
}

SongTracks getHeldTracks (const Song& song)
{
    if (const auto* values = song.getValues())
        if (const auto* told = values->getTracksAsTold())
            return *told;

    return getTracksValue (song);
}

bool setTrack (Song& song, TrackValue value, int index, const juce::ValueTree& writtenLike)
{
    const auto tracks = getHeldTracks (song);

    if (value.isMaster())
    {
        auto master = song.getState().getChildWithName (schema::MASTER);

        if (! master.isValid() || ! value.id.isValid() || tracks.master == value)
            return false;

        return writeChangesLike (master, tracks.master, value, getTreeUndo (song), writtenLike);
    }

    if (! canSet (song, tracks, value, index))
        return false;

    const auto* held = tracks.tracks.find (value.id.getRaw());

    // A new one, from its value, at its place.
    if (held == nullptr)
    {
        auto parent = findListElement (song, value.parent);

        if (! parent.isValid())
            return false;

        parent.addChild (writeElementLike (value, writtenLike), findChildIndex (parent, index, {}), getTreeUndo (song));
        return true;
    }

    if (*held == value)
        return false;

    auto element = findTrackState (song, value.id);

    if (! element.hasType (schema::TRACK))
        return false;

    // One that comes to another folder: moved there as the same element, with all it holds.
    auto moved = false;

    if (held->parent != value.parent)
    {
        const auto target = findListElement (song, value.parent);

        if (! target.isValid())
            return false;

        moved = moveElement (song, element, target, index);
    }

    return writeChangesLike (element, *held, value, getTreeUndo (song), writtenLike) || moved;
}

bool setMasterAnew (Song& song, const TrackValue& value, const juce::ValueTree& writtenLike)
{
    auto state = song.getState();
    const auto master = state.getChildWithName (schema::MASTER);

    if (! master.isValid() || ! value.isMaster() || ! value.id.isValid())
        return false;

    // In the place of the one it replaces among the song's elements.
    const auto index = state.indexOf (master);
    auto* undo = getTreeUndo (song);
    state.removeChild (index, undo);
    state.addChild (writeElementLike (value, writtenLike), index, undo);
    return true;
}

bool setNewTracks (Song& song, const std::vector<TrackValue>& made, int index, const std::vector<juce::ValueTree>& writtenLike,
                   IDsHeldElsewhere elsewhere)
{
    if (made.empty())
        return false;

    // Each one as the song would hold it with those before it, else nothing.
    auto tracks = getHeldTracks (song);

    for (const auto& value : made)
    {
        if (value.isMaster() || tracks.tracks.find (value.id.getRaw()) != nullptr || ! canSet (song, tracks, value, -1, elsewhere))
            return false;

        tracks = withTrack (tracks, value, -1);
    }

    auto parent = findListElement (song, made.front().parent);

    if (! parent.isValid())
        return false;

    // The first with the rest in it, each in its folder's element after those put there before.
    std::vector<std::pair<TrackID, juce::ValueTree>> folders;

    for (size_t i = 0; i < made.size(); ++i)
    {
        const auto& value = made[i];
        auto element = writeElementLike (value, i < writtenLike.size() ? writtenLike[i] : juce::ValueTree());

        if (! folders.empty())
        {
            const auto found =
                std::find_if (folders.begin(), folders.end(), [&value] (const auto& each) { return each.first == value.parent; });

            if (found == folders.end())
            {
                jassertfalse; // made in no folder made before it
                return false;
            }

            found->second.appendChild (element, nullptr);
        }

        folders.emplace_back (value.id, element);
    }

    parent.addChild (folders.front().second, findChildIndex (parent, index, {}), getTreeUndo (song));
    return true;
}

bool eraseTrack (Song& song, TrackID id)
{
    auto element = findTrackState (song, id);

    if (! element.hasType (schema::TRACK))
        return false;

    element.getParent().removeChild (element, getTreeUndo (song));
    return true;
}

bool setTrackPlace (Song& song, TrackID id, TrackID folder, int index)
{
    const auto tracks = getHeldTracks (song);
    const auto* held = tracks.tracks.find (id.getRaw());

    if (held == nullptr)
        return false;

    // Where it is already, at that index in that list: nothing written, so that no step only moves what isn't a track (an
    // element Phazite doesn't know) past it.
    if (held->parent == folder && index >= 0 && findPlaceInList (tracks, folder, id) == index)
        return false;

    // Refused where the song couldn't hold it there: a folder that isn't one, a folder into itself or into one in it.
    auto moved = *held;
    moved.parent = folder;

    if (withTrack (tracks, moved, index).isSameAs (tracks))
        return false;

    const auto element = findTrackState (song, id);
    const auto target = findListElement (song, folder);

    if (! element.hasType (schema::TRACK) || ! target.isValid())
        return false;

    return moveElement (song, element, target, index);
}

bool setTrackListOrder (Song& song, TrackID folder, const immer::flex_vector<TrackID>& order)
{
    const auto tracks = getHeldTracks (song);

    if (withListOrder (tracks, folder, order).isSameAs (tracks))
        return false;

    // Each in its turn at its place, from the first: those before it are where they go.
    auto moved = false;

    for (size_t i = 0; i < order.size(); ++i)
        moved = setTrackPlace (song, order[i], folder, (int) i) || moved;

    return moved;
}

bool setTrackFields (Song& song, TrackID id, const std::function<void (TrackValue&)>& edit)
{
    const auto held = findHeldTrack (song, id);

    if (! held.has_value())
        return false;

    auto next = *held;
    edit (next);
    return ! (next == *held) && setTrack (song, std::move (next));
}

TrackCopies makeTrackCopies (Song& song, TrackID original, CopiedIDs& copied)
{
    const auto tracks = getHeldTracks (song);
    const auto* held = tracks.tracks.find (original.getRaw());
    TrackCopies made;

    if (held == nullptr)
        return made;

    // Each track's IDs as a copy of its element was given them (Schema.h: giveNewIDs(), depth first): its own, its fader's
    // and its VCA's, then its devices' as whole state writes them into it (DevicesArea.h: writeDevicesIn(), each item in
    // its order, a container before its chains), then the tracks in it, each the same.
    const auto kept = keepValues (song);

    const auto newID = [&song, &copied] (std::uint64_t old)
    {
        const auto id = song.makeID();
        copied[old] = id;
        return id;
    };

    const std::function<void (const TrackValue&, TrackID)> add = [&] (const TrackValue& from, TrackID parent)
    {
        auto copy = from;
        copy.parent = parent;
        copy.id = TrackID::fromRaw (newID (from.id.getRaw()));
        copy.fader = from.fader.isValid() ? DeviceID::fromRaw (newID (from.fader.getRaw())) : DeviceID();
        copy.vca = from.vca.isValid() ? DeviceID::fromRaw (newID (from.vca.getRaw())) : DeviceID();

        // Nor does a copy of a pad's track play the pad, which plays on the original.
        copy.padDevice = {};
        copy.pad.reset();

        juce::ValueTree devices (schema::TRACK);
        devices.setProperty (schema::trackID, (juce::int64) from.id.getRaw(), nullptr);
        writeDevicesIn (devices, kept);
        devices.removeProperty (schema::trackID, nullptr);

        for (const auto& [old, given] : giveNewIDs (song, devices))
            copied[old] = given;

        made.values.push_back (copy);
        made.originalsAndCopies.emplace_back (from.id, copy.id);

        if (const auto* list = tracks.findList (from.id))
            for (const auto inside : *list)
                if (const auto* value = tracks.tracks.find (inside.getRaw()))
                    add (*value, copy.id);
    };

    add (*held, held->parent);
    return made;
}
} // namespace phazite
