// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (src/model/transport/internal/Tempo.h).
#include "TestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/commands/internal/Undo.h"
#include "model/transport/internal/Tempo.h"

TEST_CASE ("The tempo changes as one undo step, within Tracktion's limits, and clips stay on their beats")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    auto* clip = phazite::insertMidiClip (*track, { te::BeatPosition::fromBeats (8.0), te::BeatPosition::fromBeats (12.0) }, nullptr);
    REQUIRE (clip != nullptr);
    edit->getUndoManager().clearUndoHistory();
    CHECK (phazite::getTempo (*edit) == doctest::Approx (120.0));

    phazite::setTempo (*edit, 90.004);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (90.0));
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (clip->getPosition().getStart().inSeconds() == doctest::Approx (8.0 * 60.0 / 90.0));

    phazite::setTempo (*edit, 1000.0);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (phazite::maxTempo));
    phazite::setTempo (*edit, 1.0);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (phazite::minTempo));

    phazite::undo (*edit);
    phazite::undo (*edit);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (90.0));

    // The same tempo again changes nothing.
    edit->getUndoManager().clearUndoHistory();
    phazite::setTempo (*edit, 90.0);
    CHECK_FALSE (edit->getUndoManager().canUndo());
}

TEST_CASE ("The time signature changes as one undo step, to one Phazite takes")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->getUndoManager().clearUndoHistory();
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 4, 4 });

    phazite::setTimeSignature (*edit, { 6, 8 });
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 6, 8 });
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Time Signature"));

    // Not 0 beats, nor a beat of a third of a note.
    for (const auto invalid : { phazite::TimeSignature { 0, 4 }, phazite::TimeSignature { 4, 3 }, phazite::TimeSignature { 33, 4 } })
    {
        CHECK_FALSE (phazite::isValid (invalid));
        phazite::setTimeSignature (*edit, invalid);
        CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 6, 8 });
    }

    phazite::undo (*edit);
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 4, 4 });
}
