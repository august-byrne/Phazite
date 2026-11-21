// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceRunner.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phazite::engine
{
std::optional<devices::NoteEvent> toNoteEvent (const juce::MidiMessage& message, int sample)
{
    using Type = devices::NoteEvent::Type;
    const auto channel = juce::jlimit (1, 16, message.getChannel()) - 1; // JUCE counts them from 1

    if (message.isNoteOn())
        return devices::NoteEvent { Type::noteOn, sample, message.getNoteNumber(), message.getFloatVelocity(), channel };

    if (message.isNoteOff()) // a note-on of velocity 0 too
        return devices::NoteEvent { Type::noteOff, sample, message.getNoteNumber(), message.getFloatVelocity(), channel };

    if (message.isAllNotesOff() || message.isAllSoundOff())
        return devices::NoteEvent { Type::allNotesOff, sample, 0, 0.0f };

    if (message.isPitchWheel())
        return devices::NoteEvent { Type::pitchBend, sample, 0, (float) (message.getPitchWheelValue() - 8192) / 8192.0f, channel };

    if (message.isController())
        return devices::NoteEvent { Type::controller, sample, message.getControllerNumber(), (float) message.getControllerValue() / 127.0f,
                                    channel };

    if (message.isAftertouch())
        return devices::NoteEvent { Type::aftertouch, sample, message.getNoteNumber(), (float) message.getAfterTouchValue() / 127.0f,
                                    channel };

    if (message.isChannelPressure())
        return devices::NoteEvent { Type::channelPressure, sample, 0, (float) message.getChannelPressureValue() / 127.0f, channel };

    if (message.isProgramChange())
        return devices::NoteEvent { Type::programChange, sample, message.getProgramChangeNumber(), 0.0f, channel };

    return {};
}

DeviceRunner::DeviceRunner (const devices::DeviceInfo& deviceInfo, std::unique_ptr<devices::Processor> dsp)
    : info (deviceInfo),
      processor (std::move (dsp))
{
    jassert (devices::isValid (info));

    for (const auto& description : info.parameters)
    {
        currentValues.push_back (description.defaultValue);
        targetValues.push_back (description.defaultValue);
        previousValues.push_back (description.defaultValue);
        baseValues.push_back (description.defaultValue);
        glides.push_back (description.unit != devices::Unit::choice && description.unit != devices::Unit::toggle);
        travels.push_back (devices::getTravel (description));
        offsets.push_back (0.0f);
    }

    // An effect's mix, which this applies (an instrument has none).
    if (info.kind == devices::DeviceKind::effect)
        mixIndex = devices::findParameter (info, devices::mixParameterID);

    // Room for whole blocks' changes, made now.
    if (processor->takesWholeBlocks())
    {
        for (auto* list : { &movedNow, &movedBefore, &stretchParameters })
            list->reserve (info.parameters.size());

        movedIn.assign (info.parameters.size(), 0);
        sentValues.assign (info.parameters.size(), 0.0f);
        changes.reserve ((size_t) maximumChanges);
    }

    if (isInstrument() && ! processor->playsChannelsApart())
        merged.resize ((size_t) maximumEvents);
}

void DeviceRunner::prepare (double rate, int maximumBlockSize)
{
    processor->prepare (rate, maximumBlockSize);
    sampleRate = rate;
    const auto stretch = std::max (maximumBlockSize, 512);
    stereo.setSize (2, stretch);
    stretchMixes.reserve ((size_t) (stretch / smoothingStep + 2));
    latencySamples = processor->getLatencySamples();
    scope.setSampleRate (rate);
    sleepAfterSamples = std::max ((int) (sleepAfterSeconds * rate), 2 * latencySamples);
    silentSamples = 0;
    asleep = false;
    heard = false;
    letGoOfNotes();
    hasPreviousValues = false;
    startedEarly = false;
    sleptValues.assign (processor->wakesWhenValuesChange() ? currentValues.size() : 0, 0.0f);
    valuesNow.assign (sleptValues.size(), 0.0f);

    if (mixIndex >= 0)
    {
        dry.setSize (2, stretch);
        appliedMix = currentValues[(size_t) mixIndex];

        for (auto& delay : dryDelays)
            delay.prepare (std::max (latencySamples, 1));
    }
}

void DeviceRunner::reset()
{
    processor->reset();
    silentSamples = 0;
    asleep = false;
    heard = false;
    letGoOfNotes();
    hasPreviousValues = false;

    for (auto& delay : dryDelays)
        delay.reset();
}

namespace
{
    // Whether samples are silent: every one a number, and none louder than silence. What isn't a number isn't silence: a
    // device asleep doesn't look at what comes in, so one that slept through it would let it by.
    bool isQuiet (const float* samples, int numSamples, float silence) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            if (! (std::abs (samples[i]) <= silence))
                return false;

        return true;
    }

    // Whether a buffer's channels are silent there.
    bool isQuiet (const juce::AudioBuffer<float>& buffer, int start, int numSamples, int firstChannel, int numChannels, float silence)
    {
        for (int channel = firstChannel; channel < firstChannel + numChannels; ++channel)
            if (! isQuiet (buffer.getReadPointer (channel, start), numSamples, silence))
                return false;

        return true;
    }

    // The same for the outputs its parts play into (a drum sampler's pads on tracks of their own), from where a block starts
    // in them.
    bool arePartsQuiet (std::span<const devices::PartOutput> parts, int start, int numSamples, float silence)
    {
        for (const auto& part : parts)
            for (const auto* channel : part.channels)
                if (channel != nullptr && ! isQuiet (channel + start, numSamples, silence))
                    return false;

        return true;
    }

    // Whether events start a note, which can sound from silence.
    bool startsNote (std::span<const devices::NoteEvent> events) noexcept
    {
        using Type = devices::NoteEvent::Type;
        return std::any_of (events.begin(), events.end(), [] (const devices::NoteEvent& event)
                            { return event.type == Type::noteOn || event.type == Type::portamentoOn; });
    }

    // The events before a sample.
    std::span<devices::NoteEvent> getEventsBefore (std::span<devices::NoteEvent> events, int sample) noexcept
    {
        size_t count = 0;

        while (count < events.size() && events[count].sample < sample)
            ++count;

        return events.first (count);
    }
} // namespace

