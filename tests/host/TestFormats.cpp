// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-in formats the tests' helper hosts (host/core/Host.h): the app's, and the tests' stand-ins (TestPlugins.h).
#include "TestPlugins.h"
#include "host/core/Host.h"

namespace phazite::host
{
void addPluginFormats (juce::AudioPluginFormatManager& formats)
{
    // A helper that hangs as it starts, before it connects to Phazite's pipe, for the test that asks for one by this
    // variable of its environment, which the helper inherits (SandboxTests.cpp).
    if (juce::SystemStats::getEnvironmentVariable ("PHAZITE_TEST_HOST_HANGS_AT_START", {}).isNotEmpty())
        juce::Thread::sleep (60000);

    juce::addDefaultFormatsToManager (formats);
    formats.addFormat (test::TestPlugins::createFormat());
}
} // namespace phazite::host
