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

PlanCounter::PlanCounter (ProjectContext& contextToUse)
    : context (contextToUse)
{
    // The engine as a device that has started and plays no block meanwhile leaves it: not idle, so that it keeps each plan
    // it replaces (engine::Engine::setIdle(); idle, with no device, it lets go of each at once, and there is nothing to count).
    auto& playing = getPlaybackHost (getEngineContext()).getEngine();
    wasIdle = playing.isIdle();
    playing.setIdle (false);
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

PlanCounter::~PlanCounter()
{
    getPlaybackHost (getEngineContext()).getEngine().setIdle (wasIdle);
}

long PlanCounter::countNow() const
{
    // Another master's state (a plan for another rate) would start another count.
    REQUIRE (getPlayedPlan().strips.back().state == master);
    return master.use_count();
}

void PlanCounter::makePlanNow()
{
    auto* song = findSong (context);
    REQUIRE (song != nullptr);
    auto* session = findPlayingSession (*song);
    REQUIRE (session != nullptr);
    session->update();
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
    // Where they can't be counted the work still runs, uncounted: a test that looks at what the work did before it looks at
    // the count then finds it done in every build (three gates failed in the Windows release build alone for work not run).
    if (! AudioThreadMonitor::isAvailable())
    {
        work();
        return std::nullopt;
    }

    AudioThreadMonitor::reset();
    {
        const AudioThreadMonitor::ScopedAudioThread counting;
        work();
    }
    return AudioThreadMonitor::getCounts().allocations;
}
} // namespace phazite::test
