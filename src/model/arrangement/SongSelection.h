// SPDX-License-Identifier: AGPL-3.0-or-later
// What is selected in the open project, by IDs, as the UI reads and changes it: tracks, clips and sections. It is a view
// of the project's one selection (ProjectContext), which the model's commands act on through Tracktion's selection
// manager behind it (internal/SongSelection.h). IDs of items that are gone are ignored. Message thread only.
#pragma once

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
