// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Grid.h"
#include "model/base/internal/Schema.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace phazite
{
namespace
{
    constexpr auto beatLevel = GridSettings::beatLevel, barLevel = GridSettings::barLevel;
    constexpr double tiny = 1.0e-6; // in beats, for rounding errors

    // The fixed grid sizes by name, from level 5 (GridSettings::minFixedLevel) to 13.
    constexpr const char* fixedGridSteps[] = { "1/16 beat", "1/8 beat", "1/4 beat", "1/2 beat", "1 beat",
                                               "1 bar",     "2 bars",   "4 bars",   "8 bars" };
    static_assert (std::size (fixedGridSteps) == (size_t) (GridSettings::maxFixedLevel - GridSettings::minFixedLevel + 1));

    int clampLevel (int level)
    {
        return std::clamp (level, GridSettings::minLevel, GridSettings::maxLevel);
    }

    // A level finer than a beat, as a part of one: halved at each level down, and in triplets two thirds of that.
    double getPartOfBeat (int level, bool triplets)
    {
        const auto straight = std::ldexp (1.0, level - beatLevel);
        return triplets ? straight * 2.0 / 3.0 : straight;
    }

    // A level of a bar or coarser, as a number of bars: doubled at each level up.
    int getBarsPerStep (int level)
    {
        return 1 << std::max (0, level - barLevel);
    }

    // A position moved onto the grid by a rounding of how many steps it is from where they count.
    template <typename Rounding>
    BeatPosition moveOntoGrid (BeatPosition beat, GridStep step, const Bars& bars, Rounding rounding)
    {
        const auto level = clampLevel (step.level);

        if (level >= barLevel)
        {
            const auto barsPerStep = getBarsPerStep (level);
            return bars.getBarStart (barsPerStep * (int) rounding (bars.getBarsAt (beat) / barsPerStep));
        }

        const auto size = level < beatLevel ? getPartOfBeat (level, step.triplets) : 1.0;
        const auto barStart = bars.getBarStart (bars.getBarAt (beat));
        return barStart + BeatDuration::fromBeats (size * rounding ((beat - barStart).inBeats() / size));
    }
} // namespace

//==============================================================================
void GridSettings::makeFiner()
{
    if (adaptive)
        adaptiveOffset = std::max (-maxAdaptiveOffset, adaptiveOffset - 1);
    else
        fixedLevel = std::max (minFixedLevel, fixedLevel - 1);
}

void GridSettings::makeCoarser()
{
    if (adaptive)
        adaptiveOffset = std::min (maxAdaptiveOffset, adaptiveOffset + 1);
    else
        fixedLevel = std::min (maxFixedLevel, fixedLevel + 1);
}

GridSettings GridSettings::load (const juce::ValueTree& state)
{
    GridSettings grid;
    grid.snap = state.getProperty (schema::gridSnap, grid.snap);
    grid.adaptive = state.getProperty (schema::gridAdaptive, grid.adaptive);
    grid.adaptiveOffset = juce::jlimit (-maxAdaptiveOffset, maxAdaptiveOffset, (int) state.getProperty (schema::gridOffset, 0));
    grid.fixedLevel = findFixedGridLevel (state[schema::gridStep].toString()).value_or (grid.fixedLevel);
    grid.triplets = state.getProperty (schema::gridTriplets, grid.triplets);
    return grid;
}

void GridSettings::save (juce::ValueTree& state) const
{
    state.setProperty (schema::gridSnap, snap, nullptr);
    state.setProperty (schema::gridAdaptive, adaptive, nullptr);
    state.setProperty (schema::gridOffset, adaptiveOffset, nullptr);
    state.setProperty (schema::gridStep, getSavedGridStep (fixedLevel), nullptr);
    state.setProperty (schema::gridTriplets, triplets, nullptr);
}

juce::String getSavedGridStep (int fixedLevel)
{
    const auto level = juce::jlimit (GridSettings::minFixedLevel, GridSettings::maxFixedLevel, fixedLevel);
    return fixedGridSteps[(size_t) (level - GridSettings::minFixedLevel)];
}

std::optional<int> findFixedGridLevel (const juce::String& savedStep)
{
    for (size_t i = 0; i < std::size (fixedGridSteps); ++i)
        if (savedStep == fixedGridSteps[i])
            return GridSettings::minFixedLevel + (int) i;

    return std::nullopt;
}

GridSettings loadGridSettings (const Song& song, GridOwner owner)
{
    // Reading leaves the song as it is: a song that doesn't have the state yet reads the defaults.
    const auto state = song.getState();
    return GridSettings::load (owner == GridOwner::pianoRoll ? state.getChildWithName (schema::PIANOROLL) : state);
}

void saveGridSettings (Song& song, GridOwner owner, const GridSettings& grid)
{
    auto state = song.getState();

    if (owner == GridOwner::pianoRoll)
        state = state.getOrCreateChildWithName (schema::PIANOROLL, nullptr);

    grid.save (state);
}

//==============================================================================
double getStepInBeats (GridStep step, const Bars& bars, BeatPosition where)
{
    const auto level = clampLevel (step.level);

    if (level < beatLevel)
        return getPartOfBeat (level, step.triplets);

    if (level == beatLevel)
        return 1.0;

    return getBarsPerStep (level) * bars.getBeatsPerBar (where);
}

GridStep getAdaptiveGridStep (const Bars& bars, BeatPosition where, double widthPerBeat, bool triplets, double minLineSpacing)
{
    GridStep step { GridSettings::minLevel, triplets };

    while (step.level < GridSettings::maxLevel && getStepInBeats (step, bars, where) * widthPerBeat < minLineSpacing)
        ++step.level;

    return step;
}

GridStep getGridStep (const GridSettings& grid, const Bars& bars, BeatPosition where, double widthPerBeat, double minLineSpacing)
{
    if (! grid.adaptive)
        return { clampLevel (grid.fixedLevel), grid.triplets };

    const auto adaptive = getAdaptiveGridStep (bars, where, widthPerBeat, grid.triplets, minLineSpacing);
    return { clampLevel (adaptive.level + grid.adaptiveOffset), grid.triplets };
}

BeatPosition snapToGrid (BeatPosition beat, GridStep step, const Bars& bars)
{
    return moveOntoGrid (beat, step, bars, [] (double steps) { return std::round (steps); });
}

BeatPosition snapDownToGrid (BeatPosition beat, GridStep step, const Bars& bars)
{
    return moveOntoGrid (beat, step, bars, [] (double steps) { return std::floor (steps + tiny); });
}

juce::String describeGridStep (GridStep step)
{
    const auto level = clampLevel (step.level);

    if (level < beatLevel)
    {
        const auto parts = (1 << (beatLevel - level)) * (step.triplets ? 3 : 2) / 2;
        return "1/" + juce::String (parts) + " beat";
    }

    if (level == beatLevel)
        return "Beat";

    if (level == barLevel)
        return "Bar";

    return juce::String (getBarsPerStep (level)) + " bars";
}

void forEachGridLine (const Bars& bars, GridStep step, BeatRange range, const std::function<void (BeatPosition, GridLineType)>& callback)
{
    const auto level = clampLevel (step.level);
    const auto start = range.getStart(), end = range.getEnd();

    // Bar lines so many bars apart (one, at a bar or finer), from the song's start; and the lines inside each beat, so
    // far apart as a part of it (none at a beat or coarser).
    const auto barsPerLine = getBarsPerStep (level);
    const auto part = level < beatLevel ? getPartOfBeat (level, step.triplets) : 0.0;
    const auto partsPerBeat = part > 0.0 ? (int) std::round (1.0 / part) : 1;

    auto bar = std::max (0, bars.getBarAt (start));
    bar -= bar % barsPerLine;

    for (;; bar += barsPerLine)
    {
        const auto barStart = bars.getBarStart (bar);

        if (barStart > end)
            return;

        if (barStart >= start)
            callback (barStart, GridLineType::bar);

        if (level > beatLevel)
            continue;

        const auto barEnd = bars.getBarStart (bar + 1) - BeatDuration::fromBeats (tiny);

        for (auto beat = barStart; beat < barEnd; beat += BeatDuration::fromBeats (1.0))
        {
            if (beat > end)
                return;

            if (beat > barStart && beat >= start)
                callback (beat, GridLineType::beat);

            for (int i = 1; i < partsPerBeat; ++i)
            {
                const auto line = beat + BeatDuration::fromBeats (part * i);

                if (line > end || line >= barEnd)
                    break;

                if (line >= start)
                    callback (line, GridLineType::subdivision);
            }
        }
    }
}
} // namespace phazite