void DeviceRunner::letGoOfNotes() noexcept
{
    held.fill (0);
    numHeld = 0;
    pedalsDown = 0;
    channelsHolding.fill (0);
}

std::span<devices::NoteEvent> DeviceRunner::mergeChannels (std::span<devices::NoteEvent> events) noexcept
{
    using Type = devices::NoteEvent::Type;

    if (merged.empty())
        return events;

    size_t count = 0;

    for (const auto& event : events)
    {
        if (count == merged.size())
            break;

        auto& holding = channelsHolding[(size_t) std::clamp (event.number, 0, 127)];
        const auto bit = (std::uint16_t) (1u << (event.channel & 15));

        if (event.type == Type::noteOn || event.type == Type::portamentoOn)
        {
            holding = (std::uint16_t) (holding | bit);
        }
        else if (event.type == Type::noteOff)
        {
            holding = (std::uint16_t) (holding & ~bit);

            if (holding != 0)
                continue;
        }
        else if (event.type == Type::allNotesOff)
        {
            channelsHolding.fill (0);
        }

        merged[count++] = event;
    }

    return { merged.data(), count };
}

void DeviceRunner::followNotes (std::span<const devices::NoteEvent> events) noexcept
{
    using Type = devices::NoteEvent::Type;
    constexpr int sustainPedal = 64;

    for (const auto& event : events)
    {
        auto& times = held[(size_t) std::clamp (event.number, 0, 127)];

        if ((event.type == Type::noteOn || event.type == Type::portamentoOn) && times < 255)
        {
            ++times;
            ++numHeld;
        }
        else if (event.type == Type::noteOff && times > 0)
        {
            --times;
            --numHeld;
        }
        else if (event.type == Type::allNotesOff)
        {
            letGoOfNotes();
        }
        else if (event.type == Type::controller && event.number == sustainPedal)
        {
            const auto bit = (std::uint16_t) (1u << (event.channel & 15));
            pedalsDown = (std::uint16_t) (event.value >= 0.5f ? (pedalsDown | bit) : (pedalsDown & ~bit));
        }
    }
}

