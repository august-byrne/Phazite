// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Fuzzer.h"
#include "GoldenFiles.h"

#include <array>
#include <cstring>
#include <limits>

namespace phazite::test::fuzz
{
namespace
{
    // Every element of an XML document, the root first.
    void collect (juce::XmlElement& element, std::vector<std::pair<juce::XmlElement*, juce::XmlElement*>>& found, juce::XmlElement* parent)
    {
        found.push_back ({ &element, parent });

        for (auto* child : element.getChildIterator())
            if (! child->isTextElement())
                collect (*child, found, &element);
    }

    // Every object and array in a JSON value, by reference.
    void collect (const juce::var& value, std::vector<juce::var>& found)
    {
        if (auto* object = value.getDynamicObject())
        {
            found.push_back (value);

            for (const auto& property : object->getProperties())
                collect (property.value, found);
        }
        else if (const auto* array = value.getArray())
        {
            found.push_back (value);

            for (const auto& item : *array)
                collect (item, found);
        }
    }

    juce::String getEnvironment (const char* name)
    {
        return juce::SystemStats::getEnvironmentVariable (name, {});
    }
} // namespace

juce::MemoryBlock toBlock (const juce::String& text)
{
    return juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8());
}

juce::String Mutator::makeExtremeNumber()
{
    static const char* const numbers[] = { "0",
                                           "-1",
                                           "-0",
                                           "1e308",
                                           "-1e308",
                                           "1e-320",
                                           "1e999",
                                           "nan",
                                           "inf",
                                           "-inf",
                                           "NaN",
                                           "Infinity",
                                           "2147483647",
                                           "2147483648",
                                           "-2147483649",
                                           "4294967296",
                                           "9223372036854775807",
                                           "-9223372036854775808",
                                           "18446744073709551615",
                                           "99999999999999999999999999",
                                           "1e15",
                                           "0.5",
                                           "127.5",
                                           "128",
                                           "300.01",
                                           "19.99",
                                           "0x10" };
    return numbers[random.nextInt ((int) std::size (numbers))];
}

juce::String Mutator::makeAwkwardText()
{
    switch (random.nextInt (14))
    {
        case 0:
            return juce::String::repeatedString ("A", 70000);
        case 1:
            return "../../../../../../../../Windows/win.ini";
        case 2:
            return "..\\..\\..\\..\\outside.wav";
        case 3:
            return "C:\\Windows\\win.ini";
        case 4:
            return "\\\\fuzz.invalid\\share\\kick.wav";
        case 5:
            return "//fuzz.invalid/share/kick.wav";
        case 6:
            return "/etc/passwd";
        case 7:
            return "Audio/../../outside.wav";
        case 8:
            return juce::String::charToString (1) + juce::String::charToString (0xfffe) + "x";
        case 9:
            return {};
        case 10:
            return "%n%s%s%s{0}";
        case 11:
            return juce::String::fromUTF8 ("\xe2\x80\xae\xf0\x9f\x8e\xb9 \xd7\xa9");
        case 12:
            return "file:///etc/passwd";
        default:
            return " ";
    }
}

