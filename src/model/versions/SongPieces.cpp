// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongPieces.h"
#include "model/base/internal/Schema.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    namespace versions = schema::versions;

    // A list of a clip's notes or a curve's points this long or longer is in runs, each ending after about one in
    // averageRun of them: after one whose text's FNV-1a hash is a multiple of averageRun. Where a run ends depends on what
    // it holds alone, so that a note added, changed or taken away changes its own run, and the runs after it stay.
    constexpr int fewestInRuns = 128;
    constexpr std::uint64_t averageRun = 64;

    std::uint64_t hashForRuns (const char* data, size_t size) noexcept
    {
        std::uint64_t hash = 14695981039346656037ull; // FNV-1a, 64 bits: the same on every computer

        for (size_t i = 0; i < size; ++i)
        {
            hash ^= (std::uint8_t) data[i];
            hash *= 1099511628211ull;
        }

        return hash;
    }

    bool isPieceOfItsOwn (const juce::ValueTree& element)
    {
        for (const auto* type : { &schema::TRACK, &schema::MASTER, &schema::CLIP, &schema::DEVICE, &schema::CONTAINER, &schema::CURVE })
            if (element.hasType (*type))
                return true;

        return false;
    }

    bool goesInRuns (const juce::ValueTree& child)
    {
        return child.hasType (schema::NOTE) || child.hasType (schema::POINT);
    }

    void writeEscaped (juce::MemoryOutputStream& out, const juce::String& text)
    {
        // As XML, every character that isn't one of these, with characters below 32 written as numbers too, so that a value's
        // new lines and tabs come back.
        for (auto* p = text.toRawUTF8(); *p != 0; ++p)
        {
            const auto c = (unsigned char) *p;

            if (c == '&')
                out << "&amp;";
            else if (c == '<')
                out << "&lt;";
            else if (c == '>')
                out << "&gt;";
            else if (c == '"')
                out << "&quot;";
            else if (c < 32)
                out << "&#" << (int) c << ';';
            else
                out.writeByte ((char) c);
        }
    }

    // Its properties, in the order of their names, with what replaces some of them.
    using Properties = std::vector<std::pair<juce::String, juce::String>>;

    Properties getProperties (const juce::ValueTree& element)
    {
        Properties properties;
        properties.reserve ((size_t) element.getNumProperties());

        for (int i = 0; i < element.getNumProperties(); ++i)
        {
            const auto name = element.getPropertyName (i);
            properties.emplace_back (name.toString(), element[name].toString());
        }

        return properties;
    }

    void writeStart (juce::MemoryOutputStream& out, const juce::Identifier& type, Properties properties)
    {
        std::sort (properties.begin(), properties.end(), [] (const auto& a, const auto& b) { return a.first.compare (b.first) < 0; });
        out << '<' << type.toString();

        for (const auto& [name, value] : properties)
        {
            out << ' ' << name << "=\"";
            writeEscaped (out, value);
            out << '"';
        }
    }

    void writePlainElement (juce::MemoryOutputStream& out, const juce::ValueTree& element)
    {
        writeStart (out, element.getType(), getProperties (element));

        if (element.getNumChildren() == 0)
        {
            out << "/>";
            return;
        }

        out << '>';

        for (const auto& child : element)
            writePlainElement (out, child);

        out << "</" << element.getType().toString() << '>';
    }

    juce::MemoryBlock toBlock (juce::MemoryOutputStream& out)
    {
        return { out.getData(), out.getDataSize() };
    }

    //==============================================================================
    struct Splitter
    {
        SongPieces& result;
        const PluginParameterValues& parameters;

        juce::String keep (juce::MemoryBlock block)
        {
            auto name = getPieceName (block);
            result.pieces.emplace (name, std::move (block));
            return name;
        }

        void writeReference (juce::MemoryOutputStream& out, const juce::String& name)
        {
            out << '<' << versions::PIECE.toString() << ' ' << versions::piece.toString() << "=\"" << name << "\"/>";
        }

        // A plug-in's state, in its bytes (as long as they give the same text again), and its parameters' values.
        Properties getPieceProperties (const juce::ValueTree& element)
        {
            auto properties = getProperties (element);

            if (! element.hasType (schema::DEVICE))
                return properties;

            for (auto& property : properties)
            {
                if (property.first != schema::pluginState.toString())
                    continue;

                if (juce::MemoryBlock state; state.fromBase64Encoding (property.second) && state.toBase64Encoding() == property.second)
                    property = { versions::statePiece.toString(), keep (std::move (state)) };
            }

            const auto id = (std::uint64_t) element[schema::deviceID].toString().getLargeIntValue();

            if (const auto found = parameters.find (id); found != parameters.end() && ! found->second.empty())
            {
                juce::ValueTree values (versions::PARAMETERS);

                for (const auto& parameter : found->second)
                    values.appendChild (juce::ValueTree (versions::PARAMETER, { { versions::parameterID, parameter.id },
                                                                                { versions::parameterName, parameter.name },
                                                                                { versions::parameterValue, parameter.value } }),
                                        nullptr);

                properties.emplace_back (versions::parametersPiece.toString(), keep (writeElement (values)));
            }

            return properties;
        }

        // An element, which a piece of its own holds, as its piece: its name.
        juce::String split (const juce::ValueTree& element)
        {
            juce::MemoryOutputStream out;
            write (out, element);
            return keep (toBlock (out));
        }

        // An element into the piece being written: its children that are pieces of their own each a PIECE in their place,
        // and many notes or points in runs, each a piece.
        void write (juce::MemoryOutputStream& out, const juce::ValueTree& element)
        {
            writeStart (out, element.getType(), getPieceProperties (element));

            if (element.getNumChildren() == 0)
            {
                out << "/>";
                return;
            }

            out << '>';

            int numInRuns = 0;

            for (const auto& child : element)
                numInRuns += goesInRuns (child) ? 1 : 0;

            const auto inRuns = numInRuns >= fewestInRuns;
            juce::MemoryOutputStream run;
            bool runOpen = false;

            const auto endRun = [&]
            {
                if (! runOpen)
                    return;

                run << "</" << versions::RUN.toString() << '>';
                writeReference (out, keep (toBlock (run)));
                run.reset();
                runOpen = false;
            };

            for (const auto& child : element)
            {
                if (inRuns && goesInRuns (child))
                {
                    if (! runOpen)
                    {
                        run << '<' << versions::RUN.toString() << '>';
                        runOpen = true;
                    }

                    const auto start = run.getDataSize();
                    writePlainElement (run, child);
                    const auto* data = static_cast<const char*> (run.getData()) + start;

                    if (hashForRuns (data, run.getDataSize() - start) % averageRun == 0)
                        endRun();

                    continue;
                }

                endRun();

                if (isPieceOfItsOwn (child))
                    writeReference (out, split (child));
                else
                    write (out, child);
            }

            endRun();
            out << "</" << element.getType().toString() << '>';
        }
    };

    //==============================================================================
    struct Joiner
    {
        const ReadPiece& read;
        PluginParameterValues* parameters;
        bool failed = false;

        std::unique_ptr<juce::XmlElement> parse (const juce::String& name)
        {
            const auto bytes = read (name);

            if (! bytes.has_value())
            {
                failed = true;
                return {};
            }

            auto xml = juce::parseXML (bytes->toString());
            failed = failed || xml == nullptr;
            return xml;
        }

        void readParameters (const juce::XmlElement& device, const juce::String& name)
        {
            const auto xml = parse (name);

            if (xml == nullptr || parameters == nullptr)
                return;

            auto& values = (*parameters)[(std::uint64_t) device.getStringAttribute (schema::deviceID).getLargeIntValue()];

            for (const auto* parameter : xml->getChildWithTagNameIterator (versions::PARAMETER.toString()))
                values.push_back ({ parameter->getStringAttribute (versions::parameterID),
                                    parameter->getStringAttribute (versions::parameterName),
                                    (float) parameter->getDoubleAttribute (versions::parameterValue) });
        }

        juce::ValueTree convert (const juce::XmlElement& xml)
        {
            juce::ValueTree tree (xml.getTagName());

            for (const auto& attribute : xml.getAttributeIterator())
            {
                if (attribute.name == versions::statePiece)
                {
                    if (const auto state = read (attribute.value))
                        tree.setProperty (schema::pluginState, state->toBase64Encoding(), nullptr);
                    else
                        failed = true;
                }
                else if (attribute.name == versions::parametersPiece)
                {
                    if (parameters != nullptr)
                        readParameters (xml, attribute.value);
                }
                else
                {
                    tree.setProperty (attribute.name, attribute.value, nullptr);
                }
            }

            for (const auto* child : xml.getChildIterator())
            {
                if (! child->hasTagName (versions::PIECE.toString()))
                {
                    tree.appendChild (convert (*child), nullptr);
                    continue;
                }

                const auto piece = parse (child->getStringAttribute (versions::piece));

                if (piece == nullptr)
                    continue;

                // A run's notes or points go in its place, one after another.
                if (piece->hasTagName (versions::RUN.toString()))
                    for (const auto* inRun : piece->getChildIterator())
                        tree.appendChild (convert (*inRun), nullptr);
                else
                    tree.appendChild (convert (*piece), nullptr);
            }

            return tree;
        }
    };
} // namespace

juce::String getPieceName (const juce::MemoryBlock& block)
{
    return juce::SHA256 (block).toHexString();
}

SongPieces splitSong (const juce::ValueTree& song, const PluginParameterValues& parameters)
{
    SongPieces result;
    Splitter splitter { result, parameters };
    result.song = splitter.split (song);
    return result;
}

juce::ValueTree joinSong (const juce::String& name, const ReadPiece& read, PluginParameterValues* parameters)
{
    Joiner joiner { read, parameters };
    const auto xml = joiner.parse (name);

    if (xml == nullptr)
        return {};

    auto tree = joiner.convert (*xml);
    return joiner.failed ? juce::ValueTree() : tree;
}

juce::MemoryBlock writeElement (const juce::ValueTree& element)
{
    juce::MemoryOutputStream out;
    writePlainElement (out, element);
    return toBlock (out);
}

juce::ValueTree readElement (const juce::MemoryBlock& block)
{
    if (const auto xml = juce::parseXML (block.toString()))
        return juce::ValueTree::fromXml (*xml);

    return {};
}
} // namespace phazite
