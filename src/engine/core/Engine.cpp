// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "devices/core/AudioThreads.h"
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
    : played ((size_t) maximumPlayed)
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
    controlPoints.assign ((size_t) (maximumBlockSize / controlPointSpacing + 3), {});
    numControlPoints = 0;
    loudness.prepare (sampleRate, maximumBlockSize);
    lastBlockStart = 0; // a device that starts again: no gap
}

void Engine::setPlan (std::unique_ptr<Plan> plan)
{
    if (plan != nullptr)
        findEarlyStarts (*plan);

    plans.give (std::move (plan));
}

void Engine::findEarlyStarts (Plan& plan)
{
    // An instrument handed its blocks early (a plug-in in its helper), but one whose sidechain listens to another strip,
    // which it would wait for.
    plan.earlyStrips.clear();

    for (size_t i = 0; i < plan.strips.size(); ++i)
    {
        auto& strip = plan.strips[i];
        strip.startsEarly = strip.hasInstrument && ! strip.devices.empty() && strip.devices.front().sidechain < 0
                            && strip.devices.front().node->getRunner().getProcessor().startsEarly();

        if (strip.startsEarly)
            plan.earlyStrips.push_back ((int) i);
    }
}

void Engine::dropPlans()
{
    plans.give (nullptr);
    plans.freeReplaced();
}

void Engine::setRecording (std::unique_ptr<Recording> recording)
{
    recordings.give (std::move (recording));
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

void Engine::play (bool countIn)
{
    playing = true;
    push ({ Command::Type::play, 0.0, countIn });
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
                    // The notes sounding where it starts start too. A count-in from a stop: a bar there, on its beats.
                    if (command.countIn && ! transport.playing)
                    {
                        auto& countIn = transport.countIn;
                        countIn.beatsPerBar = std::max (1, plan.tempo.getTimeSignatureAt (transport.beat).numerator);
                        countIn.samplesPerBeat = plan.tempo.getSamplesPerBeat (transport.beat);
                        countIn.length = std::llround (countIn.beatsPerBar * countIn.samplesPerBeat);
                        countIn.left = countIn.length;
                        countIn.firstBeat = transport.beat - countIn.beatsPerBar;
                        countIn.barStart = plan.tempo.getBarStart (transport.beat);
                        clicking = nullptr;
                    }

                    // From a stop, the song starts from silence: tails ringing on from before (a reverb's, a delay's)
                    // stop as it starts, as Ableton Live and Bitwig reset plug-ins. Its loop's end doesn't cut them.
                    transport.chase = transport.chase || ! transport.playing;
                    transport.flush = transport.flush || ! transport.playing;
                    transport.playing = true;
                    break;

                case Command::Type::stop:
                    transport.notesOff = transport.notesOff || transport.playing;
                    transport.playing = false;
                    transport.countIn.left = 0;
                    brokeTake = true;
                    clicking = nullptr;
                    break;

                case Command::Type::locate:
                    // A jump while playing: the notes sounding stop, and those where it lands start, from silence.
                    transport.notesOff = transport.notesOff || transport.playing;
                    transport.flush = transport.flush || transport.playing;
                    brokeTake = true;
                    transport.chase = true;
                    transport.beat = command.beat;
                    transport.sample = std::llround (plan.tempo.toSamples (command.beat));
                    clicking = nullptr;
                    locatesTaken.fetch_add (1);
                    break;
            }
        });
}

void Engine::playLive (std::int64_t track, devices::NoteEvent event, bool recordable) noexcept
{
    liveQueue.push ({ track, event, getMilliseconds(), recordable });
}

