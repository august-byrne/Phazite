// SPDX-License-Identifier: AGPL-3.0-or-later
// How Phazite writes a song as text (model/base/internal/SongText.h; docs/project-format.md): one text for each value, the
// same in a project file and in a version's pieces; bytes in standard base 64; and XML that any XML reader takes.
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "devices/modulation/Modulators.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/project/internal/Project.h"

#include <cmath>

namespace
{
juce::String escaped (const juce::String& text)
{
    juce::MemoryOutputStream out;
    phazite::writeEscaped (out, text);
    return out.toString();
}

juce::String projectText (const juce::ValueTree& element)
{
    juce::MemoryOutputStream out;
    phazite::writeProjectText (out, element);
    return out.toString();
}
} // namespace

TEST_CASE ("A value has one text: whole numbers without .0, a float's in its fewest digits, others in 15")
{
    using phazite::writeValue;

    CHECK (writeValue (4) == "4");
    CHECK (writeValue ((juce::int64) 1234567890123) == "1234567890123");
    CHECK (writeValue (4.0) == "4");
    CHECK (writeValue (-0.0) == "0");
    CHECK (writeValue (true) == "1");
    CHECK (writeValue (false) == "0");

    // A float, widened to a double as juce::var holds it, in the fewest digits that read back as that float.
    CHECK (writeValue (0.3f) == "0.3");
    CHECK (writeValue (-0.1f) == "-0.1");
    CHECK (writeValue (1234.5f) == "1234.5");

    // Any other double in 15 significant digits: what arithmetic leaves past them isn't written.
    CHECK (writeValue (0.1 + 0.2) == "0.3");
    CHECK (writeValue (8.000000000000004) == "8");
    CHECK (writeValue (1.0 / 3.0) == "0.333333333333333");
    CHECK (writeValue (11.7073125) == "11.7073125");

    // Text as it is, without what XML can't hold.
    CHECK (writeValue ("Bass") == "Bass");
    CHECK (writeValue (juce::String ("a") + juce::String::charToString (1) + "b") == "ab");
    CHECK (writeValue ("line 1\nline 2") == "line 1\nline 2");
}

TEST_CASE ("Two values are the same if they write the same text, as a number read from a file and the one set are")
{
    using phazite::isSameValue;

    CHECK (isSameValue ("4", 4.0));
    CHECK (isSameValue (4.0, "4"));
    CHECK (isSameValue ("0.3", 0.3f));
    CHECK (isSameValue (0.3, 0.3f));
    CHECK (isSameValue ("1", true));
    CHECK_FALSE (isSameValue ("4.0", 4.0)); // text is text
    CHECK_FALSE (isSameValue (0.3f, 0.31f));
    CHECK_FALSE (isSameValue ("Bass", "bass"));

    // Compared without making their texts, the same as their texts compare.
    CHECK (isSameValue (4, (juce::int64) 4));
    CHECK (isSameValue (4, 4.0));
    CHECK (isSameValue (-0.0, 0.0));
    CHECK (isSameValue (-0.0, "0"));
    CHECK (isSameValue (8.000000000000004, 8.0));
    CHECK (isSameValue (std::nan (""), "nan"));
    CHECK (isSameValue (false, 0));
    CHECK (isSameValue (juce::String ("a") + juce::String::charToString (1) + "b", "ab"));
    CHECK (isSameValue (juce::String ("4") + juce::String::charToString (1), 4));
    CHECK (isSameValue (juce::var(), ""));
    CHECK_FALSE (isSameValue (4, 5.0));
    CHECK_FALSE (isSameValue ("1", false));
    CHECK_FALSE (isSameValue ("0.30", 0.3));

    // Whatever the pair, as their texts compare.
    const juce::Array<juce::var> values { 0,         1,
                                          -1,        (juce::int64) 1 << 40,
                                          0.5,       0.3f,
                                          0.1 + 0.2, -0.0,
                                          1.0e20,    std::nan (""),
                                          true,      false,
                                          "0",       "1",
                                          "0.5",     "0.3",
                                          "1e+20",   "nan",
                                          "Bass",    juce::var() };

    for (const auto& a : values)
        for (const auto& b : values)
            CHECK (isSameValue (a, b) == (phazite::writeValue (a) == phazite::writeValue (b)));
}

