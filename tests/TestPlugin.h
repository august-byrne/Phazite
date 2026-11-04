// SPDX-License-Identifier: AGPL-3.0-or-later
// A stand-in for a plug-in (a VST3 or the like) in tests: the tests' effect (host/TestPlugins.h), hosted in the tests'
// helper program as Phazite hosts every plug-in (model/plugins/internal/PluginHost.h), so that tests reach what Phazite
// does with plug-ins (their windows, menus and presets, their mix and key controls, their parameters through updates)
// without one installed. It scales its sound by its Gain, has a Tone that does nothing, and keeps both as its state. Its
// parameters are as a VST3's are: each has an ID of the plug-in's own, by which the song names it.
#pragma once

#include "TestHelpers.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

namespace phazite::test
{
// While one is alive, the tests' plug-in host knows the stand-in, as if a scan had found it: its known plug-ins list it
// (so the device lists offer it, "Test Effect" by "Phazite Tests"), and a song's session makes it, in a helper, when the
// song holds one. Make it before the songs that hold one, and let it go after them.
class TestPlugin
{
public:
    TestPlugin();
    ~TestPlugin();

    static juce::PluginDescription getDescription();

    // One at the end of a track's devices, as adding it from a device list does, in the caller's undo step; its ID.
    static DeviceID add (Song&, TrackID);

    // Its parameters, by the plug-in's own IDs (a VST3's numbers), which its updates keep: each from 0 to 1, the gain at 1
    // and the tone at 0.5 to start with; the other way round from their places in its first version, so that a test can't
    // take one for the other.
    static constexpr const char* gainID = "1";
    static constexpr const char* toneID = "0";

    // Which version the helper makes from now on, as if the plug-in had been updated: 1, Gain and Tone; 2, Tone, Gain and a
    // new Drive, which does nothing; 3, Tone alone.
    void setVersion (int);

    // Its gain as the plug-in itself has it (in its helper), and set as its editor sets it: its instance's value follows
    // once the helper has told, which this waits for. Through the song's session.
    static float getGain (const Song&, DeviceID);
    static void setGain (const Song&, DeviceID, float);

private:
    int version = 1;

    static juce::PluginDescription describe (int version);

    JUCE_DECLARE_NON_COPYABLE (TestPlugin)
};
} // namespace phazite::test