void Engine::takeLiveEvents (const Plan& plan, int numSamples, std::int64_t partClock) noexcept
{
    // Each in the block as far as it came after the last block started: a block's delay, the same for every note.
    const auto now = getMilliseconds();
    const auto since = lastBlockMilliseconds > 0.0 ? lastBlockMilliseconds : now;
    lastBlockMilliseconds = now;
    numLiveEvents = 0;

    // Where the song was as each came in, in the last part: round the loop, as it plays.
    std::int64_t loopStart = 0, loopEnd = 0;

    if (plan.loop.has_value())
    {
        loopStart = std::llround (plan.tempo.toSamples (plan.loop->start));
        loopEnd = std::llround (plan.tempo.toSamples (plan.loop->end));
    }

    for (LiveEvent live; numLiveEvents < (int) liveEvents.size() && liveQueue.pop (live);)
    {
        live.event.sample = std::clamp ((int) std::lround ((live.milliseconds - since) * sampleRate / 1000.0), 0, numSamples - 1);
        liveEvents[(size_t) numLiveEvents++] = live;

        if (! live.recordable)
            continue;

        PlayedEvent event { live.track, live.event, lastPart.clock + live.event.sample, -1 };

        if (lastPart.playing)
        {
            event.songSample = lastPart.sample + live.event.sample;

            if (loopEnd > loopStart && lastPart.sample < loopEnd && event.songSample >= loopEnd)
                event.songSample = loopStart + (event.songSample - loopEnd);
        }

        if (const auto scope = playedFifo.write (1); scope.blockSize1 > 0)
            played[(size_t) scope.startIndex1] = event;
    }

    lastPart = { partClock, transport.sample, transport.playing && transport.countIn.left == 0 };
}

int Engine::takePlayed (std::span<PlayedEvent> into) noexcept
{
    const auto scope = playedFifo.read (std::min ((int) into.size(), playedFifo.getNumReady()));
    auto taken = 0;
    scope.forEach ([this, &into, &taken] (int index) { into[(size_t) taken++] = played[(size_t) index]; });
    return taken;
}

Engine::BlockTimes Engine::takeBlockTimes() noexcept
{
    BlockTimes times;
    times.late = lateBlocks.exchange (0, std::memory_order_relaxed);
    times.longestGap = juce::Time::highResolutionTicksToSeconds (longestGapTicks.exchange (0, std::memory_order_relaxed));
    times.longestBlock = juce::Time::highResolutionTicksToSeconds (longestBlockTicks.exchange (0, std::memory_order_relaxed));
    times.blockLength = lastBlockLength.load (std::memory_order_relaxed) / sampleRate;
    return times;
}

int Engine::getOutputLatency (const Plan& plan) noexcept
{
    return plan.strips.empty() ? 0 : plan.strips.back().latency;
}

void Engine::process (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    const auto startTicks = juce::Time::getHighResolutionTicks();
    numOutputs = std::min (numOutputs, maximumChannels);
    numInputs = inputs != nullptr ? std::clamp (numInputs, 0, maximumInputs) : 0;

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

        numPartInputs = numInputs;

        for (int channel = 0; channel < numInputs; ++channel)
            partInputs[(size_t) channel] = inputs[channel] != nullptr ? inputs[channel] + done : nullptr;

        const Handover<Plan>::Block held { plans };
        const Handover<Recording>::Block heldRecording { recordings };
        auto* recording = heldRecording.get();

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
            takeLiveEvents (*plan, length, clock + done);
            const auto startBeat = transport.beat;
            auto counted = 0;

            // A take ends as playback stops or jumps.
            if (recording != nullptr && brokeTake)
                recording->end();

            brokeTake = false;

            // The count-in: its clicks, while the song waits to play; live notes and the inputs play meanwhile, as when
            // it is stopped, and the notes sounding where it starts start after it.
            if (transport.playing && transport.countIn.left > 0)
            {
                counted = (int) std::min<std::int64_t> (length, transport.countIn.left);
                const auto chase = transport.chase;
                transport.playing = false;
                playSpan (*plan, part.data(), numOutputs, 0, counted);
                transport.playing = true;
                transport.chase = chase;
                playCountIn (part.data(), numOutputs, counted);
                transport.countIn.left -= counted;
            }

            if (! transport.playing)
            {
                playSpan (*plan, part.data(), numOutputs, 0, length);
            }
            else
            {
                for (int offset = counted; offset < length;)
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

                    // What comes in meanwhile, into the take, which starts here if it hasn't yet.
                    if (recording != nullptr)
                        recording->record (partInputs.data(), numPartInputs, offset, span, transport.sample, clock + done + offset);

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

            const auto beatsPerSecond =
                transport.playing && transport.countIn.left == 0 ? sampleRate / plan->tempo.getSamplesPerBeat (startBeat) : 0.0;
            publishPlayhead (startBeat, beatsPerSecond, length / sampleRate);
        }
        else
        {
            transport.plan = nullptr; // freed once replaced: never compared with the next
        }

        done += length;
    }

    // Its share of the block's time, eased over about a second of blocks.
    const auto tookTicks = juce::Time::getHighResolutionTicks() - startTicks;
    const auto seconds = juce::Time::highResolutionTicksToSeconds (tookTicks);
    const auto share = numSamples > 0 ? seconds * sampleRate / numSamples : 0.0;
    load.store (load.load (std::memory_order_relaxed) * 0.95 + share * 0.05, std::memory_order_relaxed);

    // How it came after the one before, and how long it took (takeBlockTimes(), which resets the longest: a reset it
    // misses by a block only reports that block again).
    if (lastBlockStart != 0)
    {
        const auto gapTicks = startTicks - lastBlockStart;

        if (gapTicks > longestGapTicks.load (std::memory_order_relaxed))
            longestGapTicks.store (gapTicks, std::memory_order_relaxed);

        if (juce::Time::highResolutionTicksToSeconds (gapTicks) * sampleRate > 2.0 * numSamples)
            lateBlocks.fetch_add (1, std::memory_order_relaxed);
    }

    if (tookTicks > longestBlockTicks.load (std::memory_order_relaxed))
        longestBlockTicks.store (tookTicks, std::memory_order_relaxed);

    lastBlockStart = startTicks;
    lastBlockLength.store (numSamples, std::memory_order_relaxed);
    blocksPlayed.fetch_add (1, std::memory_order_release);
    clock += numSamples;
    publishedClock.store (clock, std::memory_order_release);
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

        // How loud it is.
        const float* out[] = { master.buffer.getReadPointer (0, offset), master.buffer.getReadPointer (1, offset) };
        loudness.process (out, 2, numSamples);
    }

    transport.notesOff = false;
    transport.chase = false;
    transport.flush = false;

    if (plan.metronome && transport.playing)
        playClicks (plan, outputs, numOutputs, offset, numSamples);
}