DeviceRunner::Begun DeviceRunner::begin (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Block& in)
{
    // Each parameter's value now, an automated one's where the song is. A continuous one that moved since the last block
    // glides there over this one, in stretches of smoothingStep, rather than jumping, so values set by hand and modulation
    // don't step (docs/archive/performance-audit.md, A7); while the song plays, automation moves its parameters a stretch at a time.
    Begun begun;
    auto* plan = in.modulation;
    auto gliding = false;
    begun.automationMoves = in.beatsPerSample > 0.0 && ! in.automation.empty();

    // Nothing moved since the last block, which ended at its values: no value set by hand, no automation now or then, no
    // modulation, and no glide or stretch then. Its values are as these loops would make them again, which a device of
    // thousands of parameters (the drum sampler's) would go through each block for nothing.
    begun.settled =
        hasPreviousValues && ! in.valuesChanged && in.automation.empty() && ! lastMoved && (plan == nullptr || plan->routes.empty());

    if (! begun.settled)
    {
        for (size_t i = 0; i < targetValues.size(); ++i)
            targetValues[i] = devices::constrain (info.parameters[i], in.values[i]);

        for (const auto& automated : in.automation)
            targetValues[(size_t) automated.parameter] =
                devices::constrain (info.parameters[(size_t) automated.parameter], automated.curve->getValueAt (in.automationBeat));

        for (size_t i = 0; i < targetValues.size(); ++i)
        {
            currentValues[i] = baseValues[i] = targetValues[i];
            gliding = gliding || (glides[i] && hasPreviousValues && ! juce::exactlyEqual (previousValues[i], targetValues[i]));
        }
    }

    // Modulated, it plays in stretches too, each with its modulators where it is.
    if (plan != nullptr)
        startModulation (*plan);

    begun.inStretches = gliding || begun.automationMoves || (plan != nullptr && ! plan->routes.empty());
    lastMoved = begun.inStretches || ! in.automation.empty();
    lastSettled = begun.settled;
    begun.targetMix = mixIndex >= 0 ? targetValues[(size_t) mixIndex] : 1.0f;

    // Its first block since it was prepared or started from silence: its mix starts at its value, as its other parameters
    // do, rather than ramp there from the mix the device is made with (a render's first block, with all its effects).
    if (! hasPreviousValues)
        appliedMix = begun.targetMix;

    begun.isMixMoved = mixIndex >= 0
                       && ((plan != nullptr && plan->isModulated[(size_t) mixIndex])
                           || (begun.automationMoves
                               && std::any_of (in.automation.begin(), in.automation.end(),
                                               [this] (const Automated& automated) { return automated.parameter == mixIndex; })));
    begun.hasSidechain = in.hasSidechain && buffer.getNumChannels() >= 4;
    const auto isSynth = isInstrument();

    if (isSynth)
        followNotes (in.events);

    begun.events = isSynth ? mergeChannels (in.events) : in.events;

    if (isSynth) // its own channels, not a sidechain's
        for (int channel = 0; channel < std::min (2, buffer.getNumChannels()); ++channel)
            buffer.clear (channel, start, numSamples);

    // Nothing to play: no events, nothing waiting in the device (a pad its editor played), and silence in (on its own
    // channels and its sidechain's).
    begun.quietIn = in.events.empty() && ! processor->isWaitingToPlay()
                    && (isSynth || isQuiet (buffer, start, numSamples, 0, std::min (2, buffer.getNumChannels()), silence))
                    && (! begun.hasSidechain || isQuiet (buffer, start, numSamples, 2, 2, silence));

    // Asleep, it sleeps on while nothing comes in; but one that plays with the song wakes as the song plays. In the engine,
    // Play resets every runner (Engine::flushStrip()), which is what wakes it there; this is the backstop for a runner played
    // without that reset (a test's, or another engine's).
    auto sleeps = asleep && begun.quietIn && ! in.isRendering && ! (in.isPlaying && processor->playsWithTheSong());

    // One that a change of its values alone can make sound wakes for a change since it fell asleep, and stays awake from
    // there as it would for sound come in (nothing moved since the last block, settled, is no change).
    if (sleeps && ! begun.settled && ! sleptValues.empty() && valuesMovedInSleep (in, numSamples))
    {
        sleeps = false;
        silentSamples = 0;
    }

    if (sleeps)
    {
        for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
            buffer.clear (channel, start, numSamples);

        // Asleep, it plays nothing, but its parameters play on as they would, for what shows them (a knob's ring, a graph's
        // handles): moved by their modulation, at the block's last control point, as its last stretch would leave them
        // (known bug 171: they stood at their values set by hand).
        if (plan != nullptr && ! plan->routes.empty())
        {
            const auto sinceControlPoint = (int) (((in.sample % smoothingStep) + smoothingStep) % smoothingStep);
            const auto toControlPoint = smoothingStep - sinceControlPoint;
            const auto stretch = numSamples > toControlPoint ? 1 + (numSamples - toControlPoint - 1) / smoothingStep : 0;
            const auto done = stretch > 0 ? toControlPoint + (stretch - 1) * smoothingStep : 0;
            setStretchValues (in, begun.automationMoves, done == 0 ? -sinceControlPoint : done, stretch, done, numSamples - done,
                              numSamples, &plan->modulated); // those alone: the others are at their values already
        }

        if (! begun.settled)
            keepValues();

        begun.slept = true;
        return begun;
    }

    asleep = false;

    // Heard: what can sound came in, a note's start, sound, or what waits in the device to play. Not an event alone that
    // ends or bends what sounds (the all-notes-off of a stop, a note's end, a controller), which leaves a device that has
    // heard nothing with nothing to ring with (known bug 195: the stop of the song closed before reached the next one's
    // instruments, and each waited for its tail).
    if (! heard && ! begun.quietIn)
        heard = processor->isWaitingToPlay() || startsNote (in.events)
                || (! isSynth && ! isQuiet (buffer, start, numSamples, 0, std::min (2, buffer.getNumChannels()), silence))
                || (begun.hasSidechain && ! isQuiet (buffer, start, numSamples, 2, 2, silence));

    blocksWorked.store (blocksWorked.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    // Taken whole, it gets the values of what moves as changes: those it moves now, and those it moved in the last block.
    if (processor->takesWholeBlocks())
        startWholeBlock (in, begun.isMixMoved);

    // What comes in, for the editor's displays: an effect's sound (an instrument's channels were cleared for it to play into).
    if (! isSynth)
        scope.pushInput (buffer.getReadPointer (0, start), buffer.getReadPointer (std::min (1, buffer.getNumChannels() - 1), start),
                         numSamples);

    return begun;
}

void DeviceRunner::end (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Block& in, const Begun& begun)
{
    scope.pushOutput (buffer.getReadPointer (0, start), buffer.getReadPointer (std::min (1, buffer.getNumChannels() - 1), start),
                      numSamples);

    // An automated parameter goes on from where its automation left it.
    if (begun.automationMoves)
        for (const auto& automated : in.automation)
            targetValues[(size_t) automated.parameter] = baseValues[(size_t) automated.parameter];

    if (! begun.settled)
        keepValues();

    // Silent in and out long enough, its parts' own outputs too (a pad on a track of its own rings on after the sampler's
    // own sound has ended): asleep from the next block. Long enough is as long as the tail it reports, as it is now, too:
    // a delay is silent between its echoes, and asleep it would never play the next; but not, while the song is stopped, for
    // one that has heard nothing since it was prepared or reset, which holds nothing to ring with.
    if (begun.quietIn && isQuiet (buffer, start, numSamples, 0, std::min (2, buffer.getNumChannels()), silence)
        && arePartsQuiet (in.partOutputs, in.outputOffset, numSamples, silence))
    {
        silentSamples = (int) std::min<std::int64_t> ((std::int64_t) silentSamples + numSamples, maximumSilentSamples);
        asleep = silentSamples >= sleepAfterSamples && ! in.isRendering && processor->sleepsOnSilence()
                 && ! (in.isPlaying && processor->playsWithTheSong())
                 && ((! heard && ! in.isPlaying) || (double) silentSamples >= processor->getTailSeconds() * sampleRate) && numHeld == 0
                 && pedalsDown == 0;

        // Falling asleep, the values it was given last, which a device that wakes when they change is woken for.
        if (asleep && ! sleptValues.empty())
            std::copy (currentValues.begin(), currentValues.end(), sleptValues.begin());
    }
    else
    {
        silentSamples = 0;
    }

    for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, start, numSamples);
}

