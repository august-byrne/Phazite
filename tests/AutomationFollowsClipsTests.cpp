// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation that goes with the clips above it (docs/design.md, section 10; model/automation/internal/Automation.h:
// moveAutomation()): clips nudged, duplicated or deleted with their gap closed, and time ranges deleted or duplicated on
// tracks, take the automation of their tracks' parameters with them, in the same undo step; other tracks keep theirs. Time
// inserted, taken out, moved or copied across the song (model/arrangement/internal/Arrangement.h) takes every track's.
#include "TestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <utility>

namespace
{
using phazite::BeatDuration;
using phazite::BeatPosition;

BeatPosition beats (double beat)
{
    return BeatPosition::fromBeats (beat);
}

// Two tracks. On the first, a clip from beat 4 to beat 8, and its volume's automation: 0.5 all along but for a peak of
// 0.9 at beat 6, under the clip. On the second, its volume's automation at 0.3.
struct Song
{
    Song()
    {
        tracks = phazite::ensureTracks (*song, 2);
        phazite::setSong (context, song.get());
        clip = phazite::insertMidiClip (getSong(), tracks[0], { beats (4.0), beats (8.0) }, nullptr);
        volume = phazite::getVolume (context, tracks[0]).getRef();
        other = phazite::getVolume (context, tracks[1]).getRef();

        for (const auto& [beat, value] :
             { std::pair (0.0, 0.5f), std::pair (5.0, 0.5f), std::pair (6.0, 0.9f), std::pair (7.0, 0.5f), std::pair (16.0, 0.5f) })
            phazite::addAutomationPoint (context, volume, { beats (beat), value });

        phazite::addAutomationPoint (context, other, { beats (0.0), 0.3f });
        phazite::addAutomationPoint (context, other, { beats (16.0), 0.3f });
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { phazite::setSong (context, nullptr); }

    // The song, and where a clip starts in it, in beats.
    phazite::Song& getSong() const { return *song; }
    double getStart (phazite::ClipID c) const { return phazite::getClipRange (getSong(), c).getStart().inBeats(); }

    float valueAt (const phazite::ParameterRef& parameter, double beat) const
    {
        return phazite::getAutomationValues (context, parameter, { beats (beat), beats (beat) }, 1).front();
    }

    float valueAt (double beat) const { return valueAt (volume, beat); }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    std::vector<phazite::TrackID> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::ClipID clip;
    phazite::ParameterRef volume, other;
};
} // namespace

TEST_CASE ("Nudged clips take the automation under them, in the same undo step")
{
    Song song;
    REQUIRE (song.clip.isValid());
    REQUIRE (song.valueAt (6.0) == doctest::Approx (0.9f));

    {
        const phazite::UndoStep step (song.getSong(), "Nudge");
        phazite::nudgeClips (song.context, { song.clip }, BeatDuration::fromBeats (4.0));
    }

    CHECK (song.getStart (song.clip) == doctest::Approx (8.0));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f)); // where it was, the line runs straight across
    CHECK (song.valueAt (2.0) == doctest::Approx (0.5f));
    CHECK (song.valueAt (14.0) == doctest::Approx (0.5f));
    CHECK (song.valueAt (song.other, 10.0) == doctest::Approx (0.3f));

    // One step takes the clip and its automation back.
    phazite::undo (song.getSong());
    CHECK (song.getStart (song.clip) == doctest::Approx (4.0));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.5f));
}

TEST_CASE ("Duplicated clips copy the automation under them, and the originals keep theirs")
{
    Song song;
    const auto copies = phazite::duplicateClips (song.getSong(), { song.clip }, false, nullptr);
    REQUIRE (copies.size() == 1);
    CHECK (song.getStart (copies.front()) == doctest::Approx (8.0));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (14.0) == doctest::Approx (0.5f));
}

TEST_CASE ("A clip deleted without closing its gap leaves its automation; closing the gap takes its time out")
{
    SUBCASE ("leaving the gap")
    {
        Song song;
        phazite::deleteClips (song.getSong(), { song.clip }, false);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    }

    SUBCASE ("closing it")
    {
        Song song;
        phazite::addAutomationPoint (song.context, song.volume, { beats (10.0), 0.1f });
        phazite::deleteClips (song.getSong(), { song.clip }, true);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.1f)); // the dip at beat 10 came back four beats
        CHECK (song.valueAt (song.other, 6.0) == doctest::Approx (0.3f));
    }
}