void Engine::playStrips (const Plan& plan, int offset, int numSamples) noexcept
{
    // Where the song is for the stretch of the block. Where automation is: moving with the song while it plays; at the
    // playhead while the song is stopped. The control points it and modulation move on are the song's while it plays,
    // time's while it is stopped.
    Span span { this, &plan, offset, numSamples };
    span.beat = plan.tempo.toBeats ((double) transport.sample);
    span.tempo = plan.tempo.getTempoAt (span.beat);
    span.beatsPerSample = transport.playing && numSamples > 0
                              ? (plan.tempo.toBeats ((double) (transport.sample + numSamples)) - span.beat) / numSamples
                              : 0.0;
    span.gridSample = transport.playing ? transport.sample : clock + offset;
    span.pass = ++passes;
    span.flush = transport.flush;
    findControlPoints (plan, span.gridSample, numSamples);

    // Each strip a task, in the plan's order, on the audio thread and the workers; each waits for what it takes in, done
    // in tasks before it, and adds it up in the plan's order, so that the sound is the same to the last bit on one thread
    // or many. Before them, a task for each instrument handed its blocks early (a plug-in in its helper), which then plays
    // beside the others while the strips before its own play. A track and the master alone can't play side by side: the
    // audio thread plays them without waking anyone.
    if (plan.strips.size() <= 2)
    {
        for (size_t i = 0; i < plan.strips.size(); ++i)
            playStrip (span, i);

        return;
    }

    span.startsEarly = true;
    workers.run ((int) (plan.earlyStrips.size() + plan.strips.size()),
                 [] (void* context, int index) noexcept
                 {
                     const auto& job = *static_cast<Span*> (context);
                     const auto& early = job.plan->earlyStrips;

                     if (index < (int) early.size())
                         job.engine->startStrip (job, (size_t) early[(size_t) index]);
                     else
                         job.engine->playStrip (job, (size_t) index - early.size());
                 },
                 &span);
}

