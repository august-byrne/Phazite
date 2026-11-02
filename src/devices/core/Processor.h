// SPDX-License-Identifier: AGPL-3.0-or-later
// How a device's DSP runs, without any engine: the device's thin Tracktion plug-in (src/model/devices/internal/DevicePlugin.h)
// prepares it before playback and hands it each block's audio, notes and parameter values.
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
        pitchBend,  // value: from -1 to 1
        controller, // number: the controller; value: from 0 to 1
        allNotesOff // every note lets go: playback stopped or jumped; when it stopped, those that only just began stop at once
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

// One block to process.
struct ProcessContext
{
    // The audio, processed in place: an effect's input, or an instrument's output, cleared before it plays.
    float* const* channels = nullptr;
    int numChannels = 0;
    int numSamples = 0;

    // A sidechain's audio, for a device that has one and a source feeding it; otherwise none.
    const float* const* sidechain = nullptr;
    int numSidechainChannels = 0;

    std::span<const NoteEvent> events {}; // in time order
    std::span<const float> parameters {}; // each parameter's value now, in the order and ranges of the device's DeviceInfo

    // For a device that takes its blocks whole: the values of the parameters that automation or modulation moves, each at
    // the block's start and on each control point after where it changes, and each that they move no more at its value
    // again, in time order; nothing for those set by hand alone, which reach it otherwise.
    std::span<const ParameterChange> changes {};

    double tempo = 120.0;     // beats per minute at the block's start, for rates in time with the song
    double beat = 0.0;        // where the song is at the block's start, in beats (quarter notes), for sweeps in time with it
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

    // How long its sound goes on after its input stops, in seconds (a reverb's tail).
    virtual double getTailSeconds() const { return 0.0; }

    // Whether its thin plug-in may let it sleep once what comes in and what it plays have been silent a while: not a
    // plug-in, which may play with nothing coming in (an arpeggiator's own pattern, a drone).
    virtual bool sleepsOnSilence() const noexcept { return true; }

    // Whether it takes a block whole, with its moved parameters' values as changes at their samples (ProcessContext::
    // changes), rather than a stretch at a time with their values: a plug-in in a helper process, to which each call is a
    // round trip.
    virtual bool takesWholeBlocks() const noexcept { return false; }

    // Whether something came in other than as a note or sound and waits for its next block, such as a pad its editor
    // played: its thin plug-in, asleep on silence, wakes for it. Real-time safe.
    virtual bool isWaitingToPlay() const noexcept { return false; }

    // Whether it plays something other than its effect now, such as a compressor letting its key be heard: its thin
    // plug-in then passes what it plays on whole, whatever its mix. Real-time safe.
    virtual bool overridesMix() const noexcept { return false; }

    // How far it turns the sound down now, in decibels (0 or more), for what shows it besides its editor, such as its slot
    // in the mixer (FallingValue.h: any number of readers); nullptr for a device that doesn't turn the sound down.
    virtual const FallingValue* getGainReduction() const noexcept { return nullptr; }
};

// How fast a gain reduction shown outside a device's editor falls back, as the editors' meters do: 15 dB a second.
inline constexpr float reductionFallPerSecond = 15.0f;
} // namespace phazite::devices
