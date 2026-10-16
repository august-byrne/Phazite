// SPDX-License-Identifier: AGPL-3.0-or-later
// The tempo's limits and the time signature, as plain values (Tempo.h changes them; TransportModel.h shows them).
#pragma once

#include <array>

namespace phazite
{
// Tracktion's limits, in beats per minute.
inline constexpr double minTempo = 20.0, maxTempo = 300.0;

struct TimeSignature
{
    int numerator = 4;   // beats in a bar
    int denominator = 4; // the note that counts as a beat: 4 for a quarter note

    bool operator== (const TimeSignature&) const = default;
};

// Whether Phazite takes it: 1 to 32 beats of a half, quarter, eighth or sixteenth note.
bool isValid (TimeSignature);

// The time signatures that menus offer, as most DAWs do.
inline constexpr std::array<TimeSignature, 8> commonTimeSignatures {
    { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } }
};
} // namespace phazite