void Engine::startStrip (const Span& span, size_t index) noexcept
{
    // Its notes, what is played live into it and its modulators, then its instrument handed the block, unless it plays
    // nothing in it; its own task takes what it played (playStrip()).
    const auto& strip = span.plan->strips[index];
    auto& state = *strip.state;

    if (span.flush)
        flushStrip (strip);

    takeEvents (span, strip);
    state.startedSilent = isSilent (strip);

    if (! state.startedSilent)
    {
        const auto& instrument = strip.devices.front();
        instrument.node->start (state.buffer, span.offset, span.numSamples, getDeviceBlock (span, strip, 0), instrument.automation);
    }

    state.startedPass.store (span.pass, std::memory_order_release);
}

void Engine::takeEvents (const Span& span, const Plan::Strip& strip) noexcept
{
    // What its instrument plays: its notes, and what is played live into it.
    auto& state = *strip.state;
    state.numEvents = 0;
    addNotes (strip, *span.plan, span.numSamples);

    for (int i = 0; i < numLiveEvents; ++i)
    {
        const auto& live = liveEvents[(size_t) i];

        if (live.track == strip.id && live.event.sample >= span.offset && live.event.sample < span.offset + span.numSamples)
        {
            auto event = live.event;
            event.sample -= span.offset;
            state.addEvent (event);
        }
    }

    state.sortEvents();
    playModulators (strip);
}

bool Engine::isSilent (const Plan::Strip& strip) noexcept
{
    // A channel muted (or soloed out) plays nothing, and its devices start from silence when it is heard again; but one a
    // sidechain listens to plays on, which hears it before its mute.
    auto& state = *strip.state;

    if (state.getChannel().isSilenced() && ! strip.feedsSidechain)
    {
        state.wasSilenced = true;
        return true;
    }

    if (state.wasSilenced)
    {
        for (const auto& device : strip.devices)
            device.node->getRunner().reset();

        state.wasSilenced = false;
    }

    return false;
}

void Engine::flushStrip (const Plan::Strip& strip) noexcept
{
    // Nothing it heard before rings on: its devices start from silence (a plug-in as its helper is handed its next block),
    // and so do the delays that line up what it adds and listens to, and its containers' dry sound.
    for (const auto& device : strip.devices)
    {
        device.node->getRunner().reset();

        if (device.sidechainDelay != nullptr)
            device.sidechainDelay->reset();
    }

    for (const auto& input : strip.inputs)
        if (input.delay != nullptr)
            input.delay->reset();

    for (const auto& container : strip.containers)
        container.state->reset();
}

DeviceRunner::Block Engine::getDeviceBlock (const Span& span, const Plan::Strip& strip, size_t index) noexcept
{
    const auto& device = strip.devices[index];
    DeviceRunner::Block block;
    block.tempo = span.tempo;
    block.beat = span.beat;
    block.isPlaying = transport.playing;
    block.isRendering = rendering;
    block.beatsPerSample = span.beatsPerSample;
    block.sample = span.gridSample;
    block.automationBeat = transport.playing && device.latency > 0 && ! device.automation.empty()
                               ? span.plan->tempo.toBeats ((double) (transport.sample - device.latency))
                               : span.beat;

    // What modulates it: its strip's modulators, on the block's control points.
    if (auto* modulation = device.modulation.get())
    {
        for (size_t source = 0; source < modulation->sources.size() && source < strip.modulators.size(); ++source)
        {
            modulation->sources[source].points = strip.modulators[source]->getValues();
            modulation->sources[source].isOn = strip.modulators[source]->isOn();
        }

        block.modulation = modulation;
    }

    if (index == 0 && strip.hasInstrument)
        block.events = std::span (strip.state->events.data(), (size_t) strip.state->numEvents);

    return block;
}

