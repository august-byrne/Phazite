// SPDX-License-Identifier: AGPL-3.0-or-later
// While one lives, plug-ins load into the tests' helper program rather than the tests' process (docs/tracktion-migration.md,
// T3; model/plugins/internal/PluginHost.h), and the test engine knows the tests' stand-ins (host/TestPlugins.h) as if a
// scan had found them: the device lists offer them, and a track adds one by its "file" ("PhaziteTestEffect"). Not with a
// TestPlugin (TestPlugin.h) alive, which makes the effect in the tests' own process.
#pragma once

#include "TestHelpers.h"
#include "host/TestPlugins.h"
#include "model/plugins/internal/PluginHost.h"

namespace phazite::test
{
class SandboxedPlugins
{
public:
    SandboxedPlugins()
        : wasOn (getHost().isOn())
    {
        getHost().setOn (true);

        for (const auto kind : TestPlugins::kinds)
            getEngine().getPluginManager().knownPluginList.addType (TestPlugins::getDescription (kind));
    }

    ~SandboxedPlugins()
    {
        for (const auto kind : TestPlugins::kinds)
            getEngine().getPluginManager().knownPluginList.removeType (TestPlugins::getDescription (kind));

        getHost().setOn (wasOn);
    }

    static PluginHost& getHost() { return getPluginHost (getEngineContext()); }

private:
    const bool wasOn;

    JUCE_DECLARE_NON_COPYABLE (SandboxedPlugins)
};
} // namespace phazite::test