devices::ProcessContext DeviceRunner::makeContext (const Block& in) const noexcept
{
    devices::ProcessContext block;
    block.parameters = currentValues;
    block.partOutputs = in.partOutputs;
    block.outputOffset = in.outputOffset;
    block.isPlaying = in.isPlaying;
    block.isRendering = in.isRendering;
    block.tempo = in.tempo;
    block.beat = in.beat;
    block.bars = in.bars;
    block.songSample = in.songSample;
    block.timeSignatureNumerator = in.timeSignatureNumerator;
    block.timeSignatureDenominator = in.timeSignatureDenominator;
    block.barLength = in.barLength;
    block.barStart = in.barStart;
    return block;
}

void DeviceRunner::start (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Block& in)
{
    // An instrument that takes its block whole in one call, on its own two channels, without a sidechain: the block as
    // process() would hand it over, which then only takes what it played.
    startedEarly = processor->startsEarly() && isInstrument() && buffer.getNumChannels() >= 2 && ! in.hasSidechain
                   && numSamples <= stereo.getNumSamples();

    if (! startedEarly)
        return;

    early = begin (buffer, start, numSamples, in);

    if (early.slept)
        return;

    const auto sinceControlPoint = (int) (((in.sample % smoothingStep) + smoothingStep) % smoothingStep);
    gatherChanges (in, early.automationMoves, false, sinceControlPoint, 0, 0, numSamples, numSamples);
    float* channels[] = { buffer.getWritePointer (0, start), buffer.getWritePointer (1, start) };
    auto block = makeContext (in);
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = numSamples;
    block.events = getEventsBefore (early.events, numSamples);
    block.changes = changes;
    processor->start (block);
}

