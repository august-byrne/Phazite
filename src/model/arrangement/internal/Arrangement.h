// SPDX-License-Identifier: AGPL-3.0-or-later
// Edits of the song's time line across every track: inserting, removing, moving and copying a range of
// beats. Sections (Sections.h) and the Insert Time and Remove Time commands use them.
//
// Clips that cross a range's edges are split there, so what lies inside goes with the range. Sections are
// never split: one that an edit cuts into gets shorter or longer instead. Automation goes with the clips.
// Tempo and key changes move with inserted and removed time, but stay where they are when a range is
// moved or copied.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// Inserts empty time: everything at or after a beat moves later. A section the beat falls inside gets longer.
void insertTime (te::Edit&, te::BeatPosition at, te::BeatDuration);

// Removes a range from the song: what lies inside is deleted, what comes after moves back, and sections
// that reach into it get shorter.
void removeTime (te::Edit&, te::BeatRange);

// Moves what lies in a range so that it starts at the destination, and what lies between them the other
// way to make room. The destination is a beat outside the range, in the song as it is before the move: a
// range moved later goes just before what was at the destination.
void moveTime (te::Edit&, te::BeatRange, te::BeatPosition destination);

// Copies what lies in a range to the destination (a beat outside the range), after inserting room for it
// there. With asAliases, the copied clips are aliases of the originals: editing one edits them all.
void copyTime (te::Edit&, te::BeatRange, te::BeatPosition destination, bool asAliases);
} // namespace phazite