TEST_CASE ("A decimal of up to six significant digits from 0.001 up is written as it is, as the loader takes it to be")
{
    // What makeConsistent() leaves as it is without writing the number (Project.h: isNumberWrittenAsPhazite()): a whole number's
    // digits, and a decimal from 0.001 up of up to six significant digits, without an exponent or a leading or trailing zero.
    juce::Random random (20261116);

    for (int i = 0; i < 200000; ++i)
    {
        const auto digits = 1 + random.nextInt (6);
        auto mantissa = (juce::int64) 1 + random.nextInt64() % 999999;
        mantissa = std::abs (mantissa) % (juce::int64) std::pow (10.0, digits);

        if (mantissa == 0 || mantissa % 10 == 0)
            continue;

        const auto mantissaText = juce::String (mantissa);
        const auto fraction = 1 + random.nextInt (mantissaText.length() + 2); // digits after the point
        juce::String text;

        if (fraction >= mantissaText.length())
            text = "0." + juce::String::repeatedString ("0", fraction - mantissaText.length()) + mantissaText;
        else
            text = mantissaText.dropLastCharacters (fraction) + "." + mantissaText.getLastCharacters (fraction);

        if (text.startsWith ("0.000"))
            continue; // below 0.001

        if (random.nextBool())
            text = "-" + text;

        INFO (text);
        const auto number = text.getDoubleValue();
        CHECK (phazite::writeValue (number) == text);
        CHECK (phazite::writeValue ((double) (float) number) == text);
    }
}

