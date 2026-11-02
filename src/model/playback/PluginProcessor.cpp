// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginProcessor.h"

namespace phazite
{
namespace
{
    // A note event as MIDI on the first channel.
    std::optional<juce::MidiMessage> toMidi (const devices::NoteEvent& event)
    {
        using Type = devices::NoteEvent::Type;

        switch (event.type)
        {
            case Type::noteOn:
                return juce::MidiMessage::noteOn (1, event.number, event.value);
            case Type::noteOff:
                return juce::MidiMessage::noteOff (1, event.number, event.value);
            case Type::pitchBend:
                return juce::MidiMessage::pitchWheel (1, juce::jlimit (0, 16383, juce::roundToInt ((event.value + 1.0f) * 8192.0f)));
            case Type::controller:
                return juce::MidiMessage::controllerEvent (1, event.number, juce::jlimit (0, 127, juce::roundToInt (event.value * 127.0f)));
            case Type::allNotesOff:
                return juce::MidiMessage::allNotesOff (1);
        }

        return {};
    }
} // namespace

PluginProcessor::PluginProcessor (te::Plugin& p)
    : plugin (&p),
      edit (&p.edit),
      typeID ("plugin"),
      name (p.getName().toStdString())
{
    info.typeID = typeID;
    info.name = name;
    info.kind = p.isSynth() ? devices::DeviceKind::instrument : devices::DeviceKind::effect;
}

PluginProcessor::~PluginProcessor()
{
    if (initialised && edit.get() != nullptr)
        plugin->baseClassDeinitialise();
}

void PluginProcessor::prepare (double rate, int maximumBlockSize)
{
    // Tracktion prepares the instance (once, however many play it).
    sampleRate = rate;

    if (initialised)
        plugin->baseClassDeinitialise();

    plugin->baseClassInitialise ({ te::TimePosition(), rate, maximumBlockSize });
    initialised = true;

    // Room for a block's notes, made now: the array keeps it as it is cleared.
    for (int i = 0; i < maximumMidiMessages; ++i)
        midi.addMidiMessage (juce::MidiMessage::allNotesOff (1), 0.0, te::MPESourceID {});

    midi.clear();
}

void PluginProcessor::process (const devices::ProcessContext& context)
{
    // Its notes, at their times in the block; and the song's place, in seconds at the block's tempo.
    midi.clear();

    for (const auto& event : context.events)
        if (const auto message = toMidi (event); message.has_value() && midi.size() < maximumMidiMessages)
            midi.addMidiMessage (*message, event.sample / sampleRate, te::MPESourceID {});

    juce::AudioBuffer<float> buffer (context.channels, context.numChannels, context.numSamples);
    const auto start = context.beat * 60.0 / std::max (1.0, context.tempo);
    const te::TimeRange time (te::TimePosition::fromSeconds (start), te::TimeDuration::fromSeconds (context.numSamples / sampleRate));
    const te::PluginRenderContext render (&buffer, 0, context.numSamples, &midi, 0.0, time, context.isPlaying, false, false, false);
    plugin->applyToBufferWithAutomation (render);
}

int PluginProcessor::getLatencySamples() const
{
    return juce::roundToInt (plugin->getLatencySeconds() * sampleRate);
}

double PluginProcessor::getTailSeconds() const
{
    return plugin->getTailLength();
}
} // namespace phazite
