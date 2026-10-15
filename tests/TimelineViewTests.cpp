// SPDX-License-Identifier: AGPL-3.0-or-later
// What an editor shows of the time line (src/ui/timeline/TimelineView.h).
#include "TestHelpers.h"
#include "ui/timeline/TimelineView.h"

using namespace tracktion::literals; // 4_bp: beat positions

TEST_CASE ("The timeline converts between beats and pixels, and zooms around a point")
{
    phazite::ui::TimelineView view;
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
    CHECK (view.pixelsPerBeat == doctest::Approx (phazite::ui::TimelineView::maxPixelsPerBeat));
    view.zoomAround (500.0, 1.0e-9);
    CHECK (view.pixelsPerBeat == doctest::Approx (phazite::ui::TimelineView::minPixelsPerBeat));
    CHECK (view.start.inBeats() == doctest::Approx (0.0));

    view.start = 4_bp;
    view.scrollBy (-1.0e6);
    CHECK (view.start.inBeats() == doctest::Approx (0.0));
}
