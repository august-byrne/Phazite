// SPDX-License-Identifier: AGPL-3.0-or-later
// Counts of work that don't depend on the machine, for the speed gates (PerformanceGateTests.cpp) and the long run
// (LongRun.cpp): how many plans a project gives Phazite's engine, and how many allocations a piece of work makes.
#pragma once

#include "TestHelpers.h"
#include "model/project/ProjectContext.h"

#include <functional>
#include <memory>
#include <optional>

namespace phazite::engine
{
class StripState;
}

namespace phazite::test
{
// Counts the plans the open project's playback session gives Phazite's engine (model/playback/internal/PlaybackSession.h:
// update()), as long as no block plays meanwhile. Every plan holds the master's strip state, which the song's player
// keeps from plan to plan; and the engine keeps every plan it replaced until a block has played since
// (engine/core/Handover.h), so while none plays, each plan given holds one more reference to it. Made while the context's
// project is the one the engine plays, after its song's changes were delivered.
class PlanCounter
{
public:
    explicit PlanCounter (ProjectContext&);

    // The plans given since it was made, or since this was last asked, the song's changes delivered first
    // (dispatchPendingMessages()).
    int take();

    // Gives the engine the song's plan as the song is now, at once: what the playback session does a turn of the message
    // loop after an edit, without that turn, in which a test's count would take in whatever else the loop delivers.
    void makePlanNow();

private:
    ProjectContext& context;
    std::shared_ptr<engine::StripState> master;
    long counted = 0;
    long countNow() const;
};

// The heap allocations (and reallocations) made on the calling thread while a function runs; nothing where they can't be
// counted (AudioThreadMonitor.h: Windows Debug and Linux builds count them). The same work allocates the same every time,
// whatever the machine, so counts compare across runs; but another compiler or standard library counts differently.
std::optional<int> countAllocations (const std::function<void()>&);
} // namespace phazite::test
