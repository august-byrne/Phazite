// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Selection.h"
#include "internal/CurveValues.h"
#include "internal/DevicesValue.h"
#include "internal/Schema.h"
#include "internal/SectionsValue.h"
#include "internal/SendsValue.h"
#include "internal/SongIndex.h"
#include "internal/SongValues.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace phazite
{
namespace
{
    // Whether the song's tree holds a track (or a folder, a return or the master) with an ID, its element's "id"; a device (or
    // a container) is the devices' to say (DevicesValue.h: holdsDevice()), and a clip the clips', which the song holds beside
    // its tree (SongValues.h), found there by its ID.
    [[maybe_unused]] bool holdsTrack (const juce::ValueTree& parent, std::uint64_t id)
    {
        for (const auto& child : parent)
        {
            const auto isTrack = child.hasType (schema::TRACK) || child.hasType (schema::MASTER);

            if (isTrack && (std::uint64_t) (juce::int64) child.getProperty (schema::trackID, (juce::int64) 0) == id)
                return true;

            // Into what holds tracks: the track list and the folders.
            if ((child.hasType (schema::TRACKS) || isTrack) && holdsTrack (child, id))
                return true;
        }

        return false;
    }

    // Whether the song's tree holds a channel's ID, a fader's or a VCA's (Schema.h: channelFader, channelVCA) that a curve's
    // points belong to (Schema.h: CURVE, curveDevice), as model/automation/ finds a curve's owner; a device's and a container's
    // chain's are the devices' (holdsCurveOwner(), below), which the walk leaves to them, and a send's the sends' (holdsSend()),
    // whose area the song holds them in, not its tree.
    bool holdsChannelOwner (const juce::ValueTree& parent, std::uint64_t id)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::AUTOMATION) || readDeviceForm (child).has_value())
                continue;

            for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                if (child.hasProperty (*property) && schema::holdsID (child, *property)
                    && (std::uint64_t) (juce::int64) child[*property] == id)
                    return true;

            if (holdsChannelOwner (child, id))
                return true;
        }

        return false;
    }

    // Whether the song holds what a curve's points belong to: a device, a container's chain (DevicesValue.h: holdsDevice()),
    // or a channel's fader or VCA.
    bool holdsCurveOwner (const Song& song, std::uint64_t id)
    {
        return holdsDevice (song, DeviceID::fromRaw (id), true) || holdsChannelOwner (song.getState(), id);
    }

    std::uint64_t pointComparisons = 0, noteComparisons = 0; // message thread

    // How many of these notes are still in their clips, of those shown: each by its ID in its own clip.
    int countNotesThere (const Song& song, const std::vector<ClipID>& clips, const std::vector<NoteRef>& notes)
    {
        auto count = 0;

        for (const auto& ref : notes)
            if (std::find (clips.begin(), clips.end(), ref.clip) != clips.end() && hasNote (song, ref))
                ++count;

        return count;
    }
} // namespace

bool hasNote (const Song& song, const NoteRef& ref)
{
    // In its clip's value (ClipNotesValue.h), which a clip that isn't the song's has none of; and a song that keeps everything in
    // its tree (one made to be read or checked: Song::getValues()) none.
    const auto* values = song.getValues();
    const auto* clip = values != nullptr ? values->findClipNotes (ref.clip) : nullptr;
    return clip != nullptr && clip->find (ref.note) != nullptr;
}

void Selection::countPointComparisons (std::uint64_t count) noexcept
{
    pointComparisons += count;
}

std::uint64_t Selection::getPointComparisons() noexcept
{
    return pointComparisons;
}

std::uint64_t Selection::getNoteComparisons() noexcept
{
    return noteComparisons;
}

void Selection::setSong (Song* newSong)
{
    if (newSong == heldSong.get())
        return;

    heldSong = newSong;
    clearItems();
    clearNotesAndPoints();
    noteClips.clear();
    changed();
}

bool Selection::isThere (Item item) const
{
    auto* song = getSong();

    if (song == nullptr || item.id == 0)
        return false;

    // A clip in the song's values; a track by the song's index; a device (in a track's chain, the master's or a container's)
    // looked for.
    if (item.kind == Kind::clip)
    {
        const auto* values = song->getValues();
        return values != nullptr && values->findClip (ClipID::fromRaw (item.id)) != nullptr;
    }

    if (item.kind == Kind::device)
        return holdsDevice (*song, DeviceID::fromRaw (item.id));

    const auto there = (std::uint64_t) (juce::int64) song->getState().getChildWithName (schema::MASTER)[schema::trackID] == item.id
                       || song->getIndex().findTrack (item.id).isValid();

    // The index and the song's tree differ.
    jassert (! SongIndex::isChecking() || there == holdsTrack (song->getState(), item.id));
    return there;
}

