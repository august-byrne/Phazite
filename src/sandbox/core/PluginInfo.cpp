// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginInfo.h"

#include <cmath>

namespace phazite::sandbox
{
void write (MessageWriter& out, const std::vector<ParameterInfo>& parameters)
{
    out.add ((std::int32_t) parameters.size());

    for (const auto& parameter : parameters)
    {
        out.add (parameter.id).add (parameter.name).add (parameter.label);
        out.add ((double) parameter.defaultValue).add ((double) parameter.value).add ((std::int32_t) parameter.numSteps);
        out.add (parameter.isBoolean).add (parameter.isAutomatable);
    }
}

namespace
{
    // The fewest bytes a parameter takes in a message (three strings of nothing but their ends, two doubles, an int and two
    // bools), and a program's name (its end).
    constexpr juce::int64 smallestParameter = 3 + 8 + 8 + 4 + 1 + 1, smallestProgram = 1;

    // A count read from a message, no more than what is left of it could hold.
    size_t readCount (MessageReader& in, juce::int64 smallestItem)
    {
        return (size_t) juce::jlimit ((juce::int64) 0, in.getNumBytesLeft() / smallestItem, (juce::int64) in.readInt());
    }
} // namespace

int keepLatency (int samples)
{
    return juce::jlimit (0, maxLatencySamples, samples);
}

std::vector<ParameterInfo> readParameters (MessageReader& in)
{
    std::vector<ParameterInfo> parameters (readCount (in, smallestParameter));

    for (auto& parameter : parameters)
    {
        parameter.id = in.readString();
        parameter.name = in.readString();
        parameter.label = in.readString();
        parameter.defaultValue = (float) in.readDouble();
        parameter.value = (float) in.readDouble();
        parameter.numSteps = in.readInt();
        parameter.isBoolean = in.readBool();
        parameter.isAutomatable = in.readBool();
    }

    return parameters;
}

void write (MessageWriter& out, const PluginInfo& info)
{
    out.add (info.name).add (info.isInstrument).add ((std::int32_t) info.latencySamples).add (info.tailSeconds);
    out.add ((std::int32_t) info.numInputChannels).add ((std::int32_t) info.numOutputChannels).add (info.hasSidechain);
    write (out, info.parameters);
    out.add ((std::int32_t) info.programs.size());

    for (const auto& program : info.programs)
        out.add (program);

    out.add ((std::int32_t) info.program).add (info.hasEditor);
}

PluginInfo readPluginInfo (MessageReader& in)
{
    PluginInfo info;
    info.name = in.readString();
    info.isInstrument = in.readBool();
    info.latencySamples = keepLatency (in.readInt());

    // A tail of up to an hour; none for what is no number.
    const auto tail = in.readDouble();
    info.tailSeconds = std::isfinite (tail) ? juce::jlimit (0.0, 3600.0, tail) : 0.0;

    info.numInputChannels = juce::jlimit (0, 64, in.readInt());
    info.numOutputChannels = juce::jlimit (0, 64, in.readInt());
    info.hasSidechain = in.readBool();
    info.parameters = readParameters (in);
    info.programs.resize (readCount (in, smallestProgram));

    for (auto& program : info.programs)
        program = in.readString();

    info.program = juce::jlimit (0, juce::jmax (0, (int) info.programs.size() - 1), in.readInt());
    info.hasEditor = in.readBool();
    return info;
}
} // namespace phazite::sandbox
