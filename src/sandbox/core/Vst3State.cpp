// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Vst3State.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace phazite::sandbox
{
namespace
{
    constexpr int classIDSize = 32;                     // the class ID, in ASCII
    constexpr int headerSize = 4 + 4 + classIDSize + 8; // "VST3", the version, the class ID, where the list starts
    constexpr int listPlace = headerSize - 8;
    constexpr int entrySize = 4 + 8 + 8; // a chunk's ID, where it starts, its size
    constexpr int maxChunks = 128;       // as many as the SDK reads
    constexpr juce::uint32 juceMagic = 0x21324356;

    bool hasID (const char* data, const char* id)
    {
        return std::memcmp (data, id, 4) == 0;
    }
} // namespace

std::optional<Vst3State> readVst3Preset (const juce::MemoryBlock& block)
{
    const auto* data = static_cast<const char*> (block.getData());
    const auto size = (juce::int64) block.getSize();

    if (size < headerSize || ! hasID (data, "VST3"))
        return std::nullopt;

    const auto listStart = (juce::int64) juce::ByteOrder::littleEndianInt64 (data + listPlace);

    if (listStart < headerSize || listStart > size - 8 || ! hasID (data + listStart, "List"))
        return std::nullopt;

    const auto count = (int) juce::ByteOrder::littleEndianInt (data + listStart + 4);

    if (count < 0 || count > maxChunks || listStart + 8 + (juce::int64) count * entrySize > size)
        return std::nullopt;

    Vst3State state;
    state.classID = juce::String::fromUTF8 (data + 8, classIDSize);
    bool hasComponent = false;

    for (int i = 0; i < count; ++i)
    {
        const auto* entry = data + listStart + 8 + (juce::int64) i * entrySize;
        const auto start = (juce::int64) juce::ByteOrder::littleEndianInt64 (entry + 4);
        const auto length = (juce::int64) juce::ByteOrder::littleEndianInt64 (entry + 12);

        if (start < headerSize || length < 0 || start > size || length > size - start)
            return std::nullopt;

        if (hasID (entry, "Comp"))
        {
            state.component = juce::MemoryBlock (data + start, (size_t) length);
            hasComponent = true;
        }
        else if (hasID (entry, "Cont"))
        {
            state.controller = juce::MemoryBlock (data + start, (size_t) length);
            state.hasController = true;
        }
    }

    if (! hasComponent)
        return std::nullopt;

    return state;
}

juce::MemoryBlock writeVst3Preset (const Vst3State& state)
{
    juce::MemoryOutputStream out;
    out.write ("VST3", 4);
    out.writeInt (1);

    char classID[classIDSize] = {};
    std::memcpy (classID, state.classID.toRawUTF8(), std::min ((size_t) classIDSize, state.classID.getNumBytesAsUTF8()));
    out.write (classID, classIDSize);
    out.writeInt64 (0); // where the list starts, written once it is known

    struct Chunk
    {
        const char* id;
        juce::int64 start, size;
    };

    std::vector<Chunk> chunks;
    chunks.push_back ({ "Comp", out.getPosition(), (juce::int64) state.component.getSize() });
    out.write (state.component.getData(), state.component.getSize());

    if (state.hasController)
    {
        chunks.push_back ({ "Cont", out.getPosition(), (juce::int64) state.controller.getSize() });
        out.write (state.controller.getData(), state.controller.getSize());
    }

    const auto listStart = out.getPosition();
    out.write ("List", 4);
    out.writeInt ((int) chunks.size());

    for (const auto& chunk : chunks)
    {
        out.write (chunk.id, 4);
        out.writeInt64 (chunk.start);
        out.writeInt64 (chunk.size);
    }

    auto block = out.getMemoryBlock();
    const auto place = juce::ByteOrder::swapIfBigEndian ((juce::uint64) listStart);
    std::memcpy (static_cast<char*> (block.getData()) + listPlace, &place, sizeof (place));
    return block;
}

juce::String readVst3ClassID (const juce::MemoryBlock& block)
{
    if (block.getSize() < (size_t) headerSize || ! hasID (static_cast<const char*> (block.getData()), "VST3"))
        return {};

    return juce::String::fromUTF8 (static_cast<const char*> (block.getData()) + 8, classIDSize);
}

std::optional<Vst3State> readJuceVst3State (const juce::MemoryBlock& block, const juce::String& classID)
{
    const auto* data = static_cast<const char*> (block.getData());

    if (block.getSize() <= 8 || juce::ByteOrder::littleEndianInt (data) != juceMagic)
        return std::nullopt;

    const auto length = (size_t) juce::ByteOrder::littleEndianInt (data + 4);

    if (length == 0 || length > block.getSize() - 8)
        return std::nullopt;

    const auto xml = juce::parseXML (juce::String::fromUTF8 (data + 8, (int) length));

    if (xml == nullptr || ! xml->hasTagName ("VST3PluginState"))
        return std::nullopt;

    Vst3State state;
    state.classID = classID;
    const auto* component = xml->getChildByName ("IComponent");

    if (component == nullptr || ! state.component.fromBase64Encoding (component->getAllSubText()))
        return std::nullopt;

    if (const auto* controller = xml->getChildByName ("IEditController"))
        state.hasController = state.controller.fromBase64Encoding (controller->getAllSubText());

    return state;
}

juce::MemoryBlock writeJuceVst3State (const Vst3State& state)
{
    juce::XmlElement xml ("VST3PluginState");
    xml.createNewChildElement ("IComponent")->addTextElement (state.component.toBase64Encoding());

    if (state.hasController)
        xml.createNewChildElement ("IEditController")->addTextElement (state.controller.toBase64Encoding());

    juce::MemoryBlock block;

    {
        juce::MemoryOutputStream out (block, false);
        out.writeInt ((int) juceMagic);
        out.writeInt (0); // the text's size, written below
        xml.writeTo (out, juce::XmlElement::TextFormat().singleLine());
        out.writeByte (0);
    }

    const auto length = juce::ByteOrder::swapIfBigEndian ((juce::uint32) (block.getSize() - 9));
    std::memcpy (static_cast<char*> (block.getData()) + 4, &length, sizeof (length));
    return block;
}
} // namespace phazite::sandbox
