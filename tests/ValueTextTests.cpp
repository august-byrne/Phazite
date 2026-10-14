// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "ui/components/ValueText.h"

TEST_CASE ("Decibels read as people write them")
{
    CHECK (phazite::formatDecibels (-6.0) == "-6.0 dB");
    CHECK (phazite::formatDecibels (2.46) == "+2.5 dB");
    CHECK (phazite::formatDecibels (-0.04) == "0.0 dB");
    CHECK (phazite::formatDecibels (-120.0) == "-inf dB");

    CHECK (phazite::parseDecibels ("-6 dB").value_or (99.0) == doctest::Approx (-6.0));
    CHECK (phazite::parseDecibels ("+3dB").value_or (99.0) == doctest::Approx (3.0));
    CHECK (phazite::parseDecibels (" 0.5 ").value_or (99.0) == doctest::Approx (0.5));
    CHECK (phazite::parseDecibels ("-inf dB").value_or (99.0) == doctest::Approx (phazite::silenceDb));
    CHECK_FALSE (phazite::parseDecibels ("loud").has_value());
    CHECK_FALSE (phazite::parseDecibels ("").has_value());
}

TEST_CASE ("Pan reads as center or a percentage left or right")
{
    CHECK (phazite::formatPan (0.0) == "Center");
    CHECK (phazite::formatPan (0.004) == "Center");
    CHECK (phazite::formatPan (-0.3) == "30% left");
    CHECK (phazite::formatPan (1.0) == "100% right");

    for (const auto pan : { -1.0, -0.3, 0.0, 0.55, 1.0 })
        CHECK (phazite::parsePan (phazite::formatPan (pan)).value_or (99.0) == doctest::Approx (pan));

    CHECK (phazite::parsePan ("-40").value_or (99.0) == doctest::Approx (-0.4));
    CHECK (phazite::parsePan ("25 R").value_or (99.0) == doctest::Approx (0.25));
    CHECK_FALSE (phazite::parsePan ("somewhere").has_value());
}
