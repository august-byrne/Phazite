// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TempoModel.h"
#include "internal/Tempo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
std::vector<TempoChange> getTempoChanges (const ProjectContext& context)
{
    const auto* song = findSong (context);
    return song != nullptr ? getTempoChanges (*song) : std::vector<TempoChange>();
}

bool hasTempoChanges (const ProjectContext& context)
{
    const auto* song = findSong (context);
    return song != nullptr && hasTempoChanges (*song);
}

double getTempoAt (const ProjectContext& context, BeatPosition beat)
{
    const auto* song = findSong (context);
    return song != nullptr ? getTempoAt (*song, beat) : 120.0;
}

std::vector<double> getTempoValues (const ProjectContext& context, BeatRange range, int count)
{
    const auto* song = findSong (context);
    return song != nullptr ? getTempoValues (*song, range, count) : std::vector<double>();
}

int addTempoChange (ProjectContext& context, BeatPosition beat, std::optional<double> bpm)
{
    auto* song = findSong (context);
    return song != nullptr ? addTempoChange (*song, beat, bpm) : -1;
}

int moveTempoChange (ProjectContext& context, int index, BeatPosition beat, double bpm)
{
    auto* song = findSong (context);
    return song != nullptr ? moveTempoChange (*song, index, beat, bpm) : -1;
}

void setTempoCurve (ProjectContext& context, int index, float curve)
{
    if (auto* song = findSong (context))
        setTempoCurve (*song, index, curve);
}

void deleteTempoChange (ProjectContext& context, int index)
{
    if (auto* song = findSong (context))
        deleteTempoChange (*song, index);
}

void removeTempoChanges (ProjectContext& context)
{
    if (auto* song = findSong (context))
        removeTempoChanges (*song);
}
} // namespace phazite
