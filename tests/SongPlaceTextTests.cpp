// SPDX-License-Identifier: AGPL-3.0-or-later
// Places and lengths in the song as Phazite writes them (src/ui/timeline/SongPlaceText.h): places in bars, beats and
// sixteenths, as the transport bar shows the playhead and a drag's readout shows what it moves, and lengths in bars and beats.
#include "TestSong.h"
#include "ui/timeline/SongPlaceText.h"

namespace
{
phazite::BeatPosition at (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

phazite::BeatDuration lasting (double beats)
{
    return phazite::BeatDuration::fromBeats (beats);
}
} // namespace

TEST_CASE ("Places read as bars, beats and sixteenths, and lengths as bars and beats, by the time signature")
{
    using namespace phazite::ui;
    phazite::test::TestSong song;
    const auto& context = song.context;

    // In 4/4: bar 17 starts at beat 64.
    CHECK (formatBarPosition (context, at (0.0)) == "1.1.1");
    CHECK (formatBarPosition (context, at (65.25)) == "17.2.2");
    CHECK (formatBarPosition (context, at (65.2)) == "17.2.1"); // rounded down, as a clock counts
    CHECK (parseBarPosition (context, "17.2.2") == at (65.25));
    CHECK (parseBarPosition (context, "17") == at (64.0));
    CHECK (parseBarPosition (context, "17 3") == at (66.0));
    CHECK_FALSE (parseBarPosition (context, "").has_value());

    CHECK (formatSongLength (context, at (0.0), lasting (9.5)) == "2 bars 1.5 beats");
    CHECK (formatSongLength (context, at (0.0), lasting (4.0)) == "1 bar");
    CHECK (formatSongLength (context, at (0.0), lasting (1.0)) == "1 beat");
    CHECK (formatSongLength (context, at (0.0), lasting (0.25)) == "0.25 beats");
    CHECK (formatSongLength (context, at (0.0), lasting (0.0)) == "0 beats");

    // In 6/8, a beat is an eighth note and a bar three quarter notes.
    song.setTimeSignature (6, 8);
    CHECK (formatBarPosition (context, at (3.5)) == "2.2.1");
    CHECK (formatSongLength (context, at (0.0), lasting (3.5)) == "1 bar 1 beat");
}
