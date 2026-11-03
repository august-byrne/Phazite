// SPDX-License-Identifier: AGPL-3.0-or-later
// What is selected in the open project, held by Phazite as IDs (roadmap A5, 2026-10-30), in place of Tracktion's selection
// manager: tracks, clips, sections and devices, in the order they were selected; or notes of the clips an editor shows; or
// automation points of one parameter. One kind of thing at a time, as the commands act on one: selecting devices deselects
// anything else and the other way round, and notes and points are each all that is selected while they are. An ID of an
// item that is gone names nothing and is left out. The model's commands reach Tracktion's objects through it, in the Edit
// it is of (the project context's); the UI reaches it by IDs (model/arrangement/SongSelection.h). It tells its listeners
// after each change, on the message thread. Message thread only.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"

#include <optional>
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

    // The Edit whose items it names, or none: another forgets what was selected. And its song (model/base/Song.h).
    void setEdit (te::Edit*);
    te::Edit* getEdit() const noexcept { return edit.get(); } // none once it is deleted
    Song* getSong() const;

    //==============================================================================
    // Tracks (folders, returns and the master too), clips and devices (plug-ins, containers), by their IDs, in the order
    // they were selected: what the model's commands act on (docs/tracktion-migration.md, T6 step 3). An item the song no
    // longer holds is left out.
    enum class Kind
    {
        track,
        clip,
        device
    };

    bool isSelected (TrackID id) const { return isSelected (Item { Kind::track, id.getRaw() }); }
    void selectOnly (TrackID id) { selectOnly (Item { Kind::track, id.getRaw() }); }
    void addToSelection (TrackID id) { addToSelection (Item { Kind::track, id.getRaw() }); }
    void deselect (TrackID id) { deselect (Item { Kind::track, id.getRaw() }); }
    bool isSelected (ClipID id) const { return isSelected (Item { Kind::clip, id.getRaw() }); }
    void selectOnly (ClipID id) { selectOnly (Item { Kind::clip, id.getRaw() }); }
    void addToSelection (ClipID id) { addToSelection (Item { Kind::clip, id.getRaw() }); }
    void deselect (ClipID id) { deselect (Item { Kind::clip, id.getRaw() }); }
    bool isSelected (DeviceID id) const { return isSelected (Item { Kind::device, id.getRaw() }); }
    void selectOnly (DeviceID id) { selectOnly (Item { Kind::device, id.getRaw() }); }
    void addToSelection (DeviceID id) { addToSelection (Item { Kind::device, id.getRaw() }); }
    void deselect (DeviceID id) { deselect (Item { Kind::device, id.getRaw() }); }

    std::vector<TrackID> getTracks() const;
    std::vector<ClipID> getClips() const;
    std::vector<DeviceID> getDevices() const;

    void deselectAll();
    int getNumObjectsSelected() const; // of every kind still there, notes and points included

    // The same for Tracktion's objects, while the model's callers hold them: anything else of Tracktion's is never selected.
    bool isSelected (const te::Selectable* item) const
    {
        const auto found = toItem (item);
        return found.has_value() && isSelected (*found);
    }

    void selectOnly (te::Selectable* item)
    {
        if (const auto found = toItem (item))
            selectOnly (*found);
    }

    void addToSelection (te::Selectable* item)
    {
        if (const auto found = toItem (item))
            addToSelection (*found);
    }

    void deselect (te::Selectable* item)
    {
        if (const auto found = toItem (item))
            deselect (*found);
    }

    bool isSelected (const te::Selectable& item) const { return isSelected (&item); }
    void selectOnly (te::Selectable& item) { selectOnly (&item); }
    void addToSelection (te::Selectable& item) { addToSelection (&item); }
    void deselect (te::Selectable& item) { deselect (&item); }

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
    // A track, a clip or a device by its kind and its ID.
    struct Item
    {
        Kind kind = Kind::track;
        std::uint64_t id = 0;

        bool operator== (const Item&) const = default;
    };

    te::SafeSelectable<te::Edit> edit;
    std::vector<Item> items;
    std::vector<SectionID> sections;
    std::vector<ClipID> noteClips;
    std::vector<NoteRef> notes;
    ParameterRef pointsParameter;
    std::vector<int> points;

    static std::optional<Item> toItem (const te::Selectable*); // nothing for anything else of Tracktion's
    te::Selectable* find (Item) const;                         // Tracktion's object, or nullptr
    bool isThere (Item) const;                                 // whether the song holds it
    bool isThere (SectionID) const;
    bool isSelected (Item) const;
    void selectOnly (Item);
    void addToSelection (Item);
    void deselect (Item);
    bool holdsDevices() const;
    void clearItems();          // tracks, clips, devices and sections, without telling
    bool clearNotesAndPoints(); // without telling; whether there were any
    void changed() { sendChangeMessage(); }

    template <typename ID>
    std::vector<ID> getIDs (Kind kind) const
    {
        std::vector<ID> ids;

        for (const auto& item : items)
            if (item.kind == kind && isThere (item))
                ids.push_back (ID::fromRaw (item.id));

        return ids;
    }

    JUCE_DECLARE_NON_COPYABLE (Selection)
};
} // namespace phazite
