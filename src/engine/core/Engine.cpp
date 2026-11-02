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
                    // The notes sounding where it starts start too.
                    transport.chase = transport.chase || ! transport.playing;
                    transport.playing = true;
                    break;

                case Command::Type::stop:
                    transport.notesOff = transport.notesOff || transport.playing;
                    transport.playing = false;
                    clicking = nullptr;
                    break;

                case Command::Type::locate:
                    // A jump while playing: the notes sounding stop, and those where it lands start.
                    transport.notesOff = transport.notesOff || transport.playing;
                    transport.chase = true;
                    transport.beat = command.beat;
                    transport.sample = std::llround (plan.tempo.toSamples (command.beat));
                    clicking = nullptr;
                    locatesTaken.fetch_add (1);
                    break;
            }
        });
}

void Engine::playLive (std::int64_t track, devices::NoteEvent event) noexcept
{
    liveQueue.push ({ track, event, getMilliseconds() });
}

void Engine::takeLiveEvents (int numSamples) noexcept
{
    // Each in the block as far as it came after the last block started: a block's delay, the same for every note.
    const auto now = getMilliseconds();
    const auto since = lastBlockMilliseconds > 0.0 ? lastBlockMilliseconds : now;
    lastBlockMilliseconds = now;
    numLiveEvents = 0;

    for (LiveEvent live; numLiveEvents < (int) liveEvents.size() && liveQueue.pop (live);)
    {
        live.event.sample = std::clamp ((int) std::lround ((live.milliseconds - since) * sampleRate / 1000.0), 0, numSamples - 1);
        liveEvents[(size_t) numLiveEvents++] = live;
    }
}

int Engine::getOutputLatency (const Plan& plan) noexcept
{
    return plan.strips.empty() ? 0 : plan.strips.back().latency;
}

void Engine::process (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    const auto startTicks = juce::Time::getHighResolutionTicks();
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
            takeLiveEvents (length);
            const auto startBeat = transport.beat;

            if (! transport.playing)
            {
                playSpan (*plan, part.data(), numOutputs, 0, length);
            }
            else
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

                    // At the loop's end, the notes sounding end on its last sample, and those at its start start again.
                    endsAtLoop = loopEnd > loopStart && transport.sample + span >= loopEnd;
                    playSpan (*plan, part.data(), numOutputs, offset, span);
                    endsAtLoop = false;
                    transport.sample += span;
                    offset += span;

                    if (loopEnd > loopStart && transport.sample >= loopEnd)
                    {
                        transport.sample = loopStart;
                        transport.chase = true;
                    }
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

    // Its share of the block's time, eased over about a second of blocks.
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks);
    const auto share = numSamples > 0 ? seconds * sampleRate / numSamples : 0.0;
    load.store (load.load (std::memory_order_relaxed) * 0.95 + share * 0.05, std::memory_order_relaxed);
}

void Engine::playSpan (const Plan& plan, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    if (! plan.strips.empty())
    {
        playStrips (plan, offset, numSamples);

        // The master's sound goes out (on the first two channels).
        const auto& master = *plan.strips.back().state;

        for (int channel = 0; channel < std::min (numOutputs, 2); ++channel)
            if (outputs[channel] != nullptr)
                juce::FloatVectorOperations::add (outputs[channel] + offset, master.buffer.getReadPointer (channel, offset), numSamples);
    }

    transport.notesOff = false;
    transport.chase = false;

    if (plan.metronome && transport.playing)
        playClicks (plan, outputs, numOutputs, offset, numSamples);
}

void Engine::playStrips (const Plan& plan, int offset, int numSamples) noexcept
{
    const auto beat = plan.tempo.toBeats ((double) transport.sample);
    const auto tempo = plan.tempo.getTempoAt (beat);

    for (const auto& strip : plan.strips)
    {
        auto& state = *strip.state;
        float* channels[] = { state.buffer.getWritePointer (0, offset), state.buffer.getWritePointer (1, offset) };
        juce::FloatVectorOperations::clear (channels[0], numSamples);
        juce::FloatVectorOperations::clear (channels[1], numSamples);

        // What it adds up, each lined up with the others.
        for (const auto& input : strip.inputs)
        {
            const auto& from = *plan.strips[(size_t) input.strip].state;
            const float* sound[] = { from.buffer.getReadPointer (0, offset), from.buffer.getReadPointer (1, offset) };

            if (input.delay != nullptr)
                input.delay->addDelayed (sound, channels, numSamples);
            else
                for (int channel = 0; channel < 2; ++channel)
                    juce::FloatVectorOperations::add (channels[channel], sound[channel], numSamples);
        }

        // What its instrument plays: its notes, and what is played live into it.
        state.numEvents = 0;
        addNotes (strip, plan, numSamples);

        for (int i = 0; i < numLiveEvents; ++i)
        {
            const auto& live = liveEvents[(size_t) i];

            if (live.track == strip.id && live.event.sample >= offset && live.event.sample < offset + numSamples)
            {
                auto event = live.event;
                event.sample -= offset;
                state.addEvent (event);
            }
        }

        // A channel muted (or soloed out) plays nothing, and its devices start from silence when it is heard again.
        if (state.getChannel().isSilenced())
        {
            state.wasSilenced = true;
        }
        else
        {
            if (state.wasSilenced)
            {
                for (const auto& device : strip.devices)
                    device->getRunner().reset();

                state.wasSilenced = false;
            }

            state.sortEvents();

            for (size_t i = 0; i < strip.devices.size(); ++i)
            {
                DeviceRunner::Block block;
                block.tempo = tempo;
                block.beat = beat;
                block.isPlaying = transport.playing;
                block.isRendering = rendering;

                if (i == 0 && strip.hasInstrument)
                    block.events = std::span (state.events.data(), (size_t) state.numEvents);

                strip.devices[i]->process (state.buffer, offset, numSamples, block);
            }
        }

        state.getChannel().process (channels, 2, numSamples);
    }
}

