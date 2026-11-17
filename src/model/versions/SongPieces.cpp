// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongPieces.h"
#include "model/base/Json.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/project/internal/Project.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    namespace versions = schema::versions;

    // A list of an element's notes or points this long or longer is in runs, each ending after about one in averageRun of
    // them: after one whose text's FNV-1a hash is a multiple of averageRun. Where a run ends depends on what it holds alone,
    // so that a note added, changed or taken away changes its own run, and the runs after it stay. How the writer chooses,
    // which a reader needn't know.
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

    juce::MemoryBlock toBlock (juce::MemoryOutputStream& out)
    {
        return { out.getData(), out.getDataSize() };
    }

    void writePlainElement (juce::OutputStream& out, const juce::ValueTree& element)
    {
        auto properties = getPropertyTexts (element);
        sortByName (properties);
        writeStartTag (out, element.getType().toString(), properties);

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

    void writeEmptyElement (juce::OutputStream& out, const juce::Identifier& type, PropertyTexts properties)
    {
        sortByName (properties);
        writeStartTag (out, type.toString(), properties);
        out << "/>";
    }

    // What tells an element from others of its type in its parent ("TRACK:12"), as a merge's clash names it.
    juce::String describe (const juce::ValueTree& element)
    {
        return element.getType().toString() + ":" + schema::getElementKey (element);
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

        // A piece in its place: its name, the type of what it holds, and the properties of that element's key.
        void writeReference (juce::OutputStream& out, const juce::String& name, const juce::String& type,
                             const juce::ValueTree& element = {})
        {
            PropertyTexts properties { { versions::pieceHash.toString(), name }, { versions::pieceType.toString(), type } };

            if (element.isValid())
                for (const auto& key : schema::getKeyProperties (element.getType()))
                    if (element.hasProperty (key))
                        properties.emplace_back (key, writeValue (element[key]));

            writeEmptyElement (out, versions::PIECE, std::move (properties));
        }

        // A plug-in's parameters' values, if given, as a piece of their own: its name, or none.
        juce::String keepParameters (const juce::ValueTree& element)
        {
            if (! element.hasType (schema::DEVICE))
                return {};

            const auto id = (std::uint64_t) element[schema::deviceID].toString().getLargeIntValue();
            const auto found = parameters.find (id);

            if (found == parameters.end() || found->second.empty())
                return {};

            juce::ValueTree values (versions::PARAMETERS);

            for (const auto& parameter : found->second)
                values.appendChild (juce::ValueTree (versions::PARAMETER, { { versions::parameterID, parameter.id },
                                                                            { versions::parameterName, parameter.name },
                                                                            { versions::parameterValue, parameter.value } }),
                                    nullptr);

            return keep (writeElement (values));
        }

        // An element, which a piece of its own holds, as its piece: its name.
        juce::String split (const juce::ValueTree& element)
        {
            juce::MemoryOutputStream out;
            write (out, element);
            return keep (toBlock (out));
        }

        // An element into the piece being written: its properties, but those whose values are bytes, each a piece of its
        // own named by a _bytes element first among its children; its plug-in's parameters' values, likewise; then its
        // children, those that are pieces of their own each a reference in its place, and many notes or points in runs.
        void write (juce::OutputStream& out, const juce::ValueTree& element)
        {
            PropertyTexts properties, bytes;

            for (int i = 0; i < element.getNumProperties(); ++i)
            {
                const auto name = element.getPropertyName (i);
                auto text = writeValue (element[name]);

                // Bytes that come back as the same text, out of line; others (none, or not base 64) as they are.
                if (schema::isBytesProperty (element, name))
                {
                    if (const auto decoded = readBytes (text);
                        decoded.has_value() && decoded->getSize() > 0 && writeBytes (*decoded) == text)
                    {
                        bytes.emplace_back (name.toString(), keep (*decoded));
                        continue;
                    }
                }

                properties.emplace_back (name.toString(), std::move (text));
            }

            sortByName (properties);
            sortByName (bytes);
            const auto type = element.getType().toString();
            writeStartTag (out, type, properties);

            const auto parametersPiece = keepParameters (element);

            if (bytes.empty() && parametersPiece.isEmpty() && element.getNumChildren() == 0)
            {
                out << "/>";
                return;
            }

            out << '>';

            for (const auto& [name, piece] : bytes)
                writeEmptyElement (out, versions::BYTES,
                                   { { versions::bytesHash.toString(), piece }, { versions::bytesName.toString(), name } });

            if (parametersPiece.isNotEmpty())
                writeEmptyElement (out, versions::PARAMETERS, { { versions::parametersHash.toString(), parametersPiece } });

            int numInRuns = 0;

            for (const auto& child : element)
                numInRuns += schema::goesInRuns (child) ? 1 : 0;

            const auto inRuns = numInRuns >= fewestInRuns;
            juce::MemoryOutputStream run;
            bool runOpen = false;

            const auto endRun = [&]
            {
                if (! runOpen)
                    return;

                run << "</" << versions::RUN.toString() << '>';
                writeReference (out, keep (toBlock (run)), versions::RUN.toString());
                run.reset();
                runOpen = false;
            };

            for (const auto& child : element)
            {
                if (inRuns && schema::goesInRuns (child))
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

                if (schema::isPieceOfItsOwn (child))
                    writeReference (out, split (child), child.getType().toString(), child);
                else
                    write (out, child);
            }

            endRun();
            out << "</" << type << '>';
        }
    };

    //==============================================================================
    struct Joiner
    {
        const ReadPiece& read;
        PluginParameterValues* parameters;
        std::vector<MissingPiece>* missing; // given: as much as can be read, with stand-ins for the rest
        bool failed = false;

        std::unique_ptr<juce::XmlElement> parse (const juce::String& name) const
        {
            const auto bytes = read (name);
            // A piece whose bytes are no UTF-8 text is damaged, as one whose text is no XML.
            if (! bytes.has_value()
                || ! juce::CharPointer_UTF8::isValidString (static_cast<const char*> (bytes->getData()), (int) bytes->getSize()))
                return nullptr;

            return juce::parseXML (bytes->toString());
        }

        // A piece that couldn't be read: listed, or the whole join fails.
        void lose (juce::StringArray path, const juce::String& what, const juce::String& piece)
        {
            if (missing == nullptr)
            {
                failed = true;
                return;
            }

            path.add (what);
            missing->push_back ({ path, piece });
        }

        // What a reference names, made of what the reference says of it: a stand-in, its type and key alone.
        static juce::ValueTree makeStandIn (const juce::XmlElement& reference)
        {
            if (! juce::XmlElement::isValidXmlName (reference.getStringAttribute (versions::pieceType)))
                return {}; // a damaged reference names no type an element can have

            juce::ValueTree standIn (reference.getStringAttribute (versions::pieceType));

            for (const auto& attribute : reference.getAttributeIterator())
                if (! attribute.name.toString().startsWithChar ('_'))
                    standIn.setProperty (attribute.name, attribute.value, nullptr);

            standIn.setProperty (versions::missing, true, nullptr);
            return standIn;
        }

        void readParameters (const juce::ValueTree& device, const juce::String& name)
        {
            const auto xml = parse (name);

            if (xml == nullptr || parameters == nullptr)
                return;

            auto& values = (*parameters)[(std::uint64_t) device[schema::deviceID].toString().getLargeIntValue()];

            for (const auto* parameter : xml->getChildWithTagNameIterator (versions::PARAMETER.toString()))
                values.push_back ({ parameter->getStringAttribute (versions::parameterID),
                                    parameter->getStringAttribute (versions::parameterName),
                                    readFloat (parameter->getStringAttribute (versions::parameterValue)) });
        }

        juce::ValueTree convert (const juce::XmlElement& xml, juce::StringArray path)
        {
            juce::ValueTree tree (xml.getTagName());

            for (const auto& attribute : xml.getAttributeIterator())
                tree.setProperty (attribute.name, attribute.value, nullptr);

            path.add (describe (tree));

            for (const auto* child : xml.getChildIterator())
            {
                if (child->isTextElement())
                    continue; // text between tags, which a piece never holds

                if (child->getTagName() == versions::BYTES.toString())
                {
                    const auto piece = child->getStringAttribute (versions::bytesHash);
                    const auto name = child->getStringAttribute (versions::bytesName);

                    // A damaged piece may name no property an element can have (none, or no name XML can write): left out.
                    if (! juce::XmlElement::isValidXmlName (name))
                        continue;

                    if (const auto bytes = read (piece))
                    {
                        tree.setProperty (name, writeBytes (*bytes), nullptr);
                    }
                    else
                    {
                        lose (path, versions::BYTES.toString() + ":" + child->getStringAttribute (versions::bytesName), piece);
                        tree.setProperty (versions::missing, true, nullptr);
                    }
                }
                else if (child->getTagName() == versions::PARAMETERS.toString())
                {
                    readParameters (tree, child->getStringAttribute (versions::parametersHash));
                }
                else if (child->getTagName() == versions::PIECE.toString())
                {
                    const auto piece = child->getStringAttribute (versions::pieceHash);
                    const auto type = child->getStringAttribute (versions::pieceType);
                    const auto held = parse (piece);

                    if (held == nullptr || held->getTagName() != type)
                    {
                        // A run lost: the element it is in stands in for itself. Another piece: a stand-in in its place.
                        if (type == versions::RUN.toString())
                        {
                            lose (path, type, piece);
                            tree.setProperty (versions::missing, true, nullptr);
                        }
                        else
                        {
                            const auto standIn = makeStandIn (*child);
                            lose (path, describe (standIn), piece);

                            if (missing != nullptr && standIn.isValid())
                                tree.appendChild (standIn, nullptr);
                        }

                        continue;
                    }

                    if (type == versions::RUN.toString())
                        for (const auto* inRun : held->getChildIterator())
                            tree.appendChild (convert (*inRun, path), nullptr);
                    else
                        tree.appendChild (convert (*held, path), nullptr);
                }
                else if (! child->getTagName().startsWithChar ('_')) // the store's own, which this build doesn't know: left out
                {
                    tree.appendChild (convert (*child, path), nullptr);
                }
            }

            return tree;
        }
    };

    //==============================================================================
    // A whole song joined made consistent as a project file is as it opens (Project.h: makeConsistent()), whoever wrote its
    // pieces, its sound files' paths against the project's folder. A stand-in for what a damaged piece held (Schema.h:
    // schema::versions::missing), which never reaches the song (ApplySong.h), is kept out of the checks meanwhile, inside an
    // element of a type they don't know, which they leave as it is, and put back in its place after.
    // Whether a song holds each item's ID once, and each clip each note's, every one of them one the checks keep (above 0,
    // below 2^62), where the checks look (elements of the types Phazite knows: Schema.h, isKnownType()), and isn't one the
    // checks give its IDs anew (Schema.h: mustGiveIDsAnew(), the loader's one rule): else they give some anew
    // (Project.cpp: keepIDs()). What it keeps out of them aside.
    bool holdsEachIDOnce (const juce::ValueTree& song, const juce::Identifier& keptOut)
    {
        std::set<std::uint64_t> items;
        auto once = true;
        const auto take = [&once] (std::set<std::uint64_t>& seen, const juce::var& value)
        {
            const auto id = readID (value);
            once = once && id > 0 && id < schema::maxID && seen.insert (id).second;
        };

        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                // As the loader walks a song: what it keeps out, and an element of a type Phazite doesn't know, which it keeps
                // as it is, with everything in it.
                if (child.hasType (keptOut) || ! schema::isKnownType (child.getType()))
                    continue;

                if (child.hasType (schema::CLIP))
                {
                    std::set<std::uint64_t> notes;

                    for (const auto& note : child)
                        if (note.hasType (schema::NOTE))
                            take (notes, note[schema::noteID]);
                }

                if (! child.hasType (schema::NOTE))
                    for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                        if (schema::holdsID (child, *property) && (property == &schema::trackID || child.hasProperty (*property)))
                            take (items, child[*property]);

                visit (child);
            }
        };

        visit (song);
        return once && ! mustGiveIDsAnew (song);
    }

    void makeJoinedSongConsistent (juce::ValueTree& song, const juce::File& projectFolder, bool* idsGivenAnew)
    {
        if (idsGivenAnew != nullptr)
            *idsGivenAnew = false;

        if (! song.hasType (schema::PHAZITE))
            return;

        static const juce::Identifier keptOut ("_KEPTOUT");
        std::vector<juce::ValueTree> wrappers;
        const std::function<void (juce::ValueTree)> wrap = [&] (juce::ValueTree element)
        {
            for (int i = 0; i < element.getNumChildren(); ++i)
            {
                auto child = element.getChild (i);

                if (! child.hasProperty (versions::missing))
                {
                    wrap (child);
                    continue;
                }

                juce::ValueTree wrapper (keptOut);
                element.removeChild (i, nullptr);
                wrapper.appendChild (child, nullptr);
                element.addChild (wrapper, i, nullptr);
                wrappers.push_back (wrapper);
            }
        };

        wrap (song);

        if (idsGivenAnew != nullptr)
            *idsGivenAnew = ! holdsEachIDOnce (song, keptOut);

        {
            Song checked (song);

            // The project's file isn't read: the checks take the folder it is in.
            if (projectFolder != juce::File())
                checked.setFile (projectFolder.getChildFile ("song.phazite"));

            // Read in part, what names an ID it hasn't may name what wasn't read: kept, for the song it is taken into to judge.
            makeConsistent (checked, wrappers.empty() ? ReadAs::whole : ReadAs::inPart);
        }

        for (auto& wrapper : wrappers)
        {
            auto parent = wrapper.getParent();

            if (! parent.isValid())
                continue; // what held it went, it with it

            const auto index = parent.indexOf (wrapper);
            auto standIn = wrapper.getChild (0);
            wrapper.removeChild (standIn, nullptr);
            parent.removeChild (index, nullptr);
            parent.addChild (standIn, index, nullptr);
        }
    }
} // namespace

