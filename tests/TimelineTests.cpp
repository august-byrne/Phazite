// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/Timeline.h"

using namespace tracktion::literals; // 4_bp: beat positions

namespace
{
using phazite::GridLineType;

te::TimecodeSnapType gridLevel (int level)
{
    return { te::TimecodeType::barsBeats, level };
}

struct Line
{
    double beat;
    GridLineType type;
};

std::vector<Line> getGridLines (const te::TempoSequence& tempo, int level, te::BeatRange range, bool triplets = false)
{
    std::vector<Line> lines;
    phazite::forEachGridLine (tempo, gridLevel (level), triplets, range,
                              [&lines] (te::BeatPosition beat, GridLineType type) { lines.push_back ({ beat.inBeats(), type }); });
    return lines;
}

void checkLines (const std::vector<Line>& lines, std::initializer_list<Line> expected)
{
    REQUIRE (lines.size() == expected.size());

    for (size_t i = 0; i < lines.size(); ++i)
    {
        CAPTURE (i);
        CHECK (lines[i].beat == doctest::Approx (std::data (expected)[i].beat));
        CHECK (lines[i].type == std::data (expected)[i].type);
    }
}
} // namespace

TEST_CASE ("The timeline converts between beats and pixels, and zooms around a point")
{
    phazite::TimelineView view;
    view.start = 4_bp;
    view.pixelsPerBeat = 10.0;

    CHECK (view.beatToX (6_bp) == doctest::Approx (20.0));
    CHECK (view.xToBeat (20.0).inBeats() == doctest::Approx (6.0));

    // The beat under the pointer stays where it is.
    view.zoomAround (20.0, 2.0);
    CHECK (view.pixelsPerBeat == doctest::Approx (20.0));
    CHECK (view.xToBeat (20.0).inBeats() == doctest::Approx (6.0));

    // The zoom has limits, and the view doesn't start before the song.
    view.zoomAround (0.0, 1.0e9);
    CHECK (view.pixelsPerBeat == doctest::Approx (phazite::TimelineView::maxPixelsPerBeat));
    view.zoomAround (500.0, 1.0e-9);
    CHECK (view.pixelsPerBeat == doctest::Approx (phazite::TimelineView::minPixelsPerBeat));
    CHECK (view.start.inBeats() == doctest::Approx (0.0));

    view.start = 4_bp;
    view.scrollBy (-1.0e6);
    CHECK (view.start.inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("The adaptive grid follows the zoom, and the settings change its size")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    const auto& tempo = edit->tempoSequence;
    const auto levelAt = [&tempo] (const phazite::GridSettings& grid, double pixelsPerBeat)
    { return phazite::getGridSnapType (grid, tempo, 0_bp, pixelsPerBeat).getLevel(); };

    // Lines at least 20 pixels apart.
    phazite::GridSettings grid;
    CHECK (levelAt (grid, 24.0) == 9); // beats, 24 pixels apart
    CHECK (levelAt (grid, 96.0) == 7); // quarter beats
    CHECK (levelAt (grid, 4.0) == 11); // a bar is 16 pixels, so every 2 bars

    grid.makeFiner();
    CHECK (levelAt (grid, 24.0) == 8); // half beats
    grid.makeCoarser();
    grid.makeCoarser();
    CHECK (levelAt (grid, 24.0) == 10); // bars

    for (int i = 0; i < 10; ++i)
        grid.makeFiner();

    CHECK (grid.adaptiveOffset == -phazite::GridSettings::maxAdaptiveOffset);

    // A fixed grid is the same size at any zoom.
    grid.adaptive = false;
    grid.fixedLevel = 7;
    CHECK (levelAt (grid, 4.0) == 7);
    CHECK (levelAt (grid, 500.0) == 7);

    for (int i = 0; i < 20; ++i)
        grid.makeCoarser();

    CHECK (grid.fixedLevel == phazite::GridSettings::maxFixedLevel);

    CHECK (phazite::getGridDescription (gridLevel (7), tempo, false) == juce::String ("1/4 beat"));
    CHECK (phazite::getGridDescription (gridLevel (9), tempo, false) == juce::String ("Beat"));
    CHECK (phazite::getGridDescription (gridLevel (11), tempo, false) == juce::String ("2 bars"));
}

TEST_CASE ("Positions snap to the nearest grid line")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto& tempo = edit->tempoSequence;
    const auto snap = [&tempo] (double beat, int level, bool triplets = false)
    { return phazite::snapToGrid (te::BeatPosition::fromBeats (beat), gridLevel (level), tempo, triplets).inBeats(); };

    CHECK (snap (1.3, 9) == doctest::Approx (1.0));
    CHECK (snap (1.6, 9) == doctest::Approx (2.0));
    CHECK (snap (1.3, 7) == doctest::Approx (1.25));
    CHECK (snap (5.9, 10) == doctest::Approx (4.0)); // bars of 4 beats
    CHECK (snap (6.1, 10) == doctest::Approx (8.0));
    CHECK (snap (0.3, 8, true) == doctest::Approx (1.0 / 3.0)); // with triplets, level 8 is a third of a beat
}

TEST_CASE ("Grid lines are bars, beats and the steps between them")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto& tempo = edit->tempoSequence;
    const auto bar = GridLineType::bar, beat = GridLineType::beat, step = GridLineType::subdivision;

    checkLines (getGridLines (tempo, 9, { 0_bp, 8_bp }),
                { { 0, bar }, { 1, beat }, { 2, beat }, { 3, beat }, { 4, bar }, { 5, beat }, { 6, beat }, { 7, beat }, { 8, bar } });
    checkLines (getGridLines (tempo, 8, { 0_bp, 2_bp }), { { 0, bar }, { 0.5, step }, { 1, beat }, { 1.5, step }, { 2, beat } });
    checkLines (getGridLines (tempo, 8, { 0_bp, 1_bp }, true), { { 0, bar }, { 1.0 / 3.0, step }, { 2.0 / 3.0, step }, { 1, beat } });
    checkLines (getGridLines (tempo, 12, { 0_bp, 40_bp }), { { 0, bar }, { 16, bar }, { 32, bar } }); // every 4 bars
    checkLines (getGridLines (tempo, 9, { te::BeatPosition::fromBeats (5.5), te::BeatPosition::fromBeats (7.5) }),
                { { 6, beat }, { 7, beat } });
}

TEST_CASE ("Grid settings are kept in the project")
{
    phazite::GridSettings grid;
    grid.snap = false;
    grid.adaptive = false;
    grid.adaptiveOffset = -2;
    grid.fixedLevel = 10;
    grid.triplets = true;

    juce::ValueTree state ("PHAZITE");
    grid.save (state);
    CHECK (phazite::GridSettings::load (state) == grid);

    // A project that has none uses the defaults.
    CHECK (phazite::GridSettings::load (juce::ValueTree()) == phazite::GridSettings());
}
