// SPDX-License-Identifier: AGPL-3.0-or-later
// The six modulators as Phazite's engine plays them, engine-neutral: an LFO, an envelope follower, a step sequencer, a
// random source, a breakpoint envelope and a note follower. Each gives its value on the song's control points (one every
// 32 samples from the song's start: engine/automation/Curve.h): one free-running moves on by its own time from where it
// was, stopped too; one synced to the transport is where the song is; one synced to notes starts again at each note. Its
// parameters are kept by their IDs ("rate") and in their units, as the song keeps them (model/base/internal/Schema.h:
// MODULATOR), each described as a device's are, with a default for one never set.
#pragma once

#include "devices/core/DeviceInfo.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace phazite::devices
{
enum class ModulatorKind
{
    lfo,
    envelopeFollower,
    steps,
    random,
    breakpoints,
    noteFollower
};

// A modulator's parameters, in the order its values are handed to next(): each with its ID, as the song keeps it, its
// name, its range and its value when none is set.
std::span<const ParameterInfo> getModulatorParameters (ModulatorKind);
int findModulatorParameter (ModulatorKind, std::string_view id); // -1 for none

// Where the song is at a control point, for the modulators that follow it.
struct SongPlace
{
    double seconds = 0.0; // from the song's start
    double beats = 0.0;   // in the time signature's own beats
    double tempo = 120.0; // in beats a minute
    int numerator = 4;    // beats in a bar
};

class Modulator
{
public:
    static constexpr int maxSteps = 64;

    explicit Modulator (ModulatorKind);
    ModulatorKind getKind() const noexcept { return kind; }

    // Before it plays: the sample rate (for the envelope follower), and the seed of its random numbers, the same each time
    // for the same modulator, so that a render plays the same every time.
    void prepare (double sampleRate, std::uint32_t seed);

    // From its start: its cycle at its beginning, its envelope silent, no note heard yet.
    void reset();

    // A note started (since the last control point): a modulator synced to notes starts its cycle again, and the note
    // follower follows its pitch or velocity (0 to 127).
    void noteOn (int pitch, int velocity) noexcept;

    // The envelope follower listens to a sound (any other ignores it), given its parameters.
    void listen (const float* const* channels, int numChannels, int numSamples, std::span<const float> parameters) noexcept;

    // Its value at a control point, where the song is, given its parameters (in getModulatorParameters()' order) and a step
    // sequencer's steps (-1 to 1); then it moves on by so many seconds, to the next control point. Real-time safe.
    float next (std::span<const float> parameters, std::span<const float> steps, const SongPlace&, double seconds) noexcept;

private:
    ModulatorKind kind;
    double sampleRate = 44100.0;
    juce::Random random;
    std::uint32_t seed = 0;

    // Where a free cycle is, from 0 to 1; the point of the cycle it was at last (a cycle starting again picks a new random
    // value); and the random values it goes from and to.
    double cycle = 0.0;
    float lastPoint = 0.0f;
    float fromRandom = 0.0f, toRandom = 0.0f;
    bool noteStarted = false;

    // The envelope follower's envelope, as its filters left it; and the note follower's last note.
    float envelope = 0.0f;
    int holdLeft = 0;
    juce::IIRFilter lowPass, highPass;
    float lowPassFrequency = 0.0f, highPassFrequency = 0.0f;
    float noteValue = -1.0f;
    int lastNote = -1, lastVelocity = 0;

    // The point of its cycle at a control point, from 0 to 1, a free one moved on after it: for a step sequencer, the cycle
    // is its pattern, so many of its rate's lengths.
    double movePhase (std::span<const float> parameters, double patternUnits, const SongPlace&, double seconds) noexcept;
    float getLfoValue (std::span<const float> parameters, float newPhase) noexcept;
    float getRandomValue (std::span<const float> parameters, float newPhase) noexcept;
    float getBreakpointValue (std::span<const float> parameters, float newPhase) const noexcept;
};
} // namespace phazite::devices
