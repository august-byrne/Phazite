// SPDX-License-Identifier: AGPL-3.0-or-later
// The grid of control steps an effect works out its coefficients and glides on (the phaser's, the flanger's and the chorus's 8
// samples, the EQ's 32): while the song plays, counted from where the song is, so that a loop going round, playback started
// anywhere and a device woken from sleep on silence step where a render of the song does, which starts at its beginning and
// never sleeps; while it is stopped, on from where it was. Steps go on across blocks: one that a block's end cuts short goes on
// in the next, so what an effect plays doesn't depend on how the samples are cut into blocks. Where the song jumps, the step
// under way ends there, and the next runs to the grid's next line from what the one cut short reached (getReached()).
// Real-time safe.
#pragma once

#include <algorithm>
#include <cstdint>

namespace phazite::devices::dsp
{
class ControlGrid
{
public:
    explicit ControlGrid (int stepSamples) noexcept
        : step (std::max (1, stepSamples))
    {
    }

    void reset() noexcept
    {
        at = left = length = 0;
        reached = 1.0f;
    }

    // As a block starts: whether the song plays, and where it is, in samples.
    void startBlock (bool playing, std::int64_t songSample) noexcept
    {
        if (! playing)
            return;

        if (const auto wanted = (int) (((songSample % step) + step) % step); wanted != at)
        {
            if (left > 0)
                reached = (float) (length - left) / (float) length;

            at = wanted;
            left = 0;
        }
    }

    // Whether a step starts at the next sample; and starting it, as long as it runs to the grid's next line.
    bool startsStep() const noexcept { return left == 0; }

    int startStep() noexcept
    {
        length = step - at;
        left = length;
        return length;
    }

    int getStepLength() const noexcept { return length; }
    int getDone() const noexcept { return length - left; } // samples into the step under way

    // How many of the samples there are the step under way takes; and moving on by them.
    int take (int available) const noexcept { return std::min (left, available); }

    void advance (int samples) noexcept
    {
        left -= samples;
        at = (at + samples) % step;

        if (left == 0)
            reached = 1.0f;
    }

    bool endsStep() const noexcept { return left == 0; } // the step under way ended with the last sample

    // How far along its way the step that ended last got, from 0 to 1: all of it, unless the song jumped in the middle of it
    // (a loop going round, playback started elsewhere, a device woken from sleep). A glide made of steps starts the next one
    // from where it reached, from + (to - from) * reached, not from where it was going.
    float getReached() const noexcept { return reached; }

    // For a device that plays a stretch at a time (the synths): the stretch of at most `available` samples that starts here,
    // within one step, starting a step if one is due; and moving on by it.
    struct Stretch
    {
        bool newStep = false;  // a step begins with it: its values are due
        int at = 0;            // samples into its step
        int untilNextStep = 0; // from its start to the step's end: what the step's values cover from here
        int length = 0;        // how many samples it plays, up to untilNextStep
    };

    Stretch next (int available) noexcept
    {
        const auto newStep = startsStep();

        if (newStep)
            startStep();

        const Stretch stretch { newStep, getDone(), left, take (std::max (0, available)) };
        advance (stretch.length);
        return stretch;
    }

private:
    int step = 1;
    int at = 0;     // where the next sample is on the grid
    int left = 0;   // samples left of the step under way
    int length = 0; // the step's length
    float reached = 1.0f;
};
} // namespace phazite::devices::dsp
