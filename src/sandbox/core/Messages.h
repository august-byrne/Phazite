// SPDX-License-Identifier: AGPL-3.0-or-later
// The control channel between Phazite and a helper, over JUCE's child-process pipe: each message is its type, the
// number of the request it asks or answers (0 for a notice), and its fields one after another, which both sides read in
// the order they were written. Nothing on it is real-time: a block goes through its instance's channel
// (BlockChannel.h).
#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>

namespace phazite::sandbox
{
// The program's argument that names the pipe it connects to ("--PhaziteSandbox:<pipe>"), and the messages' version,
// which both sides must have.
inline constexpr const char* helperProcessID = "PhaziteSandbox";
inline constexpr std::int32_t protocolVersion = 6;

enum class MessageType : std::int32_t
{
    // Phazite's requests, each answered by a reply with its number.
    hello = 1,       // the protocol's version -> the helper's
    heartbeat,       // nothing -> nothing, from the helper's message thread
    openInstance,    // instance, channel name, limits, sample rate, the plug-in's description (JUCE's XML), its state (or none)
                     // -> ok, why not, and what it is (PluginInfo.h)
    closeInstance,   // instance -> nothing
    prepareInstance, // instance, channel name, limits, sample rate -> ok: it plays the new channel from now on
    getState,        // instance -> its state: a VST3's as its format's preset file (Vst3State.h), another's as JUCE gives it
    setState,        // instance, state (the same) -> nothing (once it took it)
    parameterText,   // instance, parameter, value -> how the plug-in writes that value
    parameterValue,  // instance, parameter, text -> the value, from 0 to 1, that the plug-in reads the text as ("-6 dB")
    stats,           // nothing -> the helper's CPU seconds and its memory in bytes
    setProgram,      // instance, program -> nothing (once it has it)
    openEditor,      // instance -> ok, its width and height, whether it can be resized: the plug-in's editor made, not shown
    closeEditor,     // instance -> nothing (once it has gone)
    scan,            // a plug-in format's name, a file (or identifier) -> how many plug-ins it holds, and each one's
                     // description (JUCE's XML for it): a plug-in scan, which loads the plug-in's code in the helper

    // Phazite's notices.
    quit = 50,     // the helper ends
    setParameter,  // instance, parameter, value: a value set by hand
    placeEditor,   // instance, a window of Phazite's (its native handle; 0 to hide it), x, y, width, height: the editor shown
                   // in it there, in the window's pixels (as the screen counts them)
    setEditorKeys, // instance, whether the plug-in gets every key, how many keys, and each key's code and modifiers: the
                   // keys Phazite takes over the plug-in's editor

    // The helper's notices.
    parameterChanged = 70, // instance, parameter, value: the plug-in changed it itself (its window, a program of its own)
    gesture,               // instance, parameter, whether it begins: a control of the plug-in's window grabbed or let go
    latencyChanged,        // instance, samples
    stateChanged,          // instance, its program: something besides its parameters changed in it (its program, perhaps)
    parametersChanged,     // instance, its parameters (PluginInfo.h): their list changed
    editorResized,         // instance, width, height: the plug-in resized its editor itself
    editorKey,             // instance, whether it went down, its code and modifiers: a key Phazite takes, in the editor

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
