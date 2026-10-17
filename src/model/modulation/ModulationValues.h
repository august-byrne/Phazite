// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation as plain values (docs/design.md, section 10; roadmap step 2.3), for the model's public functions
// (ModulationModel.h) and its Tracktion side alike: the kinds of modulators, and what modulation does to a parameter.
#pragma once

#include "model/base/IDs.h"
#include "model/devices/ParameterRef.h"

#include <juce_core/juce_core.h>

namespace phazite
{
// What moves the parameters it modulates: the six modulators, which move them as the song plays, and macros, knobs
// that move several at once.
enum class ModulatorType
{
    lfo,              // a wave, repeating
    envelopeFollower, // how loud the track is
    steps,            // a step sequence
    random,           // random values
    breakpoints,      // an envelope drawn as points, repeating
    noteFollower,     // the notes the track plays: their pitch or velocity
    macro             // a knob
};

inline constexpr ModulatorType allModulatorTypes[] = { ModulatorType::lfo,    ModulatorType::envelopeFollower, ModulatorType::steps,
                                                       ModulatorType::random, ModulatorType::breakpoints,      ModulatorType::noteFollower,
                                                       ModulatorType::macro };

// "LFO", "Envelope Follower", "Step Sequencer", "Random", "Breakpoint Envelope", "Note Follower", "Macro".
juce::String getModulatorTypeName (ModulatorType);

struct ModulatorInfo
{
    ModulatorID id {};
    TrackID track {}; // whose it is: a track's, a folder's, or the master's (AutomationModel.h: getMasterTrack())
    ModulatorType type = ModulatorType::lfo;
    juce::String name {}; // "LFO 2", "Macro 1"
};

// One of a modulator's parameters, as its editor shows it: "rate", "Rate".
struct ModulatorParameter
{
    juce::String id {}, name {};
};

// A modulator moving a parameter by an amount, from -1 to 1: at most by the whole of the parameter's range, the other
// way when negative.
struct Modulation
{
    ModulatorID modulator {};
    ParameterRef parameter {};
    float amount = 0.0f;
};

// What modulation does to a parameter, as parts of its range (0 to 1), for the ring around its control: the range its
// modulations can move it over from its value (or its automation's), and where it is now.
struct ModulationRange
{
    float low = 0.0f, high = 0.0f, now = 0.0f;
};
} // namespace phazite
