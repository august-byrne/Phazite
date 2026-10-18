// SPDX-License-Identifier: AGPL-3.0-or-later
// How the piano roll lays out a drum track (src/ui/pianoroll/DrumRows.h): plain values in, rows out.
#include "TestHelpers.h"
#include "ui/pianoroll/DrumRows.h"

#include <algorithm>

TEST_CASE ("A drum track shows the usual drums, the notes it plays, in its order, without hidden rows")
{
    using phazite::ui::getDrumRows;
    const std::vector<int> played { 38, 70 }; // the snare is one of the usual drums already

    // Without a sampler: General MIDI's usual drums, then the other notes the clips play.
    auto rows = getDrumRows (std::nullopt, played, {}, {});
    REQUIRE (rows.size() == 12);
    CHECK (rows.front().note == 36);
    CHECK (rows.front().name == juce::String ("Kick"));
    CHECK (rows.back().note == 70);

    // In the track's order, without its hidden rows; rows the order doesn't mention keep theirs, after it.
    const auto order = phazite::ui::getDrumOrderAfterMove (rows, 70, 0);
    rows = getDrumRows (std::nullopt, played, order, { 37 });
    REQUIRE (rows.size() == 11);
    CHECK (rows.front().note == 70);
    CHECK (rows[1].note == 36);
    CHECK (std::none_of (rows.begin(), rows.end(), [] (const phazite::DrumRow& row) { return row.note == 37; }));
    CHECK (getDrumRows (std::nullopt, played, order, {}).size() == 12); // all shown again

    // A drum sampler's pads take the usual drums' place.
    const std::vector<phazite::DrumRow> pads { { 60, "Boom" }, { 62, "Tss" } };
    rows = getDrumRows (pads, played, {}, {});
    REQUIRE (rows.size() == 4);
    CHECK (rows[0].name == juce::String ("Boom"));
    CHECK (rows[2].note == 38);
    CHECK (rows[3].note == 70);
}

TEST_CASE ("Moving a drum row reorders the track's rows")
{
    const std::vector<phazite::DrumRow> shown { { 36, "Kick" }, { 38, "Snare" }, { 42, "Closed Hat" } };

    CHECK (phazite::ui::getDrumOrderAfterMove (shown, 42, 0) == std::vector<int> { 42, 36, 38 }); // to the front
    CHECK (phazite::ui::getDrumOrderAfterMove (shown, 36, 1) == std::vector<int> { 38, 36, 42 });
    CHECK (phazite::ui::getDrumOrderAfterMove (shown, 36, 99) == std::vector<int> { 38, 42, 36 }); // past the end: last
    CHECK (phazite::ui::getDrumOrderAfterMove (shown, 38, -5) == std::vector<int> { 38, 36, 42 }); // before the start: first
}
