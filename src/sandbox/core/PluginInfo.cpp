// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginInfo.h"

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

std::vector<ParameterInfo> readParameters (MessageReader& in)
{
    std::vector<ParameterInfo> parameters ((size_t) std::max (0, in.readInt()));

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
    info.latencySamples = in.readInt();
    info.tailSeconds = in.readDouble();
    info.numInputChannels = in.readInt();
    info.numOutputChannels = in.readInt();
    info.hasSidechain = in.readBool();
    info.parameters = readParameters (in);
    info.programs.resize ((size_t) std::max (0, in.readInt()));

    for (auto& program : info.programs)
        program = in.readString();

    info.program = in.readInt();
    info.hasEditor = in.readBool();
    return info;
}
} // namespace phazite::sandbox