void DeviceRunner::process (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Block& in)
{
    // A block started early (start()): only what it played to take, into its channels.
    if (std::exchange (startedEarly, false))
    {
        if (early.slept)
            return;

        // Its channels cleared again, as they were for it to play into (what the strip put there since is no sound of
        // an instrument's).
        for (int channel = 0; channel < 2; ++channel)
            buffer.clear (channel, start, numSamples);

        float* channels[] = { buffer.getWritePointer (0, start), buffer.getWritePointer (1, start) };
        auto block = makeContext (in);
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = numSamples;
        block.events = getEventsBefore (early.events, numSamples);
        block.changes = changes;
        processor->process (block);
        end (buffer, start, numSamples, in, early);
        return;
    }

    const auto begun = begin (buffer, start, numSamples, in);

    if (begun.slept)
        return;

    const auto whole = processor->takesWholeBlocks();
    const auto isMono = buffer.getNumChannels() < 2;
    const auto events = begun.events;
    const auto numEvents = (int) events.size();
    auto block = makeContext (in);
    const float* sidechain[2] = {};

    if (begun.hasSidechain)
    {
        block.sidechain = sidechain;
        block.numSidechainChannels = 2;
    }

    // What comes in, copied for the mix, which the device may turn as it turns its own sound (devices::ProcessContext::dry).
    float* dryChannels[2] = {};

    if (mixIndex >= 0)
    {
        dryChannels[0] = dry.getWritePointer (0);
        dryChannels[1] = dry.getWritePointer (1);
        block.dry = dryChannels;
    }

    // A stretch at a time, as long as the scratch buffers prepared beforehand, the first of smoothingStep ending on the
    // song's next control point; or taken whole, as long as those buffers, each ending on a control point but the block's
    // last. A mono channel plays in stereo, through a scratch buffer, and gets the average of the two sides back.
    int nextEvent = 0;
    const auto sinceControlPoint = (int) (((in.sample % smoothingStep) + smoothingStep) % smoothingStep);
    const auto toControlPoint = smoothingStep - sinceControlPoint;
    const auto longest = stereo.getNumSamples();

    for (int done = 0, stretch = 0; done < numSamples;)
    {
        const auto length =
            whole ? (numSamples - done <= longest ? numSamples - done : longest - (done + sinceControlPoint + longest) % smoothingStep)
                  : std::min (numSamples - done, begun.inStretches ? (done == 0 ? toControlPoint : smoothingStep) : longest);

        if (whole)
        {
            stretch = gatherChanges (in, begun.automationMoves, begun.isMixMoved, sinceControlPoint, stretch, done, length, numSamples);
        }
        else
        {
            if (begun.inStretches)
                setStretchValues (in, begun.automationMoves, done == 0 ? -sinceControlPoint : done, stretch, done, length, numSamples);

            ++stretch;
        }

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
        block.outputOffset = in.outputOffset + done;
        // Where the song is as this stretch starts, which a synced LFO follows: in the bar after the block's first once it has
        // crossed into it.
        block.beat = in.beat + done * in.beatsPerSample;
        block.bars = in.bars + done * in.barsPerSample;
        block.barStart = in.barStart + (std::floor (block.bars) - std::floor (in.bars)) * in.barLength;
        block.songSample = in.songSample + (in.isPlaying ? done : 0);
        block.events = events.subspan ((size_t) firstEvent, (size_t) (nextEvent - firstEvent));
        block.changes = changes;

        if (begun.hasSidechain)
        {
            sidechain[0] = buffer.getReadPointer (2, start + done);
            sidechain[1] = buffer.getReadPointer (3, start + done);
        }

        processor->process (block);

        // Its mix, unless it plays something else for now (a compressor's key, to listen to); modulated, as it is now (taken
        // whole, as it was in each stretch).
        if (mixIndex >= 0 && whole && begun.isMixMoved && ! processor->overridesMix())
        {
            for (const auto& part : stretchMixes)
            {
                float* const wet[] = { channels[0] + part.start, channels[1] + part.start };
                mixWithDry (wet, part.start, part.length, part.mix, part.ramp);
            }
        }
        else if (mixIndex >= 0)
        {
            // In stretches, over the whole of each up to its control point, where a block's end cuts it too.
            const auto ramp = begun.inStretches ? (done == 0 ? toControlPoint : smoothingStep) : length;
            mixWithDry (channels, 0, length,
                        processor->overridesMix() ? 1.0f
                        : begun.isMixMoved        ? currentValues[(size_t) mixIndex]
                                                  : begun.targetMix,
                        std::max (ramp, length));
        }

        if (isMono)
        {
            auto* mono = buffer.getWritePointer (0, start + done);

            for (int i = 0; i < length; ++i)
                mono[i] = 0.5f * (channels[0][i] + channels[1][i]);
        }

        done += length;
    }

    end (buffer, start, numSamples, in, begun);
}

