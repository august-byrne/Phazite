// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' stand-ins for plug-ins (a VST3 or the like), as the tests' helper hosts them (docs/tracktion-migration.md,
// T3): made in the helper's process by a format of their own, "Test", so that tests reach what Phazite does with
// plug-ins through its host without one installed. The effect scales its sound by its Gain and has a Tone that does
// nothing; its parameters have IDs of the plug-in's own, as a VST3's have.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace phazite::test
{
struct TestPlugins
{
    static constexpr const char* formatName = "Test";
    static constexpr const char* effectID = "PhaziteTestEffect"; // its "file" in the format

    static juce::PluginDescription getEffectDescription();

    // The effect itself, as the format makes it (for tests that play it in their own process to compare).
    static std::unique_ptr<juce::AudioPluginInstance> createEffect();

    // The format, for a format manager.
    static std::unique_ptr<juce::AudioPluginFormat> createFormat();
};
} // namespace phazite::test
