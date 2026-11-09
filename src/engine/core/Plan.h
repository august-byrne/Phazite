// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's engine plays: built on the message thread from the song (model/playback/), at the engine's sample
// rate, never changed once built, and handed to the audio thread whole (core/Handover.h). What sounds on from one plan
// to the next (a device's voices, a reverb's tail, a channel's fader) isn't in it: a plan points at it, kept by the
// song's IDs, so that a new plan doesn't reset the sound. Pieces that don't change (a track's notes) are shared between
// plans; each is freed with the last plan that points at it, on the message thread.
#pragma once

#include "TempoMap.h"
#include "engine/audio/AudioRegion.h"
#include "engine/nodes/Comparison.h"
#include "engine/nodes/Container.h"
#include "engine/nodes/DeviceNode.h"
#include "engine/nodes/NoteSequence.h"
#include "engine/nodes/PartOutput.h"
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
    // as it is); lined up with the others there by a delay (none when it needs none). Or one of its devices' parts, which
    // that device plays into an output of its own (a drum sampler's pad on a track of its own), from where the device is.
    struct Input
    {
        int strip = 0;
        std::shared_ptr<Delay> delay {};
        bool beforeFader = false;
        std::shared_ptr<SendLevel> level {};
        std::shared_ptr<PartOutput> part {};
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

        // Its parts that play into outputs of their own, which other strips add up (Input::part): each one's buffer, and the
        // same as the device takes them; none for most devices.
        std::vector<std::shared_ptr<PartOutput>> partOutputs {};
        std::vector<devices::PartOutput> parts {};
    };

    // A modulator moving a fader's volume or pan (a channel's, or a container's chain's) or a chain's crossover, by its place
    // among its strip's, by an amount from -1 to 1.
    struct Route
    {
        int source = 0;
        float amount = 0.0f;
    };

    // A container among a strip's devices (nodes/Container.h): all its chains' devices, in their order, so many from its
    // first among the strip's; each chain a run of them, which plays its part of the sound coming in (an instrument
    // container's, the strip's notes in its ranges, from silence) through its fader, whose volume and pan, and its
    // crossover, follow their automation (as late as its sound comes out of it) and its strip's modulators; each lined up
    // with the latest chain by a delay (none when it needs none). What came in, kept and delayed as much, mixes with what
    // they put out by the container's mix (an instrument container's has none).
    struct Container
    {
        int first = 0, count = 0;
        Split split = Split::layers;
        bool instrument = false;
        std::shared_ptr<ContainerState> state;

        struct Chain
        {
            std::int64_t id = 0; // the chain's
            int first = 0, count = 0;
            int latency = 0; // how late its sound comes out of it, in samples, from the strip's start
            std::shared_ptr<ChainState> state;
            std::shared_ptr<Delay> delay {};
            std::shared_ptr<const Curve> volume {}, pan {}, crossover {}; // or none
            std::vector<Route> volumeModulation {}, panModulation {}, crossoverModulation {};
        };

        std::vector<Chain> chains;
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
        // sidechain), which it then plays even while it is muted; and whether another adds up one of its devices' parts
        // (Input::part), which it then plays while it is muted too, so that the part follows its own channel's mute.
        bool feedsBeforeFader = false, feedsSidechain = false, feedsParts = false;

        // Whether its instrument is handed its blocks early (devices::Processor::startsEarly(), without a sidechain), its
        // notes and modulators taken first: the engine's, as it is given the plan.
        bool startsEarly = false;
    };

    // In the order they play: each after the strips it adds up, those that send to it and those its devices' sidechains
    // listen to. The master last, whose sound goes out; none in a plan of the transport alone.
    std::vector<Strip> strips;

    // The strips whose instruments are handed their blocks early, in their order: the engine's, as it is given the plan.
    std::vector<int> earlyStrips;

    // A version's bounce heard against the song, after the master (Comparison.h): its whole file from the version's start,
    // where each of the song's beats is in it (the version's tempo map, at the plan's rate), and which of the two is heard.
    // None while nothing is compared.
    struct Comparison
    {
        AudioRegion bounce;
        TempoMap tempo;
        std::shared_ptr<ComparisonState> state;
    };

    std::optional<Comparison> comparison;
};
} // namespace phazite::engine
