// SPDX-License-Identifier: AGPL-3.0-or-later
// While one lives, plug-ins load into the tests' helper program rather than the tests' process (docs/tracktion-migration.md,
// T3; model/plugins/internal/PluginHost.h), and the test engine knows the tests' stand-ins (host/TestPlugins.h) as if a
// scan had found them: the device lists offer them, and a track adds one by its "file" ("PhaziteTestEffect"). A helper
// that hangs is ended sooner than the app's (in a second of the audio thread's, two of the message thread's), and the
// plug-ins that crashed as they loaded are forgotten as it goes. Not with a TestPlugin (TestPlugin.h) alive, which makes
// the effect in the tests' own process.
#pragma once

#include "TestHelpers.h"
#include "host/TestPlugins.h"
#include "model/plugins/internal/PluginHost.h"
#include "sandbox/client/Helper.h"

namespace phazite::test
{
class SandboxedPlugins
{
public:
    static constexpr int messageHangMilliseconds = 2000, audioHangMilliseconds = 1000;

    SandboxedPlugins()
        : wasOn (getHost().isOn())
    {
        getHost().setOn (true);
        getHost().setHangLimits (messageHangMilliseconds, audioHangMilliseconds);

        for (const auto kind : TestPlugins::kinds)
            getEngine().getPluginManager().knownPluginList.addType (TestPlugins::getDescription (kind));
    }

    ~SandboxedPlugins()
    {
        for (const auto kind : TestPlugins::kinds)
            getEngine().getPluginManager().knownPluginList.removeType (TestPlugins::getDescription (kind));

        getHost().setCrashedWhileLoading ({});
        getHost().setHangLimits (sandbox::Helper::messageHangMilliseconds, sandbox::Helper::audioHangMilliseconds);
        getHost().setOn (wasOn);
    }

    static PluginHost& getHost() { return getPluginHost (getEngineContext()); }

private:
    const bool wasOn;

    JUCE_DECLARE_NON_COPYABLE (SandboxedPlugins)
};
} // namespace phazite::test
