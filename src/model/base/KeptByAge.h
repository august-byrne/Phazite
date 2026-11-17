// SPDX-License-Identifier: AGPL-3.0-or-later
// What a history kept without asking keeps as it grows old, as Time Machine thins its backups: one rule, for Local
// History's snapshots (model/versions/internal/VersionHistory.h: thinSnapshots()) and a project's backups
// (model/project/internal/Project.h).
#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <set>
#include <vector>

namespace phazite
{
namespace keptByAge
{
    inline constexpr juce::int64 hour = 60 * 60 * 1000, day = 24 * hour;
    inline constexpr juce::int64 everyOneFor = 2 * hour, hourlyFor = day, dailyFor = 30 * day;
} // namespace keptByAge

// Which of these times (milliseconds since 1970, in any order) are kept at now: every one of the last two hours, and any
// after now; then the newest of each hour for a day; then the newest of each day for 30 days; none older. Hours and days
// are counted from 1970, in UTC. Of times that are the same, the one later in the list is taken as the newer: give a list
// oldest first, as Local History and the backups do, and the newer of them is the one kept.
inline std::vector<bool> keepByAge (const std::vector<juce::int64>& times, juce::int64 now)
{
    using namespace keptByAge;
    std::vector<size_t> newestFirst (times.size());
    std::iota (newestFirst.begin(), newestFirst.end(), size_t {});
    std::sort (newestFirst.begin(), newestFirst.end(),
               [&times] (size_t a, size_t b) { return times[a] != times[b] ? times[a] > times[b] : a > b; });

    std::vector<bool> kept (times.size(), false);
    std::set<juce::int64> buckets; // an hour's by its number, a day's by its number below zero

    for (const auto i : newestFirst)
    {
        const auto age = now - times[i];

        if (age > dailyFor)
            continue;

        if (age <= everyOneFor || buckets.insert (age <= hourlyFor ? times[i] / hour : -1 - times[i] / day).second)
            kept[i] = true;
    }

    return kept;
}
} // namespace phazite
