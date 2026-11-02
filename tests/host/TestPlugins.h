// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' stand-ins for plug-ins (VST3s or the like), as the tests' helper hosts them (docs/tracktion-migration.md,
// T3): made in the helper's process by a format of their own, "Test", so that tests reach what Phazite does with
// plug-ins through its host without one installed. Their parameters have IDs of the plug-in's own, as a VST3's have.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace phazite::test
{
struct TestPlugins
{
    static constexpr const char* formatName = "Test";

    enum class Kind
    {
        effect,    // scales its sound by its Gain ("1"), and has a Tone ("0") that does nothing but make its editor larger
                   // (300 by 200, 360 by 240 above 0.9); keeps both as its state; its programs, Full and Half, set its Gain
        synth,     // a sine for each note held, at its Level ("10"); note 127 turns its Level to 0.75 itself, in a gesture
                   // held while the note is (as a control of its window would)
        crash,     // an effect that crashes as it plays a block while its Crash ("20") is on
        hang,      // an effect that plays nothing back while its Hang ("30") is on: it loops for ever
        latency,   // delays its sound by 64 samples, or 128 while its Latency ("40") is on, and says so
        sidechain, // plays what comes into its sidechain bus in place of its sound
    };

    static constexpr Kind kinds[] = { Kind::effect, Kind::synth, Kind::crash, Kind::hang, Kind::latency, Kind::sidechain };

    static juce::PluginDescription getDescription (Kind);

    // One made in the tests' own process, as the format makes it (for tests that play it here to compare).
    static std::unique_ptr<juce::AudioPluginInstance> create (Kind);

    // The format, for a format manager.
    static std::unique_ptr<juce::AudioPluginFormat> createFormat();
};
} // namespace phazite::test
