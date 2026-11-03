// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Selection.h"
#include "internal/Schema.h"

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

            // Into what holds items: the track list, tracks and containers, not clips' notes or devices' values.
            if ((child.hasType (schema::TRACKS) || isTrack || child.hasType (schema::CONTAINER)) && holds (child, kind, id))
                return true;
        }

        return false;
    }

    // How many of these notes are still in their clips: by their IDs in the song's clips.
    int countNotesThere (const juce::ValueTree& song, const std::vector<ClipID>& clips, const std::vector<NoteRef>& notes)
    {
        std::unordered_set<juce::int64> clipsShown;

        for (const auto& id : clips)
            clipsShown.insert ((juce::int64) id.getRaw());

        std::unordered_set<juce::int64> there; // the notes of those clips, by their IDs
        std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& parent)
        {
            for (const auto& child : parent)
            {
                if (child.hasType (schema::CLIP))
                {
                    if (clipsShown.contains ((juce::int64) child[schema::clipID]))
                        for (const auto& note : child)
                            if (note.hasType (schema::NOTE))
                                there.insert ((juce::int64) note[schema::noteID]);
                }
                else if (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK))
                {
                    collect (child);
                }
            }
        };

        collect (song);
        auto count = 0;

        for (const auto& ref : notes)
            if (clipsShown.contains ((juce::int64) ref.clip.getRaw()) && there.contains ((juce::int64) ref.note.getRaw()))
                ++count;

        return count;
    }
} // namespace

std::optional<Selection::Item> Selection::toItem (const te::Selectable* item)
{
    if (const auto* track = dynamic_cast<const te::Track*> (item))
        return Item { Kind::track, track->itemID.getRawID() };

    if (const auto* clip = dynamic_cast<const te::Clip*> (item))
        return Item { Kind::clip, clip->itemID.getRawID() };

    if (const auto* device = dynamic_cast<const te::Plugin*> (item))
        return Item { Kind::device, device->itemID.getRawID() };

    return std::nullopt;
}

void Selection::setEdit (te::Edit* newEdit)
{
    if (newEdit == edit.get())
        return;

    edit = newEdit;
    clearItems();
    clearNotesAndPoints();
    noteClips.clear();
    changed();
}

Song* Selection::getSong() const
{
    auto* in = edit.get();
    return in != nullptr ? &phazite::getSong (*in) : nullptr;
}

te::Selectable* Selection::find (Item item) const
{
    auto* in = edit.get();

    if (in == nullptr || item.id == 0)
        return nullptr;

    const auto id = te::EditItemID::fromRawID (item.id);

    switch (item.kind)
    {
        case Kind::track:
            return te::findTrackForID (*in, id);
        case Kind::clip:
            return te::findClipForID (*in, id);
        case Kind::device:
            return te::findPluginForID (*in, id).get();
    }

    return nullptr;
}

bool Selection::isThere (Item item) const
{
    auto* song = getSong();
    return song != nullptr && item.id != 0 && holds (song->getState(), item.kind, item.id);
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
        count += countNotesThere (song->getState(), noteClips, notes);

    return count + (int) points.size();
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
    std::vector<NoteRef> chosen;

    if (addToSelection)
        chosen = notes;

    for (const auto& ref : refs)
        if (std::find (noteClips.begin(), noteClips.end(), ref.clip) != noteClips.end()
            && std::find (chosen.begin(), chosen.end(), ref) == chosen.end())
            chosen.push_back (ref);

    if (chosen == notes && items.empty() && sections.empty() && ! pointsParameter.isValid())
        return;

    clearItems();
    pointsParameter = {};
    points.clear();
    notes = std::move (chosen);
    changed();
}

//==============================================================================
void Selection::selectPoints (const ParameterRef& parameter, const std::vector<int>& indices)
{
    auto sorted = indices;
    std::sort (sorted.begin(), sorted.end());
    sorted.erase (std::unique (sorted.begin(), sorted.end()), sorted.end());

    if (! parameter.isValid() || sorted.empty())
    {
        if (pointsParameter.isValid())
        {
            pointsParameter = {};
            points.clear();
            changed();
        }

        return;
    }

    if (parameter == pointsParameter && sorted == points && items.empty() && sections.empty() && notes.empty())
        return;

    clearItems();
    notes.clear();
    pointsParameter = parameter;
    points = std::move (sorted);
    changed();
}
} // namespace phazite
