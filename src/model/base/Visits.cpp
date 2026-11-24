// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Visits.h"

#include <atomic>

namespace phazite
{
namespace
{
    std::atomic<std::uint64_t> visited { 0 };
} // namespace

std::uint64_t countElementsVisited() noexcept
{
    return visited.load (std::memory_order_relaxed);
}

void countVisited (std::uint64_t count) noexcept
{
    visited.fetch_add (count, std::memory_order_relaxed);
}
} // namespace phazite
