// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongSelection.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "internal/SongSelection.h"
#include "internal/Tracks.h"
#include "model/notes/internal/Notes.h"

#include <unordered_map>

namespace phazite
{
struct SongSelection::Impl : private te::SelectableListener
{
    explicit Impl (te::SelectionManager& m)
        : manager (m)
    {
    }

    ~Impl() override { setNoteClips ({}); }

    te::SelectionManager& manager;

    // The note clips, and their notes as Tracktion selects them: made for the clips while they are all there.
    std::vector<ClipID> noteClipIDs;
    std::vector<te::SafeSelectable<te::MidiClip>> noteClips;
    std::unique_ptr<te::SelectedMidiEvents> noteEvents;

    void setNoteClips (const std::vector<ClipID>& ids)
    {
        forgetNoteEvents();

        for (auto& c : noteClips)
            if (auto* clip = c.get())
                clip->removeSelectableListener (this);

        noteClips.clear();
        noteClipIDs = ids;

        if (auto* edit = manager.getEdit())
        {
            for (const auto id : ids)
            {
                if (auto* clip = id.isValid() ? dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, toTracktion (id))) : nullptr)
                {
                    noteClips.emplace_back (clip);
                    clip->addSelectableListener (this);
                }
            }
        }
    }

    juce::Array<te::MidiClip*> getLiveNoteClips() const
    {
        juce::Array<te::MidiClip*> clips;

        for (const auto& c : noteClips)
            if (auto* clip = c.get())
                clips.add (clip);

        return clips;
    }

    // Made again for the clips left after one of them went.
    te::SelectedMidiEvents* getNoteEvents()
    {
        if (noteEvents == nullptr)
            if (const auto clips = getLiveNoteClips(); ! clips.isEmpty())
                noteEvents = std::make_unique<te::SelectedMidiEvents> (clips);

        return noteEvents.get();
    }

    void forgetNoteEvents()
    {
        if (noteEvents != nullptr)
        {
            noteEvents->deselect();
            noteEvents = nullptr;
        }
    }

    void selectableObjectChanged (te::Selectable*) override {}

    // The note selection holds pointers to its clips, so it goes when one of them does.
    void selectableObjectAboutToBeDeleted (te::Selectable*) override { forgetNoteEvents(); }
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
    return std::make_unique<SongSelection> (std::make_unique<SongSelection::Impl> (manager));
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

//==============================================================================
void SongSelection::setNoteClips (const std::vector<ClipID>& ids)
{
    // The same clips, all still there, change nothing.
    if (ids == impl->noteClipIDs && (ids.empty() || impl->noteEvents != nullptr))
        return;

    impl->setNoteClips (ids);
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
    auto* events = impl->getNoteEvents();

    if (events == nullptr)
        return;

    // Each note clip's notes by ID, found once.
    juce::Array<te::MidiNote*> notes;

    for (auto* clip : impl->getLiveNoteClips())
    {
        const auto clipID = getClipID (*clip);
        std::unordered_map<NoteID, te::MidiNote*> byID;

        for (const auto& ref : refs)
        {
            if (ref.clip != clipID)
                continue;

            if (byID.empty())
                for (auto* note : clip->getSequence().getNotes())
                    byID.emplace (getNoteID (*note), note);

            if (const auto found = byID.find (ref.note); found != byID.end())
                notes.add (found->second);
        }
    }

    // Adding to a selection that isn't of these clips' notes starts a new one.
    const auto adding = addToSelection && getSelectedMidiEvents (impl->manager) == events;
    events->setSelected (impl->manager, notes, adding);
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