void DeviceRunner::keepValues() noexcept
{
    std::copy (targetValues.begin(), targetValues.end(), previousValues.begin());
    hasPreviousValues = true;
}

bool DeviceRunner::valuesMovedInSleep (const Block& in, int numSamples) noexcept
{
    // The values it would be given in the block, a stretch at a time as setStretchValues() works them out: each at its value
    // as the block starts (begin() has them), an automated one at its curve's value on each stretch's control point while the
    // song plays, and a modulated one moved by its sources there. On every control point, not only the block's start: a lane
    // that switches inside the block (where an automation that lines up with a late sound switches) wakes it for that block,
    // where a render plays the switch.
    const auto* plan = in.modulation;
    const auto modulated = plan != nullptr && ! plan->routes.empty();
    const auto automationMoves = in.beatsPerSample > 0.0 && ! in.automation.empty();
    const auto sinceControlPoint = (int) (((in.sample % smoothingStep) + smoothingStep) % smoothingStep);
    const auto toControlPoint = smoothingStep - sinceControlPoint;

    for (int done = 0, stretch = 0;; ++stretch)
    {
        const auto length = std::min (numSamples - done, done == 0 ? toControlPoint : smoothingStep);
        std::copy (targetValues.begin(), targetValues.end(), valuesNow.begin());

        if (automationMoves)
            for (const auto& automated : in.automation)
                valuesNow[(size_t) automated.parameter] = devices::constrain (
                    info.parameters[(size_t) automated.parameter],
                    automated.curve->getValueAt (in.automationBeat + (done == 0 ? -sinceControlPoint : done) * in.beatsPerSample));

        if (modulated)
        {
            const auto along = numSamples > 0 ? (float) (done + length) / (float) numSamples : 0.0f;

            for (const auto parameter : plan->modulated)
                offsets[(size_t) parameter] = 0.0f;

            for (const auto& route : plan->routes)
            {
                const auto& source = plan->sources[(size_t) route.source];

                if (source.isOn)
                    offsets[(size_t) route.parameter] +=
                        route.amount
                        * (! source.points.empty() ? source.points[std::min ((size_t) stretch, source.points.size() - 1)]
                                                   : source.previous + (source.now - source.previous) * along);
            }

            for (const auto parameter : plan->modulated)
                valuesNow[(size_t) parameter] = devices::modulate (info.parameters[(size_t) parameter], travels[(size_t) parameter],
                                                                   valuesNow[(size_t) parameter], offsets[(size_t) parameter]);
        }

        if (! std::equal (valuesNow.begin(), valuesNow.end(), sleptValues.begin(),
                          [] (float a, float b) { return juce::exactlyEqual (a, b); })
            && processor->wakesFor (valuesNow))
            return true;

        done += length;

        // Nothing moves within the block, or it is played through.
        if ((! automationMoves && ! modulated) || length <= 0 || done >= numSamples)
            return false;
    }
}

