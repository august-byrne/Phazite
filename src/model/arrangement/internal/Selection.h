// SPDX-License-Identifier: AGPL-3.0-or-later
// What the user has selected beyond Phazite's selection (model/base/internal/Selection.h): a time range across tracks;
// and the commands that act on whatever is selected: delete, duplicate, select all and deselect all.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/internal/Selection.h"

#include <vector>

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
    explicit RangeSelection (const Selection&); // in whose song its tracks are

    void set (BeatRange, const std::vector<TrackID>&);

    // The same, as part of the undo step being made in the song, for a command that moves the range to what it made
    // (Duplicate): Undo puts back the range there was, and Redo sets this one again, as REAPER's undo puts razor edit
    // areas back. A range selected by hand isn't an undo step: that is set().
    void setInUndoStep (Song&, BeatRange, const std::vector<TrackID>&);

    void clear();

    // Empty if nothing is selected, or if the tracks it was on have all been deleted.
    bool isEmpty() const;
    BeatRange getRange() const noexcept { return range; }
    std::vector<TrackID> getTracks() const; // those still there

private:
    const Selection& selection;
    BeatRange range;
    std::vector<TrackID> tracks;

    JUCE_DECLARE_WEAK_REFERENCEABLE (RangeSelection) // SetRange holds it weakly: an undo history can outlive it
};

//==============================================================================
// What the selection commands do, to the first kind of thing selected, in this order: notes, a time range, sections,
// clips, tracks. These don't start an undo step: the command does (Command::undoTarget).

// Deletes it. With closeGap, what comes after a range or clips moves back to close the gap, and tracks stay.
void deleteSelection (Selection&, const RangeSelection&, bool closeGap);

// Duplicates it right after it (tracks each below its original), and selects the copies; a time range's copy becomes
// the range, in the same undo step. As aliases, only sections and clips are copied.
void duplicateSelection (Selection&, RangeSelection&, bool asAliases);

// Splits the clips in the range at its edges, or else the selected clips at the playhead.
void splitSelection (const Selection&, const RangeSelection&);

// Selects every clip, or every track while tracks are selected or there are no clips, and clears the range.
void selectAllClipsOrTracks (Selection&, RangeSelection&);
void clearSelection (Selection&, RangeSelection&); // the range, then everything else

// Mutes the selected notes, or else the selected clips, or unmutes them if they all are muted.
void toggleSelectionMuted (const Selection&);
bool isSelectionMuted (const Selection&);

// When the commands can act.
bool canDeleteSelection (const Selection&, const RangeSelection&);      // and duplicate it
bool hasSelectedRangeOrClips (const Selection&, const RangeSelection&); // close a gap, split
bool canDuplicateAsAliases (const Selection&);                          // sections or clips
bool canMuteSelection (const Selection&);                               // notes or clips
bool isAnythingSelected (const Selection&, const RangeSelection&);

// "Delete Clip", "Delete 3 Tracks", "Delete Range", or the verb alone when nothing is selected.
juce::String describeSelection (const Selection&, const RangeSelection&, const juce::String& verb);

// The selection commands, for the selection manager's Edit: delete, delete and close the gap, duplicate,
// select all and deselect all.
void addSelectionCommands (CommandRegistry&, Selection&, RangeSelection&);
} // namespace phazite
