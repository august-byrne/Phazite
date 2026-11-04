// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "host/TestPlugins.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"

namespace phazite::test
{
namespace
{
    constexpr int stateMilliseconds = 10000;

    juce::KnownPluginList& getKnownPlugins()
    {
        return getPluginHost (getEngineContext()).getKnownPlugins();
    }

    // The plug-in's instance in its helper, through the song's session.
    SandboxedInstance& getInstance (const Song& song, DeviceID id)
    {
        auto* instance = findPluginInstance (song, id);
        REQUIRE (instance != nullptr);
        return *instance;
    }

    // Its state as the plug-in in the helper has it: its gain, then its tone.
    juce::MemoryBlock getState (SandboxedInstance& instance)
    {
        const auto state = instance.getRemote().getState (stateMilliseconds);
        REQUIRE (state.has_value());
        REQUIRE (state->getSize() >= 8);
        return *state;
    }
} // namespace

TestPlugin::TestPlugin()
{
    getKnownPlugins().addType (describe (version));
}

TestPlugin::~TestPlugin()
{
    getKnownPlugins().removeType (describe (version));
}

void TestPlugin::setVersion (int newVersion)
{
    // Its description's version, which the helper makes it in (host/TestPlugins.h).
    getKnownPlugins().removeType (describe (version));
    version = newVersion;
    getKnownPlugins().addType (describe (version));
}

juce::PluginDescription TestPlugin::describe (int version)
{
    auto description = TestPlugins::getDescription (TestPlugins::Kind::effect);
    description.version = juce::String (version) + ".0";
    return description;
}

juce::PluginDescription TestPlugin::getDescription()
{
    return describe (1);
}

DeviceID TestPlugin::add (Song& song, TrackID track)
{
    const auto id = insertDeviceState (song, track, makeDeviceState (song, makeDeviceType (getDescription())));
    REQUIRE (id.isValid());
    return id;
}

float TestPlugin::getGain (const Song& song, DeviceID id)
{
    return juce::MemoryInputStream (getState (getInstance (song, id)), false).readFloat();
}

void TestPlugin::setGain (const Song& song, DeviceID id, float gain)
{
    // Its state with another gain, which it takes as its own window would set it, telling its host.
    auto& instance = getInstance (song, id);
    juce::MemoryInputStream before (getState (instance), true);
    before.readFloat();
    juce::MemoryOutputStream changed;
    changed.writeFloat (gain);
    changed.writeFloat (before.readFloat()); // its tone as it was
    changed.writeFloat (1.0f);
    REQUIRE (instance.getRemote().setState (changed.getMemoryBlock(), stateMilliseconds));

    // Its instance's value for it, once the helper has told.
    const auto index = instance.findParameter (gainID);
    REQUIRE (index >= 0);
    REQUIRE (dispatchMessagesUntil ([&] { return juce::exactlyEqual (instance.getParameterValue (index), gain); }));
}
} // namespace phazite::test
