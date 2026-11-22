// SPDX-License-Identifier: AGPL-3.0-or-later
// What a MIDI clip holds, its notes and its controllers' values, as the notes' code (Notes.h), what plays them
// (model/playback/internal/SongPlayer.h) and the playlist's read of their shapes walk them: a clip's value
// (model/base/internal/ClipNotesValue.h: ClipNotes, which Notes.h's getClipNotesValue() gives), in Phazite's own types. The
// conversion between the value and the clip's element, the one place that reads and writes NOTE and CONTROL by their names,
// is in ClipNotesValue.h too.
#pragma once

#include "model/base/internal/SongValues.h"

namespace phazite
{
// Each note of a clip, in the order the clip holds them.
template <typename Visit>
void forEachNote (const ClipNotes& clip, Visit&& visit)
{
    immer::for_each (clip.notes, [&visit] (const NoteValues& note) { visit (note); });
}

// Each of a clip's controllers' values, in their order, with its place among them.
template <typename Visit>
void forEachControl (const ClipNotes& clip, Visit&& visit)
{
    size_t place = 0;
    immer::for_each (clip.controls, [&visit, &place] (const ControlValues& control) { visit (place++, control); });
}

// Each note of a clip and then each of its controllers' values, each kind in its order: what plays the song reads.
template <typename VisitNote, typename VisitControl>
void forEachEvent (const ClipNotes& clip, VisitNote&& visitNote, VisitControl&& visitControl)
{
    forEachNote (clip, visitNote);
    immer::for_each (clip.controls, [&visitControl] (const ControlValues& control) { visitControl (control); });
}
} // namespace phazite
