// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/internal/Grid.h"
#include "model/base/internal/Schema.h"
#include "model/project/internal/Project.h"

namespace
{
using phazite::BeatPosition;
using phazite::GridLineType;

BeatPosition at (double beat)
{
    return BeatPosition::fromBeats (beat);
}

struct Line
{
    double beat;
    GridLineType type;
};

std::vector<Line> getGridLines (const phazite::Bars& bars, int level, phazite::BeatRange range, bool triplets = false)
{
    std::vector<Line> lines;
    phazite::forEachGridLine (bars, { level, triplets }, range,
                              [&lines] (BeatPosition beat, GridLineType type) { lines.push_back ({ beat.inBeats(), type }); });
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

TEST_CASE ("The song's bars follow its time signatures, each change starting a bar")
{
    // 4/4 throughout, before the song's start too.
    const phazite::Bars plain;
    CHECK (plain.getBarAt (at (7.9)) == 1);
    CHECK (plain.getBarAt (at (8.0)) == 2);
    CHECK (plain.getBarAt (at (-1.0)) == -1);
    CHECK (plain.getBarStart (3).inBeats() == doctest::Approx (12.0));
    CHECK (plain.getBarsAt (at (10.0)) == doctest::Approx (2.5));

    // Two bars of 4/4, then 3/4 from beat 8; and 6/8 from beat 13, a beat into the second bar of 3/4, which ends there.
    const phazite::Bars changing ({ { at (0.0), { 4, 4 } }, { at (8.0), { 3, 4 } }, { at (13.0), { 6, 8 } } });
    CHECK (changing.getBarStart (2).inBeats() == doctest::Approx (8.0));
    CHECK (changing.getBarStart (3).inBeats() == doctest::Approx (11.0));
    CHECK (changing.getBarStart (4).inBeats() == doctest::Approx (13.0));
    CHECK (changing.getBarStart (5).inBeats() == doctest::Approx (19.0));
    CHECK (changing.getBarAt (at (10.9)) == 2);
    CHECK (changing.getBarAt (at (12.5)) == 3);
    CHECK (changing.getBarAt (at (13.0)) == 4);
    CHECK (changing.getBeatsPerBar (at (9.0)) == 3);
    CHECK (changing.getBeatsPerBar (at (20.0)) == 6);

    for (int bar = -2; bar < 8; ++bar)
        CHECK (changing.getBarAt (changing.getBarStart (bar)) == bar);
}

TEST_CASE ("The adaptive grid follows the zoom, and the settings change its size")
{
    const phazite::Bars bars; // 4/4
    const auto levelAt = [&bars] (const phazite::GridSettings& grid, double pixelsPerBeat)
    { return phazite::getGridStep (grid, bars, at (0.0), pixelsPerBeat, 20.0).level; };

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

    // Zoomed all the way out, a line every 1,024 bars at most.
    CHECK (phazite::getAdaptiveGridStep (bars, at (0.0), 1.0e-6, false, 20.0).level == phazite::GridSettings::maxLevel);
    CHECK (phazite::getStepInBeats ({ phazite::GridSettings::maxLevel }, bars, at (0.0)) == doctest::Approx (4096.0));
}

TEST_CASE ("The grid's sizes: parts of a beat, straight or in triplets, a beat, a bar and bars")
{
    const phazite::Bars bars ({ { at (0.0), { 3, 4 } } });
    const auto sizeOf = [&bars] (int level, bool triplets = false)
    { return phazite::getStepInBeats ({ level, triplets }, bars, at (0.0)); };

    CHECK (sizeOf (3) == doctest::Approx (1.0 / 64.0));
    CHECK (sizeOf (7) == doctest::Approx (0.25));
    CHECK (sizeOf (8) == doctest::Approx (0.5));
    CHECK (sizeOf (5, true) == doctest::Approx (1.0 / 24.0));
    CHECK (sizeOf (8, true) == doctest::Approx (1.0 / 3.0));
    CHECK (sizeOf (9, true) == doctest::Approx (1.0)); // triplets only divide beats
    CHECK (sizeOf (10) == doctest::Approx (3.0));
    CHECK (sizeOf (12) == doctest::Approx (12.0));

    CHECK (phazite::describeGridStep ({ 7 }) == juce::String ("1/4 beat"));
    CHECK (phazite::describeGridStep ({ 3 }) == juce::String ("1/64 beat"));
    CHECK (phazite::describeGridStep ({ 7, true }) == juce::String ("1/6 beat"));
    CHECK (phazite::describeGridStep ({ 3, true }) == juce::String ("1/96 beat"));
    CHECK (phazite::describeGridStep ({ 9 }) == juce::String ("Beat"));
    CHECK (phazite::describeGridStep ({ 10 }) == juce::String ("Bar"));
    CHECK (phazite::describeGridStep ({ 11 }) == juce::String ("2 bars"));
    CHECK (phazite::describeGridStep ({ 20 }) == juce::String ("1024 bars"));
}

TEST_CASE ("Positions snap to the nearest grid line")
{
    const phazite::Bars bars; // 4/4
    const auto snap = [&bars] (double beat, int level, bool triplets = false)
    { return phazite::snapToGrid (at (beat), { level, triplets }, bars).inBeats(); };

    CHECK (snap (1.3, 9) == doctest::Approx (1.0));
    CHECK (snap (1.6, 9) == doctest::Approx (2.0));
    CHECK (snap (1.3, 7) == doctest::Approx (1.25));
    CHECK (snap (5.9, 10) == doctest::Approx (4.0)); // bars of 4 beats
    CHECK (snap (6.1, 10) == doctest::Approx (8.0));
    CHECK (snap (0.3, 8, true) == doctest::Approx (1.0 / 3.0)); // with triplets, level 8 is a third of a beat
    CHECK (snap (21.0, 11) == doctest::Approx (24.0));          // every two bars from the song's start

    // Down: the line at or before it, the line itself where it is on one.
    CHECK (phazite::snapDownToGrid (at (1.99), { 9 }, bars).inBeats() == doctest::Approx (1.0));
    CHECK (phazite::snapDownToGrid (at (2.0), { 9 }, bars).inBeats() == doctest::Approx (2.0));

    // Parts of a beat count from their bar's start, which a change of time signature part of the way through a bar moves.
    const phazite::Bars changing ({ { at (0.0), { 4, 4 } }, { at (6.5), { 3, 4 } } });
    CHECK (phazite::snapToGrid (at (7.4), { 9 }, changing).inBeats() == doctest::Approx (7.5));
    CHECK (phazite::snapToGrid (at (7.4), { 10 }, changing).inBeats() == doctest::Approx (6.5));
}

TEST_CASE ("Grid lines are bars, beats and the steps between them")
{
    const phazite::Bars bars; // 4/4
    const auto bar = GridLineType::bar, beat = GridLineType::beat, step = GridLineType::subdivision;

    checkLines (getGridLines (bars, 9, { at (0.0), at (8.0) }),
                { { 0, bar }, { 1, beat }, { 2, beat }, { 3, beat }, { 4, bar }, { 5, beat }, { 6, beat }, { 7, beat }, { 8, bar } });
    checkLines (getGridLines (bars, 8, { at (0.0), at (2.0) }), { { 0, bar }, { 0.5, step }, { 1, beat }, { 1.5, step }, { 2, beat } });
    checkLines (getGridLines (bars, 8, { at (0.0), at (1.0) }, true),
                { { 0, bar }, { 1.0 / 3.0, step }, { 2.0 / 3.0, step }, { 1, beat } });
    checkLines (getGridLines (bars, 12, { at (0.0), at (40.0) }), { { 0, bar }, { 16, bar }, { 32, bar } }); // every 4 bars
    checkLines (getGridLines (bars, 9, { at (5.5), at (7.5) }), { { 6, beat }, { 7, beat } });

    // In 3/4 after two bars of 4/4: bars of three beats.
    const phazite::Bars changing ({ { at (0.0), { 4, 4 } }, { at (8.0), { 3, 4 } } });
    checkLines (getGridLines (changing, 10, { at (0.0), at (14.0) }), { { 0, bar }, { 4, bar }, { 8, bar }, { 11, bar }, { 14, bar } });
}

TEST_CASE ("Grid settings are kept in the project")
{
    phazite::GridSettings grid;
    grid.snap = false;
    grid.adaptive = false;
    grid.adaptiveOffset = -2;
    grid.fixedLevel = 10;
    grid.triplets = true;

    juce::ValueTree state (phazite::schema::PHAZITE);
    grid.save (state);
    CHECK (phazite::GridSettings::load (state) == grid);

    // The fixed size is saved by name, not as a level.
    CHECK (state[phazite::schema::gridStep].toString() == juce::String ("1 bar"));

    for (int level = phazite::GridSettings::minFixedLevel; level <= phazite::GridSettings::maxFixedLevel; ++level)
        CHECK (phazite::findFixedGridLevel (phazite::getSavedGridStep (level)) == level);

    CHECK (phazite::getSavedGridStep (3) == juce::String ("1/16 beat")); // the nearest fixed size

    // A name that isn't a size reads the default, and so does a project that has none.
    state.setProperty (phazite::schema::gridStep, "1/5 beat", nullptr);
    CHECK (phazite::GridSettings::load (state).fixedLevel == 7);
    CHECK (phazite::GridSettings::load (juce::ValueTree()) == phazite::GridSettings());
}

TEST_CASE ("Each editor keeps its own grid in the project")
{
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    phazite::getSongUndoManager (*edit).clearUndoHistory();

    // A project that has none reads the defaults, and reading adds nothing to it.
    CHECK (phazite::loadGridSettings (*edit, phazite::GridOwner::pianoRoll) == phazite::GridSettings());
    CHECK (phazite::loadGridSettings (*edit, phazite::GridOwner::playlist) == phazite::GridSettings());
    CHECK_FALSE (phazite::findPhaziteState (*edit).getChildWithName (phazite::schema::PIANOROLL).isValid());
    CHECK_FALSE (phazite::findPhaziteState (*edit).hasProperty (phazite::schema::gridSnap));

    // The piano roll's grid goes on its own child of the Phazite state, and the playlist's stays as it was.
    const phazite::GridSettings pianoRollGrid { .adaptive = false, .fixedLevel = 8, .triplets = true };
    phazite::saveGridSettings (*edit, phazite::GridOwner::pianoRoll, pianoRollGrid);
    CHECK (phazite::loadGridSettings (*edit, phazite::GridOwner::pianoRoll) == pianoRollGrid);
    CHECK (phazite::loadGridSettings (*edit, phazite::GridOwner::playlist) == phazite::GridSettings());
    CHECK (phazite::GridSettings::load (phazite::findPhaziteState (*edit).getChildWithName (phazite::schema::PIANOROLL)) == pianoRollGrid);

    const phazite::GridSettings playlistGrid { .snap = false, .adaptiveOffset = 1 };
    phazite::saveGridSettings (*edit, phazite::GridOwner::playlist, playlistGrid);
    CHECK (phazite::loadGridSettings (*edit, phazite::GridOwner::playlist) == playlistGrid);
    CHECK (phazite::loadGridSettings (*edit, phazite::GridOwner::pianoRoll) == pianoRollGrid);

    // View settings: no undo steps.
    CHECK_FALSE (phazite::getSongUndoManager (*edit).canUndo());
}
