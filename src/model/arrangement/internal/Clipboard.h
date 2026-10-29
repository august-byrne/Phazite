// SPDX-License-Identifier: AGPL-3.0-or-later
// Cut, Copy and Paste (docs/design.md, section 12; roadmap step 3.7). Copy keeps what Delete would delete, as it is now and
// in Delete's order: the selected notes, as plain values; or the selected time range's clips, which Paste cuts at its edges;
// or the selected clips; with the automation under the clips or in the range (model/automation/internal/Automation.h).
// Cut copies it and deletes it as Delete does. Paste puts notes into the clip whose notes the piano roll shows at the
// playhead (SongSelection.h: getNoteClips()), or, with none of them there, into the first, as far into it as they were
// into theirs; and clips at the playhead, from the selected track down (else the first selected clip's, else the first of
// those they were copied from), each in place of what lies under it, as a duplicated range is, so that nothing plays twice.
// The automation goes with them when they go back onto their own tracks, along which automation goes with clips. What
// Paste puts in is selected (a range's becomes the range), as Duplicate selects its copies, in one undo step. The project
// context keeps the clipboard (internal/ProjectContext.h), and another project empties it: clips name the files they play
// by paths relative to their project.
#pragma once

#include "model/automation/internal/Automation.h"
#include "model/base/internal/Tracktion.h"
#include "model/notes/NoteValues.h"

#include <vector>

namespace phazite
{
class CommandRegistry;
class ProjectContext;
class RangeSelection;

struct Clipboard
{
    enum class Kind
    {
        nothing,
        notes,
        clips,
        range // a time range's clips, cut to it
    };

    // A clip as it was: a copy of its state, where it was in the song, and its track's place from the first of the copy's
    // tracks, counting those that hold clips in the playlist's order.
    struct CopiedClip
    {
        juce::ValueTree state;
        te::BeatRange beats;
        int trackOffset = 0;
    };

    Kind kind = Kind::nothing;
    std::vector<NoteData> notes; // with no clip or ID, each starting as far after the earliest as it did
    BeatDuration intoClip;       // how far into its clip the earliest note was
    std::vector<CopiedClip> clips;
    te::BeatRange range;  // the time range, or the clips' from the earliest start to the latest end
    te::EditItemID track; // the first of the clips' tracks, or of the range's
    std::vector<AutomationCopy> automation;
};

// Whether there is something to copy or cut: notes, a time range or clips.
bool canCopy (const te::SelectionManager&, const RangeSelection&);
void copySelection (Clipboard&, const te::SelectionManager&, const RangeSelection&);

// "Copy 3 Notes" or "Cut Range", as the menus and the undo step say; the verb alone while there is nothing to copy.
juce::String describeCopy (const te::SelectionManager&, const RangeSelection&, const juce::String& verb);

// Copies, then deletes what was copied as Delete does: the clips leave the automation under them, a range takes its own.
void cutSelection (Clipboard&, te::SelectionManager&, RangeSelection&);

// Whether Paste has something to put in, and somewhere to put it: a clip the piano roll shows, for notes; a track, for clips.
bool canPaste (const Clipboard&, ProjectContext&);
void paste (const Clipboard&, ProjectContext&);

// "Paste 3 Notes", "Paste Clip", "Paste Range", as its undo step and its menu item say.
juce::String describePaste (const Clipboard&);

// The commands Cut, Copy and Paste, for the project context's selection and clipboard.
void addClipboardCommands (CommandRegistry&, ProjectContext&);
} // namespace phazite