TEST_CASE ("The loader takes a number's text as Phazite's own only where Phazite writes that text for the number it reads as")
{
    // isNumberWrittenAsPhazite() is how makeConsistent() leaves a number as it is without writing it: every text it takes
    // must be what writeValue() writes for the number the loader reads from it (readValueNumber()).
    using phazite::isNumberWrittenAsPhazite;

    const auto holds = [] (const juce::var& value)
    {
        if (! isNumberWrittenAsPhazite (value))
            return true;

        const auto number = phazite::readValueNumber (value);
        return value.isString() && number.has_value() && phazite::writeValue (*number) == value.toString();
    };

    // What Phazite writes, taken.
    for (const auto* text : { "0", "5", "-5", "60", "999999999999999", "0.25", "-0.5", "0.001", "-0.001", "12345.6", "0.123456" })
    {
        INFO (text);
        CHECK (isNumberWrittenAsPhazite (juce::String (text)));
        CHECK (holds (juce::String (text)));
    }

    // What it never writes, or writes otherwise, not taken; and what isn't text.
    const auto digits = [] (int count) { return "1" + juce::String::repeatedString ("2", count - 1); };
    const juce::StringArray others { "+5",
                                     " 5",
                                     "5 ",
                                     "1e3",
                                     "1E3",
                                     "0x10",
                                     "nan",
                                     "inf",
                                     "-inf",
                                     "-0",
                                     "-0.0",
                                     "00.5",
                                     ".5",
                                     "5.",
                                     "-",
                                     "",
                                     "1.50",
                                     "01.5",
                                     "1,5",
                                     "0.0001",
                                     "1234567.5",
                                     "0.1234567",
                                     digits (16),
                                     digits (400),
                                     juce::String::fromUTF8 ("\xd9\xa5"),     // ٥, an Arabic-Indic five
                                     juce::String::fromUTF8 ("\xef\xbc\x95"), // ５, a full-width five
                                     juce::String::fromUTF8 ("1\xd9\xa5"),
                                     "5\n",
                                     "5\t",
                                     "5\r\n" };

    for (const auto& text : others)
    {
        INFO ("\"" << text << "\"");
        CHECK_FALSE (isNumberWrittenAsPhazite (text));
    }

    // JUCE's String ends at a NUL, so a text with one after its number is that number's text.
    CHECK (holds (juce::String (juce::CharPointer_UTF8 ("5\0"))));

    for (const auto& value : { juce::var (5), juce::var ((juce::int64) 5), juce::var (5.0), juce::var (true), juce::var(),
                               juce::var (juce::Array<juce::var> { "5" }) })
        CHECK_FALSE (isNumberWrittenAsPhazite (value));

    // Texts of every form: whole numbers of any size and sign, decimals with and without leading and trailing zeros, numbers
    // below 0.001 and past 10^15, floats' and doubles' own texts, and junk.
    juce::Random random (20261117);
    auto taken = 0, takenDecimals = 0;

    const auto make = [&random]() -> juce::String
    {
        switch (random.nextInt (7))
        {
            case 6:
            {
                // A decimal as Phazite writes many: up to six digits, the point anywhere in or before them.
                const auto number = juce::String (1 + random.nextInt (999999));
                const auto point = random.nextInt (number.length());
                const auto text = point == 0 ? "0." + juce::String::repeatedString ("0", random.nextInt (3)) + number
                                             : number.substring (0, point) + "." + number.substring (point);
                return random.nextBool() ? "-" + text : text;
            }
            case 0:
                return juce::String (random.nextInt64() >> random.nextInt (64));
            case 1:
            {
                auto text = juce::String (std::abs (random.nextInt64() % (juce::int64) std::pow (10.0, 1 + random.nextInt (9))));
                const auto point = random.nextInt (text.length() + 3);
                text = juce::String::repeatedString ("0", random.nextInt (3)) + text
                       + juce::String::repeatedString ("0", random.nextInt (3) == 0 ? 1 : 0);
                text = point >= text.length() ? text : text.substring (0, point) + "." + text.substring (point);
                return random.nextBool() ? "-" + text : text;
            }
            case 2:
                return phazite::writeValue ((double) (float) (random.nextDouble() * std::pow (10.0, random.nextInt (30) - 10)));
            case 3:
                return phazite::writeValue (random.nextDouble() * std::pow (10.0, random.nextInt (30) - 10));
            case 4:
                return phazite::writeValue (-random.nextDouble() * std::pow (10.0, random.nextInt (30) - 10));
            default:
            {
                juce::String text;

                for (int c = random.nextInt (9); --c >= 0;)
                    text += juce::String::charToString ((juce::juce_wchar) juce::String ("0123456789.-+eE x,0")[random.nextInt (19)]);

                return text;
            }
        }
    };

    for (int i = 0; i < 200000; ++i)
    {
        const auto text = make();
        const auto isTaken = isNumberWrittenAsPhazite (text);
        taken += isTaken ? 1 : 0;
        takenDecimals += isTaken && text.containsChar ('.') ? 1 : 0;

        if (! holds (text))
            FAIL_CHECK ("taken, but Phazite writes it otherwise: \"" << text << "\"");
    }

    // Both kinds taken many times, so that each rule is tried: the whole numbers', and the decimals'.
    INFO ("taken: " << taken << ", of which decimals " << takenDecimals);
    CHECK (taken - takenDecimals > 20000);
    CHECK (takenDecimals > 10000);
}

TEST_CASE ("Bytes are written in standard base 64, and text that isn't base 64 reads as nothing")
{
    const juce::MemoryBlock bytes ("Phazite\0\xff", 9);
    const auto text = phazite::writeBytes (bytes);
    CHECK (text == "UGhheml0ZQD/");

    const auto read = phazite::readBytes (text);
    REQUIRE (read.has_value());
    CHECK (*read == bytes);

    CHECK_FALSE (phazite::readBytes ("not base 64!").has_value());
    CHECK (phazite::readBytes ({}).has_value());
}

