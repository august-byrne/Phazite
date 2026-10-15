// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Grid.h"
#include "model/base/Schema.h"

namespace phazite
{
namespace
{
    constexpr int beatLevel = 9, barLevel = 10; // see te::TimecodeSnapType

    te::TimecodeSnapType barsBeats (int level)
    {
        return { te::TimecodeType::barsBeats, juce::jlimit (GridSettings::minLevel, GridSettings::maxLevel, level) };
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
    grid.fixedLevel = juce::jlimit (minFixedLevel, maxFixedLevel, (int) state.getProperty (schema::gridLevel, grid.fixedLevel));
    grid.triplets = state.getProperty (schema::gridTriplets, grid.triplets);
    return grid;
}

void GridSettings::save (juce::ValueTree& state) const
{
    state.setProperty (schema::gridSnap, snap, nullptr);
    state.setProperty (schema::gridAdaptive, adaptive, nullptr);
    state.setProperty (schema::gridOffset, adaptiveOffset, nullptr);
    state.setProperty (schema::gridLevel, fixedLevel, nullptr);
    state.setProperty (schema::gridTriplets, triplets, nullptr);
}

GridSettings loadGridSettings (const te::Edit& edit, GridOwner owner)
{
    // Reading leaves the project as it is: a project that doesn't have the state yet reads the defaults.
    const auto state = findPhaziteState (edit);
    return GridSettings::load (owner == GridOwner::pianoRoll ? state.getChildWithName (schema::PIANOROLL) : state);
}

void saveGridSettings (te::Edit& edit, GridOwner owner, const GridSettings& grid)
{
    auto state = getPhaziteState (edit);

    if (owner == GridOwner::pianoRoll)
        state = state.getOrCreateChildWithName (schema::PIANOROLL, nullptr);

    grid.save (state);
}

//==============================================================================
double getGridStepInBeats (te::TimecodeSnapType type, const te::TempoSequence& tempo, te::BeatPosition where, bool triplets)
{
    const auto& setting = tempo.getTempoAt (where);
    return type.getApproxIntervalTime (setting, triplets) / setting.getApproxBeatLength();
}

te::TimecodeSnapType getAdaptiveSnapType (const te::TempoSequence& tempo, te::BeatPosition where, double widthPerBeat, bool triplets,
                                          double minLineSpacing)
{
    auto level = GridSettings::minLevel;

    while (level < GridSettings::maxLevel && getGridStepInBeats (barsBeats (level), tempo, where, triplets) * widthPerBeat < minLineSpacing)
        ++level;

    return barsBeats (level);
}

te::TimecodeSnapType getGridSnapType (const GridSettings& grid, const te::TempoSequence& tempo, te::BeatPosition where, double widthPerBeat,
                                      double minLineSpacing)
{
    if (! grid.adaptive)
        return barsBeats (grid.fixedLevel);

    return barsBeats (getAdaptiveSnapType (tempo, where, widthPerBeat, grid.triplets, minLineSpacing).getLevel() + grid.adaptiveOffset);
}

te::BeatPosition snapToGrid (te::BeatPosition beat, te::TimecodeSnapType type, const te::TempoSequence& tempo, bool triplets)
{
    return tempo.toBeats (type.roundTimeNearest (tempo.toTime (beat), tempo, triplets));
}

juce::String getGridDescription (te::TimecodeSnapType type, const te::TempoSequence& tempo, bool triplets)
{
    return type.getDescription (tempo.getTempoAt (te::BeatPosition()), triplets);
}

void forEachGridLine (const te::TempoSequence& tempo, te::TimecodeSnapType type, bool triplets, te::BeatRange range,
                      const std::function<void (te::BeatPosition, GridLineType)>& callback)
{
    const auto level = type.getLevel();
    const auto start = range.getStart(), end = range.getEnd();
    const auto oneBeat = te::BeatDuration::fromBeats (1.0);
    const auto tiny = te::BeatDuration::fromBeats (1.0e-6); // for rounding errors

    // The lines between beats, if the grid is finer than a beat, and how many bars apart the bar lines
    // are, if it is coarser than a bar.
    const auto subdivision = level < beatLevel ? getGridStepInBeats (type, tempo, start, triplets) : 0.0;
    auto barsPerLine = 1;

    if (level > barLevel)
        barsPerLine =
            std::max (1, juce::roundToInt (getGridStepInBeats (type, tempo, start, triplets) / tempo.getTimeSigAt (start).numerator.get()));

    auto bar = std::max (0, tempo.toBarsAndBeats (tempo.toTime (start)).bars);
    bar -= bar % barsPerLine;

    for (;; bar += barsPerLine)
    {
        const auto barStart = tempo.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 });

        if (barStart > end)
            return;

        if (barStart >= start)
            callback (barStart, GridLineType::bar);

        if (level > beatLevel)
            continue;

        const auto barEnd = tempo.toBeats (te::tempo::BarsAndBeats { bar + 1, {}, 0 }) - tiny;

        for (auto beat = barStart; beat < barEnd; beat = beat + oneBeat)
        {
            if (beat > end)
                return;

            if (beat > barStart && beat >= start)
                callback (beat, GridLineType::beat);

            for (auto offset = subdivision; subdivision > 0.0 && offset < 1.0 - 1.0e-6; offset += subdivision)
            {
                const auto line = beat + te::BeatDuration::fromBeats (offset);

                if (line > end || line >= barEnd)
                    break;

                if (line >= start)
                    callback (line, GridLineType::subdivision);
            }
        }
    }
}
} // namespace phazite
