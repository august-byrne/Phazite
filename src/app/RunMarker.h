// SPDX-License-Identifier: AGPL-3.0-or-later
// Whether the last Phazite closed properly (docs/performance-audit.md, R2): a file in the settings folder that is there for
// as long as Phazite runs, so one that is there as Phazite starts was left by one that didn't close (a crash, perhaps in
// a plug-in of the project that was open).
#pragma once

#include <juce_core/juce_core.h>

namespace phazite
{
class RunMarker
{
public:
    explicit RunMarker (juce::File); // notes whether the last run left the file, then makes it
    ~RunMarker();                    // takes it away: this run closed properly

    bool didLastRunEndBadly() const noexcept { return lastRunEndedBadly; }

private:
    const juce::File file;
    bool lastRunEndedBadly = false;

    JUCE_DECLARE_NON_COPYABLE (RunMarker)
};
} // namespace phazite