void Engine::playStrip (const Span& span, size_t index) noexcept
{
    const auto& plan = *span.plan;
    const auto& strip = plan.strips[index];
    const auto offset = span.offset, numSamples = span.numSamples;
    const auto beat = span.beat, beatsPerSample = span.beatsPerSample;
    const auto gridSample = span.gridSample;
    const auto getAutomationBeat = [this, &plan, beat] (int latency)
    { return transport.playing && latency > 0 ? plan.tempo.toBeats ((double) (transport.sample - latency)) : beat; };

    // What it takes in, done first: what it adds up, and what its devices' sidechains listen to; and its instrument's
    // block handed over early, in a task before.
    const auto waitFor = [&span] (const std::atomic<std::uint64_t>& done)
    { devices::spinUntil ([&] { return done.load (std::memory_order_acquire) == span.pass; }); };

    for (const auto& input : strip.inputs)
        waitFor (plan.strips[(size_t) input.strip].state->donePass);

    for (const auto& device : strip.devices)
        if (device.sidechain >= 0)
            waitFor (plan.strips[(size_t) device.sidechain].state->donePass);

    const auto startedEarly = span.startsEarly && strip.startsEarly;

    if (startedEarly)
        waitFor (strip.state->startedPass);
    else if (span.flush)
        flushStrip (strip); // flushed as its instrument was handed its block, if it was

    {
        auto& state = *strip.state;
        float* channels[] = { state.buffer.getWritePointer (0, offset), state.buffer.getWritePointer (1, offset) };
        juce::FloatVectorOperations::clear (channels[0], numSamples);
        juce::FloatVectorOperations::clear (channels[1], numSamples);

        // What it adds up, each lined up with the others: strips after their faders, or before them (sends), at their sends'
        // levels.
        for (const auto& input : strip.inputs)
        {
            const auto& from = *plan.strips[(size_t) input.strip].state;
            const auto& sound = input.beforeFader ? from.beforeFader : from.buffer;
            const float* sides[] = { sound.getReadPointer (0, offset), sound.getReadPointer (1, offset) };
            const auto [startGain, endGain] = input.level != nullptr ? input.level->moveOn() : std::pair (1.0f, 1.0f);
            (input.delay != nullptr ? *input.delay : noDelay).addDelayed (sides, channels, numSamples, startGain, endGain);
        }

        // Its events and modulators, unless they were taken as its instrument was handed its block.
        if (! startedEarly)
            takeEvents (span, strip);

        if (! (startedEarly ? state.startedSilent : isSilent (strip)))
        {
            // Its audio clips play into it before its effects, beside its instrument (after it, which plays into a cleared
            // buffer).
            const auto playAudio = [&]
            {
                if (transport.playing)
                    for (const auto& region : strip.audio)
                        region.play (channels, transport.sample, numSamples, sampleRate, rendering, state.scratch);
            };

            if (! strip.hasInstrument)
            {
                addInput (strip, channels, offset, numSamples);
                playAudio();
            }

            for (size_t i = 0; i < strip.devices.size(); ++i)
            {
                auto block = getDeviceBlock (span, strip, i);

                // What its sidechain listens to: another strip after its fader and before its mute, lined up with its sound.
                if (const auto& device = strip.devices[i]; device.sidechain >= 0)
                {
                    const auto& heard = plan.strips[(size_t) device.sidechain].state->beforeMute;
                    const float* from[] = { heard.getReadPointer (0, offset), heard.getReadPointer (1, offset) };
                    float* into[] = { state.buffer.getWritePointer (2, offset), state.buffer.getWritePointer (3, offset) };
                    juce::FloatVectorOperations::clear (into[0], numSamples);
                    juce::FloatVectorOperations::clear (into[1], numSamples);
                    (device.sidechainDelay != nullptr ? *device.sidechainDelay : noDelay).addDelayed (from, into, numSamples);
                    block.hasSidechain = true;
                }

                // A container keeps what comes into its first device, to mix with what its last puts out.
                for (const auto& container : strip.containers)
                    if (container.first == (int) i)
                        container.state->keepDry (channels, numSamples);

                strip.devices[i].node->process (state.buffer, offset, numSamples, block, strip.devices[i].automation);

                for (const auto& container : strip.containers)
                    if (container.last == (int) i)
                        container.state->mix (channels, numSamples);

                if (i == 0 && strip.hasInstrument)
                    playAudio();
            }
        }

        // What it sends before its fader (nothing while it is muted).
        if (strip.feedsBeforeFader)
            for (int channel = 0; channel < 2; ++channel)
            {
                auto* kept = state.beforeFader.getWritePointer (channel, offset);

                if (state.getChannel().isSilenced())
                    juce::FloatVectorOperations::clear (kept, numSamples);
                else
                    juce::FloatVectorOperations::copy (kept, channels[channel], numSamples);
            }

        Channel::Automation faderAutomation;
        faderAutomation.volume = strip.volume.get();
        faderAutomation.pan = strip.pan.get();
        faderAutomation.beatsPerSample = beatsPerSample;
        faderAutomation.sample = gridSample;
        faderAutomation.beat = strip.volume != nullptr || strip.pan != nullptr ? getAutomationBeat (strip.latency) : beat;
        faderAutomation.volumeModulation = getFaderModulation (strip, strip.volumeModulation, state.volumeModulation);
        faderAutomation.panModulation = getFaderModulation (strip, strip.panModulation, state.panModulation);
        float* beforeMute[] = { state.beforeMute.getWritePointer (0, offset), state.beforeMute.getWritePointer (1, offset) };
        state.getChannel().process (channels, 2, numSamples, faderAutomation, strip.feedsSidechain ? beforeMute : nullptr);

        // An envelope follower listens to its track after the fader, for the next block.
        for (const auto& modulator : strip.modulators)
            modulator->listen (channels, 2, numSamples);

        state.donePass.store (span.pass, std::memory_order_release);
    }
}

