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

te::BeatPosition teBeats (double beat)
{
    return te::BeatPosition::fromBeats (beat);
}

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
        phazite::ensureTracks (*edit, 2);
        tracks = te::getAudioTracks (*edit);
        setEdit (context, edit.get());
        clip = phazite::insertMidiClip (*tracks[0], { teBeats (4.0), teBeats (8.0) }, nullptr);
        volume = phazite::getVolume (context, phazite::getTrackID (*tracks[0])).getRef();
        other = phazite::getVolume (context, phazite::getTrackID (*tracks[1])).getRef();

        for (const auto& [beat, value] :
             { std::pair (0.0, 0.5f), std::pair (5.0, 0.5f), std::pair (6.0, 0.9f), std::pair (7.0, 0.5f), std::pair (16.0, 0.5f) })
            phazite::addAutomationPoint (context, volume, { beats (beat), value });

        phazite::addAutomationPoint (context, other, { beats (0.0), 0.3f });
        phazite::addAutomationPoint (context, other, { beats (16.0), 0.3f });
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    float valueAt (const phazite::ParameterRef& parameter, double beat) const
    {
        return phazite::getAutomationValues (context, parameter, { beats (beat), beats (beat) }, 1).front();
    }

    float valueAt (double beat) const { return valueAt (volume, beat); }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    juce::Array<te::AudioTrack*> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::MidiClip* clip = nullptr;
    phazite::ParameterRef volume, other;
};
} // namespace

TEST_CASE ("Nudged clips take the automation under them, in the same undo step")
{
    Song song;
    REQUIRE (song.clip != nullptr);
    REQUIRE (song.valueAt (6.0) == doctest::Approx (0.9f));

    {
        const phazite::UndoStep step (*song.edit, "Nudge");
        phazite::nudgeClips (song.context, { phazite::getClipID (*song.clip) }, BeatDuration::fromBeats (4.0));
    }

    CHECK (song.clip->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f)); // where it was, the line runs straight across
    CHECK (song.valueAt (2.0) == doctest::Approx (0.5f));
    CHECK (song.valueAt (14.0) == doctest::Approx (0.5f));
    CHECK (song.valueAt (song.other, 10.0) == doctest::Approx (0.3f));

    // One step takes the clip and its automation back.
    phazite::undo (*song.edit);
    CHECK (song.clip->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.5f));
}

TEST_CASE ("Duplicated clips copy the automation under them, and the originals keep theirs")
{
    Song song;
    const auto copies = phazite::duplicateClips ({ song.clip }, false, nullptr);
    REQUIRE (copies.size() == 1);
    CHECK (copies.getFirst()->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (14.0) == doctest::Approx (0.5f));
}

TEST_CASE ("A clip deleted without closing its gap leaves its automation; closing the gap takes its time out")
{
    SUBCASE ("leaving the gap")
    {
        Song song;
        phazite::deleteClips ({ song.clip }, false);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    }

    SUBCASE ("closing it")
    {
        Song song;
        phazite::addAutomationPoint (song.context, song.volume, { beats (10.0), 0.1f });
        phazite::deleteClips ({ song.clip }, true);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.1f)); // the dip at beat 10 came back four beats
        CHECK (song.valueAt (song.other, 6.0) == doctest::Approx (0.3f));
    }
}

TEST_CASE ("Deleting a time range on tracks deletes their automation there; closing the gap brings what follows back")
{
    const te::BeatRange range { teBeats (4.0), teBeats (8.0) };

    SUBCASE ("leaving the gap")
    {
        Song song;
        phazite::deleteTimeRange (*song.edit, range, { song.tracks[0] }, false);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
        CHECK (song.valueAt (2.0) == doctest::Approx (0.5f));
        CHECK (song.valueAt (12.0) == doctest::Approx (0.5f));
    }

    SUBCASE ("closing it")
    {
        Song song;
        phazite::addAutomationPoint (song.context, song.volume, { beats (10.0), 0.1f });
        phazite::deleteTimeRange (*song.edit, range, { song.tracks[0] }, true);
        CHECK (song.valueAt (6.0) == doctest::Approx (0.1f));
        CHECK (song.valueAt (song.other, 6.0) == doctest::Approx (0.3f)); // a track outside the range keeps its own
    }
}

TEST_CASE ("Duplicating a time range on tracks copies their automation right after it, in place of what was there")
{
    Song song;
    phazite::addAutomationPoint (song.context, song.volume, { beats (10.0), 0.1f }); // replaced by the copy
    const auto copied = phazite::duplicateTimeRange (*song.edit, { teBeats (4.0), teBeats (8.0) }, { song.tracks[0] });
    CHECK (copied.getStart().inBeats() == doctest::Approx (8.0));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (16.0) == doctest::Approx (0.5f)); // after the copy, the line goes on as it went
    CHECK (song.valueAt (song.other, 10.0) == doctest::Approx (0.3f));
}

TEST_CASE ("Time inserted, taken out, moved and copied across the song takes every track's automation with it, in beats")
{
    Song song;
    auto& edit = *song.edit;
    const auto beatsOf = [&song] (const phazite::ParameterRef& parameter)
    {
        std::vector<double> result;

        for (const auto& point : phazite::getAutomationPoints (song.context, parameter))
            result.push_back (point.beat.inBeats());

        return result;
    };

    // Four beats inserted at beat 4: the peak at beat 6 goes to beat 10, Tracktion's at 5 seconds, and an undo puts both
    // back.
    {
        const phazite::UndoStep step (edit, "Insert Time");
        phazite::insertTime (edit, teBeats (4.0), te::BeatDuration::fromBeats (4.0));
    }

    CHECK (beatsOf (song.volume) == std::vector { 0.0, 9.0, 10.0, 11.0, 20.0 });
    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
    CHECK (beatsOf (song.other) == std::vector { 0.0, 20.0 });
    const auto volume = phazite::findParameter (edit, song.volume);
    REQUIRE (volume != nullptr);
    CHECK (volume->getCurve().getPointTime (2).inSeconds() == doctest::Approx (5.0));

    phazite::undo (edit);
    CHECK (beatsOf (song.volume) == std::vector { 0.0, 5.0, 6.0, 7.0, 16.0 });
    CHECK (volume->getCurve().getPointTime (2).inSeconds() == doctest::Approx (3.0));

    // Beats 4 to 8 taken out: the peak goes with them, and what follows comes back four beats.
    {
        const phazite::UndoStep step (edit, "Remove Time");
        phazite::removeTime (edit, { teBeats (4.0), teBeats (8.0) });
    }

    CHECK (beatsOf (song.volume) == std::vector { 0.0, 12.0 });
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
    phazite::undo (edit);

    // Beats 4 to 8 moved to beat 12: the peak plays at beat 10, and what was between comes back to where they were.
    {
        const phazite::UndoStep step (edit, "Move Time");
        phazite::moveTime (edit, { teBeats (4.0), teBeats (8.0) }, teBeats (12.0));
    }

    CHECK (song.valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (6.0) == doctest::Approx (0.5f));
    CHECK (song.valueAt (song.other, 6.0) == doctest::Approx (0.3f));
    phazite::undo (edit);

    // Copied to beat 12: the peak plays at beat 6 and at beat 14, and what was from beat 12 on goes four beats later.
    {
        const phazite::UndoStep step (edit, "Copy Time");
        phazite::copyTime (edit, { teBeats (4.0), teBeats (8.0) }, teBeats (12.0), false);
    }

    CHECK (song.valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (song.valueAt (14.0) == doctest::Approx (0.9f));
    CHECK (beatsOf (song.volume).back() == doctest::Approx (20.0));
}