void Engine::addNotes (const Plan::Strip& strip, const Plan& plan, int numSamples) noexcept
{
    using Type = devices::NoteEvent::Type;
    auto& state = *strip.state;

    if (transport.notesOff)
    {
        state.addEvent ({ Type::allNotesOff, 0, 0, 0.0f });
        state.sounding.reset();
    }

    const auto* notes = strip.notes.get();

    if (! transport.playing || notes == nullptr)
        return;

    const auto from = (double) transport.sample;
    const auto fromBeat = plan.tempo.toBeats (from), toBeat = plan.tempo.toBeats (from + numSamples);

    // The notes sounding here: those that started before it and end after it.
    const auto forEachSounding = [notes, fromBeat] (auto&& action)
    {
        for (const auto& note : notes->getNotes())
        {
            if (note.start >= fromBeat)
                break;

            if (note.end > fromBeat)
                action (note);
        }
    };

    if (state.sequence != notes)
    {
        // Another sequence (the song changed as it played, or the first): what it doesn't play any more ends, and what it
        // plays and didn't starts, while the notes both play sound on.
        std::bitset<128> nowSounding;
        forEachSounding ([&nowSounding] (const NoteSequence::Note& note) { nowSounding.set ((size_t) note.pitch); });

        for (size_t pitch = 0; pitch < 128; ++pitch)
        {
            if (state.sounding[pitch] && ! nowSounding[pitch])
            {
                state.addEvent ({ Type::noteOff, 0, (int) pitch, 0.0f });
                state.sounding.reset (pitch);
            }
        }

        state.sequence = notes;
        state.mustChase = true;
    }

    if (transport.chase || state.mustChase)
    {
        state.nextEvent = notes->findEvent (fromBeat);

        forEachSounding (
            [&state] (const NoteSequence::Note& note)
            {
                if (! state.sounding[(size_t) note.pitch])
                {
                    state.addEvent ({ Type::noteOn, 0, note.pitch, note.velocity });
                    state.sounding.set ((size_t) note.pitch);
                }
            });

        state.mustChase = false;
    }

    // Each event in this stretch, at its sample.
    const auto& events = notes->getEvents();

    for (; state.nextEvent < events.size() && events[state.nextEvent].beat < toBeat; ++state.nextEvent)
    {
        const auto& event = events[state.nextEvent];
        const auto sample = std::clamp ((int) (std::llround (plan.tempo.toSamples (event.beat)) - transport.sample), 0, numSamples - 1);
        state.addEvent ({ event.type, sample, event.number, event.value });

        if (event.type == Type::noteOn)
            state.sounding.set ((size_t) event.number);
        else if (event.type == Type::noteOff)
            state.sounding.reset ((size_t) event.number);
    }

    // The loop's end: what sounds ends on its last sample.
    if (endsAtLoop)
    {
        for (size_t pitch = 0; pitch < 128; ++pitch)
            if (state.sounding[pitch])
                state.addEvent ({ Type::noteOff, numSamples - 1, (int) pitch, 0.0f });

        state.sounding.reset();
    }
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
    // Ahead by as much as its sound comes out late (delay compensation), so that it starts on time.
    rendering = true;
    play();
    auto toSkip = plans.get() != nullptr ? getOutputLatency (*plans.get()) : 0;

    juce::AudioBuffer<float> audio (2, numSamples), block (2, maximumBlockSize);

    for (int written = 0; written < numSamples;)
    {
        float* channels[] = { block.getWritePointer (0), block.getWritePointer (1) };
        process (channels, 2, maximumBlockSize);

        const auto skipped = std::min (toSkip, maximumBlockSize);
        toSkip -= skipped;
        const auto taken = std::min (maximumBlockSize - skipped, numSamples - written);

        for (int channel = 0; channel < 2; ++channel)
            audio.copyFrom (channel, written, block, channel, skipped, taken);

        written += taken;
    }

    rendering = false;
    return audio;
}
} // namespace phazite::engine