void Engine::addInput (const Plan::Strip& strip, float* const* channels, int offset, int numSamples) noexcept
{
    // The inputs it hears: one on both sides, or a pair.
    for (size_t side = 0; side < 2 && ! strip.inputChannels.empty(); ++side)
    {
        const auto channel = strip.inputChannels[std::min (side, strip.inputChannels.size() - 1)];

        if (channel >= 0 && channel < numPartInputs && partInputs[(size_t) channel] != nullptr)
            juce::FloatVectorOperations::add (channels[side], partInputs[(size_t) channel] + offset, numSamples);
    }
}

void Engine::findControlPoints (const Plan& plan, std::int64_t gridSample, int numSamples) noexcept
{
    // From the one at or before the block's start to the one at or after its end, each where the song is: while it plays,
    // on the song's samples; stopped, on time's, all at the playhead (numbered apart from the song's, so that none is taken
    // for one given before).
    constexpr std::int64_t stoppedPoints = std::int64_t (1) << 52;
    const auto since = (int) (((gridSample % controlPointSpacing) + controlPointSpacing) % controlPointSpacing);
    const auto first = (gridSample - since) / controlPointSpacing;
    numControlPoints = std::min ((int) controlPoints.size(), (since + numSamples + controlPointSpacing - 1) / controlPointSpacing + 1);

    for (int k = 0; k < numControlPoints; ++k)
    {
        auto& point = controlPoints[(size_t) k];
        point.index = first + k + (transport.playing ? 0 : stoppedPoints);
        point.sample = std::max (0, k * controlPointSpacing - since);
        const auto songSample = transport.playing ? (double) (gridSample - since + k * controlPointSpacing) : (double) transport.sample;
        const auto beat = plan.tempo.toBeats (songSample);
        point.place = { songSample / sampleRate, beat, plan.tempo.getTempoAt (beat), plan.tempo.getTimeSignatureAt (beat).numerator };
    }
}

void Engine::playModulators (const Plan::Strip& strip) noexcept
{
    // Its modulators, on the block's control points, each after the notes its instrument starts before the point.
    if (strip.modulators.empty())
        return;

    auto& state = *strip.state;
    state.noteStarts.clear();

    for (int i = 0; i < state.numEvents && state.noteStarts.size() < state.noteStarts.capacity(); ++i)
        if (const auto& event = state.events[(size_t) i];
            event.type == devices::NoteEvent::Type::noteOn || event.type == devices::NoteEvent::Type::portamentoOn)
            state.noteStarts.push_back ({ event.sample, event.number, juce::roundToInt (event.value * 127.0f) });

    const std::span<const ModulatorNode::Point> points (controlPoints.data(), (size_t) numControlPoints);

    for (const auto& modulator : strip.modulators)
        modulator->play (points, state.noteStarts, controlPointSpacing / sampleRate);
}