juce::MemoryBlock Mutator::mutateBytes (const juce::MemoryBlock& input)
{
    juce::MemoryBlock bytes (input);
    const auto size = (int) bytes.getSize();
    auto* data = static_cast<juce::uint8*> (bytes.getData());
    const auto place = [this, size] { return size > 0 ? random.nextInt (size) : 0; };

    switch (random.nextInt (8))
    {
        case 0: // a bit flipped
            if (size > 0)
                data[place()] ^= (juce::uint8) (1 << random.nextInt (8));
            break;

        case 1: // cut short
            bytes.setSize ((size_t) place());
            break;

        case 2: // a part taken out
        {
            const auto from = place(), length = juce::jmin (size - from, 1 + random.nextInt (64));
            bytes.removeSection ((size_t) from, (size_t) length);
            break;
        }

        case 3: // a part repeated
        {
            const auto from = place(), length = juce::jmin (size - from, 1 + random.nextInt (256));
            const juce::MemoryBlock part (data + from, (size_t) length);

            for (int i = 1 + random.nextInt (4); --i >= 0;)
                bytes.insert (part.getData(), part.getSize(), (size_t) from);
            break;
        }

        case 4: // random bytes in
        {
            juce::MemoryBlock noise ((size_t) (1 + random.nextInt (32)));

            for (size_t i = 0; i < noise.getSize(); ++i)
                noise[i] = (char) random.nextInt (256);

            bytes.insert (noise.getData(), noise.getSize(), (size_t) place());
            break;
        }

        default: // a number at an edge, as an integer or a double, either way round
        {
            static const juce::uint64 edges[] = { 0,
                                                  0xffffffffffffffffull,
                                                  0x7fffffffffffffffull,
                                                  0x8000000000000000ull,
                                                  0x7fffffffull,
                                                  0x80000000ull,
                                                  0xffffffffull,
                                                  0x7ff8000000000000ull, // not a number
                                                  0x7ff0000000000000ull, // infinity
                                                  0x000000007fffffffull,
                                                  0x0000000100000000ull };
            const auto width = std::array<int, 4> { 1, 2, 4, 8 }[(size_t) random.nextInt (4)];
            auto edge = edges[random.nextInt ((int) std::size (edges))];

            if (random.nextBool())
                edge = juce::ByteOrder::swap (edge);

            if (size >= width)
            {
                const auto at = random.nextInt (size - width + 1);
                std::memcpy (data + at, &edge, (size_t) width);
            }

            break;
        }
    }

    return bytes;
}

juce::String Mutator::mutateXml (const juce::String& text)
{
    auto xml = juce::parseXML (text);

    if (xml == nullptr)
        return text;

    std::vector<std::pair<juce::XmlElement*, juce::XmlElement*>> elements; // each with its parent
    collect (*xml, elements, nullptr);
    const auto pick = [&] { return elements[(size_t) random.nextInt ((int) elements.size())]; };
    auto [element, parent] = pick();

    // An attribute of the element, and another element's attribute's value (an ID, often).
    const auto pickAttribute = [&] (juce::XmlElement& e) -> juce::String
    { return e.getNumAttributes() > 0 ? e.getAttributeName (random.nextInt (e.getNumAttributes())) : juce::String ("id"); };

    switch (random.nextInt (10))
    {
        case 0:
        case 1: // a number at an edge, or text where a number goes
            element->setAttribute (pickAttribute (*element), random.nextInt (3) > 0 ? makeExtremeNumber() : makeAwkwardText());
            break;

        case 2: // another element's value: an ID twice, or one that names something else
        {
            auto* other = pick().first;

            if (other->getNumAttributes() > 0)
            {
                const auto index = random.nextInt (other->getNumAttributes());
                element->setAttribute (pickAttribute (*element), other->getAttributeValue (index));
            }
            break;
        }

        case 3: // an element twice, or many times
            if (parent != nullptr)
                for (int i = random.nextInt (8) == 0 ? 200 : 1; --i >= 0;)
                    parent->addChildElement (new juce::XmlElement (*element));
            break;

        case 4: // an element taken out
            if (parent != nullptr)
                parent->removeChildElement (element, true);
            break;

        case 5: // an element moved where it doesn't belong
            if (auto* target = pick().first;
                parent != nullptr && target != element && target != parent && element->findParentElementOf (target) == nullptr)
            {
                parent->removeChildElement (element, false);
                target->addChildElement (element);
            }
            break;

        case 6: // an element of a kind nothing knows, or of a kind put somewhere else
            element->addChildElement (new juce::XmlElement (random.nextBool() ? juce::String ("FUZZ") : pick().first->getTagName()));
            break;

        case 7: // an attribute taken away
            if (element->getNumAttributes() > 0)
                element->removeAttribute (pickAttribute (*element));
            break;

        case 8: // nested deep: elements of its kind, one in another
        {
            auto* inner = element;

            for (int i = 0; i < 1 + random.nextInt (300); ++i)
                inner = inner->createNewChildElement (element->getTagName());
            break;
        }

        default: // its children in another order
            if (element->getNumChildElements() > 1)
            {
                auto* first = element->getFirstChildElement();
                element->removeChildElement (first, false);
                element->addChildElement (first);
            }
            break;
    }

    return xml->toString();
}

