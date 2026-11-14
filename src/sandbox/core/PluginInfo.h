// SPDX-License-Identifier: AGPL-3.0-or-later
// What a helper tells Phazite of a plug-in it hosts: its name and kind, its latency and tail, its buses, its
// parameters, each by the plug-in's own ID (a VST3's number, never its place in the list), and its programs, as the
// control channel carries them (Messages.h).
#pragma once

#include "Messages.h"

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

// What a helper said, as Phazite takes it: a list no longer than its message could hold, and every number within what a
// plug-in can have (its latency, as keepLatency() keeps it; its tail; its channels, 64 at most; its program among its
// programs), so that a helper that crashed halfway through a message, or a plug-in that tells nonsense, never has Phazite
// make room for more than it sent.
PluginInfo readPluginInfo (MessageReader&);

// A latency a plug-in tells, within 0 and maxLatencySamples (about 22 seconds at 48 kHz), which Phazite's engine makes up
// for with delay lines that long.
inline constexpr int maxLatencySamples = 1 << 20;
int keepLatency (int samples);

void write (MessageWriter&, const std::vector<ParameterInfo>&);
std::vector<ParameterInfo> readParameters (MessageReader&);
} // namespace phazite::sandbox