void DeviceRunner::startModulation (Modulation& plan) noexcept
{
    // Each source glides from where it was as the last block started to where it is as this one does.
    for (auto& source : plan.sources)
    {
        source.previous = source.started ? source.now : source.value;
        source.now = source.value;
        source.started = true;
    }
}

void DeviceRunner::setStretchValues (const Block& in, bool automationMoves, int controlPoint, int stretch, int done, int length,
                                     int numSamples, const std::vector<int>* only)
{
    const auto forEach = [this, only] (auto&& apply)
    {
        if (only != nullptr)
            for (const auto parameter : *only)
                apply ((size_t) parameter);
        else
            for (size_t i = 0; i < currentValues.size(); ++i)
                apply (i);
    };

    // A continuous parameter that moved since the last block glides there, as far as the stretch's end; an automated one
    // is at its curve's value on the stretch's control point (where it starts, or before the block for the rest of the
    // last block's last stretch), whatever the block's size.
    const auto along = (float) (done + length) / (float) numSamples;

    forEach (
        [this, along] (size_t i)
        {
            currentValues[i] =
                glides[i] && hasPreviousValues ? previousValues[i] + (targetValues[i] - previousValues[i]) * along : targetValues[i];
        });

    if (automationMoves)
        for (const auto& automated : in.automation)
            currentValues[(size_t) automated.parameter] =
                devices::constrain (info.parameters[(size_t) automated.parameter],
                                    automated.curve->getValueAt (in.automationBeat + controlPoint * in.beatsPerSample));

    forEach ([this] (size_t i) { baseValues[i] = currentValues[i]; });
    const auto* plan = in.modulation;

    if (plan == nullptr)
        return;

    // Then a modulated one is moved by its sources where the stretch is: by their values on its control point, or, for a
    // source without them, by its value gliding as far as the stretch's end.
    forEach ([this] (size_t i) { offsets[i] = 0.0f; });

    for (const auto& route : plan->routes)
    {
        const auto& source = plan->sources[(size_t) route.source];

        if (! source.isOn)
            continue;

        const auto value = ! source.points.empty() ? source.points[std::min ((size_t) stretch, source.points.size() - 1)]
                                                   : source.previous + (source.now - source.previous) * along;
        offsets[(size_t) route.parameter] += route.amount * value;
    }

    forEach (
        [this, plan] (size_t i)
        {
            if (plan->isModulated[i])
                currentValues[i] = devices::modulate (info.parameters[i], travels[i], currentValues[i], offsets[i]);
        });
}

