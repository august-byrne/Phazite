// SPDX-License-Identifier: AGPL-3.0-or-later
// How a device's DSP runs, without any engine: what plays it (Phazite's engine, through its runner:
// src/engine/nodes/DeviceRunner.h) prepares it before playback and hands it each block's audio, notes and parameter
// values.
#pragma once

#include "FallingValue.h"

#include <cstdint>
#include <span>

namespace phazite::devices
{
// A note or a controller's change for an instrument, at its sample within the block.
struct NoteEvent
{
    enum class Type : std::uint8_t
    {
        noteOn,
        noteOff,
        pitchBend,   // value: from -1 to 1
        controller,  // number: the controller; value: from 0 to 1
        allNotesOff, // every note lets go: playback stopped or jumped; when it stopped, those that only just began stop at once
        // A note on that glides in from the note played before it, as FL Studio's portamento notes do (an instrument
        // without glides plays it as a note on); and every sounding note gliding to the pitch number, evenly, over value
        // seconds, as FL Studio's slide notes make them (an instrument without glides ignores it).
        portamentoOn,
        slide
    };

    Type type = Type::noteOn;
    int sample = 0;     // within the block, from 0
    int number = 0;     // the note, or the controller
    float value = 0.0f; // a note's velocity (from 0 to 1), or the bend's or controller's value
};

// A parameter's value from a sample of the block on, for a device that takes its blocks whole (takesWholeBlocks()).
struct ParameterChange
{
    int parameter = 0;  // its index in the device's DeviceInfo
    int sample = 0;     // within the block, from 0
    float value = 0.0f; // in its range
};

// An output of a device's own besides its channels, for a part of what it plays that goes to a mixer channel of its own (a
// drum sampler's pad on a track of its own, as FL Studio's FPC sends a pad to a mixer track): the part, by the device's
// own number for it (a pad's note), and its two channels, from the block's start.
struct PartOutput
{
    int part = -1;
    float* channels[2] {};
};

// One block to process.
struct ProcessContext
{
    // The audio, processed in place: an effect's input, or an instrument's output, cleared before it plays.
    float* const* channels = nullptr;
    int numChannels = 0;
    int numSamples = 0;

    // For an effect whose runner mixes what came in back in with what it plays (its mix, DeviceInfo.h): a copy of what came
    // in, on as many channels, which an effect that turns the phase of its whole sound turns the same, in place, so that the
    // two stay in phase at any mix (Safe Bass's split, whose bands add up to an all-pass: devices/motion/SafeBass.h); most
    // leave it alone. None where nothing mixes it back in.
    float* const* dry = nullptr;

    // A sidechain's audio, for a device that has one and a source feeding it; otherwise none.
    const float* const* sidechain = nullptr;
    int numSidechainChannels = 0;

    // Outputs of their own that some of its parts play into, adding to what is there, rather than into its channels, each
    // from outputOffset (where this block starts in them); none for most devices, which have no parts.
    std::span<const PartOutput> partOutputs {};
    int outputOffset = 0;

    std::span<const NoteEvent> events {}; // in time order
    std::span<const float> parameters {}; // each parameter's value now, in the order and ranges of the device's DeviceInfo

    // For a device that takes its blocks whole: the values of the parameters that automation or modulation moves, each at
    // the block's start and on each control point after where it changes, and each that they move no more at its value
    // again, in time order; nothing for those set by hand alone, which reach it otherwise.
    std::span<const ParameterChange> changes {};

    // Where the song is as the block (or stretch) starts, for rates and sweeps in time with it and for a plug-in's playhead:
    // its tempo there, in beats (quarter notes) a minute; its place, in beats from its start, in bars (2.5 half way through
    // bar 2) and in samples at the device's rate; and its time signature there, with how long its bar is and where the bar
    // the song is in starts, in beats. A rate in bars counts the song's bars (dsp/NoteValues.h).
    double tempo = 120.0, beat = 0.0, bars = 0.0;
    std::int64_t songSample = 0;
    int timeSignatureNumerator = 4, timeSignatureDenominator = 4;
    double barLength = 4.0, barStart = 0.0;

    bool isPlaying = false;   // whether the song is playing (rather than only the computer keyboard)
    bool isRendering = false; // a render, offline, which waits for what it plays rather than keep to real time
};

class Processor
{
public:
    virtual ~Processor() = default;

    // Before playback starts, off the audio thread: it may allocate.
    virtual void prepare (double sampleRate, int maximumBlockSize) = 0;

    // Silences what is sounding: voices, tails, delay lines. Real-time safe.
    virtual void reset() = 0;

    // Real-time safe: no allocation, no locks, no waiting, no I/O.
    virtual void process (const ProcessContext&) = 0;

    // How long the device delays its sound, in samples (a limiter's look-ahead), so that the engine can delay the other
    // tracks to match. Known once prepared.
    virtual int getLatencySamples() const { return 0; }

    // For a device whose latency depends on its values set by hand (the compressor looks ahead only while it ducks): its
    // latency with these values, at the rate it is prepared at; -1 for a device whose latency doesn't. Its engine's model makes
    // the device anew when a value would change it (model/playback/internal/SongPlayer.h), giving it the values before it is
    // prepared, so that getLatencySamples() is known once prepared still. Message thread.
    virtual int getLatencySamplesFor (std::span<const float> /* valuesSetByHand */) const { return -1; }
    virtual void takeValuesBeforePrepare (std::span<const float> /* valuesSetByHand */) {}

    // How long its sound goes on after its input stops, in seconds (a reverb's tail).
    virtual double getTailSeconds() const { return 0.0; }

    // Whether its runner may let it sleep once what comes in and what it plays have been silent a while: not a plug-in,
    // which may play with nothing coming in (an arpeggiator's own pattern, a drone).
    virtual bool sleepsOnSilence() const noexcept { return true; }

    // Whether it takes a block whole, with its moved parameters' values as changes at their samples (ProcessContext::
    // changes), rather than a stretch at a time with their values: a plug-in in a helper process, to which each call is a
    // round trip.
    virtual bool takesWholeBlocks() const noexcept { return false; }

    // Whether an instrument that takes its blocks whole may be handed a block early, before the engine plays the tracks,
    // so that it plays meanwhile beside the others (a plug-in instrument, in its helper process): start() takes the block
    // as process() would, and the next process(), of the same block, only takes what it played into its channels.
    // Real-time safe.
    virtual bool startsEarly() const noexcept { return false; }
    virtual void start (const ProcessContext&) {}

    // Whether something came in other than as a note or sound and waits for its next block, such as a pad its editor
    // played: its runner, asleep on silence, wakes for it. Real-time safe.
    virtual bool isWaitingToPlay() const noexcept { return false; }

    // Whether it plays something other than its effect now, such as a compressor letting its key be heard: its runner
    // then passes what it plays on whole, whatever its mix. Real-time safe.
    virtual bool overridesMix() const noexcept { return false; }

    // How far it turns the sound down now, in decibels (0 or more), for what shows it besides its editor, such as its slot
    // in the mixer (FallingValue.h: any number of readers); nullptr for a device that doesn't turn the sound down.
    virtual const FallingValue* getGainReduction() const noexcept { return nullptr; }
};

// How fast a gain reduction shown outside a device's editor falls back, as the editors' meters do: 15 dB a second.
inline constexpr float reductionFallPerSecond = 15.0f;
} // namespace phazite::devices
