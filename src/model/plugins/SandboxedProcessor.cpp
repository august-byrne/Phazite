// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SandboxedProcessor.h"

#include <algorithm>
#include <utility>

namespace phazite
{
namespace
{
    // A note event as a MIDI message on the first channel; 0 bytes for one that has none.
    int toMidi (const devices::NoteEvent& event, std::uint8_t* bytes) noexcept
    {
        using Type = devices::NoteEvent::Type;
        const auto byte = [] (int value) { return (std::uint8_t) juce::jlimit (0, 127, value); };

        switch (event.type)
        {
            case Type::noteOn:
            case Type::portamentoOn: // a plug-in plays it as a plain note, as FL Studio's portamento notes reach plug-ins
                bytes[0] = 0x90;
                bytes[1] = byte (event.number);
                bytes[2] = byte (std::max (1, juce::roundToInt (event.value * 127.0f)));
                return 3;
            case Type::noteOff:
                bytes[0] = 0x80;
                bytes[1] = byte (event.number);
                bytes[2] = byte (juce::roundToInt (event.value * 127.0f));
                return 3;
            case Type::pitchBend:
            {
                const auto bend = juce::jlimit (0, 16383, juce::roundToInt ((event.value + 1.0f) * 8192.0f));
                bytes[0] = 0xe0;
                bytes[1] = (std::uint8_t) (bend & 0x7f);
                bytes[2] = (std::uint8_t) (bend >> 7);
                return 3;
            }
            case Type::controller:
                bytes[0] = 0xb0;
                bytes[1] = byte (event.number);
                bytes[2] = byte (juce::roundToInt (event.value * 127.0f));
                return 3;
            case Type::allNotesOff:
                bytes[0] = 0xb0;
                bytes[1] = 123;
                bytes[2] = 0;
                return 3;
            case Type::slide: // MIDI has no slide of one note to another
                return 0;
        }

        return 0;
    }
} // namespace

SandboxedProcessor::SandboxedProcessor (te::Plugin& p, SandboxedInstance& sandboxed)
    : plugin (&p),
      instance (sandboxed),
      typeID ("plugin"),
      name (p.getName().toStdString())
{
    info.typeID = typeID;
    info.name = name;
    info.kind =
        instance.acceptsMidi() && instance.getTotalNumInputChannels() == 0 ? devices::DeviceKind::instrument : devices::DeviceKind::effect;

    // The plug-in's parameters by their places, then an effect's mix with what came in, which the engine applies (an
    // instrument has none). The names and IDs are made first: the descriptions point into them.
    const auto& own = instance.getRemote().getInfo().parameters;

    for (size_t i = 0; i < own.size(); ++i)
    {
        ids.push_back (std::to_string (i));
        names.push_back (own[i].name.toStdString());
    }

    for (size_t i = 0; i < own.size(); ++i)
        parameters.push_back ({ .id = ids[i], .name = names[i], .defaultValue = juce::jlimit (0.0f, 1.0f, own[i].defaultValue) });

    if (info.kind == devices::DeviceKind::effect)
        parameters.push_back ({ .id = devices::mixParameterID, .name = "Mix", .defaultValue = 1.0f });

    info.parameters = parameters;
}

int SandboxedProcessor::findParameter (const juce::String& tracktionID) const noexcept
{
    // Tracktion names each by its place: "0", "1"...
    const auto index = tracktionID.getIntValue();
    return juce::isPositiveAndBelow (index, (int) ids.size()) && tracktionID == juce::String (index) ? index : -1;
}

void SandboxedProcessor::prepare (double sampleRate, int maximumBlockSize)
{
    rate = sampleRate;
    instance.prepareToPlay (sampleRate, maximumBlockSize);
}

void SandboxedProcessor::start (const devices::ProcessContext& context)
{
    started = true;
    handed = handOver (context);
    handedAt = juce::Time::getHighResolutionTicks();
}

void SandboxedProcessor::process (const devices::ProcessContext& context)
{
    // Handed over as the block started (start()): only its answer to wait for, for what is left of its deadline.
    if (std::exchange (started, false))
    {
        if (handed)
            collect (context, handedAt);

        return;
    }

    if (handOver (context))
        collect (context, juce::Time::getHighResolutionTicks());
}

void SandboxedProcessor::silence (const devices::ProcessContext& context) const noexcept
{
    // An instrument plays nothing; an effect lets its sound through.
    if (info.kind == devices::DeviceKind::instrument)
        for (int i = 0; i < context.numChannels; ++i)
            juce::FloatVectorOperations::clear (context.channels[i], context.numSamples);
}

void SandboxedProcessor::collect (const devices::ProcessContext& context, std::int64_t since) noexcept
{
    // As long as a block's own length in real time from when it was handed over (none in a render).
    auto& channel = instance.getRemote().getChannel();
    auto deadline = std::int64_t (-1);

    if (! context.isRendering)
    {
        const auto waited =
            (std::int64_t) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - since) * 1.0e9);
        deadline = std::max<std::int64_t> (0, SandboxedInstance::getDeadlineNanoseconds (context.numSamples, rate) - waited);
    }

    if (channel.waitForAnswer (0, deadline) == sandbox::BlockChannel::Played::done)
    {
        for (int i = 0; i < channel.getInfo().numChannels; ++i)
            juce::FloatVectorOperations::copy (context.channels[i], channel.getChannel (i), context.numSamples);

        instance.let();
        return;
    }

    instance.let();
    silence (context);
}

