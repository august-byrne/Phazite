// SPDX-License-Identifier: AGPL-3.0-or-later
// The grid that positions snap to: its settings, sizes and lines. Positions are in beats, as in FL Studio and Ableton
// Live, so a tempo change doesn't move the grid; Tracktion's TempoSequence converts beats to and from seconds.
#pragma once

#include "model/base/Tracktion.h"

#include <functional>
#include <optional>

namespace phazite
{
//==============================================================================
// The grid: how fine it is, and whether positions snap to it. Each project keeps its own.
struct GridSettings
{
    // Grid sizes are levels of Tracktion's bars-and-beats snap types (te::TimecodeSnapType): 7 is 1/4 beat, 9 a beat,
    // 10 a bar and 13 eight bars. Projects save a fixed size by name (getSavedGridStep()), not as a level.
    static constexpr int minLevel = 3, maxLevel = 18;           // 1/64 beat to 1,024 bars
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
