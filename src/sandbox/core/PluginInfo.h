// SPDX-License-Identifier: AGPL-3.0-or-later
// What a helper tells Phazite of a plug-in it hosts (docs/tracktion-migration.md, T3): its name and kind, its latency and
// tail, its buses, and its parameters, each by the plug-in's own ID (a VST3's number, never its place in the list), as
// the control channel carries them (Messages.h).
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
};

void write (MessageWriter&, const PluginInfo&);
PluginInfo readPluginInfo (MessageReader&);

void write (MessageWriter&, const std::vector<ParameterInfo>&);
std::vector<ParameterInfo> readParameters (MessageReader&);
} // namespace phazite::sandbox
