// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginInfo.h"

#include "devices/core/DeviceInfo.h"

#include <cmath>
#include <map>
#include <set>

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

std::optional<float> keepUnitValue (double value) noexcept
{
    // Compared as a double first: one past a float's range, cast, would be no float at all.
    if (! std::isfinite (value))
        return {};

    return (float) juce::jlimit (0.0, 1.0, value);
}

std::optional<float> readUnitValue (MessageReader& in)
{
    return keepUnitValue (in.readDouble());
}

juce::String keepText (const juce::String& text, int maximumLength)
{
    return text.length() > maximumLength ? text.substring (0, maximumLength) : text;
}

juce::String readText (MessageReader& in, int maximumLength)
{
    return keepText (in.readString(), maximumLength);
}

int readEditorSide (MessageReader& in)
{
    return juce::jlimit (1, maxEditorSide, (int) in.readInt());
}

int keepModifiers (int rawFlags) noexcept
{
    // juce::ModifierKeys' Shift, Ctrl, Alt and Command (Ctrl's own away from macOS): its allKeyboardModifiers, which this layer
    // can't include (juce_gui_basics), with Command's flag of macOS.
    constexpr int keyboardFlags = 1 | 2 | 4 | 8;
    return rawFlags & keyboardFlags;
}

std::vector<ParameterInfo> readParameters (MessageReader& in)
{
    std::vector<ParameterInfo> parameters (readCount (in, smallestParameter));

    for (auto& parameter : parameters)
    {
        parameter.id = readText (in, maxIDLength);
        parameter.name = readText (in);
        parameter.label = readText (in);
        parameter.defaultValue = readUnitValue (in).value_or (0.0f);
        parameter.value = readUnitValue (in).value_or (parameter.defaultValue);
        parameter.numSteps = juce::jlimit (0, maxSteps, (int) in.readInt());
        parameter.isBoolean = in.readBool();
        parameter.isAutomatable = in.readBool();
    }

    // Each ID once, as everything past this finds a parameter by it (JUCE, the engine, the song's values): the first of IDs
    // that came the same (cut short, or told twice) keeps it, and a later one by its order among them ("gain", "gain#2",
    // "gain#3"), as is one with none ("#1", "#2") and one with the ID of the mix the engine gives an effect ("mix#2"), never
    // as another's ID (known bug 181). Not by its place among all the plug-in's parameters (its second review): an update
    // that adds or moves other parameters leaves the names alone; one that reorders the twins themselves swaps them, and
    // what the song keeps of them (automation, values set by hand) with them. A VST3's IDs are its numbers, each once.
    const juce::String mix (devices::mixParameterID.data(), devices::mixParameterID.size());
    std::set<juce::String> told { mix };
    std::map<juce::String, int> seen { { mix, 1 } };

    for (const auto& parameter : parameters)
        if (parameter.id.isNotEmpty())
            told.insert (parameter.id);

    std::set<juce::String> made;

    for (auto& parameter : parameters)
    {
        auto& id = parameter.id;
        auto order = ++seen[id];

        if (id.isNotEmpty() && order == 1)
            continue;

        auto named = id + "#" + juce::String (order);

        while (told.contains (named) || made.contains (named))
            named = id + "#" + juce::String (++order);

        made.insert (named);
        id = named;
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
    info.name = readText (in);
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
        program = readText (in);

    info.program = juce::jlimit (0, juce::jmax (0, (int) info.programs.size() - 1), in.readInt());
    info.hasEditor = in.readBool();
    return info;
}
} // namespace phazite::sandbox