void DeviceRunner::startWholeBlock (const Block& in, bool isMixMoved) noexcept
{
    // What automation and modulation move in this block (not the mix, which the runner applies), and what they moved in
    // the last; and what of them each stretch's values are worked out for.
    std::swap (movedNow, movedBefore);
    movedNow.clear();
    ++blocksStarted;

    const auto add = [this] (int parameter)
    {
        if (parameter != mixIndex && movedIn[(size_t) parameter] != blocksStarted)
        {
            movedIn[(size_t) parameter] = blocksStarted;
            movedNow.push_back (parameter);
        }
    };

    for (const auto& automated : in.automation)
        add (automated.parameter);

    if (in.modulation != nullptr)
        for (const auto& route : in.modulation->routes)
            add (route.parameter);

    stretchParameters.assign (movedNow.begin(), movedNow.end());

    if (isMixMoved)
        stretchParameters.push_back (mixIndex);
}

int DeviceRunner::gatherChanges (const Block& in, bool automationMoves, bool isMixMoved, int sinceControlPoint, int stretch, int from,
                                 int length, int numSamples)
{
    changes.clear();
    stretchMixes.clear();

    const auto add = [this] (int parameter, int sample, float value)
    {
        if (changes.size() < changes.capacity())
            changes.push_back ({ parameter, sample, value });
    };

    // As the block starts, each moved in the last block and no more goes back to its value set by hand.
    if (from == 0)
        for (const auto parameter : movedBefore)
            if (movedIn[(size_t) parameter] != blocksStarted)
                add (parameter, 0, targetValues[(size_t) parameter]);

    if (stretchParameters.empty())
        return stretch + 1;

    // Each moved at its value on each control point, as a stretch would play it: at the block's start whatever it was (so
    // that nothing else keeps it from there), then where it changes.
    const auto toControlPoint = smoothingStep - sinceControlPoint;

    for (int done = from; done < from + length; ++stretch)
    {
        const auto stretchLength = std::min (from + length - done, done == 0 ? toControlPoint : smoothingStep);
        setStretchValues (in, automationMoves, done == 0 ? -sinceControlPoint : done, stretch, done, stretchLength, numSamples,
                          &stretchParameters);

        for (const auto parameter : movedNow)
        {
            const auto value = currentValues[(size_t) parameter];

            if (done == 0 || ! juce::exactlyEqual (value, sentValues[(size_t) parameter]))
                add (parameter, done - from, value);

            sentValues[(size_t) parameter] = value;
        }

        if (isMixMoved)
            stretchMixes.push_back ({ done - from, stretchLength, currentValues[(size_t) mixIndex],
                                      std::max (stretchLength, done == 0 ? toControlPoint : smoothingStep) });

        done += stretchLength;
    }

    return stretch;
}

void DeviceRunner::mixWithDry (float* const* wet, int dryStart, int length, float targetMix, int ramp)
{
    // The dry sound, delayed as much as the effect delays its own, so that the two line up; unless the effect has delayed it
    // itself, through the filters that delay its own (devices::Processor::delaysDry()).
    if (latencySamples > 0 && ! processor->delaysDry())
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            auto* samples = dry.getWritePointer (channel, dryStart);
            auto& delay = dryDelays[(size_t) channel];

            for (int i = 0; i < length; ++i)
            {
                delay.push (samples[i]);
                samples[i] = delay.read (latencySamples);
            }
        }
    }

    // Ramped from the mix the last stretch ended with, so a moving knob doesn't click, over the whole ramp: a stretch a block's
    // end cuts ends where its ramp has reached, and the rest of it goes on from there. Nothing to do at 100 %.
    const auto from = appliedMix;
    const auto rampLength = (float) std::max (ramp, length);
    appliedMix = from + (targetMix - from) * (float) length / rampLength;

    if (from >= 1.0f && targetMix >= 1.0f)
        return;

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto* drySamples = dry.getReadPointer (channel, dryStart);

        for (int i = 0; i < length; ++i)
        {
            const auto mix = from + (targetMix - from) * (float) (i + 1) / rampLength;
            wet[channel][i] = mix * wet[channel][i] + (1.0f - mix) * drySamples[i];
        }
    }
}
} // namespace phazite::engine
