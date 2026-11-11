// SPDX-License-Identifier: AGPL-3.0-or-later
// The grid, as plain values: its settings (how fine it is and whether positions snap to it, which editor keeps which),
// its sizes and the lines it draws over the song's bars. Positions are in beats (quarter notes), as in FL Studio and Ableton
// Live, so a tempo change doesn't move the grid. TimelineModel.h is how the editors reach it in the open project.
#pragma once

#include "model/base/Beats.h"
#include "model/transport/TempoValues.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <optional>

namespace phazite
{
//==============================================================================
// The grid's sizes are levels: 3 to 8 parts of a beat, from a 64th to a half, each twice the last (in triplets, a third
// shorter: a 96th to a third); 9 a beat; 10 a bar; and above, bars, each twice as many as the last, to 1,024 at 20. A beat
// is the time signature's, the note it counts (a quarter note in 4/4, an eighth in 6/8, a half in 2/2), and a bar as long
// as it says (TempoValues.h: TimeSignature).
// Projects save a fixed size by name (getSavedGridStep()), not as a level.
struct GridSettings
{
    static constexpr int minLevel = 3, maxLevel = 20;           // 1/64 beat to 1,024 bars
    static constexpr int beatLevel = 9, barLevel = 10;          // a beat, a bar
    static constexpr int minFixedLevel = 5, maxFixedLevel = 13; // 1/16 beat to 8 bars
    static constexpr int maxAdaptiveOffset = 3;

    bool snap = true;
    bool adaptive = true;   // the grid follows the zoom, like Ableton Live's zoom-adaptive grid
    int adaptiveOffset = 0; // adaptive: this many levels coarser (above 0) or finer than the zoom suggests
    int fixedLevel = 7;
    bool triplets = false;

    void makeFiner();   // Ableton Live's "Narrow Grid"
    void makeCoarser(); // "Widen Grid"

    // Read from and written to a tree of the project's Phazite state (each editor's: loadGridSettings()), without undo
    // steps.
    static GridSettings load (const juce::ValueTree& state);
    void save (juce::ValueTree& state) const;

    bool operator== (const GridSettings&) const = default;
};

// A fixed grid size as projects save it ("1/4 beat", "1 bar", "8 bars"...), for a level 5..13 (the nearest of those
// for another level), and back: nothing for a name that isn't one of them.
juce::String getSavedGridStep (int fixedLevel);
std::optional<int> findFixedGridLevel (const juce::String& savedStep);

// The editors that keep a grid of their own in the project: the playlist's on the Phazite state, the piano roll's on its
// PIANOROLL child. View settings, without undo steps.
enum class GridOwner
{
    playlist,
    pianoRoll
};

enum class GridLineType
{
    bar,
    beat,
    subdivision
};

//==============================================================================
// A size of the grid: one of its levels, straight or in triplets (which only parts of a beat have).
struct GridStep
{
    int level = GridSettings::beatLevel;
    bool triplets = false;

    bool operator== (const GridStep&) const = default;
};

// How long a step is, in beats (quarter notes), in the bar at a beat (bars and their beats are as long as their time
// signature says).
double getStepInBeats (GridStep, const Bars&, BeatPosition where);

// The finest step whose lines are no closer than minLineSpacing at a zoom (widthPerBeat, in the same unit: the editors
// use pixels); and the step that a grid's settings give there: the fixed size, or the adaptive one moved by its offset.
GridStep getAdaptiveGridStep (const Bars&, BeatPosition where, double widthPerBeat, bool triplets, double minLineSpacing);
GridStep getGridStep (const GridSettings&, const Bars&, BeatPosition where, double widthPerBeat, double minLineSpacing);

// The grid line nearest to a position, and the one at or before it. Parts of a beat and beats count from the start of
// their bar; bars, from the song's start.
BeatPosition snapToGrid (BeatPosition, GridStep, const Bars&);
BeatPosition snapDownToGrid (BeatPosition, GridStep, const Bars&);

// A short name for a size, such as "1/4 beat", "Beat", "Bar" or "2 bars".
juce::String describeGridStep (GridStep);

// Calls back with each line of the grid from the start of the range to its end, in order. A line that is both a bar and
// a beat counts as a bar.
void forEachGridLine (const Bars&, GridStep, BeatRange, const std::function<void (BeatPosition, GridLineType)>&);
} // namespace phazite
