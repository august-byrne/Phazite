// SPDX-License-Identifier: AGPL-3.0-or-later
// What the user has selected beyond Tracktion's selection manager (a time range across tracks), and the
// commands that act on whatever is selected: delete, duplicate, select all and deselect all.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
class CommandRegistry;

//==============================================================================
// A time range across some tracks, as dragging empty space or a clip's upper half selects it in the
// playlist, like REAPER's razor edits. Delete, Split and the time commands act on it. It tells its
// listeners when it changes.
class RangeSelection : public juce::ChangeBroadcaster
{
public:
    void set (te::BeatRange, const juce::Array<te::Track*>&);
    void clear();

    // Empty if nothing is selected, or if the tracks it was on have all been deleted.
    bool isEmpty() const;
    te::BeatRange getRange() const noexcept { return range; }
    juce::Array<te::Track*> getTracks() const; // those still there

private:
    te::BeatRange range;
    std::vector<te::SafeSelectable<te::Track>> tracks;
};

//==============================================================================
// "Delete Clip", "Delete 3 Tracks", "Delete Range", or the verb alone when nothing is selected.
juce::String describeSelection (const te::SelectionManager&, const RangeSelection&, const juce::String& verb);

// The selection commands, for the selection manager's Edit: delete, delete and close the gap, duplicate,
// select all and deselect all.
void addSelectionCommands (CommandRegistry&, te::SelectionManager&, RangeSelection&);
} // namespace phazite
