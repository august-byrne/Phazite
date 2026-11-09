// SPDX-License-Identifier: AGPL-3.0-or-later
// How Phazite writes a song as text (model/base/internal/SongText.h; docs/project-format.md): one text for each value, the
// same in a project file and in a version's pieces; bytes in standard base 64; and XML that any XML reader takes.
#include "TestHelpers.h"
#include "devices/modulation/Modulators.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"

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
            phazite::keepValueSetByHand (element, parameter, value, nullptr);
            const auto saved = element.getChildWithName (
                phazite::schema::VALUES)[juce::Identifier (juce::String (parameter.id.data(), parameter.id.size()))];

            // Saved as text, the choice's name, unless it is the default, which isn't saved at all.
            if (! phazite::isDefaultValue (parameter, value))
                CHECK (saved.toString()
                       == juce::String::fromUTF8 (phazite::devices::getChoiceName (parameter, i).data(),
                                                  (int) phazite::devices::getChoiceName (parameter, i).size()));

            CHECK (phazite::readValueSetByHand (element, parameter) == doctest::Approx (value));
        }
    }
}
