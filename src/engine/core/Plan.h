// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's engine plays (docs/tracktion-migration.md, T2): built on the message thread from the song
// (model/playback/), at the engine's sample rate, never changed once built, and handed to the audio thread whole
// (core/Handover.h). What sounds on from one plan to the next (a device's voices, a reverb's tail, a channel's fader) isn't
// in it: a plan points at it, kept by the song's IDs, so that a new plan doesn't reset the sound. Pieces that don't
// change (a track's notes) are shared between plans; each is freed with the last plan that points at it, on the message
// thread.
#pragma once

#include "TempoMap.h"
#include "engine/audio/AudioRegion.h"
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

    // A strip whose sound another adds up: after its fader, or before it (a send before the fader); at a send's level (or
    // as it is); lined up with the others there by a delay (none when it needs none).
    struct Input
    {
        int strip = 0;
        std::shared_ptr<Delay> delay {};
        bool beforeFader = false;
        std::shared_ptr<SendLevel> level {};
    };

    // A device, with the parameters its automation moves, which follow the song as late as the sound comes into it (from
    // the devices before it, and for the master's, from the tracks it adds up), so that they line up with its sound; and
    // what modulates it, its sources its strip's modulators in their order (or none), whose values on a block's control
    // points the audio thread gives it for each block.
    struct Device
    {
        std::shared_ptr<DeviceNode> node {};
        std::vector<ParameterCurve> automation {};
        int latency = 0;
        std::shared_ptr<Modulation> modulation {};

        // The strip its sidechain listens to (after that strip's fader and before its mute), lined up with its sound by a
        // delay (none when it needs none); or none.
        int sidechain = -1;
        std::shared_ptr<Delay> sidechainDelay {};
    };

    // A container of effects among a strip's devices: from its first to its last, and its mix with what came in.
    struct Container
    {
        int first = 0, last = 0;
        std::shared_ptr<ContainerState> state;
    };

    // A modulator moving a fader's volume or pan, by its place among its strip's, by an amount from -1 to 1.
    struct Route
    {
        int source = 0;
        float amount = 0.0f;
    };

    // A track, or the master: what it plays (its notes into its instrument, or the strips it adds up), through its devices
    // and its channel, whose volume (in the fader's positions) and pan may follow their automation.
    struct Strip
    {
        std::int64_t id = 0; // the track's ID, 0 for the master
        std::shared_ptr<StripState> state;
        std::shared_ptr<const NoteSequence> notes; // or none
        std::vector<AudioRegion> audio;            // its audio clips, which play beside its instrument
        std::vector<Device> devices;               // in the order the sound goes through them, an instrument first
        bool hasInstrument = false;
        std::vector<Input> inputs;

        // The audio device's inputs it hears (a track without an instrument): one, on both sides, or a pair, by their
        // channels; or none.
        std::vector<int> inputChannels;
        int latency = 0; // how late its sound comes out, in samples, from its devices and what it adds up, as its fader's
                         // automation follows the song
        std::shared_ptr<const Curve> volume, pan;               // or none
        std::vector<std::shared_ptr<ModulatorNode>> modulators; // its modulators and macros, kept by their IDs
        std::vector<Route> volumeModulation, panModulation;
        std::vector<Container> containers; // in their order among its devices

        // Whether another strip takes its sound before its fader (a send), or after its fader and before its mute (a
        // sidechain), which it then plays even while it is muted.
        bool feedsBeforeFader = false, feedsSidechain = false;
    };

    // In the order they play: each after the strips it adds up, those that send to it and those its devices' sidechains
    // listen to. The master last, whose sound goes out; none in a plan of the transport alone.
    std::vector<Strip> strips;
};
} // namespace phazite::engine