juce::String getPieceName (const juce::MemoryBlock& block)
{
    return juce::SHA256 (block).toHexString();
}

namespace
{
    std::atomic<int> songsSplit { 0 }, songsSplitOnMessageThread { 0 };
}

SongsSplit countSongsSplit()
{
    return { songsSplit.load(), songsSplitOnMessageThread.load() };
}

SongPieces splitSong (const juce::ValueTree& song, const PluginParameterValues& parameters)
{
    if (song.hasType (schema::PHAZITE))
    {
        ++songsSplit;

        if (juce::MessageManager::existsAndIsCurrentThread())
            ++songsSplitOnMessageThread;
    }

    SongPieces result;
    Splitter splitter { result, parameters };
    result.song = splitter.split (song);
    return result;
}

juce::ValueTree joinSong (const juce::String& name, const ReadPiece& read, PluginParameterValues* parameters,
                          std::vector<MissingPiece>* missing, const juce::File& projectFolder, bool* idsGivenAnew)
{
    Joiner joiner { read, parameters, missing };
    const auto xml = joiner.parse (name);

    if (xml == nullptr)
    {
        if (missing != nullptr)
            missing->push_back ({ {}, name });

        return {};
    }

    auto tree = joiner.convert (*xml, {});

    if (joiner.failed)
        return {};

    makeExponentsReadable (tree); // before anything reads a number of it, as a project file's (known bug 170)
    makeJoinedSongConsistent (tree, projectFolder, idsGivenAnew);
    return tree;
}

std::vector<PieceReference> findReferences (const juce::MemoryBlock& piece)
{
    std::vector<PieceReference> found;
    const auto xml = juce::parseXML (piece.toString());

    if (xml == nullptr)
        return found;

    const std::function<void (const juce::XmlElement&)> visit = [&] (const juce::XmlElement& element)
    {
        for (const auto* child : element.getChildIterator())
        {
            if (child->getTagName() == versions::PIECE.toString())
            {
                const auto type = child->getStringAttribute (versions::pieceType);
                found.push_back ({ child->getStringAttribute (versions::pieceHash),
                                   type == versions::RUN.toString() ? type : describe (Joiner::makeStandIn (*child)) });
            }
            else if (child->getTagName() == versions::BYTES.toString())
            {
                found.push_back ({ child->getStringAttribute (versions::bytesHash),
                                   versions::BYTES.toString() + ":" + child->getStringAttribute (versions::bytesName) });
            }
            else if (child->getTagName() == versions::PARAMETERS.toString())
            {
                found.push_back ({ child->getStringAttribute (versions::parametersHash), versions::PARAMETERS.toString() });
            }
            else
            {
                visit (*child);
            }
        }
    };

    visit (*xml);
    return found;
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
