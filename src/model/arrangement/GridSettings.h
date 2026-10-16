// SPDX-License-Identifier: AGPL-3.0-or-later
// The grid's settings, as plain values: how fine it is and whether positions snap to it, which editor keeps which, and
// the kinds of its lines (Grid.h works with them; TimelineModel.h is how the editors reach it).
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <optional>

namespace phazite
{
//==============================================================================
// The grid: how fine it is, and whether positions snap to it. Each project keeps its own.
struct GridSettings
{
    // Grid sizes are levels of the bars-and-beats sizes (Tracktion's snap types, Grid.h): 7 is 1/4 beat, 9 a beat,
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

enum class GridLineType
{
    bar,
    beat,
    subdivision
};
} // namespace phazite
