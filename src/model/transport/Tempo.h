// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (docs/design.md, section 4; roadmap step 1.7), which the top bar shows and
// changes. For now a song has one of each, from its start; changes along the song come later, with a tempo lane. Clips
// stay on their beats when the tempo changes, as the playlist's time line is in beats.
#pragma once

#include "model/base/Tracktion.h"

#include <array>

namespace phazite
{
// Tracktion's limits, in beats per minute.
inline constexpr double minTempo = 20.0, maxTempo = 300.0;

double getTempo (const te::Edit&);

// Changes the tempo, as one undo step, "Change Tempo" (a drag's changes join the drag's step). It stays within the
// limits, to a hundredth of a beat per minute.
void setTempo (te::Edit&, double bpm);

struct TimeSignature
{
    int numerator = 4;   // beats in a bar
    int denominator = 4; // the note that counts as a beat: 4 for a quarter note

    bool operator== (const TimeSignature&) const = default;
};

TimeSignature getTimeSignature (const te::Edit&);

// Whether Phazite takes it: 1 to 32 beats of a half, quarter, eighth or sixteenth note.
bool isValid (TimeSignature);

// The time signatures that menus offer, as most DAWs do.
inline constexpr std::array<TimeSignature, 8> commonTimeSignatures {
    { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } }
};

// Changes the time signature, as one undo step, "Change Time Signature". An invalid one changes nothing.
void setTimeSignature (te::Edit&, TimeSignature);
} // namespace phazite
