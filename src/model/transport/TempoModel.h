// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo changes as the playlist's tempo lane shows and changes them (docs/design.md, section 4), by plain values
// (TempoChange, TempoValues.h): each tempo from a beat on, the first at the song's start, by its place among them, and how
// its line goes on to the next, held and then jumping or ramping, as an automation lane's line goes. Each function takes
// the open project's context, and gives nothing, or does nothing, while no project is open. Each change is an undo step of
// its own, as model/transport/internal/Tempo.h names them, which a drag's step takes in while one is open.
#pragma once

#include "TempoValues.h"
#include "model/base/Beats.h"

#include <optional>
#include <vector>

namespace phazite
{
class ProjectContext;

// The song's tempos in order, the first at its start; and whether there are more than one, which shows the lane.
std::vector<TempoChange> getTempoChanges (const ProjectContext&);
bool hasTempoChanges (const ProjectContext&);

// The tempo at a beat as the engine plays it, and at evenly spaced beats over a range, from its start to its end (at least
// two values), to draw the lane's line.
double getTempoAt (const ProjectContext&, BeatPosition);
std::vector<double> getTempoValues (const ProjectContext&, BeatRange, int count);

// A tempo from a beat, on the line there unless a bpm is given (a tempo already at that beat takes the bpm), its line to
// the next going as the one it was on did: returns its place, or -1 while no project is open. Moving one (the first stays
// at the song's start) returns where it is after the move. Deleting one never deletes the first; removing the changes
// leaves the first tempo all the way through.
int addTempoChange (ProjectContext&, BeatPosition, std::optional<double> bpm = std::nullopt);
int moveTempoChange (ProjectContext&, int index, BeatPosition, double bpm);
void setTempoCurve (ProjectContext&, int index, float curve);
void deleteTempoChange (ProjectContext&, int index);
void removeTempoChanges (ProjectContext&);
} // namespace phazite
