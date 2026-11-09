// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongText.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

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

    juce::String writeNumber (double number)
    {
        if (! std::isfinite (number))
            return std::isnan (number) ? "nan" : (number > 0 ? "inf" : "-inf");

        // A whole number in its digits (as far as a double holds whole numbers exactly).
        if (juce::exactlyEqual (number, std::trunc (number)) && std::abs (number) < 1.0e15)
            return juce::String ((juce::int64) number);

        // A float's value in the fewest digits that read back as that float; any other in 15 significant digits, as C's
        // "%.15g" writes it, so that what arithmetic leaves past them ("8.000000000000004") is no change.
        char text[48];
        const auto isFloats =
            std::abs (number) <= (double) std::numeric_limits<float>::max() && juce::exactlyEqual ((double) (float) number, number);
        const auto written = isFloats ? std::to_chars (text, text + sizeof (text), (float) number)
                                      : std::to_chars (text, text + sizeof (text), number, std::chars_format::general, 15);
        return juce::String (text, (size_t) (written.ptr - text));
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
    if (value.isBool())
        return (bool) value ? "1" : "0";

    if (value.isInt() || value.isInt64())
        return juce::String ((juce::int64) value);

    if (value.isDouble())
        return writeNumber ((double) value);

    return withoutForbidden (value.toString());
}

bool isSameValue (const juce::var& a, const juce::var& b)
{
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
