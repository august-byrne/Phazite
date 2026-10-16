// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own beats and IDs (src/model/base/Beats.h and IDs.h), which the model's public functions and the UI pass
// instead of Tracktion's, and their conversions in the model's prelude.
#include "TestHelpers.h"
#include "model/base/internal/Tracktion.h"

#include <unordered_set>

namespace
{
using phazite::BeatDuration;
using phazite::BeatPosition;
using phazite::BeatRange;
} // namespace

TEST_CASE ("Beat positions and lengths add up as numbers of beats do")
{
    const auto two = BeatPosition::fromBeats (2.0);
    const auto half = BeatDuration::fromBeats (0.5);

    CHECK ((two + half).inBeats() == doctest::Approx (2.5));
    CHECK ((two - half).inBeats() == doctest::Approx (1.5));
    CHECK ((BeatPosition::fromBeats (5.0) - two).inBeats() == doctest::Approx (3.0));
    CHECK ((half * 4.0).inBeats() == doctest::Approx (2.0));
    CHECK ((half / 2.0).inBeats() == doctest::Approx (0.25));
    CHECK (BeatDuration::fromBeats (3.0) / half == doctest::Approx (6.0));
    CHECK ((-half).inBeats() == doctest::Approx (-0.5));

    auto moving = two;
    moving += half;
    moving -= BeatDuration::fromBeats (1.0);
    CHECK (moving.inBeats() == doctest::Approx (1.5));

    CHECK (two < two + half);
    CHECK (two + half > two);
    CHECK (two <= two);
    CHECK (two >= two);
    CHECK (two == BeatPosition::fromBeats (2.0));
    CHECK (two != two + half);
    CHECK (BeatPosition() == BeatPosition::fromBeats (0.0));
}

TEST_CASE ("A beat range runs from its start to its end, never backwards")
{
    const BeatRange bar { BeatPosition::fromBeats (4.0), BeatPosition::fromBeats (8.0) };
    CHECK (bar.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (bar.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (bar.getLength().inBeats() == doctest::Approx (4.0));
    CHECK (bar.getCentre().inBeats() == doctest::Approx (6.0));
    CHECK_FALSE (bar.isEmpty());

    // Its start is in it, its end isn't.
    CHECK (bar.contains (BeatPosition::fromBeats (4.0)));
    CHECK (bar.contains (BeatPosition::fromBeats (7.99)));
    CHECK_FALSE (bar.contains (BeatPosition::fromBeats (8.0)));

    // An end before the start makes an empty range at the start; between() puts two positions in order.
    const BeatRange backwards { BeatPosition::fromBeats (8.0), BeatPosition::fromBeats (4.0) };
    CHECK (backwards.isEmpty());
    CHECK (backwards.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (BeatRange::between (BeatPosition::fromBeats (8.0), BeatPosition::fromBeats (4.0)) == bar);
    CHECK (BeatRange (BeatPosition::fromBeats (4.0), BeatDuration::fromBeats (4.0)) == bar);
    CHECK (BeatRange().isEmpty());

    CHECK (bar.withStart (BeatPosition::fromBeats (6.0)).getLength().inBeats() == doctest::Approx (2.0));
    CHECK (bar.withEnd (BeatPosition::fromBeats (12.0)).getLength().inBeats() == doctest::Approx (8.0));
    CHECK (bar.movedBy (BeatDuration::fromBeats (-4.0)).getStart().inBeats() == doctest::Approx (0.0));

    const BeatRange later { BeatPosition::fromBeats (6.0), BeatPosition::fromBeats (10.0) };
    CHECK (bar.overlaps (later));
    CHECK (bar.getIntersectionWith (later) == BeatRange (BeatPosition::fromBeats (6.0), BeatPosition::fromBeats (8.0)));
    CHECK_FALSE (bar.overlaps (BeatRange (BeatPosition::fromBeats (8.0), BeatPosition::fromBeats (9.0))));
    CHECK (bar.getIntersectionWith (BeatRange (BeatPosition::fromBeats (9.0), BeatPosition::fromBeats (10.0))).isEmpty());
}

TEST_CASE ("The model turns Phazite's beats and IDs into Tracktion's and back")
{
    const BeatRange range { BeatPosition::fromBeats (1.5), BeatPosition::fromBeats (3.25) };
    const auto inTracktion = phazite::toTracktion (range);
    CHECK (inTracktion.getStart().inBeats() == doctest::Approx (1.5));
    CHECK (inTracktion.getEnd().inBeats() == doctest::Approx (3.25));
    CHECK (phazite::fromTracktion (inTracktion) == range);
    CHECK (phazite::fromTracktion (phazite::toTracktion (BeatDuration::fromBeats (0.75))) == BeatDuration::fromBeats (0.75));

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    REQUIRE (track != nullptr);
    const auto id = phazite::getTrackID (*track);
    CHECK (id.isValid());
    CHECK (phazite::toTracktion (id) == track->itemID);
    CHECK_FALSE (phazite::TrackID().isValid());

    // Each kind of item has an ID type of its own; IDs order and hash as numbers.
    std::unordered_set<phazite::TrackID> ids { id, phazite::TrackID::fromRaw (id.getRaw()) };
    CHECK (ids.size() == 1);
    CHECK (phazite::ClipID::fromRaw (1) < phazite::ClipID::fromRaw (2));
}
