// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's engine plays (docs/tracktion-migration.md, T2): built on the message thread from the song
// (model/playback/), at the engine's sample rate, never changed once built, and handed to the audio thread whole
// (core/Handover.h). What sounds on from one plan to the next (a device's voices, a reverb's tail, a channel's fader) isn't
// in it: a plan points at it, kept by the song's IDs, so that a new plan doesn't reset the sound. Pieces that don't
// change (a track's notes) are shared between plans; each is freed with the last plan that points at it, on the message
// thread.
#pragma once

#include "TempoMap.h"
#include "engine/nodes/DeviceNode.h"
#include "engine/nodes/NoteSequence.h"
#include "engine/nodes/Strip.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

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

    // A strip whose sound another adds up, lined up with the others there by a delay (none when it needs none).
    struct Input
    {
        int strip = 0;
        std::shared_ptr<Delay> delay;
    };

    // A track, or the master: what it plays (its notes into its instrument, or the strips it adds up), through its devices
    // and its channel.
    struct Strip
    {
        std::int64_t id = 0; // the track's ID, 0 for the master
        std::shared_ptr<StripState> state;
        std::shared_ptr<const NoteSequence> notes;        // or none
        std::vector<std::shared_ptr<DeviceNode>> devices; // in the order the sound goes through them, an instrument first
        bool hasInstrument = false;
        std::vector<Input> inputs;
        int latency = 0; // how late its sound comes out, in samples, from its devices and what it adds up
    };

    // In the order they play: each after the strips it adds up. The master last, whose sound goes out; none in a plan of
    // the transport alone.
    std::vector<Strip> strips;
};
} // namespace phazite::engine
