// SPDX-License-Identifier: AGPL-3.0-or-later
// Scanning plug-ins in Phazite's helper program (sandbox/client/Helper.h), a file at a time, with a time limit for each
// (docs/archive/performance-audit.md, R5): the helper loads the plug-in's code to find what the file holds, so that one that
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

// What a helper's answer to a scan says a file holds, as Phazite takes it (known bug 181): no more plug-ins than the
// answer's bytes could hold, nor than maxScannedPlugins (several thousand: a shell file, as Waves' WaveShell, holds
// hundreds), each description's XML of maxScannedXml characters at most, and its texts (name, maker, category, version,
// identifier) cut to what Phazite keeps. What that leaves out of what the helper told is said in leftOut ("12 of the 9000
// plug-ins it holds left out (...)"), empty when nothing is; the scan notes it with the host (PluginHost::noteScanProblem()),
// which the plug-in list's window shows.
inline constexpr int maxScannedPlugins = 8192, maxScannedXml = 65536, maxScannedPath = 4096;
juce::OwnedArray<juce::PluginDescription> readScannedPlugins (const juce::MemoryBlock& answer, juce::String* leftOut = nullptr);
} // namespace phazite
