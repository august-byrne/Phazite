// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongText.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

namespace phazite
{
namespace
{
    // What XML 1.0 can't hold, not even as a character reference.
    bool isForbiddenInXml (juce::juce_wchar c) noexcept
    {
        return (c < 0x20 && c != 0x09 && c != 0x0a && c != 0x0d) || c == 0xfffe || c == 0xffff;
    }

    juce::String withoutForbidden (const juce::String& text)
    {
        for (auto p = text.getCharPointer(); ! p.isEmpty();)
        {
            if (isForbiddenInXml (p.getAndAdvance()))
            {
                juce::String kept;

                for (auto q = text.getCharPointer(); ! q.isEmpty();)
                    if (const auto c = q.getAndAdvance(); ! isForbiddenInXml (c))
                        kept += juce::String::charToString (c);

                return kept;
            }
        }

        return text;
    }

    bool holdsForbidden (const juce::String& text)
    {
        for (auto p = text.getCharPointer(); ! p.isEmpty();)
            if (isForbiddenInXml (p.getAndAdvance()))
                return true;

        return false;
    }

    // A number's text, into a buffer of the caller's (no String made: isSameValue() compares many).
    using NumberText = std::array<char, 48>;

    std::string_view writeNumber (double number, NumberText& text)
    {
        if (! std::isfinite (number))
            return std::isnan (number) ? "nan" : (number > 0 ? "inf" : "-inf");

        const auto begin = text.data(), end = text.data() + text.size();

        // A whole number in its digits (as far as a double holds whole numbers exactly).
        if (juce::exactlyEqual (number, std::trunc (number)) && std::abs (number) < 1.0e15)
            return { begin, (size_t) (std::to_chars (begin, end, (juce::int64) number).ptr - begin) };

        // A float's value in the fewest digits that read back as that float; any other in 15 significant digits, as C's
        // "%.15g" writes it, so that what arithmetic leaves past them ("8.000000000000004") is no change.
        const auto isFloats =
            std::abs (number) <= (double) std::numeric_limits<float>::max() && juce::exactlyEqual ((double) (float) number, number);
        const auto written =
            isFloats ? std::to_chars (begin, end, (float) number) : std::to_chars (begin, end, number, std::chars_format::general, 15);
        return { begin, (size_t) (written.ptr - begin) };
    }

    // The text writeValue() writes for a switch or a number, into a buffer of the caller's; nothing for any other value.
    std::optional<std::string_view> writeNumberOrSwitch (const juce::var& value, NumberText& text)
    {
        if (value.isBool())
            return (bool) value ? "1" : "0";

        if (value.isInt() || value.isInt64())
            return std::string_view { text.data(), (size_t) (std::to_chars (text.data(), text.data() + text.size(), (juce::int64) value).ptr
                                                             - text.data()) };

        if (value.isDouble())
            return writeNumber ((double) value, text);

        return std::nullopt;
    }

    void writeSpaces (juce::OutputStream& out, int count)
    {
        for (int i = 0; i < count; ++i)
            out.writeByte (' ');
    }