bool Selection::isThere (SectionID id) const
{
    auto* song = getSong();

    if (song == nullptr || ! id.isValid())
        return false;

    // Among the song's sections, without a value made of them (SectionsValue.h: holdsSection()): a paint asks this of each
    // section in view.
    return holdsSection (*song, id);
}

bool Selection::holdsDevices() const
{
    return ! items.empty() && items.front().kind == Kind::device;
}

void Selection::clearItems()
{
    items.clear();
    sections.clear();
}

bool Selection::clearNotesAndPoints()
{
    const auto had = ! notes.empty() || pointsParameter.isValid();
    notes.clear();
    pointsParameter = {};
    pointKeys.clear();
    sortedPointKeys.clear();
    return had;
}

bool Selection::isSelected (Item item) const
{
    return item.id != 0 && std::find (items.begin(), items.end(), item) != items.end();
}

void Selection::selectOnly (Item item)
{
    if (item.id == 0)
        return;

    if (items == std::vector { item } && sections.empty() && notes.empty() && ! pointsParameter.isValid())
        return;

    clearItems();
    clearNotesAndPoints();
    items = { item };
    changed();
}

void Selection::addToSelection (Item item)
{
    if (item.id == 0 || isSelected (item))
        return;

    // Devices are selected on their own: one added to a selection of anything else starts a new one, and so does anything
    // else added to a selection of devices.
    if ((item.kind == Kind::device) != holdsDevices())
        clearItems();

    clearNotesAndPoints();
    items.push_back (item);
    changed();
}

void Selection::deselect (Item item)
{
    const auto found = std::find (items.begin(), items.end(), item);

    if (item.id == 0 || found == items.end())
        return;

    items.erase (found);
    changed();
}

void Selection::deselectAll()
{
    const auto had = ! items.empty() || ! sections.empty();
    clearItems();

    if (clearNotesAndPoints() || had)
        changed();
}

Selection::Kept Selection::keep() const
{
    return { items, sections, noteClips, notes, pointsParameter, pointKeys };
}

void Selection::putBack (const Kept& kept)
{
    items = kept.items;
    sections = kept.sections;
    noteClips = kept.noteClips;
    notes = kept.notes;
    pointsParameter = kept.pointsParameter;
    pointKeys = kept.pointKeys;
    sortedPointKeys = pointKeys;
    std::sort (sortedPointKeys.begin(), sortedPointKeys.end());
    changed();
}

int Selection::getNumObjectsSelected() const
{
    auto count = 0;

    for (const auto& item : items)
        if (isThere (item))
            ++count;

    for (const auto& id : sections)
        if (isThere (id))
            ++count;

    if (auto* song = getSong(); song != nullptr && ! notes.empty())
        count += countNotesThere (*song, noteClips, notes);

    // Points still in the parameter's curve, of a device the song holds, as getSelectedPoints() (automation/internal/
    // Automation.h) finds them: the curve's points walked once, each key looked for among those selected.
    if (auto* song = getSong(); song != nullptr && ! pointKeys.empty())
    {
        const auto* values = song->getValues();
        const auto* curve = values != nullptr ? values->findCurve (pointsParameter) : nullptr;

        if (curve != nullptr && (holdsCurveOwner (*song, pointsParameter.device.getRaw()) || holdsSend (*song, pointsParameter.device)))
            for (const auto& point : curve->points)
                if (isPointSelected (point.key))
                    ++count;
    }

    return count;
}

std::vector<TrackID> Selection::getTracks() const
{
    return getIDs<TrackID> (Kind::track);
}

std::vector<ClipID> Selection::getClips() const
{
    return getIDs<ClipID> (Kind::clip);
}

std::vector<DeviceID> Selection::getDevices() const
{
    return getIDs<DeviceID> (Kind::device);
}

//==============================================================================
bool Selection::isSelected (SectionID id) const
{
    return std::find (sections.begin(), sections.end(), id) != sections.end() && isThere (id);
}

void Selection::selectOnly (SectionID id)
{
    if (! isThere (id))
        return;

    if (sections == std::vector { id } && items.empty() && notes.empty() && ! pointsParameter.isValid())
        return;

    clearItems();
    clearNotesAndPoints();
    sections = { id };
    changed();
}

