// SPDX-License-Identifier: AGPL-3.0-or-later
// The time range selected across tracks in the open project (Selection.h's RangeSelection), by beats and track IDs, as
// the UI reads and sets it: dragging empty space or a clip's upper half in the playlist, like REAPER's razor edits.
// The model's commands act on the RangeSelection behind it (internal/SongRange.h). IDs of tracks that are gone are
// ignored. Message thread only.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

namespace phazite
{
class SongRange
{
public:
    ~SongRange();

    // Empty if nothing is selected, or if the tracks it was on have all been deleted.
    bool isEmpty() const;
    BeatRange getRange() const;
    std::vector<TrackID> getTracks() const; // those still there, in the order given

    void set (BeatRange, const std::vector<TrackID>&); // not an undo step, as a range selected by hand isn't
    void clear();

    // Tells a listener after each change, on the message thread; isSource() tells its messages from others.
    void addChangeListener (juce::ChangeListener*);
    void removeChangeListener (juce::ChangeListener*);
    bool isSource (const juce::ChangeBroadcaster*) const noexcept;

    struct Impl; // the range selection and the selection manager (for the Edit) behind it
    explicit SongRange (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (SongRange)
};
} // namespace phazite
