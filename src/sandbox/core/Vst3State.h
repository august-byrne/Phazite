// SPDX-License-Identifier: AGPL-3.0-or-later
// A VST3's state, as its format keeps it outside any host: Steinberg's preset file (.vstpreset), which the song holds as a
// plug-in's state (model/base/internal/Schema.h: pluginState) and which DAWproject holds too; and as JUCE's host of VST3s
// takes and gives the same streams (its getStateInformation(), which the helper calls: src/host/core/Instance.h), so that
// the helper turns one into the other and nothing of JUCE's is saved. Engine-neutral, juce_core alone; any thread.
#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace phazite::sandbox
{
// The two streams a VST3 keeps its state in, its processor's (IComponent) and its controller's (IEditController, which a
// plug-in may not have), and its class ID: 32 hexadecimal digits, as the preset file writes it.
struct Vst3State
{
    juce::String classID;
    juce::MemoryBlock component, controller;
    bool hasController = false;
};

// A .vstpreset, as the VST3 SDK's PresetFile writes one: "VST3"; the format's version (1); the class ID, 32 characters;
// where the list of chunks starts; the chunks, the processor's ("Comp") and the controller's ("Cont"); then the list:
// "List", how many chunks, and for each its ID, where it starts and its size. Numbers are little-endian: the version and the
// count 32 bits, where and how big 64. Nothing for what isn't one, or has no processor's chunk; other chunks (a program's,
// the meta information) are left out.
std::optional<Vst3State> readVst3Preset (const juce::MemoryBlock&);
juce::MemoryBlock writeVst3Preset (const Vst3State&);

// A preset file's class ID; none for what isn't a preset file.
juce::String readVst3ClassID (const juce::MemoryBlock&);

// JUCE's binary block of a VST3's state (AudioProcessor::copyXmlToBinary()): a magic number and the text's size, then the
// XML element VST3PluginState, holding IComponent and IEditController, each in JUCE's own base 64, then a zero byte. It
// doesn't hold the class ID, which the plug-in gives. Nothing for what isn't one.
std::optional<Vst3State> readJuceVst3State (const juce::MemoryBlock&, const juce::String& classID);
juce::MemoryBlock writeJuceVst3State (const Vst3State&);
} // namespace phazite::sandbox
