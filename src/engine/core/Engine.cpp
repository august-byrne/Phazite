// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "engine/transport/Click.h"

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
namespace
{
    constexpr int maximumChannels = 64; // the most output channels a block plays into; any more stay silent

    double getMilliseconds() noexcept
    {
        return juce::Time::getMillisecondCounterHiRes();
    }
} // namespace

Engine::Engine()
{
    prepare (sampleRate, maximumBlockSize);
}

Engine::~Engine() = default;

void Engine::prepare (double rate, int blockSize)
{
    jassert (rate > 0.0 && blockSize > 0);
    sampleRate = rate;
    maximumBlockSize = std::max (1, blockSize);
    click = makeClick (sampleRate, false);
    accentClick = makeClick (sampleRate, true);
    clicking = nullptr;
    transport.plan = nullptr; // a plan for another rate: the playhead stays on its beat
}

void Engine::setPlan (std::unique_ptr<Plan> plan)
{
    plans.give (std::move (plan));
}

//==============================================================================
void Engine::push (Command command)
{
    const auto scope = commandFifo.write (1);

    if (scope.blockSize1 > 0)
        commands[(size_t) scope.startIndex1] = command;
    else
        jassertfalse; // the audio thread hasn't taken the commands for 64 blocks: is it running?
}

void Engine::play()
{
    playing = true;
    push ({ Command::Type::play });
}

void Engine::stop()
{
    playing = false;
    push ({ Command::Type::stop });
}

void Engine::locate (double beat)
{
    requestedBeat.store (beat);
    locatesAsked.fetch_add (1);
    push ({ Command::Type::locate, beat });
}

double Engine::getPlayheadBeat() const noexcept
{
    // A locate the audio thread hasn't taken yet: where the playhead will be.
    if (locatesAsked.load() != locatesTaken.load())
        return requestedBeat.load();

    double beat = 0.0, milliseconds = 0.0, rate = 0.0, limit = 0.0;

    for (;;)
    {
        const auto before = playheadSequence.load (std::memory_order_acquire);

        if ((before & 1u) == 0)
        {
            beat = playheadBeat.load (std::memory_order_relaxed);
            milliseconds = playheadMilliseconds.load (std::memory_order_relaxed);
            rate = beatsPerMillisecond.load (std::memory_order_relaxed);
            limit = playheadLimit.load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);

            if (playheadSequence.load (std::memory_order_relaxed) == before)
                break;
        }
    }

    return beat + std::clamp ((getMilliseconds() - milliseconds) * rate, 0.0, limit);
}

void Engine::publishPlayhead (double beat, double beatsPerSecond, double blockSeconds) noexcept
{
    playheadSequence.fetch_add (1, std::memory_order_acq_rel); // odd: being written
    std::atomic_thread_fence (std::memory_order_release);
    playheadBeat.store (beat, std::memory_order_relaxed);
    playheadMilliseconds.store (getMilliseconds(), std::memory_order_relaxed);
    beatsPerMillisecond.store (beatsPerSecond / 1000.0, std::memory_order_relaxed);
    playheadLimit.store (beatsPerSecond * blockSeconds, std::memory_order_relaxed);
    playheadSequence.fetch_add (1, std::memory_order_release); // even: written
}

//==============================================================================
void Engine::takeCommands (const Plan& plan) noexcept
{
    const auto scope = commandFifo.read (commandFifo.getNumReady());

    scope.forEach (
        [this, &plan] (int index)
        {
            const auto& command = commands[(size_t) index];

            switch (command.type)
            {
                case Command::Type::play:
                    transport.playing = true;
                    break;

                case Command::Type::stop:
                    transport.playing = false;
                    clicking = nullptr;
                    break;

                case Command::Type::locate:
                    transport.beat = command.beat;
                    transport.sample = std::llround (plan.tempo.toSamples (command.beat));
                    clicking = nullptr;
                    locatesTaken.fetch_add (1);
                    break;
            }
        });
}

