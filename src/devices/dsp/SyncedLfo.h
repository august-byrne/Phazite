// SPDX-License-Identifier: AGPL-3.0-or-later
// An LFO for the effects that sweep (the phaser, the flanger, the chorus): a sine or a triangle, at a rate in hertz or on a
// note value at the song's tempo (NoteValues.h). In time with the song, while the song plays, its phase comes from where the
// song is, a note value's from its beats and a bar's from its bars, so a sweep lands on the same beat each time round, a
// bar's cycle starts with each bar whatever the time signature, and a render plays it the same each time; stopped, it runs
// free. When the song jumps (a loop going round, the playhead moved), the phase glides to where the song is over 10 ms
// rather than jumping. The right channel reads it a part of a cycle ahead. Real-time safe.
#pragma once

#include "NoteValues.h"

#include <cmath>

namespace phazite::devices::dsp
{
class SyncedLfo
{
public:
    enum class Shape
    {
        sine,
        triangle
    };

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        glideSamples = (int) std::lround (0.01 * sampleRate);
        reset();
    }

    void reset() noexcept
    {
        phase = 0.0;
        correction = 0.0;
        correctionLeft = 0;
        fresh = true;
    }

    // At each block's start: its rate, and, in time with the song while it plays, where the song is. In time with it, a cycle
    // is a note value (NoteValues.h), so many beats (quarter notes) or so many of the song's bars, at its tempo (beats a
    // minute) and its bar's length (in beats), and it follows the song's place: its beat, or its bars (2.5 half way through
    // bar 2).
    void startBlock (bool synced, double hertz, NoteLength cycle, double tempo, double barLength, bool playing, double beat,
                     double bars) noexcept
    {
        // A note value counts the song's beats, a bar its bars: how many go by a second, and how many a cycle is.
        const auto inBars = cycle.bars > 0.0;
        const auto units = inBars ? cycle.bars : (cycle.beats > 0.0 ? cycle.beats : 1.0);
        const auto unitsPerSecond = inBars ? tempo / 60.0 / (barLength > 0.0 ? barLength : 4.0) : tempo / 60.0;
        const auto cyclesPerSecond = synced ? unitsPerSecond / units : hertz;
        increment = cyclesPerSecond / sampleRate;

        if (! synced || ! playing)
        {
            fresh = false;
            return;
        }

        const auto wanted = (inBars ? bars : beat) / units;
        const auto target = wanted - std::floor (wanted);

        if (fresh)
        {
            phase = target;
            fresh = false;
            return;
        }

        // Where it should be, against where it is: a little drift is put right at once, a jump over 10 ms.
        auto difference = target - phase;
        difference -= std::floor (difference + 0.5);

        if (std::abs (difference) < 1.0e-4 || glideSamples <= 0)
        {
            phase = target;
            correctionLeft = 0;
        }
        else
        {
            correction = difference / glideSamples;
            correctionLeft = glideSamples;
        }
    }

    // Moves on by some samples (a control step); then gives its value, from -1 to 1, at an offset into its cycle.
    void advance (int samples) noexcept
    {
        phase += increment * samples;

        if (correctionLeft > 0)
        {
            const auto steps = samples < correctionLeft ? samples : correctionLeft;
            phase += correction * steps;
            correctionLeft -= steps;
        }

        phase -= std::floor (phase);
    }

    float valueAt (Shape shape, double offset) const noexcept { return getValue (shape, phase + offset); }

    double getPhase() const noexcept { return phase; } // from 0 to 1

    // A shape's value at a point in its cycle (whole cycles in or not), from -1 to 1: both start at the bottom of the sweep,
    // so that it rises from the downbeat, and reach the top halfway.
    static float getValue (Shape shape, double at) noexcept
    {
        at -= std::floor (at);

        if (shape == Shape::triangle)
            return (float) (1.0 - 4.0 * std::abs (at - 0.5));

        return (float) -std::cos (6.283185307179586 * at);
    }

private:
    double sampleRate = 48000.0, phase = 0.0, increment = 0.0, correction = 0.0;
    int glideSamples = 480, correctionLeft = 0;
    bool fresh = true; // the first block in time with the song takes its phase at once
};
} // namespace phazite::devices::dsp