TEST_CASE ("Deleting a time range on tracks deletes their automation there; closing the gap brings what follows back")
{
    const phazite::BeatRange range { beats (4.0), beats (8.0) };

    SUBCASE ("leaving the gap")
    {
        Song song;
        phazite::deleteTimeRange (song.getSong(), range, { song.tracks[0] }, false);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
        CHECK (song.valueAt (2.0) == doctest::Approx (0.5f));
        CHECK (song.valueAt (12.0) == doctest::Approx (0.5f));
    }

    SUBCASE ("closing it")
    {
        Song song;
        phazite::addAutomationPoint (song.context, song.volume, { beats (10.0), 0.1f });
        phazite::deleteTimeRange (song.getSong(), range, { song.tracks[0] }, true);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.1f));
        CHECK (song.valueAt (song.other, 6.0) == doctest::Approx (0.3f)); // a track outside the range keeps its own
    }
}

TEST_CASE ("Duplicating a time range on tracks copies their automation right after it, in place of what was there")
{
    Song song;
    phazite::addAutomationPoint (song.context, song.volume, { beats (10.0), 0.1f }); // replaced by the copy
    const auto copied = phazite::duplicateTimeRange (song.getSong(), { beats (4.0), beats (8.0) }, { song.tracks[0] });
    CHECK (copied.getStart().inBeats() == doctest::Approx (8.0));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (16.0) == doctest::Approx (0.5f)); // after the copy, the line goes on as it went
    CHECK (song.valueAt (song.other, 10.0) == doctest::Approx (0.3f));
}

TEST_CASE ("Time inserted, taken out, moved and copied across the song takes every track's automation with it, in beats")
{
    Song song;
    auto& document = song.getSong();
    const auto beatsOf = [&song] (const phazite::ParameterRef& parameter)
    {
        std::vector<double> result;

        for (const auto& point : phazite::getAutomationPoints (song.context, parameter))
            result.push_back (point.beat.inBeats());

        return result;
    };

    // Four beats inserted at beat 4: the peak at beat 6 goes to beat 10, and an undo puts it back.
    {
        const phazite::UndoStep step (document, "Insert Time");
        phazite::insertTime (document, beats (4.0), BeatDuration::fromBeats (4.0));
    }

    CHECK (beatsOf (song.volume) == std::vector { 0.0, 9.0, 10.0, 11.0, 20.0 });
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
    CHECK (beatsOf (song.other) == std::vector { 0.0, 20.0 });

    phazite::undo (document);
    CHECK (beatsOf (song.volume) == std::vector { 0.0, 5.0, 6.0, 7.0, 16.0 });
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));

    // Beats 4 to 8 taken out: the peak goes with them, and what follows comes back four beats.
    {
        const phazite::UndoStep step (document, "Remove Time");
        phazite::removeTime (document, { beats (4.0), beats (8.0) });
    }

    CHECK (beatsOf (song.volume) == std::vector { 0.0, 12.0 });
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
    phazite::undo (document);

    // Beats 4 to 8 moved to beat 12: the peak plays at beat 10, and what was between comes back to where they were.
    {
        const phazite::UndoStep step (document, "Move Time");
        phazite::moveTime (document, { beats (4.0), beats (8.0) }, beats (12.0));
    }

    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
    CHECK (song.valueAt (song.other, 6.0) == doctest::Approx (0.3f));
    phazite::undo (document);

    // Copied to beat 12: the peak plays at beat 6 and at beat 14, and what was from beat 12 on goes four beats later.
    {
        const phazite::UndoStep step (document, "Copy Time");
        phazite::copyTime (document, { beats (4.0), beats (8.0) }, beats (12.0), false);
    }

    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (14.0) == doctest::Approx (0.9f));
    CHECK (beatsOf (song.volume).back() == doctest::Approx (20.0));
}
