// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceRunner.h"

#include <algorithm>
#include <utility>

namespace phazite::engine
{
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
        ranges.push_back (devices::getRange (description));
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
    hasPreviousValues = false;
    startedEarly = false;

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
    hasPreviousValues = false;

    for (auto& delay : dryDelays)
        delay.reset();
}

namespace
{
    // Whether a buffer's channels are silent there.
    bool isQuiet (const juce::AudioBuffer<float>& buffer, int start, int numSamples, int firstChannel, int numChannels, float silence)
    {
        for (int channel = firstChannel; channel < firstChannel + numChannels; ++channel)
            if (buffer.getMagnitude (channel, start, numSamples) > silence)
                return false;

        return true;
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

DeviceRunner::Begun DeviceRunner::begin (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Block& in)
{
    // Each parameter's value now, an automated one's where the song is. A continuous one that moved since the last block
    // glides there over this one, in stretches of smoothingStep, rather than jumping, so values set by hand and modulation
    // don't step (docs/performance-audit.md, A7); while the song plays, automation moves its parameters a stretch at a time.
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
    begun.isMixMoved = mixIndex >= 0
                       && ((plan != nullptr && plan->isModulated[(size_t) mixIndex])
                           || (begun.automationMoves
                               && std::any_of (in.automation.begin(), in.automation.end(),
                                               [this] (const Automated& automated) { return automated.parameter == mixIndex; })));
    begun.hasSidechain = in.hasSidechain && buffer.getNumChannels() >= 4;
    const auto isSynth = isInstrument();

    if (isSynth) // its own channels, not a sidechain's
        for (int channel = 0; channel < std::min (2, buffer.getNumChannels()); ++channel)
            buffer.clear (channel, start, numSamples);

    // Nothing to play: no events, nothing waiting in the device (a pad its editor played), and silence in (on its own
    // channels and its sidechain's).
    begun.quietIn = in.events.empty() && ! processor->isWaitingToPlay()
                    && (isSynth || isQuiet (buffer, start, numSamples, 0, std::min (2, buffer.getNumChannels()), silence))
                    && (! begun.hasSidechain || isQuiet (buffer, start, numSamples, 2, 2, silence));

    if (asleep && begun.quietIn && ! in.isRendering)
    {
        for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
            buffer.clear (channel, start, numSamples);

        if (! begun.settled)
            keepValues();

        begun.slept = true;
        return begun;
    }

    asleep = false;

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

    // Silent in and out long enough: asleep from the next block.
    if (begun.quietIn && isQuiet (buffer, start, numSamples, 0, std::min (2, buffer.getNumChannels()), silence))
    {
        silentSamples = std::min (silentSamples + numSamples, sleepAfterSamples);
        asleep = silentSamples >= sleepAfterSamples && ! in.isRendering && processor->sleepsOnSilence();
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
    block.events = getEventsBefore (in.events, numSamples);
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
        block.events = getEventsBefore (in.events, numSamples);
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
    const auto numEvents = (int) in.events.size();
    auto block = makeContext (in);
    const float* sidechain[2] = {};

    if (begun.hasSidechain)
    {
        block.sidechain = sidechain;
        block.numSidechainChannels = 2;
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

        while (nextEvent < numEvents && in.events[(size_t) nextEvent].sample < done + length)
            in.events[(size_t) nextEvent++].sample -= done;

        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.outputOffset = in.outputOffset + done;
        block.events = in.events.subspan ((size_t) firstEvent, (size_t) (nextEvent - firstEvent));
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
                mixWithDry (wet, part.start, part.length, part.mix);
            }
        }
        else if (mixIndex >= 0)
        {
            mixWithDry (channels, 0, length,
                        processor->overridesMix() ? 1.0f
                        : begun.isMixMoved        ? currentValues[(size_t) mixIndex]
                                                  : begun.targetMix);
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
                currentValues[i] = devices::modulate (info.parameters[i], ranges[i], currentValues[i], offsets[i]);
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
            stretchMixes.push_back ({ done - from, stretchLength, currentValues[(size_t) mixIndex] });

        done += stretchLength;
    }

    return stretch;
}

void DeviceRunner::mixWithDry (float* const* wet, int dryStart, int length, float targetMix)
{
    // The dry sound, delayed as much as the effect delays its own, so that the two line up.
    if (latencySamples > 0)
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

    // Ramped from the mix the last stretch ended with, so a moving knob doesn't click; nothing to do at 100 %.
    const auto from = appliedMix;
    appliedMix = targetMix;

    if (from >= 1.0f && targetMix >= 1.0f)
        return;

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto* drySamples = dry.getReadPointer (channel, dryStart);

        for (int i = 0; i < length; ++i)
        {
            const auto mix = from + (targetMix - from) * (float) (i + 1) / (float) length;
            wet[channel][i] = mix * wet[channel][i] + (1.0f - mix) * drySamples[i];
        }
    }
}
} // namespace phazite::engine