std::span<const float> Engine::getFaderModulation (const Plan::Strip& strip, const std::vector<Plan::Route>& routes,
                                                   std::vector<float>& offsets) noexcept
{
    // Each modulator's value by its amount, added up on each of the block's control points.
    if (routes.empty())
        return {};

    const auto count = std::min ((size_t) numControlPoints, offsets.size());
    std::fill_n (offsets.begin(), count, 0.0f);

    for (const auto& route : routes)
    {
        const auto& modulator = *strip.modulators[(size_t) route.source];

        if (! modulator.isOn())
            continue;

        const auto values = modulator.getValues();

        for (size_t k = 0; k < count && k < values.size(); ++k)
            offsets[k] += route.amount * values[k];
    }

    return { offsets.data(), count };
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

    // The notes sounding here: those that started before it and end after its first sample. A note that ends where the
    // span starts ends on that sample, however its beat rounds to it, so that a song played from where one note ends
    // and the next starts plays the next alone.
    const auto firstSampleEnd = plan.tempo.toBeats (from + 0.5);
    const auto forEachSounding = [notes, fromBeat, firstSampleEnd] (auto&& action)
    {
        for (const auto& note : notes->getNotes())
        {
            if (note.start >= fromBeat)
                break;

            if (note.end > firstSampleEnd)
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

        // A slide's length, in beats in the sequence, as the instrument takes it: in seconds at the song's tempo.
        const auto value =
            event.type == Type::slide
                ? (float) ((plan.tempo.toSamples (event.beat + (double) event.value) - plan.tempo.toSamples (event.beat)) / sampleRate)
                : event.value;
        state.addEvent ({ event.type, sample, event.number, value });

        if (event.type == Type::noteOn || event.type == Type::portamentoOn)
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

void Engine::continueClick (float* const* outputs, int numOutputs, int offset, int from, int to) noexcept
{
    // The click playing on, as far as a sample of the span.
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
}

void Engine::playCountIn (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    // The song's beats in the bar before where it starts, as long as the beats there: each starts a click, a bar's first
    // the accented one.
    const auto& countIn = transport.countIn;
    const auto from = (double) (countIn.length - countIn.left);
    auto done = 0;

    for (auto beat = std::ceil (countIn.firstBeat + from / countIn.samplesPerBeat - 1.0e-9);; beat += 1.0)
    {
        const auto at = std::ceil ((beat - countIn.firstBeat) * countIn.samplesPerBeat - 1.0e-6) - from;

        if (at >= numSamples || beat >= countIn.firstBeat + countIn.beatsPerBar - 1.0e-9)
            break;

        const auto start = std::max (0, (int) at);
        continueClick (outputs, numOutputs, 0, done, start);
        done = start;
        const auto inBar = (beat - countIn.barStart) / countIn.beatsPerBar;
        clicking = std::abs (inBar - std::round (inBar)) < 1.0e-6 ? &accentClick : &click;
        clickPosition = 0;
    }

    continueClick (outputs, numOutputs, 0, done, numSamples);
}

void Engine::playClicks (const Plan& plan, float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
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
        continueClick (outputs, numOutputs, offset, done, start);
        done = start;
        clicking = std::abs (plan.tempo.getBarStart (beat) - beat) < 1.0e-6 ? &accentClick : &click;
        clickPosition = 0;
    }

    continueClick (outputs, numOutputs, offset, done, numSamples);
}

//==============================================================================
juce::AudioBuffer<float> Engine::render (int numSamples)
{
    juce::AudioBuffer<float> audio (2, numSamples);
    beginRender();
    renderPart (audio.getArrayOfWritePointers(), numSamples);
    endRender();
    return audio;
}

void Engine::beginRender()
{
    // Ahead by as much as its sound comes out late (delay compensation), so that it starts on time.
    rendering = true;
    play();
    renderSkip = plans.get() != nullptr ? getOutputLatency (*plans.get()) : 0;
    renderBlock.setSize (2, maximumBlockSize);
    renderTaken = renderFilled = 0;
}

void Engine::renderPart (float* const* channels, int numSamples)
{
    for (int written = 0; written < numSamples;)
    {
        // The next block, once the last is all taken: its start skipped while the render is still ahead.
        if (renderTaken == renderFilled)
        {
            process (renderBlock.getArrayOfWritePointers(), 2, maximumBlockSize);
            renderFilled = maximumBlockSize;
            renderTaken = std::min (renderSkip, maximumBlockSize);
            renderSkip -= renderTaken;
            continue;
        }

        const auto taken = std::min (renderFilled - renderTaken, numSamples - written);

        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::copy (channels[channel] + written, renderBlock.getReadPointer (channel, renderTaken), taken);

        renderTaken += taken;
        written += taken;
    }
}

void Engine::endRender()
{
    rendering = false;
}
} // namespace phazite::engine
