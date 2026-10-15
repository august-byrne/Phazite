// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DevicePlugin.h"

#include <algorithm>

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // A MIDI message as a device's event, or nothing for the messages devices don't use. The channel doesn't matter.
    std::optional<devices::NoteEvent> toNoteEvent (const juce::MidiMessage& message, int sample)
    {
        using Type = devices::NoteEvent::Type;

        if (message.isNoteOn())
            return devices::NoteEvent { Type::noteOn, sample, message.getNoteNumber(), message.getFloatVelocity() };

        if (message.isNoteOff()) // a note-on of velocity 0 too
            return devices::NoteEvent { Type::noteOff, sample, message.getNoteNumber(), message.getFloatVelocity() };

        if (message.isAllNotesOff() || message.isAllSoundOff())
            return devices::NoteEvent { Type::allNotesOff, sample, 0, 0.0f };

        if (message.isPitchWheel())
            return devices::NoteEvent { Type::pitchBend, sample, 0, (float) (message.getPitchWheelValue() - 8192) / 8192.0f };

        if (message.isController())
            return devices::NoteEvent { Type::controller, sample, message.getControllerNumber(),
                                        (float) message.getControllerValue() / 127.0f };

        return {};
    }
} // namespace

DevicePlugin::DevicePlugin (te::PluginCreationInfo creation, const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> dsp)
    : te::Plugin (creation),
      deviceInfo (info),
      processor (std::move (dsp))
{
    jassert (devices::isValid (info));
    auto* undoManager = getUndoManager();

    for (const auto& description : info.parameters)
    {
        const auto id = toString (description.id);
        auto* stored = storedValues.add (new juce::CachedValue<float>());
        stored->referTo (state, juce::Identifier (id), undoManager, description.defaultValue);

        auto* parameter = addParam (id, toString (description.name), devices::getRange (description));
        parameter->attachToCurrentValue (*stored);
        parameters.push_back (parameter);
        currentValues.push_back (description.defaultValue);
    }
}

DevicePlugin::~DevicePlugin()
{
    notifyListenersOfDeletion();

    for (const auto& parameter : parameters)
        parameter->detachFromCurrentValue();
}

te::AutomatableParameter* DevicePlugin::findParameter (std::string_view id) const
{
    const auto index = devices::findParameter (deviceInfo, id);
    return index >= 0 ? parameters[(size_t) index].get() : nullptr;
}

juce::String DevicePlugin::getName() const
{
    return toString (deviceInfo.name);
}

juce::String DevicePlugin::getPluginType()
{
    return toString (deviceInfo.typeID);
}

juce::String DevicePlugin::getShortName (int)
{
    return getName();
}

juce::String DevicePlugin::getSelectableDescription()
{
    return getName();
}

bool DevicePlugin::takesMidiInput()
{
    return deviceInfo.kind == devices::DeviceKind::instrument;
}

bool DevicePlugin::isSynth()
{
    return deviceInfo.kind == devices::DeviceKind::instrument;
}

bool DevicePlugin::producesAudioWhenNoAudioInput()
{
    return isSynth() || ! noTail();
}

bool DevicePlugin::noTail()
{
    return getTailLength() <= 0.0;
}

double DevicePlugin::getTailLength() const
{
    return processor->getTailSeconds();
}

double DevicePlugin::getLatencySeconds()
{
    return latencySeconds;
}

te::Plugin::BusLayout DevicePlugin::getBusses() const
{
    // An instrument too, as Tracktion's own synths: being a synth keeps the track's audio from coming in.
    return BusLayout::singleStereoInOut();
}

void DevicePlugin::initialise (const te::PluginInitialisationInfo& info)
{
    processor->prepare (info.sampleRate, info.blockSizeSamples);
    stereo.setSize (2, std::max (info.blockSizeSamples, 512));
    tempoPosition.emplace (te::createPosition (edit.tempoSequence));
    latencySeconds = processor->getLatencySamples() / info.sampleRate;
}

void DevicePlugin::reset()
{
    processor->reset();
}

int DevicePlugin::collectEvents (const te::PluginRenderContext& context)
{
    auto* midi = context.bufferForMidiMessages;

    if (midi == nullptr)
        return 0;

    int count = 0;

    if (midi->isAllNotesOff)
        events[(size_t) count++] = { devices::NoteEvent::Type::allNotesOff, 0, 0, 0.0f };

    // The messages are in time order; one that rounds to the block's end stays in it.
    const auto lastSample = context.bufferNumSamples - 1;

    for (const auto& message : *midi)
    {
        if (count == maximumEvents)
            break;

        const auto sample =
            juce::jlimit (0, lastSample, juce::roundToInt ((message.getTimeStamp() + context.midiBufferOffset) * sampleRate));

        if (const auto event = toNoteEvent (message, sample))
            events[(size_t) count++] = *event;
    }

    return count;
}

void DevicePlugin::applyToBuffer (const te::PluginRenderContext& context)
{
    if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
        return;

    for (size_t i = 0; i < parameters.size(); ++i)
        currentValues[i] = devices::constrain (deviceInfo.parameters[i], parameters[i]->getCurrentValue());

    const auto numEvents = collectEvents (context);
    devices::ProcessContext block;
    block.parameters = currentValues;
    block.isPlaying = context.isPlaying;

    if (tempoPosition.has_value())
    {
        tempoPosition->set (context.editTime.getStart());
        block.tempo = tempoPosition->getTempo();
    }

    auto& buffer = *context.destBuffer;
    const auto start = context.bufferStartSample, numSamples = context.bufferNumSamples;

    if (isSynth())
        buffer.clear (start, numSamples);

    if (buffer.getNumChannels() >= 2)
    {
        float* channels[] = { buffer.getWritePointer (0, start), buffer.getWritePointer (1, start) };
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = numSamples;
        block.events = { events.data(), (size_t) numEvents };
        processor->process (block);

        for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
            buffer.clear (channel, start, numSamples);

        return;
    }

    // A mono channel: the device plays in stereo, a scratch buffer's length at a time, and the channel gets the
    // average of its two sides.
    auto* mono = buffer.getWritePointer (0, start);
    int nextEvent = 0;

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (numSamples - done, stereo.getNumSamples());
        stereo.copyFrom (0, 0, mono + done, length);
        stereo.copyFrom (1, 0, mono + done, length);

        // This stretch's events, counted from its start.
        const auto firstEvent = nextEvent;

        while (nextEvent < numEvents && events[(size_t) nextEvent].sample < done + length)
            events[(size_t) nextEvent++].sample -= done;

        float* channels[] = { stereo.getWritePointer (0), stereo.getWritePointer (1) };
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.events = { events.data() + firstEvent, (size_t) (nextEvent - firstEvent) };
        processor->process (block);

        for (int i = 0; i < length; ++i)
            mono[done + i] = 0.5f * (channels[0][i] + channels[1][i]);

        done += length;
    }
}

void DevicePlugin::restorePluginStateFromValueTree (const juce::ValueTree& source)
{
    for (auto* stored : storedValues)
    {
        if (const auto* property = source.getPropertyPointer (stored->getPropertyID()))
            *stored = (float) *property;
        else
            stored->resetToDefault();
    }

    for (const auto& parameter : parameters)
        parameter->updateFromAttachedValue();
}
} // namespace phazite
