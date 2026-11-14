// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SandboxedProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
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
            case Type::allNotesOff: // which the helper ends the notes it gave the plug-in for (host/core/Instance.h)
                bytes[0] = 0xb0;
                bytes[1] = 123;
                bytes[2] = 0;
                return 3;
            case Type::slide: // MIDI has no slide of one note to another
                return 0;
        }

        return 0;
    }

    constexpr size_t maximumWaiting = 1024; // the events kept for the next block at most

    // Whether every sample is a number: none an infinity or a NaN, whose exponents' bits are all set.
    bool areNumbers (const float* samples, int numSamples) noexcept
    {
        constexpr std::uint32_t exponent = 0x7f800000u;
        auto others = false;

        for (int i = 0; i < numSamples; ++i)
        {
            std::uint32_t bits;
            std::memcpy (&bits, samples + i, sizeof (bits));
            others |= (bits & exponent) == exponent;
        }

        return ! others;
    }

    bool isNoteOn (const devices::NoteEvent& event) noexcept
    {
        return event.type == devices::NoteEvent::Type::noteOn || event.type == devices::NoteEvent::Type::portamentoOn;
    }
} // namespace

SandboxedProcessor::SandboxedProcessor (std::shared_ptr<SandboxedInstance> sandboxed)
    : kept (std::move (sandboxed)),
      instance (*kept),
      typeID ("plugin"),
      name (instance.getDescription().name.toStdString())
{
    // An instrument as its description says, which the song files it by: not by its buses, as an instrument may take sound
    // in too (a vocoder's, or a sidechain as its main input).
    info.typeID = typeID;
    info.name = name;
    info.kind = instance.getDescription().isInstrument ? devices::DeviceKind::instrument : devices::DeviceKind::effect;

    // The plug-in's parameters, by their own IDs in their places, then an effect's mix with what came in, which the engine
    // applies (an instrument has none). The names and IDs are made first: the descriptions point into them.
    const auto& own = instance.getRemote().getInfo().parameters;

    for (size_t i = 0; i < own.size(); ++i)
    {
        ids.push_back (own[i].id.toStdString());
        names.push_back (own[i].name.toStdString());
    }

    for (size_t i = 0; i < own.size(); ++i)
        parameters.push_back ({ .id = ids[i], .name = names[i], .defaultValue = juce::jlimit (0.0f, 1.0f, own[i].defaultValue) });

    if (info.kind == devices::DeviceKind::effect)
        parameters.push_back ({ .id = devices::mixParameterID, .name = "Mix", .defaultValue = 1.0f });

    info.parameters = parameters;
    waiting.reserve (maximumWaiting);
}

int SandboxedProcessor::findParameter (const juce::String& ownID) const
{
    return instance.findParameter (ownID);
}

void SandboxedProcessor::prepare (double sampleRate, int maximumBlockSize)
{
    rate = sampleRate;
    maximumBlock = std::max (1, maximumBlockSize);
    instance.prepareToPlay (sampleRate, maximumBlockSize);
    fadeStep = 1.0f / (float) std::max (1, juce::roundToInt (fadeSeconds * sampleRate));
    heard = instance.hasCrashed() ? 0.0f : 1.0f; // nothing to fade out of one that plays nothing from the start

    // An effect's input, for as long as it delays its sound and a block more, to fade out from.
    if (info.kind == devices::DeviceKind::effect)
    {
        latency = std::max (0, getLatencySamples());
        history.setSize (2, latency + std::max (maximumBlock, 512) + 1);
        history.clear();
        historyAt = 0;
    }
}

void SandboxedProcessor::reset()
{
    // What waited for a block was of the sound before, which the plug-in, reset, plays no more of; nor fades out from.
    resetWanted = true;
    waiting.clear();
    history.clear();
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

    remember (context);

    if (handOver (context))
        collect (context, juce::Time::getHighResolutionTicks());
}

void SandboxedProcessor::remember (const devices::ProcessContext& context) noexcept
{
    const auto size = history.getNumSamples();

    if (size == 0 || context.numChannels <= 0)
        return;

    const auto numSamples = std::min (context.numSamples, size);

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto* input = context.channels[std::min (channel, context.numChannels - 1)];
        auto* remembered = history.getWritePointer (channel);

        for (int i = 0; i < numSamples; ++i)
            remembered[(historyAt + i) % size] = input[i];
    }

    historyAt = (historyAt + numSamples) % size;
}