    void writeIndented (juce::OutputStream& out, const juce::ValueTree& element, int indent)
    {
        auto properties = getPropertyTexts (element);
        sortByName (properties);

        const auto type = element.getType().toString();
        writeSpaces (out, indent);
        out << '<' << type;

        // Each property on its line: the first beside the type, the others under it.
        for (size_t i = 0; i < properties.size(); ++i)
        {
            if (i == 0)
            {
                out << ' ';
            }
            else
            {
                out << '\n';
                writeSpaces (out, indent + 1 + type.length() + 1);
            }

            out << properties[i].first << "=\"";
            writeEscaped (out, properties[i].second);
            out << '"';
        }

        if (element.getNumChildren() == 0)
        {
            out << "/>\n";
            return;
        }

        out << ">\n";

        for (const auto& child : element)
            writeIndented (out, child, indent + 2);

        writeSpaces (out, indent);
        out << "</" << type << ">\n";
    }
} // namespace

juce::String writeValue (const juce::var& value)
{
    NumberText text;

    if (const auto written = writeNumberOrSwitch (value, text))
        return juce::String (written->data(), written->size());

    return withoutForbidden (value.toString());
}

bool isSameValue (const juce::var& a, const juce::var& b)
{
    // What writeValue() writes, compared without making a String of either: the comparison with a project's saved copy and
    // the loader's checks compare every number of a song (a String each made them 30 times slower).
    if (a.isDouble() && b.isDouble() && juce::exactlyEqual ((double) a, (double) b))
        return true; // the same number writes the same text (0 and -0 too); others may yet, past 15 digits

    NumberText bufferA, bufferB;
    const auto numberA = writeNumberOrSwitch (a, bufferA), numberB = writeNumberOrSwitch (b, bufferB);

    if (numberA.has_value() && numberB.has_value())
        return *numberA == *numberB;

    if (numberA.has_value() || numberB.has_value())
    {
        // A number and text: the text as it is, unless it holds what writeValue() leaves out.
        if (const auto text = (numberA.has_value() ? b : a).toString(); ! holdsForbidden (text))
        {
            const auto number = numberA.has_value() ? *numberA : *numberB;
            return text.getNumBytesAsUTF8() == number.size() && std::memcmp (text.toRawUTF8(), number.data(), number.size()) == 0;
        }
    }
    else if (const auto textA = a.toString(), textB = b.toString(); textA == textB)
    {
        return true;
    }
    else if (! holdsForbidden (textA) && ! holdsForbidden (textB))
    {
        return false;
    }

    return writeValue (a) == writeValue (b);
}

juce::String writeBytes (const juce::MemoryBlock& bytes)
{
    return juce::Base64::toBase64 (bytes.getData(), bytes.getSize());
}

std::optional<juce::MemoryBlock> readBytes (const juce::String& text)
{
    juce::MemoryOutputStream bytes;

    if (! juce::Base64::convertFromBase64 (bytes, text))
        return std::nullopt;

    return bytes.getMemoryBlock();
}

void writeEscaped (juce::OutputStream& out, const juce::String& text)
{
    for (auto p = text.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();

        switch (c)
        {
            case '&':
                out << "&amp;";
                break;
            case '<':
                out << "&lt;";
                break;
            case '>':
                out << "&gt;";
                break;
            case '"':
                out << "&quot;";
                break;
            case '\t':
                out << "&#9;";
                break;
            case '\n':
                out << "&#10;";
                break;
            case '\r':
                out << "&#13;";
                break;
            default:
                if (! isForbiddenInXml (c))
                {
                    char utf8[8] = {};
                    juce::CharPointer_UTF8 end (utf8);
                    end.write (c);
                    out.write (utf8, (size_t) (end.getAddress() - utf8));
                }

                break;
        }
    }
}

PropertyTexts getPropertyTexts (const juce::ValueTree& element)
{
    PropertyTexts properties;
    properties.reserve ((size_t) element.getNumProperties());

    for (int i = 0; i < element.getNumProperties(); ++i)
    {
        const auto name = element.getPropertyName (i);
        properties.emplace_back (name.toString(), writeValue (element[name]));
    }

    return properties;
}

void sortByName (PropertyTexts& properties)
{
    std::sort (properties.begin(), properties.end(), [] (const auto& a, const auto& b) { return a.first.compare (b.first) < 0; });
}

void writeStartTag (juce::OutputStream& out, const juce::String& type, const PropertyTexts& sortedProperties)
{
    out << '<' << type;

    for (const auto& [name, value] : sortedProperties)
    {
        out << ' ' << name << "=\"";
        writeEscaped (out, value);
        out << '"';
    }
}

void writeProjectText (juce::OutputStream& out, const juce::ValueTree& element)
{
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n";
    writeIndented (out, element, 0);
}
} // namespace phazite
