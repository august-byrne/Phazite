// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceRunner.h"

#include <algorithm>

namespace phazite::engine
{
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
        glides.push_back (description.unit != devices::Unit::choice && description.unit != devices::Unit::toggle);
        ranges.push_back (devices::getRange (description));
        offsets.push_back (0.0f);
    }

    // An effect's mix, which this applies (an instrument has none).
    if (info.kind == devices::DeviceKind::effect)
        mixIndex = devices::findParameter (info, devices::mixParameterID);
}

void DeviceRunner::prepare (double rate, int maximumBlockSize)
{
    processor->prepare (rate, maximumBlockSize);
    const auto stretch = std::max (maximumBlockSize, 512);
    stereo.setSize (2, stretch);
    latencySamples = processor->getLatencySamples();
    scope.setSampleRate (rate);
    sleepAfterSamples = std::max ((int) (sleepAfterSeconds * rate), 2 * latencySamples);
    silentSamples = 0;
    asleep = false;
    hasPreviousValues = false;

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

void DeviceRunner::process (juce::AudioBuffer<float>& buffer, int start, int numSamples, const Block& in)
{
    // Each parameter's value now. A continuous one that moved since the last block glides there over this one, in stretches
    // of smoothingStep, rather than jumping, so automation and modulation don't step (docs/performance-audit.md, A7).
    auto* plan = in.modulation;
    auto gliding = false;

    for (size_t i = 0; i < targetValues.size(); ++i)
    {
        targetValues[i] = devices::constrain (info.parameters[i], in.values[i]);
        currentValues[i] = targetValues[i];
        gliding = gliding || (glides[i] && hasPreviousValues && ! juce::exactlyEqual (previousValues[i], targetValues[i]));
    }

    const auto numEvents = (int) in.events.size();
    devices::ProcessContext block;
    block.parameters = currentValues;
    block.isPlaying = in.isPlaying;
    block.tempo = in.tempo;
    block.beat = in.beat;

    // Modulated, it plays in stretches too, each with its modulators where it is.
    if (plan != nullptr)
        startModulation (*plan);

    const auto inStretches = gliding || (plan != nullptr && ! plan->routes.empty());

    const auto isMono = buffer.getNumChannels() < 2;
    const auto targetMix = mixIndex >= 0 ? targetValues[(size_t) mixIndex] : 1.0f;
    const auto hasSidechain = in.hasSidechain && buffer.getNumChannels() >= 4;
    const auto isSynth = isInstrument();

    if (isSynth) // its own channels, not a sidechain's
        for (int channel = 0; channel < std::min (2, buffer.getNumChannels()); ++channel)
            buffer.clear (channel, start, numSamples);

    // Nothing to play: no events, nothing waiting in the device (a pad its editor played), and silence in (on its own
    // channels and its sidechain's).
    const auto isQuiet = [&buffer, start, numSamples] (int firstChannel, int numChannels)
    {
        for (int channel = firstChannel; channel < firstChannel + numChannels; ++channel)
            if (buffer.getMagnitude (channel, start, numSamples) > silence)
                return false;

        return true;
    };

    const auto quietIn = numEvents == 0 && ! processor->isWaitingToPlay() && (isSynth || isQuiet (0, std::min (2, buffer.getNumChannels())))
                         && (! hasSidechain || isQuiet (2, 2));

    if (asleep && quietIn && ! in.isRendering)
    {
        for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
            buffer.clear (channel, start, numSamples);

        keepValues();
        return;
    }

    asleep = false;

    // What comes in, for the editor's displays: an effect's sound (an instrument's channels were cleared for it to play into).
    const auto* inLeft = buffer.getReadPointer (0, start);
    const auto* inRight = isMono ? inLeft : buffer.getReadPointer (1, start);

    if (! isSynth)
        scope.pushInput (inLeft, inRight, numSamples);

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
        const auto length = std::min (numSamples - done, inStretches ? smoothingStep : stereo.getNumSamples());

        if (inStretches)
            setStretchValues (plan, done, length, numSamples);

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
        block.events = in.events.subspan ((size_t) firstEvent, (size_t) (nextEvent - firstEvent));

        if (hasSidechain)
        {
            sidechain[0] = buffer.getReadPointer (2, start + done);
            sidechain[1] = buffer.getReadPointer (3, start + done);
        }

        processor->process (block);

        // Its mix, unless it plays something else for now (a compressor's key, to listen to); modulated, as it is now.
        if (mixIndex >= 0)
        {
            const auto isMixModulated = plan != nullptr && plan->isModulated[(size_t) mixIndex];
            mixWithDry (channels, length, processor->overridesMix() ? 1.0f : isMixModulated ? currentValues[(size_t) mixIndex] : targetMix);
        }

        if (isMono)
        {
            auto* mono = buffer.getWritePointer (0, start + done);

            for (int i = 0; i < length; ++i)
                mono[i] = 0.5f * (channels[0][i] + channels[1][i]);
        }

        done += length;
    }

    scope.pushOutput (buffer.getReadPointer (0, start), isMono ? buffer.getReadPointer (0, start) : buffer.getReadPointer (1, start),
                      numSamples);
    keepValues();

    // Silent in and out long enough: asleep from the next block.
    if (quietIn && isQuiet (0, std::min (2, buffer.getNumChannels())))
    {
        silentSamples = std::min (silentSamples + numSamples, sleepAfterSamples);
        asleep = silentSamples >= sleepAfterSamples && ! in.isRendering;
    }
    else
    {
        silentSamples = 0;
    }

    for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, start, numSamples);
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

void DeviceRunner::setStretchValues (const Modulation* plan, int done, int length, int numSamples)
{
    // A continuous parameter that moved since the last block glides there, as far as the stretch's end.
    const auto along = (float) (done + length) / (float) numSamples;

    for (size_t i = 0; i < currentValues.size(); ++i)
        currentValues[i] =
            glides[i] && hasPreviousValues ? previousValues[i] + (targetValues[i] - previousValues[i]) * along : targetValues[i];

    if (plan == nullptr)
        return;

    // Then a modulated one is moved by its sources where the stretch is: an LFO by its wave at the stretch's middle, a
    // modulator or a macro by its value gliding as far as the stretch's end.
    std::fill (offsets.begin(), offsets.end(), 0.0f);
    const auto middle = done + length / 2;

    for (const auto& route : plan->routes)
    {
        const auto& source = plan->sources[(size_t) route.source];

        if (! source.isOn)
            continue;

        const auto value = source.followsWave
                               ? devices::getLfoValue (source.shape, devices::movePhase (source.phase, middle * source.cyclesPerSample))
                               : source.previous + (source.now - source.previous) * along;
        offsets[(size_t) route.parameter] += route.amount * value;
    }

    for (size_t i = 0; i < currentValues.size(); ++i)
        if (plan->isModulated[i])
            currentValues[i] = devices::modulate (info.parameters[i], ranges[i], currentValues[i], offsets[i]);
}

void DeviceRunner::mixWithDry (float* const* wet, int length, float targetMix)
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
} // namespace phazite::engine
