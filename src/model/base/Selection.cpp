// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Selection.h"
#include "internal/Schema.h"
#include "internal/SongIndex.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace phazite
{
namespace
{
    // Whether the song holds an item of a kind with an ID: a track (or a folder, a return or the master), a clip on a track,
    // or a device (or a container) of a track, of the master or of a container. Each kind's ID is its element's "id".
    bool holds (const juce::ValueTree& parent, Selection::Kind kind, std::uint64_t id)
    {
        for (const auto& child : parent)
        {
            const auto isTrack = child.hasType (schema::TRACK) || child.hasType (schema::MASTER);
            const auto isOfKind = kind == Selection::Kind::track  ? isTrack
                                  : kind == Selection::Kind::clip ? child.hasType (schema::CLIP)
                                                                  : child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER);

            if (isOfKind && (std::uint64_t) (juce::int64) child.getProperty (schema::trackID, (juce::int64) 0) == id)
                return true;

            // Into what holds items: the track list, tracks, containers and their chains, not clips' notes or devices' values.
            if ((child.hasType (schema::TRACKS) || isTrack || child.hasType (schema::CONTAINER) || child.hasType (schema::CHAIN))
                && holds (child, kind, id))
                return true;
        }

        return false;
    }

    // Whether the song holds what a curve's points belong to (Schema.h: CURVE, curveDevice): a device, a container's chain, a
    // send, or a channel's fader or VCA, as model/automation/ finds a curve's owner.
    bool holdsCurveOwner (const juce::ValueTree& parent, std::uint64_t id)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::CLIP) || child.hasType (schema::AUTOMATION))
                continue;

            for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                if (child.hasProperty (*property) && schema::holdsID (child, *property)
                    && (std::uint64_t) (juce::int64) child[*property] == id)
                    return true;

            if (holdsCurveOwner (child, id))
                return true;
        }

        return false;
    }

    std::uint64_t pointComparisons = 0, noteComparisons = 0; // message thread

    // How many of these notes are still in their clips, of those shown: each by its ID in its own clip.
    int countNotesThere (const Song& song, const std::vector<ClipID>& clips, const std::vector<NoteRef>& notes)
    {
        const auto& index = song.getIndex();
        auto count = 0;

        for (const auto& ref : notes)
            if (std::find (clips.begin(), clips.end(), ref.clip) != clips.end()
                && index.findNote (index.findClip (ref.clip.getRaw()), ref.note.getRaw()).isValid())
                ++count;

        return count;
    }
} // namespace

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

    // A track or a clip by the song's index; a device (in a track's chain, the master's or a container's) looked for.
    if (item.kind == Kind::device)
        return holds (song->getState(), item.kind, item.id);

    const auto& index = song->getIndex();
    const auto there = item.kind == Kind::clip
                           ? index.findClip (item.id).isValid()
                           : (std::uint64_t) (juce::int64) song->getState().getChildWithName (schema::MASTER)[schema::trackID] == item.id
                                 || index.findTrack (item.id).isValid();

    jassert (! SongIndex::isChecking() || there == holds (song->getState(), item.kind, item.id)); // the index and the song differ
    return there;
}

bool Selection::isThere (SectionID id) const
{
    auto* song = getSong();

    if (song == nullptr || ! id.isValid())
        return false;

    for (const auto& section : song->getState().getChildWithName (schema::SECTIONS))
        if ((std::uint64_t) (juce::int64) section[schema::sectionID] == id.getRaw())
            return true;

    return false;
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
    points.clear();
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
    // Automation.h) finds them.
    if (auto* song = getSong(); song != nullptr && ! points.empty())
    {
        const auto automation = song->getState().getChildWithName (schema::AUTOMATION);
        const auto device = juce::String ((juce::int64) pointsParameter.device.getRaw());
        const auto isTheCurve = [&] (const juce::ValueTree& curve)
        {
            return curve.hasType (schema::CURVE) && curve.getParent() == automation && curve[schema::curveDevice].toString() == device
                   && curve[schema::curveParameter].toString() == pointsParameter.parameter;
        };

        if (holdsCurveOwner (song->getState(), pointsParameter.device.getRaw()))
            for (const auto& point : points)
                if (isTheCurve (point.getParent()))
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
    points.clear();
    notes = std::move (chosen);
    changed();
}

//==============================================================================
void Selection::selectPoints (const ParameterRef& parameter, const std::vector<juce::ValueTree>& pointStates)
{
    // Each once, in the order given: sorted by beat first, so that only points at one beat are compared with each other.
    std::vector<size_t> order;

    for (size_t i = 0; i < pointStates.size(); ++i)
        if (pointStates[i].isValid())
            order.push_back (i);

    const auto beatOf = [&pointStates] (size_t i) { return (double) pointStates[i][schema::pointBeat]; };
    std::uint64_t comparisons = 0;
    std::stable_sort (order.begin(), order.end(),
                      [&] (size_t a, size_t b)
                      {
                          ++comparisons;
                          return beatOf (a) < beatOf (b);
                      });

    std::vector<bool> repeated (pointStates.size(), false);

    for (size_t run = 0; run < order.size();)
    {
        auto end = run + 1;

        while (end < order.size() && juce::exactlyEqual (beatOf (order[end]), beatOf (order[run])))
            ++end;

        for (auto i = run + 1; i < end; ++i)
            for (auto j = run; j < i && ! repeated[order[i]]; ++j)
                if (++comparisons; ! repeated[order[j]] && pointStates[order[i]] == pointStates[order[j]])
                    repeated[order[i]] = true; // stable: the later of the two as given

        run = end;
    }

    countPointComparisons (comparisons);
    std::vector<juce::ValueTree> chosen;

    for (size_t i = 0; i < pointStates.size(); ++i)
        if (pointStates[i].isValid() && ! repeated[i])
            chosen.push_back (pointStates[i]);

    if (! parameter.isValid() || chosen.empty())
    {
        if (pointsParameter.isValid())
        {
            pointsParameter = {};
            points.clear();
            changed();
        }

        return;
    }

    if (parameter == pointsParameter && chosen == points && items.empty() && sections.empty() && notes.empty())
        return;

    clearItems();
    notes.clear();
    pointsParameter = parameter;
    points = std::move (chosen);
    changed();
}
} // namespace phazite
