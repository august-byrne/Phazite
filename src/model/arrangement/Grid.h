// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's time axis: which part of the song is on screen, how wide a beat is, and the grid that
// positions snap to. Positions are in beats, as in FL Studio and Ableton Live, so a tempo change doesn't
// move the grid; Tracktion's TempoSequence converts beats to and from seconds.
#pragma once

#include "model/base/Tracktion.h"

#include <functional>

namespace phazite
{
//==============================================================================
// The visible part of the timeline: the beat at the left edge, and the zoom in pixels per beat.
struct TimelineView
{
    static constexpr double minPixelsPerBeat = 0.5;    // about 500 bars across a wide window
    static constexpr double maxPixelsPerBeat = 1024.0; // a 1/64 beat is 16 pixels wide
    static constexpr double defaultPixelsPerBeat = 24.0;

    te::BeatPosition start;
    double pixelsPerBeat = defaultPixelsPerBeat;

    double beatToX (te::BeatPosition) const;
    te::BeatPosition xToBeat (double x) const;

    // Zooms in (a factor above 1) or out, keeping the beat at x where it is. The zoom stays within its
    // limits, and the view doesn't start before the song does.
    void zoomAround (double x, double factor);

    // Moves the view sideways by some pixels (positive: later in the song), but not before beat 0.
    void scrollBy (double pixels);
};

//==============================================================================
// The grid: how fine it is, and whether positions snap to it. Each project keeps its own.
struct GridSettings
{
    // Grid sizes are levels of Tracktion's bars-and-beats snap types (te::TimecodeSnapType): 7 is
    // 1/4 beat, 9 a beat, 10 a bar and 13 eight bars.
    static constexpr int minLevel = 3, maxLevel = 18;           // 1/64 beat to 65,536 bars
    static constexpr int minFixedLevel = 5, maxFixedLevel = 13; // 1/16 beat to 8 bars
    static constexpr int maxAdaptiveOffset = 3;

    bool snap = true;
    bool adaptive = true;   // the grid follows the zoom, like Ableton Live's zoom-adaptive grid
    int adaptiveOffset = 0; // adaptive: this many levels coarser (above 0) or finer than the zoom suggests
    int fixedLevel = 7;
    bool triplets = false;

    void makeFiner();   // Ableton Live's "Narrow Grid"
    void makeCoarser(); // "Widen Grid"

    // Kept in the project's Phazite state (getPhaziteState()), without undo steps.
    static GridSettings load (const juce::ValueTree& phaziteState);
    void save (juce::ValueTree& phaziteState) const;

    bool operator== (const GridSettings&) const = default;
};

// The lines of an adaptive grid are at least this far apart, in pixels.
inline constexpr double minGridSpacing = 20.0;

// The coarsest grid whose lines are no closer than minSpacing pixels at this zoom, as a Tracktion snap
// type. "where" matters only for bar lengths, which depend on the time signature.
te::TimecodeSnapType getAdaptiveSnapType (const te::TempoSequence&, te::BeatPosition where, double pixelsPerBeat, bool triplets,
                                          double minSpacing = minGridSpacing);

// The grid these settings give at this zoom: the fixed size, or the adaptive one moved by its offset.
te::TimecodeSnapType getGridSnapType (const GridSettings&, const te::TempoSequence&, te::BeatPosition where, double pixelsPerBeat);

// How long one step of the grid is, in beats.
double getGridStepInBeats (te::TimecodeSnapType, const te::TempoSequence&, te::BeatPosition where, bool triplets);

// The grid line nearest to a position.
te::BeatPosition snapToGrid (te::BeatPosition, te::TimecodeSnapType, const te::TempoSequence&, bool triplets);

// A short name for a grid size, such as "1/4 beat" or "Bar".
juce::String getGridDescription (te::TimecodeSnapType, const te::TempoSequence&, bool triplets);

enum class GridLineType
{
    bar,
    beat,
    subdivision
};

// Calls back with each grid line from the start of the range to its end, in order. A line that is both
// a bar and a beat counts as a bar.
void forEachGridLine (const te::TempoSequence&, te::TimecodeSnapType, bool triplets, te::BeatRange,
                      const std::function<void (te::BeatPosition, GridLineType)>&);
} // namespace phazite
