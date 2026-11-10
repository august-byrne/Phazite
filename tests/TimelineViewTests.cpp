// SPDX-License-Identifier: AGPL-3.0-or-later
// What an editor shows of the time line (src/ui/timeline/TimelineView.h).
#include "TestHelpers.h"
#include "ui/timeline/TimelineView.h"

#include <cmath>
#include <optional>

using phazite::BeatPosition;

TEST_CASE ("The timeline converts between beats and pixels, and zooms around a point")
{
    phazite::ui::TimelineView view;
    view.start = BeatPosition::fromBeats (4.0);
    view.pixelsPerBeat = 10.0;

    CHECK (view.beatToX (BeatPosition::fromBeats (6.0)) == doctest::Approx (20.0));
    CHECK (view.xToBeat (20.0).inBeats() == doctest::Approx (6.0));

    // The beat under the pointer stays where it is.
    view.zoomAround (20.0, 2.0);
    CHECK (view.pixelsPerBeat == doctest::Approx (20.0));
    CHECK (view.xToBeat (20.0).inBeats() == doctest::Approx (6.0));

    // The zoom has limits, and the view doesn't start before the song.
    view.zoomAround (0.0, 1.0e9);
    CHECK (view.pixelsPerBeat == doctest::Approx (phazite::ui::TimelineView::maxPixelsPerBeat));
    view.zoomAround (500.0, 1.0e-9);
    CHECK (view.pixelsPerBeat == doctest::Approx (phazite::ui::TimelineView::minPixelsPerBeat));
    CHECK (view.start.inBeats() == doctest::Approx (0.0));

    view.start = BeatPosition::fromBeats (4.0);
    view.scrollBy (-1.0e6);
    CHECK (view.start.inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("A beat's column moves with a view that moves by whole pixels, a line half a pixel in too, within half a pixel of its place")
{
    // At 36 pixels a beat, eighth notes are 4.5 pixels apart: every other one's place is half a pixel in.
    phazite::ui::TimelineView view;
    view.pixelsPerBeat = 36.0;

    for (int eighth = 0; eighth < 64; ++eighth)
    {
        const auto beat = BeatPosition::fromBeats (eighth * 0.5);
        CAPTURE (eighth);
        std::optional<int> firstColumn;

        // The view starting at one whole pixel after another, as scrolling continuously starts it: the column moves a pixel
        // each time, never two or none.
        for (int pixels = 1000; pixels < 1100; ++pixels)
        {
            view.start = BeatPosition::fromBeats (pixels / view.pixelsPerBeat);
            const auto column = view.beatToColumn (beat);
            CHECK (std::abs (column - view.beatToX (beat)) <= 0.5 + 1.0e-6);

            if (! firstColumn.has_value())
                firstColumn = column + pixels;

            CHECK (column + pixels == *firstColumn);
        }
    }

    // A view a part of a pixel along: still within half a pixel, a column as wide as its beats.
    view.start = BeatPosition::fromBeats (1000.3 / view.pixelsPerBeat);
    CHECK (std::abs (view.beatToColumn (BeatPosition::fromBeats (40.0)) - view.beatToX (BeatPosition::fromBeats (40.0))) <= 0.5);
    CHECK (view.beatToColumn (BeatPosition::fromBeats (41.0)) - view.beatToColumn (BeatPosition::fromBeats (40.0)) == 36);

    // Far past any song, a column is still a number.
    view.start = {};
    CHECK (view.beatToColumn (BeatPosition::fromBeats (1.0e12)) == 1000000000);
}