bool SandboxedProcessor::handOver (const devices::ProcessContext& context) noexcept
{
    // The channel: a render waits for it, the audio thread never does (it skips the block while a render has it).
    if (context.isRendering)
    {
        instance.hold();
    }
    else if (! instance.tryHold())
    {
        silence (context);
        return false;
    }

    auto& channel = instance.getRemote().getChannel();
    const auto& limits = channel.getLimits();

    // Still on a block it was late with: one kept too long is a hang, which its helper is ended for (told without waiting).
    if (! instance.hasCrashed() && channel.isBusy())
    {
        const auto now = juce::Time::getHighResolutionTicks();

        if (busySince == 0)
            busySince = now;
        else if (juce::Time::highResolutionTicksToSeconds (now - busySince) * 1000.0
                 > instance.getRemote().getHelper().getAudioHangMilliseconds())
            instance.getRemote().getHelper().reportHung();
    }
    else
    {
        busySince = 0;
    }

    if (instance.hasCrashed() || channel.isBusy() || context.numSamples > limits.maxSamples)
    {
        instance.let();
        silence (context);
        return false;
    }

    auto& block = channel.getInfo();
    block.numSamples = context.numSamples;
    block.numChannels = std::min (context.numChannels, limits.maxChannels);
    block.numSidechainChannels = 0;

    for (int i = 0; i < block.numChannels; ++i)
        juce::FloatVectorOperations::copy (channel.getChannel (i), context.channels[i], context.numSamples);

    if (context.sidechain != nullptr && instance.getRemote().getInfo().hasSidechain)
    {
        block.numSidechainChannels = std::min (context.numSidechainChannels, limits.maxSidechainChannels);

        for (int i = 0; i < block.numSidechainChannels; ++i)
            juce::FloatVectorOperations::copy (channel.getSidechain (i), context.sidechain[i], context.numSamples);
    }

    block.numEvents = 0;

    for (const auto& event : context.events)
    {
        if (block.numEvents >= limits.maxEvents)
            break;

        auto& midi = channel.getEvents()[block.numEvents];
        midi.sample = event.sample;
        midi.size = (std::uint8_t) toMidi (event, midi.bytes);

        if (midi.size > 0)
            ++block.numEvents;
    }

    // Values set while a block played (none, unless a render set them), then what the engine moves, at their samples.
    block.numChanges = 0;
    instance.takeChanges (block, channel.getChanges(), limits.maxChanges);

    for (const auto& change : context.changes)
    {
        if (block.numChanges >= limits.maxChanges)
            break;

        if (change.parameter < (int) ids.size())
            channel.getChanges()[block.numChanges++] = { change.parameter, change.sample, change.value };
    }

    block.tempo = context.tempo;
    block.beat = context.beat;
    block.sample = (std::int64_t) (context.beat * 60.0 / std::max (1.0, context.tempo) * rate);
    block.isPlaying = context.isPlaying ? 1 : 0;
    block.isRendering = context.isRendering ? 1 : 0;
    block.reset = resetWanted ? 1 : 0;

    if (channel.handOver())
    {
        resetWanted = false;
        return true;
    }

    instance.let();
    silence (context);
    return false;
}

int SandboxedProcessor::getLatencySamples() const
{
    return instance.getRemote().getLatencySamples();
}

double SandboxedProcessor::getTailSeconds() const
{
    return instance.getTailLengthSeconds();
}
} // namespace phazite
