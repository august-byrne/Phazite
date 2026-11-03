// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongSelection.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "internal/SongSelection.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
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
    return phazite::getSelectedClips (impl->manager);
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
    return impl->manager.isSelected (id);
}

bool SongSelection::isSelected (ClipID id) const
{
    return impl->manager.isSelected (id);
}

bool SongSelection::isSelected (SectionID id) const
{
    return impl->manager.isSelected (id);
}

void SongSelection::selectOnly (TrackID id)
{
    impl->manager.selectOnly (id);
}

void SongSelection::selectOnly (ClipID id)
{
    impl->manager.selectOnly (id);
}

void SongSelection::selectOnly (SectionID id)
{
    impl->manager.selectOnly (id);
}

void SongSelection::add (TrackID id)
{
    impl->manager.addToSelection (id);
}

void SongSelection::add (ClipID id)
{
    impl->manager.addToSelection (id);
}

void SongSelection::add (SectionID id)
{
    impl->manager.addToSelection (id);
}

void SongSelection::remove (TrackID id)
{
    impl->manager.deselect (id);
}

void SongSelection::remove (ClipID id)
{
    impl->manager.deselect (id);
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
    return impl->manager.getDevices();
}

bool SongSelection::isSelected (DeviceID id) const
{
    return impl->manager.isSelected (id);
}

void SongSelection::selectOnly (DeviceID id)
{
    impl->manager.selectOnly (id);
}

void SongSelection::add (DeviceID id)
{
    impl->manager.addToSelection (id);
}

void SongSelection::remove (DeviceID id)
{
    impl->manager.deselect (id);
}

//==============================================================================
void SongSelection::setNoteClips (const std::vector<ClipID>& ids)
{
    impl->manager.setNoteClips (ids);
}

std::vector<ClipID> SongSelection::getNoteClips() const
{
    std::vector<ClipID> ids;
    auto* song = impl->manager.getSong();

    for (const auto id : impl->manager.getNoteClips())
        if (song != nullptr && isMidiState (findClipState (*song, id)))
            ids.push_back (id);

    return ids;
}

std::vector<NoteRef> SongSelection::getNotes() const
{
    return getSelectedNotes (impl->manager);
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
    auto* song = impl->manager.getSong();
    auto* edit = song != nullptr ? findEdit (*song) : nullptr;
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
