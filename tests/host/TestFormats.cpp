// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-in formats the tests' helper hosts (host/core/Host.h): the app's, and the tests' stand-ins (TestPlugins.h).
#include "TestPlugins.h"
#include "host/core/Host.h"

namespace phazite::host
{
void addPluginFormats (juce::AudioPluginFormatManager& formats)
{
    juce::addDefaultFormatsToManager (formats);
    formats.addFormat (test::TestPlugins::createFormat());
}
} // namespace phazite::host
