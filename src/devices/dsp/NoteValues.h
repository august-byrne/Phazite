// SPDX-License-Identifier: AGPL-3.0-or-later
// The note values a rate in time with the song takes (an LFO's, a sweep's), from a 32nd note to eight bars, and their
// lengths in beats (quarter notes): T for a triplet, D for dotted; a bar is four beats. The wavetable synth's LFOs, the
// phaser and the flanger share them, so their choices read alike (their saved values are positions in this list).
#pragma once

#include <iterator>
#include <string_view>

namespace phazite::devices::dsp
{
inline constexpr std::string_view noteValueNames[] = { "1/32", "1/16T", "1/16", "1/16D", "1/8T",  "1/8",    "1/8D",   "1/4T",  "1/4",
                                                       "1/4D", "1/2T",  "1/2",  "1/2D",  "1 bar", "2 bars", "4 bars", "8 bars" };
inline constexpr double noteValueBeats[] = { 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0,
                                             1.5,   4.0 / 3.0, 2.0,  3.0,   4.0,       8.0, 16.0, 32.0 };
static_assert (std::size (noteValueNames) == std::size (noteValueBeats));

inline constexpr float quarterNoteValue = 8.0f, barNoteValue = 13.0f, twoBarsNoteValue = 14.0f; // positions in the list

// A position in the list, as a parameter's value, to its length in beats.
inline double getNoteValueBeats (float position) noexcept
{
    const auto index = position < 0.0f                                  ? 0
                       : position >= (float) std::size (noteValueBeats) ? (int) std::size (noteValueBeats) - 1
                                                                        : (int) (position + 0.5f);
    return noteValueBeats[index];
}
} // namespace phazite::devices::dsp
