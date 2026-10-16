// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DevicePlugin.h"

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

    // An effect's mix, which this applies (an instrument has none).
    if (info.kind == devices::DeviceKind::effect)
        mixIndex = devices::findParameter (info, devices::mixParameterID);
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

void DevicePlugin::getChannelNames (juce::StringArray* ins, juce::StringArray* outs)
{
    te::Plugin::getChannelNames (ins, outs);

    // A sidechain comes in as two more inputs, which Tracktion fills from the source track: picking one wires the
    // source's two channels to them (te::Plugin::guessSidechainRouting()).
    if (ins != nullptr && deviceInfo.hasSidechain)
        ins->addArray (juce::StringArray { "Sidechain Left", "Sidechain Right" });
}

te::Plugin::BusLayout DevicePlugin::getBusses() const
{
    // An instrument too, as Tracktion's own synths: being a synth keeps the track's audio from coming in.
    return BusLayout::singleStereoInOut();
}

void DevicePlugin::initialise (const te::PluginInitialisationInfo& info)
{
    processor->prepare (info.sampleRate, info.blockSizeSamples);
    const auto stretch = std::max (info.blockSizeSamples, 512);
    stereo.setSize (2, stretch);
    tempoPosition.emplace (te::createPosition (edit.tempoSequence));
    latencySamples = processor->getLatencySamples();
    latencySeconds = latencySamples / info.sampleRate;

    if (mixIndex >= 0)
    {
        dry.setSize (2, stretch);
        appliedMix = currentValues[(size_t) mixIndex];

        for (auto& delay : dryDelays)
            delay.prepare (std::max (latencySamples, 1));
    }

    updateSidechain();
}

void DevicePlugin::initialiseWithoutStopping (const te::PluginInitialisationInfo&)
{
    updateSidechain();
}

void DevicePlugin::updateSidechain()
{
    // On the message thread, as a new playback graph is built: Tracktion builds one when the source changes.
    sidechainConnected.store (deviceInfo.hasSidechain && getSidechainSourceID().isValid() && getNumWires() > 0, std::memory_order_relaxed);
}

void DevicePlugin::reset()
{
    processor->reset();

    for (auto& delay : dryDelays)
        delay.reset();
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
    const auto isMono = buffer.getNumChannels() < 2;
    const auto targetMix = mixIndex >= 0 ? currentValues[(size_t) mixIndex] : 1.0f;

    // The sidechain's sound, after the device's own two channels. A graph built before the source changed may still
    // play a block or two without it.
    const auto hasSidechain = sidechainConnected.load (std::memory_order_relaxed) && buffer.getNumChannels() >= 4;

    if (isSynth()) // its own channels, not a sidechain's
        for (int channel = 0; channel < std::min (2, buffer.getNumChannels()); ++channel)
            buffer.clear (channel, start, numSamples);

    const float* sidechain[2] = {};

    if (hasSidechain)
    {
        block.sidechain = sidechain;
        block.numSidechainChannels = 2;
    }

    // A stretch at a time, as long as the scratch buffers prepared beforehand. A mono channel plays in stereo, through
    // a scratch buffer, and gets the average of the two sides back.
    int nextEvent = 0;

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (numSamples - done, stereo.getNumSamples());
        float* channels[2];

        if (isMono)
        {
            const auto* mono = buffer.getReadPointer (0, start + done);
            stereo.copyFrom (0, 0, mono, length);
            stereo.copyFrom (1, 0, mono, length);
            channels[0] = stereo.getWritePointer (0);
            channels[1] = stereo.getWritePointer (1);
        }
        else
        {
            channels[0] = buffer.getWritePointer (0, start + done);
            channels[1] = buffer.getWritePointer (1, start + done);
        }

        if (mixIndex >= 0) // what comes in, for the mix
            for (int channel = 0; channel < 2; ++channel)
                dry.copyFrom (channel, 0, channels[channel], length);

        // This stretch's events, counted from its start.
        const auto firstEvent = nextEvent;

        while (nextEvent < numEvents && events[(size_t) nextEvent].sample < done + length)
            events[(size_t) nextEvent++].sample -= done;

        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.events = { events.data() + firstEvent, (size_t) (nextEvent - firstEvent) };

        if (hasSidechain)
        {
            sidechain[0] = buffer.getReadPointer (2, start + done);
            sidechain[1] = buffer.getReadPointer (3, start + done);
        }

        processor->process (block);

        if (mixIndex >= 0)
            mixWithDry (channels, length, targetMix);

        if (isMono)
        {
            auto* mono = buffer.getWritePointer (0, start + done);

            for (int i = 0; i < length; ++i)
                mono[i] = 0.5f * (channels[0][i] + channels[1][i]);
        }

        done += length;
    }

    for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, start, numSamples);
}

void DevicePlugin::mixWithDry (float* const* wet, int length, float targetMix)
{
    // The dry sound, delayed as much as the effect delays its own, so that the two line up.
    if (latencySamples > 0)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            auto* samples = dry.getWritePointer (channel);
            auto& delay = dryDelays[(size_t) channel];

            for (int i = 0; i < length; ++i)
            {
                delay.push (samples[i]);
                samples[i] = delay.read (latencySamples);
            }
        }
    }

    // Ramped from the mix the last stretch ended with, so a moving knob doesn't click; nothing to do at 100 %.
    const auto from = appliedMix;
    appliedMix = targetMix;

    if (from >= 1.0f && targetMix >= 1.0f)
        return;

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto* drySamples = dry.getReadPointer (channel);

        for (int i = 0; i < length; ++i)
        {
            const auto mix = from + (targetMix - from) * (float) (i + 1) / (float) length;
            wet[channel][i] = mix * wet[channel][i] + (1.0f - mix) * drySamples[i];
        }
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
