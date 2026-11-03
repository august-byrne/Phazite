// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongSelection.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "internal/SongSelection.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/notes/internal/Notes.h"

#include <unordered_map>

namespace phazite
{
struct SongSelection::Impl
{
    explicit Impl (Selection& m)
        : manager (m)
    {
    }

    Selection& manager;
};

namespace
{
    te::Track* findTrack (const Selection& manager, TrackID id)
    {
        auto* edit = manager.getEdit();
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::Clip* findClip (const Selection& manager, ClipID id)
    {
        auto* edit = manager.getEdit();
        return edit != nullptr && id.isValid() ? te::findClipForID (*edit, toTracktion (id)) : nullptr;
    }

    te::Plugin* findDevice (const Selection& manager, DeviceID id)
    {
        auto* edit = manager.getEdit();
        return edit != nullptr && id.isValid() ? te::findPluginForID (*edit, toTracktion (id)).get() : nullptr;
    }
} // namespace

std::unique_ptr<SongSelection> createSongSelection (Selection& manager)
{
    return std::make_unique<SongSelection> (std::make_unique<SongSelection::Impl> (manager));
}

SongSelection::SongSelection (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

SongSelection::~SongSelection() = default;

std::vector<TrackID> SongSelection::getTracks() const
{
    return phazite::getSelectedTracks (impl->manager);
}

std::vector<ClipID> SongSelection::getClips() const
{
    std::vector<ClipID> ids;

    for (auto* clip : getSelectedClips (impl->manager))
        ids.push_back (getClipID (*clip));

    return ids;
}

std::vector<SectionID> SongSelection::getSections() const
{
    std::vector<SectionID> ids;

    for (const auto& section : getSelectedSections (impl->manager))
        ids.push_back (section.id);

    return ids;
}

bool SongSelection::isSelected (TrackID id) const
{
    auto* track = findTrack (impl->manager, id);
    return track != nullptr && impl->manager.isSelected (track);
}

bool SongSelection::isSelected (ClipID id) const
{
    auto* clip = findClip (impl->manager, id);
    return clip != nullptr && impl->manager.isSelected (clip);
}

bool SongSelection::isSelected (SectionID id) const
{
    return impl->manager.isSelected (id);
}

void SongSelection::selectOnly (TrackID id)
{
    impl->manager.selectOnly (findTrack (impl->manager, id));
}

void SongSelection::selectOnly (ClipID id)
{
    impl->manager.selectOnly (findClip (impl->manager, id));
}

void SongSelection::selectOnly (SectionID id)
{
    impl->manager.selectOnly (id);
}

void SongSelection::add (TrackID id)
{
    impl->manager.addToSelection (findTrack (impl->manager, id));
}

void SongSelection::add (ClipID id)
{
    impl->manager.addToSelection (findClip (impl->manager, id));
}

void SongSelection::add (SectionID id)
{
    impl->manager.addToSelection (id);
}

void SongSelection::remove (TrackID id)
{
    impl->manager.deselect (findTrack (impl->manager, id));
}

void SongSelection::remove (ClipID id)
{
    impl->manager.deselect (findClip (impl->manager, id));
}

void SongSelection::remove (SectionID id)
{
    impl->manager.deselect (id);
}

void SongSelection::clear()
{
    impl->manager.deselectAll();
}

//==============================================================================
std::vector<DeviceID> SongSelection::getDevices() const
{
    std::vector<DeviceID> ids;

    for (auto* device : impl->manager.getItemsOfType<te::Plugin>())
        ids.push_back (getDeviceID (*device));

    return ids;
}

bool SongSelection::isSelected (DeviceID id) const
{
    auto* device = findDevice (impl->manager, id);
    return device != nullptr && impl->manager.isSelected (device);
}

void SongSelection::selectOnly (DeviceID id)
{
    impl->manager.selectOnly (findDevice (impl->manager, id));
}

void SongSelection::add (DeviceID id)
{
    impl->manager.addToSelection (findDevice (impl->manager, id));
}

void SongSelection::remove (DeviceID id)
{
    impl->manager.deselect (findDevice (impl->manager, id));
}

//==============================================================================
void SongSelection::setNoteClips (const std::vector<ClipID>& ids)
{
    impl->manager.setNoteClips (ids);
}

std::vector<ClipID> SongSelection::getNoteClips() const
{
    std::vector<ClipID> ids;
    auto* edit = impl->manager.getEdit();

    for (const auto id : impl->manager.getNoteClips())
        if (edit != nullptr && dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, toTracktion (id))) != nullptr)
            ids.push_back (id);

    return ids;
}

std::vector<NoteRef> SongSelection::getNotes() const
{
    std::vector<NoteRef> refs;

    for (const auto& n : getSelectedNotes (impl->manager))
        refs.push_back (getNoteRef (*n.clip, *n.note));

    return refs;
}

bool SongSelection::isSelected (NoteRef ref) const
{
    const auto notes = getNotes();
    return std::find (notes.begin(), notes.end(), ref) != notes.end();
}

void SongSelection::selectNotes (const std::vector<NoteRef>& refs, bool addToSelection)
{
    impl->manager.selectNotes (refs, addToSelection);
}

//==============================================================================
SongSelection::AutomationPoints SongSelection::getSelectedAutomationPoints() const
{
    if (const auto selected = getSelectedPoints (impl->manager))
        return { getParameterRef (*selected->parameter), selected->indices };

    return {};
}

bool SongSelection::isSelected (const ParameterRef& parameter, int index) const
{
    const auto selected = getSelectedAutomationPoints();
    return selected.parameter == parameter && std::find (selected.indices.begin(), selected.indices.end(), index) != selected.indices.end();
}

void SongSelection::selectAutomationPoints (const ParameterRef& parameter, const std::vector<int>& indices, bool addToSelection)
{
    auto* edit = impl->manager.getEdit();
    const auto found = edit != nullptr ? findParameter (*edit, parameter) : nullptr;

    if (found == nullptr)
        return;

    // Adding to points of the same parameter keeps them; otherwise these are all that is selected.
    auto all = indices;

    if (const auto before = getSelectedAutomationPoints(); addToSelection && before.parameter == parameter)
        all.insert (all.end(), before.indices.begin(), before.indices.end());

    selectPoints (impl->manager, *found, all);
}

void SongSelection::addChangeListener (juce::ChangeListener* listener)
{
    impl->manager.addChangeListener (listener);
}

void SongSelection::removeChangeListener (juce::ChangeListener* listener)
{
    impl->manager.removeChangeListener (listener);
}
} // namespace phazite
