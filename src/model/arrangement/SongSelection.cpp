// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongSelection.h"
#include "Clips.h"
#include "Sections.h"
#include "Tracks.h"
#include "internal/SongSelection.h"

namespace phazite
{
struct SongSelection::Impl
{
    te::SelectionManager& manager;
};

namespace
{
    te::Track* findTrack (const te::SelectionManager& manager, TrackID id)
    {
        auto* edit = manager.getEdit();
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::Clip* findClip (const te::SelectionManager& manager, ClipID id)
    {
        auto* edit = manager.getEdit();
        auto* clip = edit != nullptr && id.isValid() ? te::findClipForID (*edit, toTracktion (id)) : nullptr;
        return dynamic_cast<te::ArrangerClip*> (clip) == nullptr ? clip : nullptr; // sections are arranger clips
    }

    te::ArrangerClip* findSection (const te::SelectionManager& manager, SectionID id)
    {
        auto* edit = manager.getEdit();
        return edit != nullptr && id.isValid() ? dynamic_cast<te::ArrangerClip*> (te::findClipForID (*edit, toTracktion (id))) : nullptr;
    }

    void selectOnlyItem (te::SelectionManager& manager, te::Selectable* item)
    {
        if (item != nullptr)
            manager.selectOnly (item);
    }

    void addItem (te::SelectionManager& manager, te::Selectable* item)
    {
        if (item != nullptr)
            manager.addToSelection (item);
    }

    void removeItem (te::SelectionManager& manager, te::Selectable* item)
    {
        if (item != nullptr)
            manager.deselect (item);
    }
} // namespace

std::unique_ptr<SongSelection> createSongSelection (te::SelectionManager& manager)
{
    return std::make_unique<SongSelection> (std::make_unique<SongSelection::Impl> (SongSelection::Impl { manager }));
}

SongSelection::SongSelection (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

SongSelection::~SongSelection() = default;

std::vector<TrackID> SongSelection::getTracks() const
{
    std::vector<TrackID> ids;

    for (auto* track : getSelectedTracks (impl->manager))
        ids.push_back (getTrackID (*track));

    return ids;
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

    for (auto* section : getSelectedSections (impl->manager))
        ids.push_back (getSectionID (*section));

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
    auto* section = findSection (impl->manager, id);
    return section != nullptr && impl->manager.isSelected (section);
}

void SongSelection::selectOnly (TrackID id)
{
    selectOnlyItem (impl->manager, findTrack (impl->manager, id));
}

void SongSelection::selectOnly (ClipID id)
{
    selectOnlyItem (impl->manager, findClip (impl->manager, id));
}

void SongSelection::selectOnly (SectionID id)
{
    selectOnlyItem (impl->manager, findSection (impl->manager, id));
}

void SongSelection::add (TrackID id)
{
    addItem (impl->manager, findTrack (impl->manager, id));
}

void SongSelection::add (ClipID id)
{
    addItem (impl->manager, findClip (impl->manager, id));
}

void SongSelection::add (SectionID id)
{
    addItem (impl->manager, findSection (impl->manager, id));
}

void SongSelection::remove (TrackID id)
{
    removeItem (impl->manager, findTrack (impl->manager, id));
}

void SongSelection::remove (ClipID id)
{
    removeItem (impl->manager, findClip (impl->manager, id));
}

void SongSelection::remove (SectionID id)
{
    removeItem (impl->manager, findSection (impl->manager, id));
}

void SongSelection::clear()
{
    impl->manager.deselectAll();
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
