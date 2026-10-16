// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineModel.h"
#include "Grid.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/Playback.h"

#include <cmath>

namespace phazite
{
namespace
{
    constexpr int beatLevel = 9; // a grid step of one beat (GridSettings.h)

    te::TimecodeSnapType toSnapType (GridStep step)
    {
        return { te::TimecodeType::barsBeats, step.level };
    }
} // namespace

//==============================================================================
int getBeatsPerBar (const ProjectContext& context, BeatPosition where)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? edit->tempoSequence.getTimeSigAt (toTracktion (where)).numerator.get() : 4;
}

int getBarAt (const ProjectContext& context, BeatPosition where)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return (int) std::floor (where.inBeats() / 4.0);

    const auto& tempo = edit->tempoSequence;
    return tempo.toBarsAndBeats (tempo.toTime (toTracktion (where))).bars;
}

BeatPosition getBarStart (const ProjectContext& context, int bar)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? fromTracktion (edit->tempoSequence.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 }))
                           : BeatPosition::fromBeats (bar * 4.0);
}

//==============================================================================
GridStep getAdaptiveGridStep (const ProjectContext& context, BeatPosition where, double widthPerBeat, bool triplets, double minLineSpacing)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return { beatLevel, triplets };

    return { getAdaptiveSnapType (edit->tempoSequence, toTracktion (where), widthPerBeat, triplets, minLineSpacing).getLevel(), triplets };
}

GridStep getGridStep (const ProjectContext& context, const GridSettings& grid, BeatPosition where, double widthPerBeat,
                      double minLineSpacing)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return { beatLevel, grid.triplets };

    return { getGridSnapType (grid, edit->tempoSequence, toTracktion (where), widthPerBeat, minLineSpacing).getLevel(), grid.triplets };
}

double getStepInBeats (const ProjectContext& context, GridStep step, BeatPosition where)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? getGridStepInBeats (toSnapType (step), edit->tempoSequence, toTracktion (where), step.triplets) : 1.0;
}

BeatPosition snapToGrid (const ProjectContext& context, BeatPosition beat, GridStep step)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? fromTracktion (snapToGrid (toTracktion (beat), toSnapType (step), edit->tempoSequence, step.triplets)) : beat;
}

BeatPosition snapDownToGrid (const ProjectContext& context, BeatPosition beat, GridStep step)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return beat;

    const auto& tempo = edit->tempoSequence;
    return fromTracktion (tempo.toBeats (toSnapType (step).roundTimeDown (tempo.toTime (toTracktion (beat)), tempo, step.triplets)));
}

juce::String describeGridStep (const ProjectContext& context, GridStep step)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? getGridDescription (toSnapType (step), edit->tempoSequence, step.triplets) : juce::String();
}

void forEachGridLine (const ProjectContext& context, GridStep step, BeatRange range,
                      const std::function<void (BeatPosition, GridLineType)>& callback)
{
    if (auto* edit = getEdit (context))
        forEachGridLine (edit->tempoSequence, toSnapType (step), step.triplets, toTracktion (range),
                         [&callback] (te::BeatPosition beat, GridLineType type) { callback (fromTracktion (beat), type); });
}

GridSettings loadGridSettings (const ProjectContext& context, GridOwner owner)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? loadGridSettings (*edit, owner) : GridSettings();
}

void saveGridSettings (ProjectContext& context, GridOwner owner, const GridSettings& grid)
{
    if (auto* edit = getEdit (context))
        saveGridSettings (*edit, owner, grid);
}

//==============================================================================
BeatPosition getPlayheadBeat (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr ? fromTracktion (edit->tempoSequence.toBeats (edit->getTransport().getPosition())) : BeatPosition();
}

void movePlayhead (ProjectContext& context, BeatPosition beat)
{
    if (auto* edit = getEdit (context))
        movePlayhead (*edit, edit->tempoSequence.toTime (toTracktion (beat)));
}

std::optional<BeatRange> getLoop (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    const auto loop = edit != nullptr ? getLoopBeatRange (*edit) : std::nullopt;

    if (! loop.has_value())
        return {};

    return BeatRange (BeatPosition::fromBeats (loop->first), BeatPosition::fromBeats (loop->second));
}

void loopRange (ProjectContext& context, BeatRange range)
{
    if (auto* edit = getEdit (context))
        loopBeatRange (*edit, range.getStart().inBeats(), range.getEnd().inBeats());
}

void toggleLoop (ProjectContext& context)
{
    if (auto* edit = getEdit (context))
        toggleLoop (*edit);
}

void loopSong (ProjectContext& context)
{
    if (auto* edit = getEdit (context))
        loopSong (*edit);
}
} // namespace phazite
