// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PerformanceCounts.h"
#include "AudioThreadMonitor.h"
#include "UiTestHelpers.h"
#include "model/engine/internal/EngineContext.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite::test
{
namespace
{
    const engine::Plan& getPlayedPlan()
    {
        const auto* plan = getPlaybackHost (getEngineContext()).getEngine().getPlan();
        REQUIRE (plan != nullptr);
        REQUIRE (! plan->strips.empty()); // the master last
        return *plan;
    }
} // namespace

PlanCounter::PlanCounter (ProjectContext& context)
{
    dispatchPendingMessages();
    auto* song = findSong (context);
    REQUIRE (song != nullptr);
    auto* session = findPlayingSession (*song);
    REQUIRE (session != nullptr);

    // A plan given now lets go of every plan replaced before the blocks that played since: from here on, none is let go.
    session->update();
    master = getPlayedPlan().strips.back().state;
    REQUIRE (master != nullptr);
    counted = countNow();
}

long PlanCounter::countNow() const
{
    // Another master's state (a plan for another rate) would start another count.
    REQUIRE (getPlayedPlan().strips.back().state == master);
    return master.use_count();
}

int PlanCounter::take()
{
    dispatchPendingMessages();
    const auto now = countNow();
    const auto given = (int) (now - counted);
    counted = now;
    return given;
}

std::optional<int> countAllocations (const std::function<void()>& work)
{
    if (! AudioThreadMonitor::isAvailable())
        return std::nullopt;

    AudioThreadMonitor::reset();
    {
        const AudioThreadMonitor::ScopedAudioThread counting;
        work();
    }
    return AudioThreadMonitor::getCounts().allocations;
}
} // namespace phazite::test