juce::String Mutator::mutateJson (const juce::String& text)
{
    auto json = juce::JSON::parse (text);
    std::vector<juce::var> containers;
    collect (json, containers);

    if (containers.empty())
        return text;

    auto container = containers[(size_t) random.nextInt ((int) containers.size())];

    const auto makeValue = [this]() -> juce::var
    {
        switch (random.nextInt (7))
        {
            case 0:
                return makeExtremeNumber().getDoubleValue();
            case 1:
                return makeAwkwardText();
            case 2:
                return random.nextBool() ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
            case 3:
                return juce::var (juce::Array<juce::var> { 1, "two", juce::var() });
            case 4:
                return juce::var (new juce::DynamicObject());
            case 5:
                return random.nextBool();
            default:
                return (juce::int64) std::numeric_limits<juce::int64>::max();
        }
    };

    if (auto* object = container.getDynamicObject())
    {
        auto& properties = object->getProperties();
        const auto name = properties.size() > 0 ? properties.getName (random.nextInt (properties.size())) : juce::Identifier ("fuzz");

        switch (random.nextInt (4))
        {
            case 0:
                object->removeProperty (name);
                break;

            case 1: // nested deep
            {
                juce::var deep = 1;

                for (int i = 1 + random.nextInt (300); --i >= 0;)
                    deep = juce::var (juce::Array<juce::var> { deep });

                object->setProperty (name, deep);
                break;
            }

            default:
                object->setProperty (name, makeValue());
                break;
        }
    }
    else if (auto* array = container.getArray())
    {
        const auto index = array->isEmpty() ? 0 : random.nextInt (array->size());

        switch (random.nextInt (4))
        {
            case 0:
                if (! array->isEmpty())
                    array->remove (index);
                break;

            case 1: // an item many times
                if (! array->isEmpty())
                    for (int i = random.nextInt (8) == 0 ? 500 : 2; --i >= 0;)
                    {
                        const auto item = array->getReference (index); // a copy: the array may move what it holds as it grows
                        array->add (item);
                    }
                break;

            default:
                if (array->isEmpty())
                    array->add (makeValue());
                else
                    array->set (index, makeValue());
                break;
        }
    }

    return juce::JSON::toString (json);
}

juce::MemoryBlock Mutator::mutate (const juce::MemoryBlock& input, Shape shape)
{
    auto output = input;

    for (int i = 1 + random.nextInt (3); --i >= 0;)
    {
        // Text damaged as text two times in three, the bytes the rest, so that both parsers and what is behind them see it.
        if (const auto isText = juce::CharPointer_UTF8::isValidString (static_cast<const char*> (output.getData()), (int) output.getSize());
            shape != Shape::bytes && isText && random.nextInt (3) > 0)
        {
            const auto text = output.toString();
            output = toBlock (shape == Shape::xml ? mutateXml (text) : mutateJson (text));
        }
        else
        {
            output = mutateBytes (output);
        }
    }

    return output;
}

int getRounds (int ownRounds)
{
    const auto text = getEnvironment ("PHAZITE_FUZZ_ROUNDS");
    return text.isNotEmpty() ? juce::jmax (1, text.getIntValue()) : ownRounds;
}

std::string joinProblems (const std::vector<std::string>& problems)
{
    std::string joined;

    for (size_t i = 0; i < problems.size() && i < 4; ++i)
        joined += (i > 0 ? "; " : "") + problems[i];

    if (problems.size() > 4)
        joined += "; and " + std::to_string (problems.size() - 4) + " more";

    return joined;
}

