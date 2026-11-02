// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Messages.h"

namespace phazite::sandbox
{
MessageWriter::MessageWriter (MessageType type, std::int64_t number)
    : request (number)
{
    out.writeInt ((int) type);
    out.writeInt64 (number);
}

MessageWriter& MessageWriter::add (std::int32_t value)
{
    out.writeInt (value);
    return *this;
}

MessageWriter& MessageWriter::add (std::int64_t value)
{
    out.writeInt64 (value);
    return *this;
}

MessageWriter& MessageWriter::add (double value)
{
    out.writeDouble (value);
    return *this;
}

MessageWriter& MessageWriter::add (bool value)
{
    out.writeBool (value);
    return *this;
}

MessageWriter& MessageWriter::add (const juce::String& value)
{
    out.writeString (value);
    return *this;
}

MessageWriter& MessageWriter::add (const juce::MemoryBlock& value)
{
    out.writeInt64 ((juce::int64) value.getSize());
    out.write (value.getData(), value.getSize());
    return *this;
}

juce::MemoryBlock MessageWriter::take()
{
    return out.getMemoryBlock();
}

//==============================================================================
MessageReader::MessageReader (const juce::MemoryBlock& message)
    : in (message, false)
{
    type = (MessageType) readInt();
    request = readInt64();
}

std::int32_t MessageReader::readInt()
{
    return in.getNumBytesRemaining() >= 4 ? in.readInt() : 0;
}

std::int64_t MessageReader::readInt64()
{
    return in.getNumBytesRemaining() >= 8 ? in.readInt64() : 0;
}

double MessageReader::readDouble()
{
    return in.getNumBytesRemaining() >= 8 ? in.readDouble() : 0.0;
}

bool MessageReader::readBool()
{
    return in.getNumBytesRemaining() >= 1 && in.readBool();
}

juce::String MessageReader::readString()
{
    return in.isExhausted() ? juce::String() : in.readString();
}

juce::MemoryBlock MessageReader::readBlock()
{
    const auto size = readInt64();

    if (size <= 0 || size > in.getNumBytesRemaining())
        return {};

    juce::MemoryBlock block ((size_t) size);
    in.read (block.getData(), (int) size);
    return block;
}
} // namespace phazite::sandbox
