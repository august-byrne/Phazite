// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Selection.h"
#include "internal/Schema.h"

#include <algorithm>
#include <unordered_set>

namespace phazite
{
te::EditItemID getSelectableID (const te::Selectable& item)
{
    if (const auto* track = dynamic_cast<const te::Track*> (&item))
        return track->itemID;

    if (const auto* clip = dynamic_cast<const te::Clip*> (&item))
        return clip->itemID;

    if (const auto* device = dynamic_cast<const te::Plugin*> (&item))
        return device->itemID;

    return {};
}

namespace
{
    bool isDevice (const te::Selectable* item)
    {
        return dynamic_cast<const te::Plugin*> (item) != nullptr;
    }

    // How many of these notes are still in their clips, found as model/notes/internal/Notes.h finds a note: by the ID the
    // project stores on it.
    int countNotesThere (te::Edit& edit, const std::vector<ClipID>& clips, const std::vector<NoteRef>& notes)
    {
        auto count = 0;

        for (const auto& id : clips)
        {
            auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (edit, toTracktion (id)));

            if (clip == nullptr)
                continue;

            std::unordered_set<juce::int64> there;

            for (auto* note : clip->getSequence().getNotes())
                there.insert (static_cast<juce::int64> (note->state[schema::phaziteNoteID]));

            for (const auto& ref : notes)
                if (ref.clip == id && there.contains (static_cast<juce::int64> (ref.note.getRaw())))
                    ++count;
        }

        return count;
    }
} // namespace

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

te::Selectable* Selection::find (te::EditItemID id) const
{
    auto* in = edit.get();

    if (in == nullptr || ! id.isValid())
        return nullptr;

    if (auto* track = te::findTrackForID (*in, id))
        return track;

    if (auto* clip = te::findClipForID (*in, id))
        return clip;

    return te::findPluginForID (*in, id).get();
}

bool Selection::isThere (SectionID id) const
{
    auto* in = edit.get();

    if (in == nullptr || ! id.isValid())
        return false;

    for (const auto& section : findPhaziteState (*in).getChildWithName (schema::SECTIONS))
        if (te::EditItemID::fromVar (section[schema::sectionID]).getRawID() == id.getRaw())
            return true;

    return false;
}

bool Selection::holdsDevices() const
{
    return ! items.empty() && isDevice (find (items.front()));
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

bool Selection::isSelected (const te::Selectable* item) const
{
    const auto id = item != nullptr ? getSelectableID (*item) : te::EditItemID();
    return id.isValid() && std::find (items.begin(), items.end(), id) != items.end();
}

void Selection::selectOnly (te::Selectable* item)
{
    const auto id = item != nullptr ? getSelectableID (*item) : te::EditItemID();

    if (! id.isValid())
        return;

    if (items == std::vector { id } && sections.empty() && notes.empty() && ! pointsParameter.isValid())
        return;

    clearItems();
    clearNotesAndPoints();
    items = { id };
    changed();
}

void Selection::addToSelection (te::Selectable* item)
{
    const auto id = item != nullptr ? getSelectableID (*item) : te::EditItemID();

    if (! id.isValid() || isSelected (item))
        return;

    // Devices are selected on their own: one added to a selection of anything else starts a new one, and so does anything
    // else added to a selection of devices.
    if (isDevice (item) != holdsDevices())
        clearItems();

    clearNotesAndPoints();
    items.push_back (id);
    changed();
}

void Selection::deselect (te::Selectable* item)
{
    const auto id = item != nullptr ? getSelectableID (*item) : te::EditItemID();
    const auto found = std::find (items.begin(), items.end(), id);

    if (! id.isValid() || found == items.end())
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

    for (const auto& id : items)
        if (find (id) != nullptr)
            ++count;

    for (const auto& id : sections)
        if (isThere (id))
            ++count;

    if (auto* in = edit.get(); in != nullptr && ! notes.empty())
        count += countNotesThere (*in, noteClips, notes);

    return count + (int) points.size();
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
