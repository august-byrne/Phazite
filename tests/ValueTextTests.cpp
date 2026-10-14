// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "ui/components/ValueText.h"

TEST_CASE ("Decibels read as people write them")
{
    CHECK (phazite::ui::formatDecibels (-6.0) == "-6.0 dB");
    CHECK (phazite::ui::formatDecibels (2.46) == "+2.5 dB");
    CHECK (phazite::ui::formatDecibels (-0.04) == "0.0 dB");
    CHECK (phazite::ui::formatDecibels (-120.0) == "-inf dB");

    CHECK (phazite::ui::parseDecibels ("-6 dB").value_or (99.0) == doctest::Approx (-6.0));
    CHECK (phazite::ui::parseDecibels ("+3dB").value_or (99.0) == doctest::Approx (3.0));
    CHECK (phazite::ui::parseDecibels (" 0.5 ").value_or (99.0) == doctest::Approx (0.5));
    CHECK (phazite::ui::parseDecibels ("-inf dB").value_or (99.0) == doctest::Approx (phazite::ui::silenceDb));
    CHECK_FALSE (phazite::ui::parseDecibels ("loud").has_value());
    CHECK_FALSE (phazite::ui::parseDecibels ("").has_value());
}

TEST_CASE ("Pan reads as center or a percentage left or right")
{
    CHECK (phazite::ui::formatPan (0.0) == "Center");
    CHECK (phazite::ui::formatPan (0.004) == "Center");
    CHECK (phazite::ui::formatPan (-0.3) == "30% left");
    CHECK (phazite::ui::formatPan (1.0) == "100% right");

    for (const auto pan : { -1.0, -0.3, 0.0, 0.55, 1.0 })
        CHECK (phazite::ui::parsePan (phazite::ui::formatPan (pan)).value_or (99.0) == doctest::Approx (pan));

    CHECK (phazite::ui::parsePan ("-40").value_or (99.0) == doctest::Approx (-0.4));
    CHECK (phazite::ui::parsePan ("25 R").value_or (99.0) == doctest::Approx (0.25));
    CHECK_FALSE (phazite::ui::parsePan ("somewhere").has_value());
}
