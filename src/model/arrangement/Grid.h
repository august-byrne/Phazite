// SPDX-License-Identifier: AGPL-3.0-or-later
// The grid that positions snap to: its sizes and lines, by Tracktion's snap types and tempo sequence (its settings are
// in GridSettings.h; the editors reach it through TimelineModel.h). Positions are in beats, as in FL Studio and Ableton
// Live, so a tempo change doesn't move the grid; Tracktion's TempoSequence converts beats to and from seconds.
#pragma once

#include "GridSettings.h"
#include "model/base/Tracktion.h"

#include <functional>

namespace phazite
{
GridSettings loadGridSettings (const te::Edit&, GridOwner); // the defaults if there are none; reading adds nothing
void saveGridSettings (te::Edit&, GridOwner, const GridSettings&);

// The coarsest grid whose lines are no closer than minLineSpacing at this zoom, as a Tracktion snap type. widthPerBeat and
// minLineSpacing are in the caller's unit (the editors use pixels). "where" matters only for bar lengths, which depend on
// the time signature.
te::TimecodeSnapType getAdaptiveSnapType (const te::TempoSequence&, te::BeatPosition where, double widthPerBeat, bool triplets,
                                          double minLineSpacing);

// The grid these settings give at this zoom: the fixed size, or the adaptive one (getAdaptiveSnapType()) moved by its
// offset.
te::TimecodeSnapType getGridSnapType (const GridSettings&, const te::TempoSequence&, te::BeatPosition where, double widthPerBeat,
                                      double minLineSpacing);

// How long one step of the grid is, in beats.
double getGridStepInBeats (te::TimecodeSnapType, const te::TempoSequence&, te::BeatPosition where, bool triplets);

// The grid line nearest to a position.
te::BeatPosition snapToGrid (te::BeatPosition, te::TimecodeSnapType, const te::TempoSequence&, bool triplets);

// A short name for a grid size, such as "1/4 beat" or "Bar".
juce::String getGridDescription (te::TimecodeSnapType, const te::TempoSequence&, bool triplets);

// Calls back with each grid line from the start of the range to its end, in order. A line that is both
// a bar and a beat counts as a bar.
void forEachGridLine (const te::TempoSequence&, te::TimecodeSnapType, bool triplets, te::BeatRange,
                      const std::function<void (te::BeatPosition, GridLineType)>&);
} // namespace phazite
