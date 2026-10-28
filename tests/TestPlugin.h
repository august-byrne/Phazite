// SPDX-License-Identifier: AGPL-3.0-or-later
// A stand-in for a plug-in (a VST3 or the like) in tests: an effect made in this process, which Tracktion's
// te::ExternalPlugin wraps as it wraps a plug-in it loaded, so that tests reach what Phazite does with plug-ins (their
// windows, menus and presets, their mix and key controls, their parameters through updates) without one installed. It
// scales its sound by its Gain, has a Tone that does nothing, keeps both as its state, and has an editor: a knob for each.
// Its parameters are as a VST3's are: each has an ID of the plug-in's own, and Tracktion knows them by their places.
#pragma once

#include "TestHelpers.h"

#include <functional>

namespace phazite::test
{
// While one is alive, the test engine knows the stand-in, as if a scan had found it: its known plug-ins list it (so the
// device lists offer it, "Test Effect" by "Phazite Tests"), and Tracktion makes it when an Edit asks. Make it before the
// Edits that hold one, and let it go after them.
class TestPlugin
{
public:
    TestPlugin();
    ~TestPlugin();

    static juce::PluginDescription getDescription();

    // One for an Edit, not on a track yet, as phazite::createDevice() makes a plug-in.
    static te::ExternalPlugin::Ptr create (te::Edit&);

    // Its parameters, as Tracktion names them in its first version, by their places: each from 0 to 1, the gain at 1 and
    // the tone at 0.5 to start with.
    static constexpr const char* gainID = "0";
    static constexpr const char* toneID = "1";

    // The plug-in's own IDs for them (a VST3's numbers), which its updates keep: the other way round from their places in
    // the first version, so that a test can't take one for the other.
    static constexpr const char* gainOwnID = "1";
    static constexpr const char* toneOwnID = "0";

    // Which version Tracktion makes from now on, as if the plug-in had been updated: 1, Gain and Tone; 2, Tone, Gain and a
    // new Drive, which does nothing; 3, Tone alone.
    void setVersion (int);

    // Its gain as the plug-in itself has it, set as its editor sets it. (Tracktion's parameter follows a change the plug-in
    // makes once the message loop runs.)
    static float getGain (const te::ExternalPlugin&);
    static void setGain (te::ExternalPlugin&, float);

private:
    std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::PluginDescription&, double, int, juce::String&)> previous;
    int version = 1;

    JUCE_DECLARE_NON_COPYABLE (TestPlugin)
};
} // namespace phazite::test
