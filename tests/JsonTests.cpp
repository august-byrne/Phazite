// SPDX-License-Identifier: AGPL-3.0-or-later
// JSON as Phazite reads it (src/model/base/Json.h): what juce::JSON::parse() reads, but for a text with a number its
// parser can't count, which is no JSON. Under UndefinedBehaviorSanitizer (linux-asan) these also show that no such number
// reaches the parser.
#include "TestHelpers.h"
#include "model/base/Json.h"

#include <cmath>

TEST_CASE ("JSON is read as JUCE reads it, the largest whole number of 64 bits too")
{
    const auto json = phazite::parseJson (R"({"name": "Lead", "largest": 9223372036854775807, "least": -9223372036854775807,
                                              "level": -1.5e10, "small": 2.5E-300, "list": [1, 2.0, true, null]})");
    REQUIRE (json.isObject());
    CHECK (json["name"].toString() == "Lead");
    CHECK ((juce::int64) json["largest"] == std::numeric_limits<juce::int64>::max());
    CHECK ((juce::int64) json["least"] == -std::numeric_limits<juce::int64>::max());
    CHECK ((double) json["level"] == doctest::Approx (-1.5e10));
    CHECK ((double) json["small"] == doctest::Approx (2.5e-300));
    REQUIRE (json["list"].isArray());
    CHECK (json["list"].size() == 4);

    CHECK (phazite::parseJson ("no JSON").isVoid());
    CHECK (phazite::parseJson (juce::String()).isVoid());
}

TEST_CASE ("A text with a number too long to count is no JSON")
{
    // The fuzzer's finding with many rounds (the nightly run, 2026-11-15): JUCE's parser counts a number's digits up in an
    // integer before it knows whether they fit.
    for (const auto* text : { R"({"a": 9223372036854775808})",              // one past the largest
                              R"({"a": 12345678901234567890})",             // twenty digits
                              R"([-9223372036854775808])",                  // counted before its sign is given
                              R"([12345678901234567890123.5])",             // counted before the point is met
                              R"([1e1234567890])",                          // an exponent past an int
                              R"([1.5E-1234567890])",                       //
                              R"([2e+1234567890])",                         //
                              R"([-.12345678901234567890])",                // a point that follows no digit starts no fraction
                              R"(["a\\", 12345678901234567890123])",        // after a string that ends in a backslash
                              R"({"a": 'b', "c": 99999999999999999999})" }) // after a string in single quotes
    {
        CAPTURE (text);
        CHECK (phazite::parseJson (juce::String (text)).isVoid());
    }
}

TEST_CASE ("Long runs of digits that are no number, or that a number doesn't count, are read")
{
    // In a string, a quote in it and all.
    const auto named = phazite::parseJson (R"({"name": "Take \"12345678901234567890123\"", "other": '99999999999999999999'})");
    REQUIRE (named.isObject());
    CHECK (named["name"].toString() == "Take \"12345678901234567890123\"");
    CHECK (named["other"].toString() == "99999999999999999999");

    // After a point: as many as a writer cares to give.
    const auto fractions = phazite::parseJson (R"([0.1234567890123456789012345, 123.000000000000000000000001e2])");
    REQUIRE (fractions.isArray());
    REQUIRE (fractions.size() == 2);
    CHECK ((double) fractions[0] == doctest::Approx (0.1234567890123456789));
    CHECK ((double) fractions[1] == doctest::Approx (12300.0));

    // Eighteen digits, and an exponent of nine: each fits.
    const auto fitting = phazite::parseJson (R"([999999999999999999, 1e000000308])");
    REQUIRE (fitting.isArray());
    REQUIRE (fitting.size() == 2);
    CHECK ((juce::int64) fitting[0] == 999999999999999999);
    CHECK ((double) fitting[1] == doctest::Approx (1.0e308));
}

TEST_CASE ("A number whose exponent JUCE's reader would overflow is told, and made the same number in a text it reads")
{
    // Known bug 170: CharacterFunctions::readDoubleValue(), which every number read from XML goes through, counts an
    // exponent's digits into an int. What it would read as a number's exponent, and nothing else.
    for (const auto* text : { "1e12345678901", "-2.5E-98765432109", " +7e+9999999999", "1e0000000002147483648", ".5e12345678901 1" })
    {
        CAPTURE (text);
        CHECK (phazite::hasExponentTooLongToRead (text));
    }

    // A digit of another script is none to the reader, which looks at a juce::String's UTF-8 bytes (CharPointer_UTF8::isDigit()):
    // no exponent at all.
    CHECK_FALSE (phazite::hasExponentTooLongToRead (juce::String ("1e") + juce::String::charToString ((juce::juce_wchar) 0x0669)));

    for (const auto* text : { "1e999999999", "1e0000000000000308", "Take e12345678901", "e12345678901", "-.e12345678901", "12345678901",
                              "0.1234567890123", "1e", "" })
    {
        CAPTURE (text);
        CHECK_FALSE (phazite::hasExponentTooLongToRead (text));
    }

    // The same number as a double holds it, which JUCE then reads without overflowing; what follows it stays.
    CHECK (phazite::makeExponentReadable ("1e12345678901") == juce::String ("1e999"));
    CHECK (phazite::makeExponentReadable ("-3.5e12345678901") == juce::String ("-1e999"));
    CHECK (phazite::makeExponentReadable ("2e-12345678901") == juce::String ("0"));
    CHECK (phazite::makeExponentReadable ("-2e-12345678901 4") == juce::String ("-0 4"));
    CHECK (phazite::makeExponentReadable ("Lead") == juce::String ("Lead"));
    CHECK (std::isinf (phazite::makeExponentReadable ("1e12345678901").getDoubleValue()));
    CHECK (phazite::makeExponentReadable ("2e-12345678901").getDoubleValue() == 0.0);
}
