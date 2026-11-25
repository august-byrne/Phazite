// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks' switches and view are areas of the song's values (roadmap step S5.20, the tracks' leaf switch;
// model/base/internal/SongValues.h: trackSwitches, keyed; tracksOutside, outside the history; TracksArea.h). A track deleted
// keeps its entries, which its Undo brings back with it; a duplicate's copy takes its
// original's (its frozen switch aside), its lanes by the copy's IDs, its switches outside the history (a mute taken off the
// copy since survives the step's Undo and Redo); a version's track new to the song, which the switches hold no entry of,
// takes them outside the history too, and one a step took away in the step, keeping the row the song kept of it; a version
// applied and a merge's choice set them in their step, whose Undo keeps a solo set since. Each through UndoCheck where its
// Undo could go wrong.
//   phazite_tests.exe --test-case="Song values' track leaves*"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"

#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::TrackID;

phazite::ParameterRef getVolume (const phazite::Song& song, TrackID track)
{
    return { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
}
} // namespace

TEST_CASE ("Song values' track leaves: a track deleted keeps its mute, its height, its mode and its lanes, which its Undo brings "
           "back with it")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Kept");
    const auto other = s.addTrack ("Other");
    phazite::toggleTracksMuted (song, { track });
    phazite::setSavedTrackHeight (song, track, 100);
    phazite::setAutomationMode (song, track, phazite::AutomationMode::latch);
    phazite::showLane (song, track, getVolume (song, track));
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    undo.check ("Delete Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Delete Track");
                    phazite::deleteTracks (song, { track }, nullptr);
                });

    // Gone, its entries kept, never written nor read while it is.
    REQUIRE_FALSE (phazite::hasTrack (song, track));
    CHECK_FALSE (phazite::isTrackMuted (song, track, false));
    CHECK (phazite::getTrackSwitches (song).mutes.find (track.getRaw()) != nullptr);
    CHECK (phazite::getTracksOutside (song).tracks.find (track.getRaw()) != nullptr);
    CHECK_FALSE (phazite::isWritingAutomation (song));
    CHECK (phazite::getTrackSwitches (song).modes.find (track.getRaw()) != nullptr);

    // A solo set since, outside the history, survives the Undo, which brings the track back with what it held.
    phazite::toggleTracksSoloed (song, { other });
    undo.stepBackOrForth ("Undo Delete Track", [&song] { phazite::undo (song); });
    REQUIRE (phazite::hasTrack (song, track));
    CHECK (phazite::isTrackMuted (song, track, false));
    CHECK (phazite::getSavedTrackHeight (song, track) == std::optional (100));
    CHECK (phazite::getAutomationMode (song, track) == phazite::AutomationMode::latch);
    CHECK (phazite::getLanes (song, track).size() == 1);
    CHECK (phazite::isTrackSoloed (song, other));
    CHECK ((bool) phazite::test::copyTrack (song, track)[schema::trackMute]);
    undo.stepBackOrForth ("Redo Delete Track", [&song] { phazite::redo (song); });
    CHECK_FALSE (phazite::hasTrack (song, track));
    CHECK (phazite::isTrackSoloed (song, other));
    undo.stepBackOrForth ("Undo Delete Track", [&song] { phazite::undo (song); });
    phazite::toggleTracksSoloed (song, { other });
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' track leaves: a duplicate's copy takes its original's switches but its frozen one, and its view with its "
           "lanes by the copy's IDs")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Original");
    phazite::toggleTracksMuted (song, { track });
    phazite::toggleTracksSoloed (song, { track });
    phazite::setAutomationMode (song, track, phazite::AutomationMode::touch);
    phazite::setSavedTrackHeight (song, track, 120);
    phazite::setPinned (song, track, true);
    phazite::showLane (song, track, getVolume (song, track));
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);
    std::vector<TrackID> copies;

    undo.check ("Duplicate Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Duplicate Track");
                    copies = phazite::duplicateTracks (song, { track }, nullptr);
                });

    REQUIRE (copies.size() == 1);
    const auto copy = copies.front();
    CHECK (phazite::isTrackMuted (song, copy, false));
    CHECK (phazite::isTrackSoloed (song, copy));
    CHECK (phazite::getAutomationMode (song, copy) == phazite::AutomationMode::touch);
    CHECK (phazite::getSavedTrackHeight (song, copy) == std::optional (120));
    CHECK (phazite::isPinned (song, copy));
    const auto lanes = phazite::getLanes (song, copy);
    REQUIRE (lanes.size() == 1);
    CHECK (lanes.front().parameter == getVolume (song, copy));
    CHECK (phazite::getFader (song, copy) != phazite::getFader (song, track));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();

    // A copy unmuted since, outside the history: the step's Undo takes the copy away, and its Redo brings it back unmuted, as
    // its element carried its mute while the tree held the switches (the copy's switches are given outside the history:
    // Tracks.cpp, copyTrackLeaves()); its solo and its mode as it was given them, the original as it was.
    std::vector<TrackID> again;

    {
        const phazite::UndoStep step (song, "Duplicate Track");
        again = phazite::duplicateTracks (song, { track }, nullptr);
    }

    REQUIRE (again.size() == 1);
    const auto second = again.front();
    REQUIRE (phazite::isTrackMuted (song, second, false));
    phazite::toggleTracksMuted (song, { second });
    REQUIRE_FALSE (phazite::isTrackMuted (song, second, false));
    phazite::undo (song);
    CHECK_FALSE (phazite::hasTrack (song, second));
    phazite::redo (song);
    REQUIRE (phazite::hasTrack (song, second));
    CHECK_FALSE (phazite::isTrackMuted (song, second, false));
    CHECK (phazite::isTrackSoloed (song, second));
    CHECK (phazite::getAutomationMode (song, second) == phazite::AutomationMode::touch);
    CHECK (phazite::isTrackMuted (song, track, false));
    CHECK (phazite::isTrackSoloed (song, track));
    CHECK (phazite::getAutomationMode (song, track) == phazite::AutomationMode::touch);
    CHECK (phazite::getSavedTrackHeight (song, track) == std::optional (120));
    phazite::test::checkSong (song);

    // A frozen track's copy plays live: the file is the original's (a song without a project, whose freeze keeper would
    // compare the original's holdings with a file that isn't there).
    auto bare = phazite::createSong();
    const auto frozen = phazite::ensureTracks (*bare, 1).front();
    phazite::setTrackFrozen (*bare, frozen, true);
    REQUIRE (phazite::isTrackFrozen (*bare, frozen));
    std::vector<TrackID> frozenCopies;

    {
        const phazite::UndoStep step (*bare, "Duplicate Track");
        frozenCopies = phazite::duplicateTracks (*bare, { frozen }, nullptr);
    }

    REQUIRE (frozenCopies.size() == 1);
    CHECK_FALSE (phazite::isTrackFrozen (*bare, frozenCopies.front()));
    CHECK (phazite::isTrackFrozen (*bare, frozen));
}

