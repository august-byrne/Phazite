// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "host/TestPlugins.h"
#include "model/plugins/internal/PluginHost.h"

namespace phazite::test
{
namespace
{
    constexpr int stateMilliseconds = 10000;

    // The plug-in's instance in its helper.
    SandboxedInstance& getInstance (const te::ExternalPlugin& plugin)
    {
        auto* instance = dynamic_cast<SandboxedInstance*> (plugin.getAudioPluginInstance());
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
    getEngine().getPluginManager().knownPluginList.addType (describe (version));
}

TestPlugin::~TestPlugin()
{
    getEngine().getPluginManager().knownPluginList.removeType (describe (version));
}

void TestPlugin::setVersion (int newVersion)
{
    // Its description's version, which the helper makes it in (host/TestPlugins.h).
    auto& known = getEngine().getPluginManager().knownPluginList;
    known.removeType (describe (version));
    version = newVersion;
    known.addType (describe (version));
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

float TestPlugin::getGain (const te::ExternalPlugin& plugin)
{
    return juce::MemoryInputStream (getState (getInstance (plugin)), false).readFloat();
}

void TestPlugin::setGain (te::ExternalPlugin& plugin, float gain)
{
    // Its state with another gain, which it takes as its own window would set it, telling its host.
    juce::MemoryInputStream before (getState (getInstance (plugin)), true);
    before.readFloat();
    juce::MemoryOutputStream changed;
    changed.writeFloat (gain);
    changed.writeFloat (before.readFloat()); // its tone as it was
    changed.writeFloat (1.0f);
    REQUIRE (getInstance (plugin).getRemote().setState (changed.getMemoryBlock(), stateMilliseconds));

    // Tracktion's parameter for it (by its place), once the helper has told.
    const auto& parameters = getInstance (plugin).getRemote().getInfo().parameters;
    const auto own = std::find_if (parameters.begin(), parameters.end(), [] (const auto& p) { return p.id == gainOwnID; });
    REQUIRE (own != parameters.end());
    const auto tracktions = plugin.getAutomatableParameterByID (juce::String ((int) (own - parameters.begin())));
    REQUIRE (tracktions != nullptr);
    REQUIRE (dispatchMessagesUntil ([&] { return juce::exactlyEqual (tracktions->getCurrentValue(), gain); }));
}

te::ExternalPlugin::Ptr TestPlugin::create (te::Edit& edit)
{
    te::ExternalPlugin::Ptr plugin =
        dynamic_cast<te::ExternalPlugin*> (edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, getDescription()).get());
    REQUIRE (plugin != nullptr);
    plugin->initialiseFully();
    REQUIRE (plugin->getAudioPluginInstance() != nullptr);
    return plugin;
}
} // namespace phazite::test