void Selection::addToSelection (SectionID id)
{
    if (! isThere (id) || std::find (sections.begin(), sections.end(), id) != sections.end())
        return;

    if (holdsDevices())
        clearItems();

    clearNotesAndPoints();
    sections.push_back (id);
    changed();
}

void Selection::deselect (SectionID id)
{
    const auto found = std::find (sections.begin(), sections.end(), id);

    if (found == sections.end())
        return;

    sections.erase (found);
    changed();
}

//==============================================================================
void Selection::setNoteClips (const std::vector<ClipID>& clips)
{
    if (clips == noteClips)
        return;

    noteClips = clips;

    // Notes of clips no longer shown aren't selected.
    const auto before = notes.size();
    notes.erase (std::remove_if (notes.begin(), notes.end(), [this] (const NoteRef& note)
                                 { return std::find (noteClips.begin(), noteClips.end(), note.clip) == noteClips.end(); }),
                 notes.end());

    if (notes.size() != before)
        changed();
}

void Selection::selectNotes (const std::vector<NoteRef>& refs, bool addToSelection)
{
    // Only notes of the note clips; adding to notes keeps them, and selecting them deselects anything else.
    // Each once: a note chosen is looked for among those chosen by its hash, not compared with each of them.
    std::vector<NoteRef> chosen;
    std::unordered_set<NoteRef> taken;

    if (addToSelection)
    {
        chosen = notes;
        taken.insert (notes.begin(), notes.end());
    }

    for (const auto& ref : refs)
    {
        if (std::find (noteClips.begin(), noteClips.end(), ref.clip) == noteClips.end())
            continue;

        ++noteComparisons;

        if (taken.insert (ref).second)
            chosen.push_back (ref);
    }

    if (chosen == notes && items.empty() && sections.empty() && ! pointsParameter.isValid())
        return;

    clearItems();
    pointsParameter = {};
    pointKeys.clear();
    sortedPointKeys.clear();
    notes = std::move (chosen);
    changed();
}

//==============================================================================
bool Selection::isPointSelected (std::uint64_t key) const
{
    // Among the keys sorted, by a binary search, its comparisons counted.
    std::uint64_t comparisons = 1;
    const auto found = std::lower_bound (sortedPointKeys.begin(), sortedPointKeys.end(), key,
                                         [&comparisons] (std::uint64_t a, std::uint64_t b)
                                         {
                                             ++comparisons;
                                             return a < b;
                                         });
    countPointComparisons (comparisons);
    return key != 0 && found != sortedPointKeys.end() && *found == key;
}

void Selection::selectPoints (const ParameterRef& parameter, const std::vector<std::uint64_t>& keys)
{
    // Each once, in the order given: sorted aside, so that a key is compared with its neighbours alone, and the later of two
    // alike left out.
    std::uint64_t comparisons = 0;
    std::vector<std::uint64_t> sorted;
    sorted.reserve (keys.size());

    for (const auto key : keys)
        if (key != 0)
            sorted.push_back (key);

    std::sort (sorted.begin(), sorted.end(),
               [&comparisons] (std::uint64_t a, std::uint64_t b)
               {
                   ++comparisons;
                   return a < b;
               });
    sorted.erase (std::unique (sorted.begin(), sorted.end()), sorted.end());

    std::vector<std::uint64_t> chosen;
    chosen.reserve (sorted.size());
    std::vector<bool> taken (sorted.size(), false);

    for (const auto key : keys)
    {
        if (key == 0)
            continue;

        const auto place = (size_t) (std::lower_bound (sorted.begin(), sorted.end(), key,
                                                       [&comparisons] (std::uint64_t a, std::uint64_t b)
                                                       {
                                                           ++comparisons;
                                                           return a < b;
                                                       })
                                     - sorted.begin());

        if (! taken[place])
        {
            taken[place] = true;
            chosen.push_back (key);
        }
    }

    countPointComparisons (comparisons);

    if (! parameter.isValid() || chosen.empty())
    {
        if (pointsParameter.isValid())
        {
            pointsParameter = {};
            pointKeys.clear();
            sortedPointKeys.clear();
            changed();
        }

        return;
    }

    if (parameter == pointsParameter && chosen == pointKeys && items.empty() && sections.empty() && notes.empty())
        return;

    clearItems();
    notes.clear();
    pointsParameter = parameter;
    pointKeys = std::move (chosen);
    sortedPointKeys = std::move (sorted);
    changed();
}
} // namespace phazite
