// SPDX-License-Identifier: AGPL-3.0-or-later
// The control channel between Phazite and a helper (docs/tracktion-migration.md, T3), over JUCE's child-process pipe:
// each message is its type, the number of the request it asks or answers (0 for a notice), and its fields one after
// another, which both sides read in the order they were written. Nothing on it is real-time: a block goes through its
// instance's channel (BlockChannel.h).
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>

namespace phazite::sandbox
{
// The program's argument that names the pipe it connects to ("--PhaziteSandbox:<pipe>"), and the messages' version,
// which both sides must have.
inline constexpr const char* helperProcessID = "PhaziteSandbox";
inline constexpr std::int32_t protocolVersion = 1;

enum class MessageType : std::int32_t
{
    // Phazite's requests, each answered by a reply with its number.
    hello = 1,     // the protocol's version -> the helper's
    heartbeat,     // nothing -> nothing, from the helper's message thread
    openInstance,  // instance, channel name, limits, sample rate, the plug-in's format and file -> ok, error, latency
    closeInstance, // instance -> nothing
    stats,         // nothing -> the helper's CPU seconds and its memory in bytes

    // Phazite's notices.
    quit = 50, // the helper ends

    reply = 100 // an answer: its fields are the request's
};

class MessageWriter
{
public:
    MessageWriter (MessageType, std::int64_t request);

    std::int64_t getRequest() const noexcept { return request; }

    MessageWriter& add (std::int32_t);
    MessageWriter& add (std::int64_t);
    MessageWriter& add (double);
    MessageWriter& add (bool);
    MessageWriter& add (const juce::String&);
    MessageWriter& add (const juce::MemoryBlock&);

    juce::MemoryBlock take();

private:
    juce::MemoryOutputStream out;
    std::int64_t request = 0;
};

class MessageReader
{
public:
    explicit MessageReader (const juce::MemoryBlock&);

    MessageType getType() const noexcept { return type; }
    std::int64_t getRequest() const noexcept { return request; }

    // Each field in turn; past the end, an empty one (a message from another version reads as nothing).
    std::int32_t readInt();
    std::int64_t readInt64();
    double readDouble();
    bool readBool();
    juce::String readString();
    juce::MemoryBlock readBlock();

private:
    juce::MemoryInputStream in;
    MessageType type = MessageType::reply;
    std::int64_t request = 0;
};
} // namespace phazite::sandbox
