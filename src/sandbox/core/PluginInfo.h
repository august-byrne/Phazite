// SPDX-License-Identifier: AGPL-3.0-or-later
// What a helper tells Phazite of a plug-in it hosts: its name and kind, its latency and tail, its buses, its
// parameters, each by the plug-in's own ID (a VST3's number, never its place in the list), and its programs, as the
// control channel carries them (Messages.h).
#pragma once

#include "Messages.h"

#include <optional>
#include <vector>

namespace phazite::sandbox
{
struct ParameterInfo
{
    juce::String id; // the plug-in's own
    juce::String name, label;
    float defaultValue = 0.0f, value = 0.0f; // from 0 to 1
    int numSteps = 0;                        // a choice's or a switch's; 0 for a continuous one
    bool isBoolean = false, isAutomatable = true;
};

struct PluginInfo
{
    juce::String name;
    bool isInstrument = false;
    int latencySamples = 0;
    double tailSeconds = 0.0;
    int numInputChannels = 0, numOutputChannels = 0;
    bool hasSidechain = false; // a sidechain bus of two channels, after the main ones
    std::vector<ParameterInfo> parameters {};
    std::vector<juce::String> programs {}; // its programs' names (none for a plug-in without programs of its own)
    int program = 0;                       // the one it has
    bool hasEditor = false;                // a window of its own (MessageType::openEditor)
};

void write (MessageWriter&, const PluginInfo&);

// What a helper said, as Phazite takes it: a list no longer than its message could hold, every number within what a
// plug-in can have (its latency, as keepLatency() keeps it; its tail; its channels, 64 at most; its program among its
// programs; its parameters' values, as readUnitValue() reads them, a value that is no number its parameter's default, or
// 0), and every text no longer than readText() keeps it, so that a helper that crashed halfway through a message, or a
// plug-in that tells nonsense, never has Phazite make room for more than it sent, nor hands it a value that is no number
// (known bug 181).
PluginInfo readPluginInfo (MessageReader&);

// A latency a plug-in tells, within 0 and maxLatencySamples (about 22 seconds at 48 kHz), which Phazite's engine makes up
// for with delay lines that long.
inline constexpr int maxLatencySamples = 1 << 20;
int keepLatency (int samples);

// What a plug-in tells, checked where it comes in (known bug 181), so that nothing past it sees what isn't a number or a
// text of any length: a parameter's value from 0 to 1 (none for NaN or an infinity, and one past either end at that end);
// a text, of so many characters at most (a name, a label, a program's name or a value's text: maxTextLength; an ID:
// maxIDLength); a side of an editor, in pixels, from 1 to maxEditorSide; a step count, from 0 to maxSteps; and the
// modifier keys of a key, the keyboard's alone (no mouse button).
inline constexpr int maxTextLength = 256, maxIDLength = 128, maxEditorSide = 16384, maxSteps = 1 << 24;
std::optional<float> keepUnitValue (double) noexcept;
std::optional<float> readUnitValue (MessageReader&);
juce::String keepText (const juce::String&, int maximumLength = maxTextLength);
juce::String readText (MessageReader&, int maximumLength = maxTextLength);
int readEditorSide (MessageReader&);
int keepModifiers (int rawFlags) noexcept;

void write (MessageWriter&, const std::vector<ParameterInfo>&);
std::vector<ParameterInfo> readParameters (MessageReader&);
} // namespace phazite::sandbox
