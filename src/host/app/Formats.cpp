// SPDX-License-Identifier: AGPL-3.0-or-later
// The plug-in formats the app's helper hosts (host/core/Host.h): VST3 (and Audio Units on macOS, after v1's Windows).
#include "host/core/Host.h"

namespace phazite::host
{
void addPluginFormats (juce::AudioPluginFormatManager& formats)
{
    juce::addDefaultFormatsToManager (formats);
}
} // namespace phazite::host
