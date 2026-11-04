// SPDX-License-Identifier: AGPL-3.0-or-later
// While one lives, the test engine knows the tests' stand-ins (host/TestPlugins.h) as if a scan had found them, each
// loading into the tests' helper program as every plug-in does (model/plugins/internal/PluginHost.h): the device lists
// offer them, and a track adds one by its "file" ("PhaziteTestEffect"). A helper that hangs is ended sooner than the
// app's (in a second of the audio thread's, two of the message thread's), and the plug-ins that crashed as they loaded
// are forgotten as it goes. Not with a TestPlugin (TestPlugin.h) alive, which lists the effect in a version of its own.
#pragma once

#include "TestHelpers.h"
#include "host/TestPlugins.h"
#include "model/engine/internal/EngineContext.h"
#include "model/plugins/internal/PluginHost.h"
#include "sandbox/client/Helper.h"

namespace phazite::test
{
class SandboxedPlugins
{
public:
    static constexpr int messageHangMilliseconds = 2000, audioHangMilliseconds = 1000;

    SandboxedPlugins()
    {
        getHost().setHangLimits (messageHangMilliseconds, audioHangMilliseconds);

        for (const auto kind : TestPlugins::kinds)
            getHost().getKnownPlugins().addType (TestPlugins::getDescription (kind));
    }

    ~SandboxedPlugins()
    {
        for (const auto kind : TestPlugins::kinds)
            getHost().getKnownPlugins().removeType (TestPlugins::getDescription (kind));

        getHost().setCrashedWhileLoading ({});
        getHost().setHangLimits (sandbox::Helper::messageHangMilliseconds, sandbox::Helper::audioHangMilliseconds);
    }

    static PluginHost& getHost() { return getPluginHost (getEngineContext()); }

private:
    JUCE_DECLARE_NON_COPYABLE (SandboxedPlugins)
};
} // namespace phazite::test
