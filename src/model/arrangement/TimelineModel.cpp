// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineModel.h"
#include "internal/Grid.h"
#include "model/base/internal/Schema.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <cmath>

namespace phazite
{
namespace
{
    // The open song's bars; while none is open, a new song's, in 4/4.
    Bars getBars (const ProjectContext& context)
    {
        auto* song = findSong (context);
        return song != nullptr ? phazite::getBars (*song) : Bars();
    }
} // namespace

//==============================================================================
BeatDuration getBarLength (const ProjectContext& context, BeatPosition where)
{
    return getBars (context).getBarLength (where);
}

BeatDuration getBeatLength (const ProjectContext& context, BeatPosition where)
{
    return getBars (context).getBeatLength (where);
}

int getBarAt (const ProjectContext& context, BeatPosition where)
{
    return getBars (context).getBarAt (where);
}

BeatPosition getBarStart (const ProjectContext& context, int bar)
{
    return getBars (context).getBarStart (bar);
}

//==============================================================================
GridStep getAdaptiveGridStep (const ProjectContext& context, BeatPosition where, double widthPerBeat, bool triplets, double minLineSpacing)
{
    if (! context.hasProject())
        return { GridSettings::beatLevel, triplets };

    return getAdaptiveGridStep (getBars (context), where, widthPerBeat, triplets, minLineSpacing);
}

GridStep getGridStep (const ProjectContext& context, const GridSettings& grid, BeatPosition where, double widthPerBeat,
                      double minLineSpacing)
{
    if (! context.hasProject())
        return { GridSettings::beatLevel, grid.triplets };

    return getGridStep (grid, getBars (context), where, widthPerBeat, minLineSpacing);
}

double getStepInBeats (const ProjectContext& context, GridStep step, BeatPosition where)
{
    return context.hasProject() ? getStepInBeats (step, getBars (context), where) : 1.0;
}

BeatPosition snapToGrid (const ProjectContext& context, BeatPosition beat, GridStep step)
{
    return context.hasProject() ? snapToGrid (beat, step, getBars (context)) : beat;
}

BeatPosition snapDownToGrid (const ProjectContext& context, BeatPosition beat, GridStep step)
{
    return context.hasProject() ? snapDownToGrid (beat, step, getBars (context)) : beat;
}

void forEachGridLine (const ProjectContext& context, GridStep step, BeatRange range,
                      const std::function<void (BeatPosition, GridLineType)>& callback)
{
    if (context.hasProject())
        forEachGridLine (getBars (context), step, range, callback);
}

GridSettings loadGridSettings (const ProjectContext& context, GridOwner owner)
{
    auto* song = findSong (context);
    return song != nullptr ? loadGridSettings (*song, owner) : GridSettings();
}

void saveGridSettings (ProjectContext& context, GridOwner owner, const GridSettings& grid)
{
    if (auto* song = findSong (context))
        saveGridSettings (*song, owner, grid);
}

//==============================================================================
BeatPosition getPlayheadBeat (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getPlayheadBeat (*song) : BeatPosition();
}

BeatPosition getHeardPlayheadBeat (const ProjectContext& context, double milliseconds)
{
    auto* song = findSong (context);
    return song != nullptr ? getHeardPlayheadBeat (*song, milliseconds) : BeatPosition();
}

void movePlayhead (ProjectContext& context, BeatPosition beat)
{
    if (auto* song = findSong (context))
        movePlayhead (*song, beat);
}

BeatPosition getPlayStart (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getPlayStartBeat (*song) : BeatPosition();
}

void setPlayStart (ProjectContext& context, BeatPosition beat)
{
    if (auto* song = findSong (context))
        setPlayStart (*song, beat);
}

double getSecondsAt (const ProjectContext& context, BeatPosition beat)
{
    constexpr double secondsPerBeatAt120 = 0.5;
    auto* song = findSong (context);
    return song != nullptr ? toSeconds (*song, beat) : beat.inBeats() * secondsPerBeatAt120;
}

BeatPosition getBeatAtSeconds (const ProjectContext& context, double seconds)
{
    constexpr double beatsPerSecondAt120 = 2.0;
    auto* song = findSong (context);
    return song != nullptr ? toBeats (*song, seconds) : BeatPosition::fromBeats (seconds * beatsPerSecondAt120);
}

std::optional<BeatRange> getLoop (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getLoop (*song) : std::nullopt;
}

void loopRange (ProjectContext& context, BeatRange range)
{
    if (auto* song = findSong (context))
        loopBeats (*song, range);
}

void toggleLoop (ProjectContext& context)
{
    if (auto* song = findSong (context))
        toggleLoop (*song);
}

void loopSong (ProjectContext& context)
{
    if (auto* song = findSong (context))
        loopSong (*song);
}

LoopState getLoopState (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? LoopState { getLoopBeats (*song), doesLoopFollowSong (*song), isLoopOn (*song) } : LoopState();
}

void setLoopState (ProjectContext& context, const LoopState& state)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return;

    // Each turns looping on.
    if (state.followsSong)
        loopSong (*song);
    else
        loopBeats (*song, state.range);

    if (isLoopOn (*song) != state.on)
        toggleLoop (*song);
}
} // namespace phazite
