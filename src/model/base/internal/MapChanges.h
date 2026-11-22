// SPDX-License-Identifier: AGPL-3.0-or-later
// What a keyed value of an area (ValueArea.h, KeyedAreaValue) made of immer maps writes its findChanges() and withChanges()
// from: the entries that differ between two maps, each by its key with the entry before and after (findMapChanges(), through
// immer::diff, which costs by what differs where one map was made from the other, the entries compared with their type's
// ==), and a map with exactly those entries moved back or forward (withMapChanges(), through a transient). A value of several
// maps keeps a list of each, and moves each map by its own.
#pragma once

#include "model/base/internal/Immer.h"
#include "model/base/internal/ValueArea.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
// An entry that differs between two maps: by its key, with the entry before and after, either none (added or removed).
template <typename Key, typename Entry>
struct MapChange
{
    Key key {};
    std::optional<Entry> before {}, after {};
};

// Each for a map of any hash, key equality, memory policy and branching (as SongCurves' map, by its parameters' own hash).
template <typename Key, typename Entry, typename Hash, typename Equal, typename MemoryPolicy, auto bits>
std::vector<MapChange<Key, Entry>> findMapChanges (const immer::map<Key, Entry, Hash, Equal, MemoryPolicy, bits>& before,
                                                   const immer::map<Key, Entry, Hash, Equal, MemoryPolicy, bits>& after)
{
    std::vector<MapChange<Key, Entry>> changes;

    const auto added = [&changes] (const auto& entry) { changes.push_back ({ entry.first, std::nullopt, entry.second }); };
    const auto removed = [&changes] (const auto& entry) { changes.push_back ({ entry.first, entry.second, std::nullopt }); };
    const auto changed = [&changes] (const auto& old, const auto& now) { changes.push_back ({ old.first, old.second, now.second }); };

    immer::diff (before, after, added, removed, changed);
    return changes;
}

// The map with each entry of the changes as it was before them (ChangeDirection::back) or after (forward), every other entry
// as it is; the very map where each of those entries is so already.
template <typename Key, typename Entry, typename Hash, typename Equal, typename MemoryPolicy, auto bits>
immer::map<Key, Entry, Hash, Equal, MemoryPolicy, bits> withMapChanges (immer::map<Key, Entry, Hash, Equal, MemoryPolicy, bits> now,
                                                                        const std::vector<MapChange<Key, Entry>>& changes,
                                                                        ChangeDirection direction)
{
    const auto wanted = [direction] (const MapChange<Key, Entry>& change) -> const std::optional<Entry>&
    { return direction == ChangeDirection::back ? change.before : change.after; };

    const auto isSoAlready = [&now, &wanted] (const MapChange<Key, Entry>& change)
    {
        const auto* held = now.find (change.key);
        const auto& entry = wanted (change);
        return entry.has_value() ? held != nullptr && *held == *entry : held == nullptr;
    };

    if (std::all_of (changes.begin(), changes.end(), isSoAlready))
        return now;

    auto moved = std::move (now).transient();

    for (const auto& change : changes)
    {
        if (const auto& entry = wanted (change); entry.has_value())
            moved.set (change.key, *entry);
        else
            moved.erase (change.key);
    }

    return std::move (moved).persistent();
}
} // namespace phazite