void Engine::process (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    numOutputs = std::min (numOutputs, maximumChannels);

    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::clear (outputs[channel], numSamples);

    // A block longer than prepared plays in parts.
    std::array<float*, maximumChannels> part {};

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (numSamples - done, maximumBlockSize);

        for (int channel = 0; channel < numOutputs; ++channel)
            part[(size_t) channel] = outputs[channel] != nullptr ? outputs[channel] + done : nullptr;

        const Handover<Plan>::Block held { plans };

        // A plan for another rate (the device's changed, and the new one is still being built) plays nothing.
        if (const auto* plan = held.get(); plan != nullptr && juce::exactlyEqual (plan->tempo.getSampleRate(), sampleRate))
        {
            // A new plan, perhaps of another tempo or rate: the playhead stays on its beat.
            if (transport.plan != plan)
            {
                transport.sample = std::llround (plan->tempo.toSamples (transport.beat));
                transport.plan = plan;
            }

            takeCommands (*plan);
            const auto startBeat = transport.beat;

            if (transport.playing)
            {
                for (int offset = 0; offset < length;)
                {
                    auto span = length - offset;
                    std::int64_t loopStart = 0, loopEnd = 0;

                    if (plan->loop.has_value())
                    {
                        loopStart = std::llround (plan->tempo.toSamples (plan->loop->start));
                        loopEnd = std::llround (plan->tempo.toSamples (plan->loop->end));

                        // Past the loop's end, playback starts again at its start; it plays as far as its end at most.
                        if (loopEnd > loopStart)
                        {
                            if (transport.sample >= loopEnd)
                                transport.sample = loopStart;

                            span = (int) std::min<std::int64_t> (span, loopEnd - transport.sample);
                        }
                    }

                    playSpan (*plan, part.data(), numOutputs, offset, span);
                    transport.sample += span;
                    offset += span;

                    if (loopEnd > loopStart && transport.sample >= loopEnd)
                        transport.sample = loopStart;
                }

                transport.beat = plan->tempo.toBeats ((double) transport.sample);
            }

            const auto beatsPerSecond = transport.playing ? sampleRate / plan->tempo.getSamplesPerBeat (startBeat) : 0.0;
            publishPlayhead (startBeat, beatsPerSecond, length / sampleRate);
        }
        else
        {
            transport.plan = nullptr; // freed once replaced: never compared with the next
        }

        done += length;
    }
}

void Engine::playSpan (const Plan& plan, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    if (plan.metronome)
        playClicks (plan, outputs, numOutputs, offset, numSamples);
}

void Engine::playClicks (const Plan& plan, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    // The click playing on, as far as a sample of the span.
    const auto continueClick = [this, outputs, numOutputs, offset] (int from, int to)
    {
        if (clicking == nullptr)
            return;

        const auto length = std::min (to - from, clicking->getNumSamples() - clickPosition);
        const auto* samples = clicking->getReadPointer (0, clickPosition);

        for (int channel = 0; channel < numOutputs; ++channel)
            if (outputs[channel] != nullptr)
                juce::FloatVectorOperations::add (outputs[channel] + offset + from, samples, length);

        clickPosition += length;

        if (clickPosition >= clicking->getNumSamples())
            clicking = nullptr;
    };

    // Each beat starts a click at the first sample at or after it, a bar's first beat the accented one.
    const auto from = (double) transport.sample;
    const auto to = from + numSamples;
    auto done = 0;

    for (auto beat = std::ceil (plan.tempo.toBeats (from) - 1.0e-9);; beat += 1.0)
    {
        const auto at = std::ceil (plan.tempo.toSamples (beat) - 1.0e-6);

        if (at >= to)
            break;

        const auto start = std::max (0, (int) (at - from));
        continueClick (done, start);
        done = start;
        clicking = std::abs (plan.tempo.getBarStart (beat) - beat) < 1.0e-6 ? &accentClick : &click;
        clickPosition = 0;
    }

    continueClick (done, numSamples);
}

//==============================================================================
juce::AudioBuffer<float> Engine::render (int numSamples)
{
    juce::AudioBuffer<float> audio (2, numSamples);
    play();

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (numSamples - done, maximumBlockSize);
        float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
        process (channels, 2, length);
        done += length;
    }

    return audio;
}
} // namespace phazite::engine
