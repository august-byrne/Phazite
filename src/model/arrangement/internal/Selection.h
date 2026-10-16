// SPDX-License-Identifier: AGPL-3.0-or-later
// What the user has selected beyond Tracktion's selection manager (a time range across tracks), and the
// commands that act on whatever is selected: delete, duplicate, select all and deselect all.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
class CommandRegistry;

//==============================================================================
// A time range across some tracks, as dragging empty space or a clip's upper half selects it in the
// playlist, like REAPER's razor edits. Delete, Duplicate, Split and the time commands act on it. It tells
// its listeners when it changes.
class RangeSelection : public juce::ChangeBroadcaster
{
public:
    void set (te::BeatRange, const juce::Array<te::Track*>&);

    // The same, as part of the undo step being made in the Edit, for a command that moves the range to what it made
    // (Duplicate): Undo puts back the range there was, and Redo sets this one again, as REAPER's undo puts razor edit
    // areas back. A range selected by hand isn't an undo step: that is set().
    void setInUndoStep (te::Edit&, te::BeatRange, const juce::Array<te::Track*>&);

    void clear();

    // Empty if nothing is selected, or if the tracks it was on have all been deleted.
    bool isEmpty() const;
    te::BeatRange getRange() const noexcept { return range; }
    juce::Array<te::Track*> getTracks() const; // those still there

private:
    te::BeatRange range;
    std::vector<te::SafeSelectable<te::Track>> tracks;

    JUCE_DECLARE_WEAK_REFERENCEABLE (RangeSelection) // SetRange holds it weakly: an undo history can outlive it
};

//==============================================================================
// What the selection commands do, to the first kind of thing selected, in this order: notes, a time range, sections,
// clips, tracks. These don't start an undo step: the command does (Command::undoTarget).

// Deletes it. With closeGap, what comes after a range or clips moves back to close the gap, and tracks stay.
void deleteSelection (te::SelectionManager&, const RangeSelection&, bool closeGap);

// Duplicates it right after it (tracks each below its original), and selects the copies; a time range's copy becomes
// the range, in the same undo step. As aliases, only sections and clips are copied.
void duplicateSelection (te::SelectionManager&, RangeSelection&, bool asAliases);

// Splits the clips in the range at its edges, or else the selected clips at the playhead.
void splitSelection (const te::SelectionManager&, const RangeSelection&);

// Selects every clip, or every track while tracks are selected or there are no clips, and clears the range.
void selectAllClipsOrTracks (te::SelectionManager&, RangeSelection&);
void clearSelection (te::SelectionManager&, RangeSelection&); // the range, then everything else

// Mutes the selected notes, or else the selected clips, or unmutes them if they all are muted.
void toggleSelectionMuted (const te::SelectionManager&);
bool isSelectionMuted (const te::SelectionManager&);

// When the commands can act.
bool canDeleteSelection (const te::SelectionManager&, const RangeSelection&);      // and duplicate it
bool hasSelectedRangeOrClips (const te::SelectionManager&, const RangeSelection&); // close a gap, split
bool canDuplicateAsAliases (const te::SelectionManager&);                          // sections or clips
bool canMuteSelection (const te::SelectionManager&);                               // notes or clips
bool isAnythingSelected (const te::SelectionManager&, const RangeSelection&);

// "Delete Clip", "Delete 3 Tracks", "Delete Range", or the verb alone when nothing is selected.
juce::String describeSelection (const te::SelectionManager&, const RangeSelection&, const juce::String& verb);

// The selection commands, for the selection manager's Edit: delete, delete and close the gap, duplicate,
// select all and deselect all.
void addSelectionCommands (CommandRegistry&, te::SelectionManager&, RangeSelection&);
} // namespace phazite
