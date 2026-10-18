// SPDX-License-Identifier: AGPL-3.0-or-later
// A stand-in for a plug-in (a VST3 or the like) in tests: an effect made in this process, which Tracktion's
// te::ExternalPlugin wraps as it wraps a plug-in it loaded, so that tests reach what Phazite does with plug-ins (their
// windows, menus and presets, their mix and key controls) without one installed. It scales its sound by its Gain, has a
// Tone that does nothing, keeps both as its state, and has an editor: a knob for each.
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

    // Its parameters, as Tracktion names them: each from 0 to 1, the gain at 1 and the tone at 0.5 to start with.
    static constexpr const char* gainID = "gain";
    static constexpr const char* toneID = "tone";

    // Its gain as the plug-in itself has it, set as its editor sets it. (Tracktion's parameter follows a change the plug-in
    // makes once the message loop runs.)
    static float getGain (const te::ExternalPlugin&);
    static void setGain (te::ExternalPlugin&, float);

private:
    std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::PluginDescription&, double, int, juce::String&)> previous;

    JUCE_DECLARE_NON_COPYABLE (TestPlugin)
};
} // namespace phazite::test