TEST_CASE ("Song values' track leaves: a version's track new to the song, whose switches the song holds no entry of, takes them "
           "outside the history, so that its Redo keeps a mute taken off since")
{
    // The song saved without the track and opened again (makeSong() of its whole copy, as Project::open() makes it): its
    // switches hold no entry of the track, which the delete's step kept while that step was in the history. The track
    // brought back by its line (makeTrackLike()) and by the whole version (makeSongLike()) takes its mute outside the history
    // (ApplySong.cpp: makeTrackLeavesLike()): unmuted since, Undo and Redo bring it back unmuted, as its element carried it.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Muted Then");
    phazite::toggleTracksMuted (song, { track });
    const auto version = phazite::test::copyWholeSong (song); // the track muted

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { track }, nullptr);
    }

    REQUIRE (phazite::holdsSwitchesOf (phazite::getTrackSwitches (song), track.getRaw())); // kept for the delete's Undo
    const auto saved = phazite::test::copyWholeSong (song);

    for (const auto wholeVersion : { false, true })
    {
        INFO ("the whole version: " << (wholeVersion ? "yes" : "no"));
        auto opened = phazite::makeSong (saved.createCopy()); // the song takes the tree it is given as its own
        REQUIRE (opened != nullptr);
        REQUIRE_FALSE (phazite::hasTrack (*opened, track));
        CHECK_FALSE (phazite::holdsSwitchesOf (phazite::getTrackSwitches (*opened), track.getRaw()));

        {
            const phazite::UndoStep step (*opened, "Restore");

            if (wholeVersion)
                phazite::makeSongLike (*opened, version);
            else
                CHECK (phazite::makeTrackLike (*opened, track, version));
        }

        REQUIRE (phazite::hasTrack (*opened, track));
        CHECK (phazite::isTrackMuted (*opened, track, false));
        phazite::toggleTracksMuted (*opened, { track });
        REQUIRE_FALSE (phazite::isTrackMuted (*opened, track, false));
        phazite::undo (*opened);
        CHECK_FALSE (phazite::hasTrack (*opened, track));
        phazite::redo (*opened);
        REQUIRE (phazite::hasTrack (*opened, track));
        CHECK_FALSE (phazite::isTrackMuted (*opened, track, false));
        phazite::test::checkSong (*opened);
    }
}

