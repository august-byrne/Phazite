// SPDX-License-Identifier: AGPL-3.0-or-later
// Scanning plug-ins in Phazite's helper program (sandbox/client/Helper.h), a file at a time, with a time limit for each
// (docs/performance-audit.md, R5): the helper loads the plug-in's code to find what the file holds, so that one that
// crashes or hangs as it is scanned takes only the helper with it. One that takes longer than its time is left out and
// blacklisted (the helper ended), so later scans skip it; a crash is tried once more with a fresh helper, in case the
// plug-in scanned before was to blame. A scan cancelled ends the helper at once and blacklists nothing. Without the
// helper program, a scan finds nothing and blacklists nothing.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace phazite
{
inline constexpr int pluginScanSecondsEach = 60;

class PluginHost;

// The scanner, for a test to drive a file at a time (findPluginTypesFor()), with the time each may take; and the scanner of
// the plug-in host's list (PluginHost::getKnownPlugins()), which its cancelScan() stops, which createEngineContext() sets.
std::unique_ptr<juce::KnownPluginList::CustomScanner> createPluginScanner (PluginHost&, int secondsEach = pluginScanSecondsEach);
void usePluginScannerWithTimeLimit (PluginHost&);
} // namespace phazite
