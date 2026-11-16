// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Json.h"

#include <cmath>
#include <limits>
#include <string_view>

namespace phazite
{
namespace
{
    // The most digits of an exponent that always fit an int as JUCE counts them (CharacterFunctions::readDoubleValue(),
    // JSONParser::parseNumber()).
    constexpr int maxExponentDigits = 9;

    bool isExponentMark (juce::juce_wchar character)
    {
        return character == 'e' || character == 'E';
    }

    // A number at the text's start as CharacterFunctions::readDoubleValue() reads it: its sign, and whether it has an
    // exponent that reader would overflow (hasExponentTooLongToRead()), with the exponent's sign and where it ends.
    struct ReadExponent
    {
        bool tooLong = false, negative = false, exponentNegative = false;
        juce::String::CharPointerType end;
    };

    ReadExponent readExponent (const juce::String& text)
    {
        ReadExponent read { false, false, false, text.getCharPointer() };
        auto t = text.getCharPointer().findEndOfWhitespace();

        if (*t == '-' || *t == '+')
            read.negative = t.getAndAdvance() == '-';

        // Its digits, with a point among them, as the reader's isDigit() takes them: a juce::String's UTF-8 bytes, the ten alone.
        auto digits = false, point = false;

        for (;;)
        {
            if (t.isDigit())
                digits = true;
            else if (*t == '.' && ! point)
                point = true;
            else
                break;

            ++t;
        }

        if (! digits || ! isExponentMark (*t))
            return read;

        ++t;

        if (*t == '-' || *t == '+')
            read.exponentNegative = t.getAndAdvance() == '-';

        // The reader leaves out the zeros it starts with, and counts each other digit in.
        auto significant = 0;

        while (t.isDigit())
        {
            if (t.getAndAdvance() != '0' || significant > 0)
                ++significant;
        }

        read.tooLong = read.tooLong || significant > maxExponentDigits;
        read.end = t;
        return read;
    }

    // Whether the text has, outside its strings, a number JUCE's parser would overflow as it counts its digits: the digits
    // a number starts with, where they make more than the largest whole number of 64 bits (the parser counts them before
    // it meets a point or an exponent, so those of 1234567890123456789012.5 too), or an exponent of more digits than
    // always fit an int. The digits after a point are never counted. Also a digit that isn't one of the ten, which JSON
    // has none of outside a string, and which the parser's reader of exponents would count as a number far past nine.
    bool hasNumberTooLongToCount (const juce::String& text)
    {
        constexpr std::string_view largest = "9223372036854775807";

        enum class Part
        {
            whole,
            fraction,
            exponent
        };

        auto part = Part::whole;
        char run[32] = {};
        int digits = 0;
        bool digitsBeforePoint = false;
        juce::juce_wchar quote = 0, last = 0, lastButOne = 0;

        for (auto t = text.getCharPointer(); ! t.isEmpty();)
        {
            const auto c = t.getAndAdvance();

            if (quote != 0)
            {
                // In a string, as the parser reads one: to its quote, a backslash taking what follows it along.
                if (c == '\\' && ! t.isEmpty())
                    ++t;
                else if (c == quote)
                    quote = 0;

                continue;
            }

            if (c >= '0' && c <= '9')
            {
                if (digits == 0)
                {
                    if (last == '.' && digitsBeforePoint)
                        part = Part::fraction;
                    else if (isExponentMark (last) || ((last == '+' || last == '-') && isExponentMark (lastButOne)))
                        part = Part::exponent;
                    else
                        part = Part::whole;
                }

                if (part == Part::whole)
                {
                    if (digits >= (int) largest.size())
                        return true;

                    run[digits] = (char) c;

                    if (digits + 1 == (int) largest.size() && std::string_view (run, largest.size()) > largest)
                        return true;
                }
                else if (part == Part::exponent && digits >= maxExponentDigits)
                {
                    return true;
                }

                ++digits;
            }
            else
            {
                if (c > 127 && juce::CharacterFunctions::isDigit (c))
                    return true;

                digitsBeforePoint = c == '.' && digits > 0;
                digits = 0;

                if (c == '"' || c == '\'')
                    quote = c;
            }

            lastButOne = last;
            last = c;
        }

        return false;
    }
} // namespace

juce::var parseJson (const juce::String& text)
{
    return hasNumberTooLongToCount (text) ? juce::var() : juce::JSON::parse (text);
}

juce::var parseJson (const juce::File& file)
{
    return parseJson (file.loadFileAsString());
}

bool hasExponentTooLongToRead (const juce::String& text)
{
    return readExponent (text).tooLong;
}

juce::String makeExponentReadable (const juce::String& text)
{
    const auto read = readExponent (text);

    if (! read.tooLong)
        return text;

    return juce::String (read.negative ? "-" : "") + (read.exponentNegative ? "0" : "1e999") + juce::String (read.end);
}

std::optional<double> readFiniteDouble (const juce::String& text)
{
    const auto value = makeExponentReadable (text).getDoubleValue();
    return std::isfinite (value) ? std::optional (value) : std::nullopt;
}

double readDouble (const juce::String& text, double otherwise)
{
    return readFiniteDouble (text).value_or (otherwise);
}

float readFloat (const juce::String& text, float otherwise)
{
    constexpr auto largest = (double) std::numeric_limits<float>::max();
    const auto value = readFiniteDouble (text);
    return value.has_value() ? (float) juce::jlimit (-largest, largest, *value) : otherwise;
}
} // namespace phazite