TEST_CASE ("Song values' track leaves: a track a step took away, brought back by its line, keeps the row the song kept and takes "
           "the version's switches in the step, whose Undo puts back its own")
{
    // Its view: deleted 120 high with a lane shown, its line brought back from a version where it was 60 high with none: the
    // row the song kept of it stays (ApplySong.cpp: makeTrackLeavesLike(), a view given only where the song keeps none), and
    // so after the two Undos, as before the switches and the view left the tree.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Row");
    phazite::setSavedTrackHeight (song, track, 60);
    const auto version = phazite::test::copyWholeSong (song); // 60 high, no lane
    phazite::setSavedTrackHeight (song, track, 120);
    phazite::showLane (song, track, getVolume (song, track));
    song.getUndoManager().clearUndoHistory();

    {
        const phazite::UndoStep step (song, "Delete Track");
        phazite::deleteTracks (song, { track }, nullptr);
    }

    {
        const phazite::UndoStep step (song, "Restore Track");
        REQUIRE (phazite::makeTrackLike (song, track, version));
    }

    REQUIRE (phazite::hasTrack (song, track));
    CHECK (phazite::getSavedTrackHeight (song, track) == std::optional (120));
    CHECK (phazite::getLanes (song, track).size() == 1);
    phazite::test::checkSong (song);
    phazite::undo (song);
    CHECK_FALSE (phazite::hasTrack (song, track));
    phazite::undo (song);
    REQUIRE (phazite::hasTrack (song, track));
    CHECK (phazite::getSavedTrackHeight (song, track) == std::optional (120));
    CHECK (phazite::getLanes (song, track).size() == 1);
    phazite::test::checkSong (song);

    // Its switches: soloed in the version, muted and not soloed as it was deleted (entries the switches keep): brought back
    // soloed and unmuted in the step, whose Undo and the delete's bring the original back with its own (written outside the
    // history, they would survive both Undos).
    const auto other = s.addTrack ("Soloed Then");
    phazite::toggleTracksSoloed (song, { other });
    const auto soloed = phazite::test::copyWholeSong (song);
    phazite::toggleTracksSoloed (song, { other });
    phazite::toggleTracksMuted (song, { other });
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    undo.check ("Delete Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Delete Track");
                    phazite::deleteTracks (song, { other }, nullptr);
                });

    REQUIRE (phazite::holdsSwitchesOf (phazite::getTrackSwitches (song), other.getRaw()));

    undo.check ("Restore Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Restore Track");
                    REQUIRE (phazite::makeTrackLike (song, other, soloed));
                });

    REQUIRE (phazite::hasTrack (song, other));
    CHECK (phazite::isTrackSoloed (song, other));
    CHECK_FALSE (phazite::isTrackMuted (song, other, false));
    phazite::undo (song);
    phazite::undo (song);
    REQUIRE (phazite::hasTrack (song, other));
    CHECK (phazite::isTrackMuted (song, other, false));
    CHECK_FALSE (phazite::isTrackSoloed (song, other));
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' track leaves: a version applied that changes a mute changes it in its step, whose Undo puts it back and "
           "keeps a solo set since; a merge's choice of a mute the same")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Muted Then");
    const auto other = s.addTrack ("Soloed Since");
    phazite::toggleTracksMuted (song, { track });
    const auto version = phazite::test::copyWholeSong (song); // the track muted
    phazite::toggleTracksMuted (song, { track });
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    undo.check ("Apply Version",
                [&]
                {
                    const phazite::UndoStep step (song, "Apply Version");
                    phazite::makeSongLike (song, version);
                });

    CHECK (phazite::isTrackMuted (song, track, false));
    phazite::toggleTracksSoloed (song, { other });
    undo.stepBackOrForth ("Undo Apply Version", [&song] { phazite::undo (song); });
    CHECK_FALSE (phazite::isTrackMuted (song, track, false));
    CHECK (phazite::isTrackSoloed (song, other));
    undo.stepBackOrForth ("Redo Apply Version", [&song] { phazite::redo (song); });
    CHECK (phazite::isTrackMuted (song, track, false));
    CHECK (phazite::isTrackSoloed (song, other));
    phazite::test::checkSong (song);

    // A merge's clash on the mute: the other line's value (none: unmuted) chosen, in its step.
    phazite::MergeClash clash;
    clash.path = { juce::String ("TRACKS:TRACKS"), "TRACK:" + juce::String ((juce::int64) track.getRaw()) };
    clash.property = schema::trackMute;
    clash.ours = true;

    undo.check ("Take the Other Branch's",
                [&]
                {
                    const phazite::UndoStep step (song, "Take the Other Branch's");
                    CHECK (phazite::chooseTrackClash (song, clash, 2, {}) == phazite::AreaChoice::chosen);
                });

    CHECK_FALSE (phazite::isTrackMuted (song, track, false));
    CHECK (phazite::isTrackSoloed (song, other));
    undo.stepBackOrForth ("Undo Take the Other Branch's", [&song] { phazite::undo (song); });
    CHECK (phazite::isTrackMuted (song, track, false));
    phazite::test::checkSong (song);
}
