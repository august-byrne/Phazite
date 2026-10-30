// SPDX-License-Identifier: AGPL-3.0-or-later
// What is selected in the open project, held by Phazite as IDs (roadmap A5, 2026-10-30), in place of Tracktion's selection
// manager: tracks, clips, sections and devices, in the order they were selected; or notes of the clips an editor shows; or
// automation points of one parameter. One kind of thing at a time, as the commands act on one: selecting devices deselects
// anything else and the other way round, and notes and points are each all that is selected while they are. An ID of an
// item that is gone names nothing and is left out. The model's commands reach Tracktion's objects through it, in the Edit
// it is of (the project context's); the UI reaches it by IDs (model/arrangement/SongSelection.h). It tells its listeners
// after each change, on the message thread. Message thread only.
#pragma once

#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"

#include <vector>

namespace phazite
{
class Selection : public juce::ChangeBroadcaster
{
public:
    explicit Selection (te::Engine& e)
        : engine (e)
    {
    }

    te::Engine& engine; // whose Edits it selects in, for the commands that need it while no project is open

    // The Edit whose items it names, or none: another forgets what was selected.
    void setEdit (te::Edit*);
    te::Edit* getEdit() const noexcept { return edit.get(); } // none once it is deleted

    //==============================================================================
    // Tracks (folders too), clips and devices (plug-ins, containers), as Tracktion's objects: what the model's commands act
    // on. Anything else of Tracktion's is never selected.
    bool isSelected (const te::Selectable*) const;
    void selectOnly (te::Selectable*);
    void addToSelection (te::Selectable*);
    void deselect (te::Selectable*);
    bool isSelected (const te::Selectable& item) const { return isSelected (&item); }
    void selectOnly (te::Selectable& item) { selectOnly (&item); }
    void addToSelection (te::Selectable& item) { addToSelection (&item); }
    void deselect (te::Selectable& item) { deselect (&item); }
    void deselectAll();
    int getNumObjectsSelected() const; // of every kind still there, notes and points included

    //==============================================================================
    // Sections, which the project keeps itself (model/arrangement/internal/Sections.h), by their IDs: with tracks and
    // clips, and without devices, as tracks and clips are.
    bool isSelected (SectionID) const;
    void selectOnly (SectionID);
    void addToSelection (SectionID);
    void deselect (SectionID);

    // Those of a type still there, in the order they were selected.
    template <typename Type>
    juce::Array<Type*> getItemsOfType() const
    {
        juce::Array<Type*> found;

        for (const auto& id : items)
            if (auto* item = dynamic_cast<Type*> (find (id)))
                found.add (item);

        return found;
    }

    template <typename Type>
    Type* getFirstItemOfType() const
    {
        for (const auto& id : items)
            if (auto* item = dynamic_cast<Type*> (find (id)))
                return item;

        return nullptr;
    }

    //==============================================================================
    // Notes, of the clips an editor shows (the piano roll's), by their references: other clips deselect them, and so does
    // selecting anything else.
    void setNoteClips (const std::vector<ClipID>&);
    const std::vector<ClipID>& getNoteClips() const noexcept { return noteClips; }
    const std::vector<NoteRef>& getNotes() const noexcept { return notes; }      // as selected: some may be gone
    void selectNotes (const std::vector<NoteRef>&, bool addToSelection = false); // of the note clips

    //==============================================================================
    // Automation points of one parameter, by their indices: selecting anything else deselects them.
    const ParameterRef& getPointsParameter() const noexcept { return pointsParameter; } // invalid while none are selected
    const std::vector<int>& getPoints() const noexcept { return points; }
    void selectPoints (const ParameterRef&, const std::vector<int>& indices);

private:
    te::SafeSelectable<te::Edit> edit;
    std::vector<te::EditItemID> items;
    std::vector<SectionID> sections;
    std::vector<ClipID> noteClips;
    std::vector<NoteRef> notes;
    ParameterRef pointsParameter;
    std::vector<int> points;

    te::Selectable* find (te::EditItemID) const; // a track, a clip or a device of the Edit, or nullptr
    bool isThere (SectionID) const;
    bool holdsDevices() const;
    void clearItems();          // tracks, clips, devices and sections, without telling
    bool clearNotesAndPoints(); // without telling; whether there were any
    void changed() { sendChangeMessage(); }

    JUCE_DECLARE_NON_COPYABLE (Selection)
};

// The ID of a track, a clip or a device, as the selection keeps it; invalid for anything else.
te::EditItemID getSelectableID (const te::Selectable&);
} // namespace phazite
