// SPDX-License-Identifier: AGPL-3.0-or-later
// The note values a rate in time with the song takes (an LFO's, a sweep's, a slice's), from a 32nd note to eight bars, and
// their lengths: a note value's in beats (quarter notes, four to a whole note, whatever the time signature), T for a
// triplet, D for dotted; and a bar's in the song's bars, each as long as its time signature makes it (three beats in 6/8:
// ProcessContext::barLength). The wavetable synth's LFOs, the phaser, the flanger, the chorus and the sampler's slices
// share them, so their choices read alike (their saved values are positions in this list).
#pragma once

#include <iterator>
#include <string_view>

namespace phazite::devices::dsp
{
// A length in time with the song: so many beats, or so many of the song's bars.
struct NoteLength
{
    double beats = 0.0, bars = 0.0; // one of the two

    // In beats, where a bar is so many beats long.
    constexpr double inBeats (double barLength) const noexcept { return bars > 0.0 ? bars * barLength : beats; }
};

inline constexpr std::string_view noteValueNames[] = { "1/32", "1/16T", "1/16", "1/16D", "1/8T",  "1/8",    "1/8D",   "1/4T",  "1/4",
                                                       "1/4D", "1/2T",  "1/2",  "1/2D",  "1 bar", "2 bars", "4 bars", "8 bars" };
inline constexpr NoteLength noteValueLengths[] = {
    { .beats = 0.125 }, { .beats = 1.0 / 6.0 }, { .beats = 0.25 }, { .beats = 0.375 }, { .beats = 1.0 / 3.0 }, { .beats = 0.5 },
    { .beats = 0.75 },  { .beats = 2.0 / 3.0 }, { .beats = 1.0 },  { .beats = 1.5 },   { .beats = 4.0 / 3.0 }, { .beats = 2.0 },
    { .beats = 3.0 },   { .bars = 1.0 },        { .bars = 2.0 },   { .bars = 4.0 },    { .bars = 8.0 }
};
static_assert (std::size (noteValueNames) == std::size (noteValueLengths));

inline constexpr float quarterNoteValue = 8.0f, barNoteValue = 13.0f, twoBarsNoteValue = 14.0f; // positions in the list

// A position in the list, as a parameter's value, to its length.
inline NoteLength getNoteLength (float position) noexcept
{
    const auto index = position < 0.0f                                    ? 0
                       : position >= (float) std::size (noteValueLengths) ? (int) std::size (noteValueLengths) - 1
                                                                          : (int) (position + 0.5f);
    return noteValueLengths[index];
}
} // namespace phazite::devices::dsp
