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

TEST_CASE ("A device parameter's value reads as its unit says, and back")
{
    using phazite::devices::ParameterInfo;
    using phazite::devices::Unit;
    using phazite::ui::formatParameterValue;
    using phazite::ui::parseParameterValue;

    const auto parse = [] (const ParameterInfo& parameter, const char* text)
    { return parseParameterValue (parameter, text).value_or (-999.0f); };

    const ParameterInfo hertz { .id = "f",
                                .name = "F",
                                .minimum = 20.0f,
                                .maximum = 20000.0f,
                                .defaultValue = 1000.0f,
                                .unit = Unit::hertz };
    CHECK (formatParameterValue (hertz, 120.0f) == "120 Hz");
    CHECK (formatParameterValue (hertz, 45.5f) == "45.5 Hz");
    CHECK (formatParameterValue (hertz, 1200.0f) == "1.20 kHz");
    CHECK (formatParameterValue (hertz, 12500.0f) == "12.5 kHz");
    CHECK (parse (hertz, "1.2k") == doctest::Approx (1200.0f));
    CHECK (parse (hertz, "1.5 kHz") == doctest::Approx (1500.0f));
    CHECK (parse (hertz, "80 Hz") == doctest::Approx (80.0f));
    CHECK (parse (hertz, "1200") == doctest::Approx (1200.0f));
    CHECK (parse (hertz, "5") == doctest::Approx (20.0f)); // kept to the range
    CHECK_FALSE (parseParameterValue (hertz, "loud").has_value());

    const ParameterInfo gain { .id = "g", .name = "G", .minimum = -24.0f, .maximum = 24.0f, .unit = Unit::decibels };
    CHECK (formatParameterValue (gain, 3.0f) == "+3.0 dB");
    CHECK (parse (gain, "-6") == doctest::Approx (-6.0f));

    const ParameterInfo amount { .id = "a", .name = "A", .unit = Unit::percent };
    CHECK (formatParameterValue (amount, 0.35f) == "35%");
    CHECK (parse (amount, "35%") == doctest::Approx (0.35f));
    CHECK (parse (amount, "50") == doctest::Approx (0.5f));

    const ParameterInfo attack { .id = "t", .name = "T", .minimum = 0.0f, .maximum = 5000.0f, .unit = Unit::milliseconds };
    CHECK (formatParameterValue (attack, 12.5f) == "12.5 ms");
    CHECK (formatParameterValue (attack, 2.5f) == "2.50 ms");
    CHECK (formatParameterValue (attack, 1200.0f) == "1.20 s");
    CHECK (parse (attack, "250ms") == doctest::Approx (250.0f));
    CHECK (parse (attack, "1.5 s") == doctest::Approx (1500.0f));
    CHECK (parse (attack, "40") == doctest::Approx (40.0f));

    const ParameterInfo decay { .id = "d", .name = "D", .minimum = 0.0f, .maximum = 30.0f, .unit = Unit::seconds };
    CHECK (formatParameterValue (decay, 0.25f) == "250 ms");
    CHECK (formatParameterValue (decay, 1.2f) == "1.20 s");
    CHECK (parse (decay, "300 ms") == doctest::Approx (0.3f));
    CHECK (parse (decay, "2") == doctest::Approx (2.0f));

    const ParameterInfo tune { .id = "n", .name = "N", .minimum = -24.0f, .maximum = 24.0f, .unit = Unit::semitones };
    CHECK (formatParameterValue (tune, 7.0f) == "+7 st");
    CHECK (formatParameterValue (tune, -12.0f) == "-12 st");
    CHECK (formatParameterValue (tune, 0.0f) == "0 st");
    CHECK (formatParameterValue (tune, 0.5f) == "+0.5 st");
    CHECK (parse (tune, "+7 st") == doctest::Approx (7.0f));

    const ParameterInfo fine { .id = "c", .name = "C", .minimum = -100.0f, .maximum = 100.0f, .unit = Unit::cents };
    CHECK (formatParameterValue (fine, -15.0f) == "-15 ct");
    CHECK (parse (fine, "-15 cents") == doctest::Approx (-15.0f));

    const ParameterInfo ratio { .id = "r", .name = "R", .minimum = 1.0f, .maximum = 20.0f, .defaultValue = 4.0f, .unit = Unit::ratio };
    CHECK (formatParameterValue (ratio, 4.0f) == "4.0:1");
    CHECK (parse (ratio, "8:1") == doctest::Approx (8.0f));

    static constexpr std::string_view shapes[] = { "Sine", "Saw", "Square" };
    const ParameterInfo shape { .id = "s", .name = "S", .minimum = 0.0f, .maximum = 2.0f, .unit = Unit::choice, .choices = shapes };
    CHECK (formatParameterValue (shape, 1.0f) == "Saw");
    CHECK (parse (shape, "square") == doctest::Approx (2.0f));
    CHECK (parse (shape, "1") == doctest::Approx (0.0f)); // the first choice
    CHECK_FALSE (parseParameterValue (shape, "triangle").has_value());

    const ParameterInfo power { .id = "p", .name = "P", .unit = Unit::toggle };
    CHECK (formatParameterValue (power, 1.0f) == "On");
    CHECK (formatParameterValue (power, 0.0f) == "Off");
    CHECK (parse (power, "off") == doctest::Approx (0.0f));

    const ParameterInfo q { .id = "q", .name = "Q", .minimum = 0.1f, .maximum = 18.0f, .defaultValue = 0.71f };
    CHECK (formatParameterValue (q, 0.71f) == "0.71");
    CHECK (parse (q, "2.5") == doctest::Approx (2.5f));

    // What a value shows, read back, is the value again (to the digits shown).
    for (const auto& [parameter, value] : { std::pair { hertz, 632.0f }, std::pair { hertz, 4370.0f }, std::pair { gain, -7.5f },
                                            std::pair { attack, 87.5f }, std::pair { decay, 3.25f }, std::pair { tune, -5.0f } })
    {
        CAPTURE (value);
        CHECK (parse (parameter, formatParameterValue (parameter, value).toRawUTF8()) == doctest::Approx (value).epsilon (0.005));
    }
}