void SandboxedProcessor::silence (const devices::ProcessContext& context) noexcept
{
    // An instrument plays nothing. An effect's input, as late as the plug-in would have played it (where its delay of it
    // would leave it), faded out from as much of the plug-in's sound as was heard, then nothing: never what came in, dry.
    const auto size = history.getNumSamples();
    const auto numSamples = std::min (context.numSamples, size);
    auto level = heard;

    for (int channel = 0; channel < context.numChannels; ++channel)
    {
        auto* samples = context.channels[channel];
        level = heard;

        if (info.kind == devices::DeviceKind::effect && size > 0 && level > 0.0f)
        {
            const auto* remembered = history.getReadPointer (std::min (channel, 1));
            const auto delayed = std::min (latency, size - numSamples);
            const auto first = historyAt - numSamples - delayed + 2 * size; // remember() has had this block

            for (int i = 0; i < numSamples; ++i)
            {
                level = std::max (0.0f, level - fadeStep);
                samples[i] = remembered[(first + i) % size] * level;
            }

            juce::FloatVectorOperations::clear (samples + numSamples, context.numSamples - numSamples);
        }
        else
        {
            level = 0.0f;
            juce::FloatVectorOperations::clear (samples, context.numSamples);
        }
    }

    heard = context.numChannels > 0 ? level : 0.0f;
}

void SandboxedProcessor::fadeIn (const devices::ProcessContext& context) noexcept
{
    if (heard >= 1.0f)
        return;

    auto level = heard;

    for (int channel = 0; channel < context.numChannels; ++channel)
    {
        auto* samples = context.channels[channel];
        level = heard;

        for (int i = 0; i < context.numSamples && level < 1.0f; ++i)
        {
            level = std::min (1.0f, level + fadeStep);
            samples[i] *= level;
        }
    }

    if (context.numChannels > 0)
        heard = level;
}

void SandboxedProcessor::skip (const devices::ProcessContext& context) noexcept
{
    keep (context.events);
    silence (context);
}

void SandboxedProcessor::keep (std::span<const devices::NoteEvent> events) noexcept
{
    using Type = devices::NoteEvent::Type;

    for (auto event : events)
    {
        if (event.type == Type::slide) // nothing to a plug-in
            continue;

        event.sample = 0;

        if (event.type == Type::noteOff)
        {
            // A note that began among them: neither its start nor its end is played.
            const auto last = std::find_if (waiting.rbegin(), waiting.rend(), [&event] (const devices::NoteEvent& other)
                                            { return (isNoteOn (other) || other.type == Type::noteOff) && other.number == event.number; });

            if (last != waiting.rend() && isNoteOn (*last))
            {
                waiting.erase (std::next (last).base());
                continue;
            }
        }
        else if (event.type == Type::allNotesOff)
        {
            // Every note among them ends with it.
            std::erase_if (waiting, [] (const devices::NoteEvent& other)
                           { return isNoteOn (other) || other.type == Type::noteOff || other.type == Type::allNotesOff; });
        }

        // No room for more: every note ends, rather than one that sounds on for the end it lost.
        if (waiting.size() >= maximumWaiting)
        {
            waiting.clear();
            event = { Type::allNotesOff, 0, 0, 0.0f };
        }

        waiting.push_back (event);
    }
}

bool SandboxedProcessor::waitInRender() noexcept
{
    // For as long as the plug-in takes, a little at a time: until its helper has gone, or it has kept the block so long
    // that it hangs, which its helper is ended for, or the render is cancelled.
    const auto limit = (std::int64_t) instance.getRemote().getHelper().getAudioHangMilliseconds() * renderHangFactor * 1'000'000;

    switch (instance.waitForAnswer (limit, &renderCancelled))
    {
        case SandboxedInstance::Answer::played:
            return true;
        case SandboxedInstance::Answer::cancelled:
            return false;
        case SandboxedInstance::Answer::gone:
        case SandboxedInstance::Answer::hung:
            break;
    }

    failedInRender.store (true, std::memory_order_release);
    return false;
}

void SandboxedProcessor::collect (const devices::ProcessContext& context, std::int64_t since) noexcept
{
    auto& channel = instance.getRemote().getChannel();
    auto played = false;

    if (context.isRendering)
    {
        played = waitInRender();
    }
    else
    {
        // As long as a block's own length in real time from when it was handed over (a millisecond at least).
        const auto waited =
            (std::int64_t) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - since) * 1.0e9);
        const auto deadline =
            std::max<std::int64_t> (0, SandboxedInstance::getDeadlineNanoseconds (context.numSamples, maximumBlock, rate) - waited);
        played = channel.waitForAnswer (0, deadline) == sandbox::BlockChannel::Played::done;
    }

    // What it played, as it was handed over: by the channels and samples Phazite handed it, never by what the helper's side
    // of the memory says now, which a helper gone wrong could have left anything in (a block that says otherwise is as one
    // it didn't play); unless some of it isn't a number, which every device after it would keep, which is as one it didn't
    // play too.
    if (played)
    {
        const auto& block = channel.getInfo();
        played = block.numChannels == handedChannels && block.numSamples == context.numSamples;

        for (int i = 0; i < handedChannels && played; ++i)
            played = areNumbers (channel.getChannel (i), context.numSamples);

        for (int i = 0; i < handedChannels && played; ++i)
            juce::FloatVectorOperations::copy (context.channels[i], channel.getChannel (i), context.numSamples);
    }

    instance.let();

    if (played)
        fadeIn (context);
    else
        silence (context);
}

