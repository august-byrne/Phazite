// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks' setters (internal/Tracks.h: setTrack() and those beside it): the one place that changes the tracks' area of a
// live song (the versions' passes and a merge's choice
// write through it too). Each takes what the song holds from the area (getTracksValue(): no element read), works out the next
// value (model/base/internal/TracksValue.h: withTrack(), withoutTrack(), withListOrder(); immer: a path copied, the rest
// shared) and sets it once through the area's setters (model/base/internal/SongValues.h), in the mode of the moment.
#include "internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
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
} // namespace

//==============================================================================
std::optional<TrackValue> findHeldTrack (const Song& song, TrackID id)
{
    if (const auto* value = getTracksValue (song).find (id))
        return *value;

    return std::nullopt;
}

SongTracks getHeldTracks (const Song& song)
{
    return getTracksValue (song);
}

bool setTrack (Song& song, TrackValue value, int index)
{
    auto* values = song.getValues();

    const auto& tracks = values->tracks.get();

    if (value.isMaster())
    {
        // The master's own fields, in its place: refused without an ID.
        if (! value.id.isValid() || tracks.master == value)
            return false;

        return values->setTrack (song, std::move (value));
    }

    if (! canSet (song, tracks, value, index))
        return false;

    return values->setTrack (song, std::move (value), index);
}

bool setMasterAnew (Song& song, const TrackValue& value)
{
    auto* values = song.getValues();

    if (! value.isMaster() || ! value.id.isValid())
        return false;

    // Put in anew whole: told as the master made (SongTracks::masterPutIn), as its element put in anew was.
    auto next = values->tracks.get();
    next.master = value;
    next.master.parent = {};
    ++next.masterPutIn;
    return values->setAllTracks (song, std::move (next));
}

bool setNewTracks (Song& song, const std::vector<TrackValue>& made, int index, IDsHeldElsewhere elsewhere)
{
    auto* values = song.getValues();

    if (made.empty())
        return false;

    // Each one as the song would hold it with those before it, the first at its place, the rest in the folders made before
    // them, after the tracks put in them before; else nothing.
    auto tracks = values->tracks.get();

    for (size_t i = 0; i < made.size(); ++i)
    {
        const auto& value = made[i];

        if (value.isMaster() || tracks.tracks.find (value.id.getRaw()) != nullptr || ! canSet (song, tracks, value, -1, elsewhere))
            return false;

        if (i > 0
            && ! std::any_of (made.begin(), made.begin() + (std::ptrdiff_t) i,
                              [&value] (const TrackValue& before) { return before.id == value.parent; }))
        {
            jassertfalse; // made in no folder made before it
            return false;
        }

        tracks = withTrack (tracks, value, i == 0 ? index : -1);
    }

    return values->setAllTracks (song, std::move (tracks));
}

bool eraseTrack (Song& song, TrackID id)
{
    auto* values = song.getValues();
    return values->eraseTrack (song, id);
}

bool setTrackPlace (Song& song, TrackID id, TrackID folder, int index)
{
    auto* values = song.getValues();

    const auto& tracks = values->tracks.get();
    const auto* held = tracks.tracks.find (id.getRaw());

    if (held == nullptr)
        return false;

    // Where it is already, at that index in that list: nothing written, no step.
    const auto place = findPlaceInList (tracks, folder, id);

    if (held->parent == folder && index >= 0 && place == index)
        return false;

    // Refused where the song couldn't hold it there: a folder that isn't one, a folder into itself or into one in it.
    auto moved = *held;
    moved.parent = folder;

    if (held->parent != folder)
    {
        auto next = withTrack (tracks, std::move (moved), index);

        if (next.isSameAs (tracks))
            return false;

        return values->setAllTracks (song, std::move (next));
    }

    // Within its folder: the list with it at its index there (from 0, before the track at that index now; past the end, or -1,
    // after the last), the same IDs.
    const auto* list = tracks.findList (folder);

    if (list == nullptr || place < 0)
        return false;

    auto order = list->erase ((size_t) place);
    const auto at = index < 0 || index > (int) order.size() ? order.size() : (size_t) index;
    order = order.insert (at, id);
    return values->setTrackListOrder (song, folder, std::move (order));
}

bool setTrackListOrder (Song& song, TrackID folder, const immer::flex_vector<TrackID>& order)
{
    auto* values = song.getValues();
    return values->setTrackListOrder (song, folder, order);
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
    const auto kept = getSongState (song);

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
