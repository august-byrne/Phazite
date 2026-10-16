// SPDX-License-Identifier: AGPL-3.0-or-later
// The time line as the editors reach it, in Phazite's beats (src/model/arrangement/TimelineModel.h), and a drag's undo
// step for the open project (src/model/commands/SongGesture.h).
#include "TestHelpers.h"
#include "model/arrangement/TimelineModel.h"
#include "model/commands/SongGesture.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

using phazite::BeatPosition;

namespace
{
BeatPosition beat (double beats)
{
    return BeatPosition::fromBeats (beats);
}
} // namespace

TEST_CASE ("The time line's bars follow the time signature, and the grid snaps in beats")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };

    // With no project open, a new song's 4/4.
    CHECK (phazite::getBeatsPerBar (context, beat (0.0)) == 4);
    CHECK (phazite::getBarStart (context, 2).inBeats() == doctest::Approx (8.0));
    CHECK (phazite::snapToGrid (context, beat (2.7), { 9, false }).inBeats() == doctest::Approx (2.7)); // nothing to snap to

    setEdit (context, edit.get());
    phazite::setTimeSignature (*edit, { 3, 4 });
    CHECK (phazite::getBeatsPerBar (context, beat (0.0)) == 3);
    CHECK (phazite::getBarAt (context, beat (7.0)) == 2);
    CHECK (phazite::getBarStart (context, 2).inBeats() == doctest::Approx (6.0));

    // A beat's grid, and a bar's.
    const phazite::GridStep beatStep { 9, false }, barStep { 10, false };
    CHECK (phazite::getStepInBeats (context, beatStep, beat (0.0)) == doctest::Approx (1.0));
    CHECK (phazite::getStepInBeats (context, barStep, beat (0.0)) == doctest::Approx (3.0));
    CHECK (phazite::snapToGrid (context, beat (2.7), beatStep).inBeats() == doctest::Approx (3.0));
    CHECK (phazite::snapDownToGrid (context, beat (2.7), beatStep).inBeats() == doctest::Approx (2.0));
    CHECK (phazite::snapToGrid (context, beat (4.0), barStep).inBeats() == doctest::Approx (3.0));
    CHECK (phazite::describeGridStep (context, beatStep).isNotEmpty());

    // The lines in a range, in order: bars, beats and what is between them.
    std::vector<std::pair<double, phazite::GridLineType>> lines;
    phazite::forEachGridLine (context, beatStep, { beat (0.0), beat (4.0) },
                              [&lines] (BeatPosition at, phazite::GridLineType type) { lines.emplace_back (at.inBeats(), type); });
    REQUIRE (lines.size() >= 4);
    CHECK (lines[0].first == doctest::Approx (0.0));
    CHECK (lines[0].second == phazite::GridLineType::bar);
    CHECK (lines[1].second == phazite::GridLineType::beat);
    CHECK (std::is_sorted (lines.begin(), lines.end(), [] (const auto& a, const auto& b) { return a.first < b.first; }));

    // An adaptive grid is coarser when zoomed out.
    phazite::GridSettings grid;
    const auto zoomedIn = phazite::getGridStep (context, grid, beat (0.0), 200.0, 20.0);
    const auto zoomedOut = phazite::getGridStep (context, grid, beat (0.0), 2.0, 20.0);
    CHECK (zoomedOut.level > zoomedIn.level);
    grid.adaptive = false;
    grid.fixedLevel = 7;
    CHECK (phazite::getGridStep (context, grid, beat (0.0), 2.0, 20.0).level == 7);
    setEdit (context, nullptr);
}

TEST_CASE ("The playhead and the loop by beats, through the project's context")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };

    CHECK (phazite::getPlayheadBeat (context).inBeats() == doctest::Approx (0.0));
    CHECK_FALSE (phazite::getLoop (context).has_value());
    phazite::loopRange (context, { beat (4.0), beat (8.0) }); // no project: nothing to loop

    setEdit (context, edit.get());
    phazite::movePlayhead (context, beat (6.0));
    CHECK (phazite::getPlayheadBeat (context).inBeats() == doctest::Approx (6.0));
    CHECK (phazite::getPlayStart (*edit) == edit->tempoSequence.toTime (te::BeatPosition::fromBeats (6.0)));

    if (phazite::getLoop (context).has_value())
        phazite::toggleLoop (context); // off, to begin with

    CHECK_FALSE (phazite::getLoop (context).has_value());
    phazite::loopRange (context, { beat (4.0), beat (8.0) });
    REQUIRE (phazite::getLoop (context).has_value());
    CHECK (phazite::getLoop (context)->getStart().inBeats() == doctest::Approx (4.0));
    CHECK (phazite::getLoop (context)->getEnd().inBeats() == doctest::Approx (8.0));

    phazite::toggleLoop (context);
    CHECK_FALSE (phazite::getLoop (context).has_value());
    phazite::loopSong (context);
    CHECK (phazite::getLoop (context).has_value());
    setEdit (context, nullptr);
}

TEST_CASE ("A song gesture is one undo step for the open project, and nothing without one")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    const auto name = track->getName();
    phazite::ProjectContext context { phazite::test::getEngineContext() };

    {
        phazite::SongGesture none (context, "Nothing");
        CHECK_FALSE (none.isOpen());
    }

    setEdit (context, edit.get());
    edit->getUndoManager().clearUndoHistory();

    {
        phazite::SongGesture gesture (context, "Rename Twice");
        CHECK (gesture.isOpen());
        track->setName ("Once");
        track->setName ("Twice");
    }

    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Rename Twice"));
    edit->getUndoManager().undo();
    CHECK (track->getName() == name);

    // Rolled back, it leaves the song as it was, and nothing to redo.
    {
        phazite::SongGesture gesture (context, "Rename");
        track->setName ("Gone");
        gesture.rollBack();
        CHECK_FALSE (gesture.isOpen());
    }

    CHECK (track->getName() == name);
    setEdit (context, nullptr);
}
