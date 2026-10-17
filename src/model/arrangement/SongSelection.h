// SPDX-License-Identifier: AGPL-3.0-or-later
// What is selected in the open project, by IDs, as the UI reads and changes it: tracks, clips, sections, notes,
// automation points and devices. It is a view of the project's one selection (ProjectContext), which the model's
// commands act on through Tracktion's selection manager behind it (internal/SongSelection.h). IDs of items that are gone
// are ignored. Message thread only.
#pragma once

#include "model/automation/AutomationValues.h"
#include "model/base/IDs.h"

#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

namespace phazite
{
class SongSelection
{
public:
    ~SongSelection();

    std::vector<TrackID> getTracks() const;     // in the playlist's order
    std::vector<ClipID> getClips() const;       // top to bottom by track, then left to right
    std::vector<SectionID> getSections() const; // in the song's order

    bool isSelected (TrackID) const;
    bool isSelected (ClipID) const;
    bool isSelected (SectionID) const;

    // An item alone, or added to what is selected, or taken out of it.
    void selectOnly (TrackID);
    void selectOnly (ClipID);
    void selectOnly (SectionID);
    void add (TrackID);
    void add (ClipID);
    void add (SectionID);
    void remove (TrackID);
    void remove (ClipID);
    void remove (SectionID);
    void clear();

    // Devices, in the mixer (model/mixer/MixerModel.h), in the order they were selected, as the device commands act on them
    // (model/mixer/ContainerModel.h): selected on their own, so that selecting them deselects anything else, and selecting
    // anything else deselects them.
    std::vector<DeviceID> getDevices() const;
    bool isSelected (DeviceID) const;
    void selectOnly (DeviceID);
    void add (DeviceID);
    void remove (DeviceID);

    // Notes, of the clips an editor shows (the piano roll's: setNoteClips()). Selecting notes selects nothing else with
    // them, and selecting anything else deselects them; deleted notes, or notes of deleted clips, are no longer selected.
    void setNoteClips (const std::vector<ClipID>&); // other clips than before: their notes are deselected
    std::vector<NoteRef> getNotes() const;
    bool isSelected (NoteRef) const;
    void selectNotes (const std::vector<NoteRef>&, bool addToSelection = false); // those of the note clips

    // Automation points of one curve (a lane's, or an automation clip's: model/automation/AutomationValues.h), by their
    // indices in time order. Selecting points selects nothing else with them, and selecting anything else deselects them;
    // points of a curve that is gone, or indices of points that aren't there, aren't selected.
    struct AutomationPoints
    {
        CurveRef curve {};
        std::vector<int> indices {};
    };

    AutomationPoints getSelectedAutomationPoints() const; // an invalid curve while none are selected
    bool isSelected (const CurveRef&, int index) const;
    void selectAutomationPoints (const CurveRef&, const std::vector<int>& indices, bool addToSelection = false);

    // Tells a listener after each change, on the message thread.
    void addChangeListener (juce::ChangeListener*);
    void removeChangeListener (juce::ChangeListener*);

    struct Impl; // the selection manager behind it (internal/SongSelection.h)
    explicit SongSelection (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (SongSelection)
};
} // namespace phazite
