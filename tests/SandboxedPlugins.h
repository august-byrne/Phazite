// SPDX-License-Identifier: AGPL-3.0-or-later
// While one lives, the test engine knows the tests' stand-ins (host/TestPlugins.h) as if a scan had found them, each
// loading into the tests' helper program as every plug-in does (model/plugins/internal/PluginHost.h): the device lists
// offer them, and a track adds one by its "file" ("PhaziteTestEffect"). A helper is taken for hung only after the tests'
// limit of a wait (hangTimeout), which only a hang reaches: a busy machine never ends one (known bug 180); a test of a
// hang sets the short limits below itself (useShortHangLimits()), with a stand-in that stalls until it is let go. The
// plug-ins that crashed as they loaded are forgotten as it goes. Not with a TestPlugin (TestPlugin.h) alive, which lists
// the effect in a version of its own.
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
    // A hang test's limits: a helper whose message thread doesn't answer for two seconds, or whose audio thread keeps a
    // block for one, is ended (a stand-in stalled on purpose, which nothing else lets go of).
    static constexpr int messageHangMilliseconds = 2000, audioHangMilliseconds = 1000;

    SandboxedPlugins()
    {
        getHost().setHangLimits (hangTimeout, hangTimeout);

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

    // For a test of a hang: the short limits above, for the helpers started from now on (a hanging stand-in added after).
    static void useShortHangLimits (int message = messageHangMilliseconds, int audio = audioHangMilliseconds)
    {
        getHost().setHangLimits (message, audio);
    }

private:
    JUCE_DECLARE_NON_COPYABLE (SandboxedPlugins)
};
} // namespace phazite::test
