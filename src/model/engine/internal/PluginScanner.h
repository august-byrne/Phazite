// SPDX-License-Identifier: AGPL-3.0-or-later
// Scanning plug-ins in a child copy of Phazite, as Tracktion does, with a time limit for each (docs/performance-audit.md,
// R5). Tracktion's scanner waits for ever for a plug-in that hangs as it is scanned, since the child goes on answering
// pings while it does. One that takes longer than pluginScanSecondsEach is stopped (the child with it), left out and
// blacklisted, so later scans skip it; a crash is tried once more with a fresh child, as Tracktion does. A scan cancelled
// blacklists nothing, where Tracktion's would blacklist the plug-in it was on.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
inline constexpr int pluginScanSecondsEach = 60;

// Puts Phazite's scanner in place of Tracktion's, with Tracktion's Abort pointing at it. createEngine() calls it when
// plug-ins are scanned in a child copy.
void usePluginScannerWithTimeLimit (te::Engine&);
} // namespace phazite