TEST_CASE ("Text in XML: entities for what XML marks, references for tabs and line ends, the rest as UTF-8")
{
    CHECK (escaped ("a & b < c > d \"e\"") == "a &amp; b &lt; c &gt; d &quot;e&quot;");
    CHECK (escaped ("one\ttwo\nthree\rfour") == "one&#9;two&#10;three&#13;four");
    CHECK (escaped (juce::String::fromUTF8 ("Caf\xc3\xa9 \xe2\x86\x92 \xf0\x9f\x8e\xb5"))
           == juce::String::fromUTF8 ("Caf\xc3\xa9 \xe2\x86\x92 \xf0\x9f\x8e\xb5"));
    CHECK (escaped (juce::String ("x") + juce::String::charToString (2) + "y") == "xy");

    // What a strict reader takes: JUCE's reads it back as it was.
    const juce::ValueTree element ("TRACK", { { "name", juce::String::fromUTF8 ("Caf\xc3\xa9 & \"friends\"\nline 2") } });
    const auto xml = juce::parseXML (projectText (element));
    REQUIRE (xml != nullptr);
    CHECK (xml->getStringAttribute ("name") == element["name"].toString());
}

TEST_CASE ("A project file's text: a property on each line, in the order of their names, under the first")
{
    const juce::ValueTree song (
        "PHAZITE", { { "nextID", 5 }, { "loopStart", 0.0 } },
        { juce::ValueTree ("TRACKS", {}, { juce::ValueTree ("TRACK", { { "kind", "track" }, { "id", 3 }, { "volume", -6.0f } }) }),
          juce::ValueTree ("TEMPOMAP") });

    CHECK (projectText (song)
           == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "\n"
              "<PHAZITE loopStart=\"0\"\n"
              "         nextID=\"5\">\n"
              "  <TRACKS>\n"
              "    <TRACK id=\"3\"\n"
              "           kind=\"track\"\n"
              "           volume=\"-6\"/>\n"
              "  </TRACKS>\n"
              "  <TEMPOMAP/>\n"
              "</PHAZITE>\n");
}

TEST_CASE ("Every choice of Phazite's devices and modulators is saved by a name of its own, and read back as the same choice")
{
    std::vector<phazite::devices::ParameterInfo> choices;

    for (const auto* device : phazite::getOwnDevices())
        for (const auto& parameter : device->parameters)
            if (parameter.unit == phazite::devices::Unit::choice)
                choices.push_back (parameter);

    for (const auto kind : { phazite::devices::ModulatorKind::lfo, phazite::devices::ModulatorKind::envelopeFollower,
                             phazite::devices::ModulatorKind::steps, phazite::devices::ModulatorKind::random,
                             phazite::devices::ModulatorKind::breakpoints, phazite::devices::ModulatorKind::noteFollower })
        for (const auto& parameter : phazite::devices::getModulatorParameters (kind))
            if (parameter.unit == phazite::devices::Unit::choice)
                choices.push_back (parameter);

    REQUIRE (choices.size() > 10);

    for (const auto& parameter : choices)
    {
        INFO (std::string (parameter.id));
        CHECK (phazite::devices::isValid (parameter));

        for (int i = 0; i < (int) parameter.choices.size(); ++i)
        {
            const auto value = parameter.minimum + (float) i;
            juce::ValueTree element ("DEVICE");
            phazite::test::writeValueSetByHand (element, parameter, value);
            const auto saved = element.getChildWithName (
                phazite::schema::VALUES)[juce::Identifier (juce::String (parameter.id.data(), parameter.id.size()))];

            // Saved as text, the choice's name, unless it is the default, which isn't saved at all.
            if (! phazite::isDefaultValue (parameter, value))
                CHECK (saved.toString()
                       == juce::String::fromUTF8 (phazite::devices::getChoiceName (parameter, i).data(),
                                                  (int) phazite::devices::getChoiceName (parameter, i).size()));

            CHECK (phazite::readDeviceParameterValue (element, parameter) == doctest::Approx (value));
        }
    }
}
