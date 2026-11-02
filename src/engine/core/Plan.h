// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's engine plays (docs/tracktion-migration.md, T2): built on the message thread from the song
// (model/playback/), at the engine's sample rate, never changed once built, and handed to the audio thread whole
// (core/Handover.h). What sounds on from one plan to the next (a device's voices, a reverb's tail) isn't in it: a plan
// points at it, kept by the song's IDs, so that a new plan doesn't reset the sound.
#pragma once

#include "TempoMap.h"

#include <optional>

namespace phazite::engine
{
struct Plan
{
    TempoMap tempo;

    // What plays over and over while the loop is on, in beats; none while it is off.
    struct BeatRange
    {
        double start = 0.0, end = 0.0;
    };

    std::optional<BeatRange> loop;

    // Whether the metronome clicks on each beat while the song plays.
    bool metronome = false;
};
} // namespace phazite::engine