namespace
{
    // What JUCE's own readers assert against in a damaged file, in Debug builds, and go on from: a name its XML parser read
    // that XML allows no element or attribute, a tag like the one asked for but for its case (its lookups of a child by
    // name ask that way), and in a zip a date that is none and a file's name that isn't UTF-8. A damaged file is nothing
    // Phazite did wrong, and what its readers make of one is what the cases check (open question Q7,
    // docs/juce-workarounds.md).
    constexpr const char* readersAssertions =
        "juce_XmlElement.cpp:76|juce_XmlElement.cpp:112|juce_XmlElement.cpp:489|juce_Time.cpp:170-175|juce_String.cpp:2143";

    // And, in JSON, an object's member with no name, which its parser makes an Identifier of, and a minus sign with no
    // digit after it.
    constexpr const char* jsonReadersAssertions =
        "juce_XmlElement.cpp:76|juce_XmlElement.cpp:112|juce_XmlElement.cpp:489|juce_Time.cpp:170-175|juce_String.cpp:2143|"
        "juce_Identifier.cpp:61|juce_JSON.cpp:268";

    Totals runRoundsExpecting (const char* assertions, const juce::String& name, int ownRounds, const Round& round);
} // namespace

Totals runRounds (const juce::String& name, int ownRounds, const Round& round)
{
    return runRoundsExpecting (readersAssertions, name, ownRounds, round);
}

namespace
{
    Totals runRoundsExpecting (const char* assertions, const juce::String& name, int ownRounds, const Round& round)
    {
        const ExpectedAssertion ofReaders (assertions);
        const auto seed = name.hashCode64();
        const auto rounds = getRounds (ownRounds);
        const auto only = getEnvironment ("PHAZITE_FUZZ_ROUND");
        const auto first = only.isNotEmpty() ? only.getIntValue() : 0;
        const auto end = only.isNotEmpty() ? first + 1 : rounds;
        Totals totals;

        for (int r = first; r < end; ++r)
        {
            Mutator mutator (seed + r);
            juce::MemoryBlock fed;
            const auto problem = round (mutator, fed);
            ++totals.rounds;

            if (problem.empty())
                continue;

            ++totals.failures;
            const auto saved = getActualFolder().getChildFile ("fuzz").getChildFile (juce::File::createLegalFileName (name) + " round "
                                                                                     + juce::String (r) + ".bin");
            saved.getParentDirectory().createDirectory();
            saved.replaceWithData (fed.getData(), fed.getSize());
            INFO ("fuzz case \"" << name << "\", seed " << seed << ", round " << r << " (PHAZITE_FUZZ_ROUND=" << r
                                 << " runs it alone; what it fed is in " << saved.getFullPathName() << "): " << problem);
            CHECK (false);

            if (totals.failures >= 5)
                break; // enough to go on with
        }

        return totals;
    }
} // namespace

Totals run (const juce::String& name, const std::vector<juce::MemoryBlock>& seeds, Shape shape, int ownRounds, const Reader& read)
{
    REQUIRE (! seeds.empty());

    // Each seed as it is first: a valid input must hold too.
    for (size_t i = 0; i < seeds.size() && getEnvironment ("PHAZITE_FUZZ_ROUND").isEmpty(); ++i)
    {
        const auto problem = read (seeds[i]);
        INFO ("fuzz case \"" << name << "\": seed input " << (int) i << " as it is: " << problem);
        CHECK (problem.empty());
    }

    return runRoundsExpecting (shape == Shape::json ? jsonReadersAssertions : readersAssertions, name, ownRounds,
                               [&] (Mutator& mutator, juce::MemoryBlock& fed)
                               {
                                   const auto& input = seeds[(size_t) mutator.random.nextInt ((int) seeds.size())];
                                   fed = mutator.mutate (input, shape);
                                   return read (fed);
                               });
}
} // namespace phazite::test::fuzz
