// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own beats and IDs (src/model/base/Beats.h and IDs.h), which the model's functions and the UI pass.
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/project/internal/Project.h"

#include <algorithm>
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

TEST_CASE ("A song's items have IDs, each kind its own type, which order and hash as numbers")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::getTrackIDs (*song);
    REQUIRE (tracks.size() == 1);
    const auto id = tracks.front();
    CHECK (id.isValid());
    CHECK (phazite::findTrackState (*song, id).isValid());
    CHECK_FALSE (phazite::TrackID().isValid());

    // Each kind of item has an ID type of its own; IDs order and hash as numbers.
    std::unordered_set<phazite::TrackID> ids { id, phazite::TrackID::fromRaw (id.getRaw()) };
    CHECK (ids.size() == 1);
    CHECK (phazite::ClipID::fromRaw (1) < phazite::ClipID::fromRaw (2));
}

TEST_CASE ("Beats compare, and ranges meet and join, with numbers the compiler can't know")
{
    // CI's Windows release build (MSVC 19.52) made the part of a clip from 0 to 8 that lies in a range from 2 to 6 end at 2
    // (known bug 204), where every other build made it end at 6. The steps of that sum on their own, with numbers read
    // through volatiles so that none is worked out as the test is compiled.
    volatile double numbers[] = { 0.0, 8.0, 2.0, 6.0 };
    const auto clipStart = BeatPosition::fromBeats (numbers[0]), clipEnd = BeatPosition::fromBeats (numbers[1]);
    const auto rangeStart = BeatPosition::fromBeats (numbers[2]), rangeEnd = BeatPosition::fromBeats (numbers[3]);

    CHECK (rangeEnd < clipEnd);
    CHECK_FALSE (clipEnd < rangeEnd);
    CHECK (clipStart < rangeStart);
    CHECK (std::min (clipEnd, rangeEnd).inBeats() == doctest::Approx (6.0));
    CHECK (std::min (rangeEnd, clipEnd).inBeats() == doctest::Approx (6.0));
    CHECK (std::max (clipStart, rangeStart).inBeats() == doctest::Approx (2.0));
    CHECK (std::max (rangeStart, rangeEnd).inBeats() == doctest::Approx (6.0));
    CHECK (std::max (std::max (clipStart, rangeStart), std::min (clipEnd, rangeEnd)).inBeats() == doctest::Approx (6.0));

    const BeatRange clip (clipStart, clipEnd), range (rangeStart, rangeEnd);
    CHECK (clip.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clip.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (range.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (range.getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (range.getLength().inBeats() == doctest::Approx (4.0));

    // Of what the getters return. (Not here: a BeatRange made of the two at once, std::max (...) and std::min (...) of the
    // getters as its arguments, which is what that compiler gets wrong; the ranges' own functions below are used instead.)
    CHECK (std::min (clip.getEnd(), range.getEnd()).inBeats() == doctest::Approx (6.0));
    CHECK (std::max (clip.getStart(), range.getStart()).inBeats() == doctest::Approx (2.0));

    const auto met = clip.getIntersectionWith (range);
    CHECK (met.getStart().inBeats() == doctest::Approx (2.0));
    CHECK (met.getEnd().inBeats() == doctest::Approx (6.0));
    CHECK (met == range);
    CHECK (range.getIntersectionWith (clip) == range);
    CHECK (clip.overlaps (range));
    CHECK (clip.contains (rangeStart));

    const auto joined = range.getUnionWith (clip);
    CHECK (joined.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (joined.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (joined == clip);
    CHECK (BeatRange::between (clipEnd, rangeStart) == BeatRange (rangeStart, clipEnd));
    CHECK (range.withEnd (clipEnd).getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (range.movedBy (BeatDuration::fromBeats (numbers[2])).getEnd().inBeats() == doctest::Approx (8.0));

    // Lengths, the same way.
    const auto shorter = BeatDuration::fromBeats (numbers[2]), longer = BeatDuration::fromBeats (numbers[3]);
    CHECK (std::min (longer, shorter).inBeats() == doctest::Approx (2.0));
    CHECK (std::max (shorter, longer).inBeats() == doctest::Approx (6.0));
    CHECK (std::min (clip.getLength(), range.getLength()).inBeats() == doctest::Approx (4.0));
}
