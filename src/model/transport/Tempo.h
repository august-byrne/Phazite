// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (docs/design.md, section 4; roadmap step 1.7), which the top bar shows and
// changes. For now a song has one of each, from its start; changes along the song come later, with a tempo lane. Clips
// stay on their beats when the tempo changes, as the playlist's time line is in beats.
#pragma once

#include "TempoValues.h"
#include "model/base/Tracktion.h"

namespace phazite
{
double getTempo (const te::Edit&);

// Changes the tempo, as one undo step, "Change Tempo" (a drag's changes join the drag's step). It stays within the
// limits (TempoValues.h), to a hundredth of a beat per minute.
void setTempo (te::Edit&, double bpm);

TimeSignature getTimeSignature (const te::Edit&);

// Changes the time signature, as one undo step, "Change Time Signature". An invalid one changes nothing.
void setTimeSignature (te::Edit&, TimeSignature);
} // namespace phazite