bool SandboxedProcessor::handOver (const devices::ProcessContext& context) noexcept
{
    // The channel: a render waits for it, the audio thread never does (it skips the block while a render has it).
    if (context.isRendering)
    {
        instance.hold();

        // A block of live playback the helper is still on (late) is waited for, as a render's own would be; then the
        // channel prepared for the rate the render plays at, where it waited for that block to be.
        if (instance.getRemote().getChannel().isBusy() && ! instance.hasCrashed() && ! waitInRender())
        {
            instance.let();
            skip (context);
            return false;
        }

        instance.finishPreparing();
    }
    else if (! instance.tryHold())
    {
        skip (context);
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

    // Its helper gone: it plays nothing more, so nothing waits for it; a render it is in says so.
    if (instance.hasCrashed())
    {
        if (context.isRendering)
            failedInRender.store (true, std::memory_order_release);

        instance.let();
        waiting.clear();
        silence (context);
        return false;
    }

    // Still on a block, too long a block for its channel, or its channel waiting for the helper to give a block back before
    // it is prepared for another rate.
    if (channel.isBusy() || context.numSamples > limits.maxSamples || instance.isPreparing())
    {
        instance.let();
        skip (context);
        return false;
    }

    auto& block = channel.getInfo();
    handedChannels = std::min (context.numChannels, limits.maxChannels);
    block.numSamples = context.numSamples;
    block.numChannels = handedChannels;
    block.numSidechainChannels = 0;

    for (int i = 0; i < handedChannels; ++i)
        juce::FloatVectorOperations::copy (channel.getChannel (i), context.channels[i], context.numSamples);

    if (context.sidechain != nullptr && instance.getRemote().getInfo().hasSidechain)
    {
        block.numSidechainChannels = std::min (context.numSidechainChannels, limits.maxSidechainChannels);

        for (int i = 0; i < block.numSidechainChannels; ++i)
            juce::FloatVectorOperations::copy (channel.getSidechain (i), context.sidechain[i], context.numSamples);
    }

    // The events that waited for a block, at its start, then its own; those there is no room for wait for the next.
    block.numEvents = 0;

    const auto send = [&block, &channel, &limits] (const devices::NoteEvent& event, int sample) noexcept
    {
        if (block.numEvents >= limits.maxEvents)
            return false;

        auto& midi = channel.getEvents()[block.numEvents];
        midi.sample = sample;
        midi.size = (std::uint8_t) toMidi (event, midi.bytes);

        if (midi.size > 0)
            ++block.numEvents;

        return true;
    };

    size_t sentWaiting = 0, sentNow = 0;

    while (sentWaiting < waiting.size() && send (waiting[sentWaiting], 0))
        ++sentWaiting;

    if (sentWaiting == waiting.size())
        while (sentNow < context.events.size() && send (context.events[sentNow], context.events[sentNow].sample))
            ++sentNow;

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

    // Where the song is, as a plug-in's playhead has it: in quarter notes (its ppqPosition) and in samples, the time
    // signature there, and the bar it is in: where it starts and which it is.
    block.tempo = context.tempo;
    block.beat = context.beat;
    block.sample = context.songSample;
    block.timeSignatureNumerator = context.timeSignatureNumerator;
    block.timeSignatureDenominator = context.timeSignatureDenominator;
    block.barStart = context.barStart;
    block.barCount = (std::int64_t) std::floor (context.bars);
    block.barLength = context.barLength;
    block.isPlaying = context.isPlaying ? 1 : 0;
    block.isRendering = context.isRendering ? 1 : 0;
    block.reset = resetWanted ? 1 : 0;

    if (channel.handOver())
    {
        resetWanted = false;
        waiting.erase (waiting.begin(), waiting.begin() + (std::ptrdiff_t) sentWaiting);
        keep (context.events.subspan (sentNow));
        return true;
    }

    instance.let();
    skip (context);
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
