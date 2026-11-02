// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "engine/transport/Click.h"

#include <algorithm>
#include <cmath>
#include <thread>

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
    controlPoints.assign ((size_t) (maximumBlockSize / controlPointSpacing + 3), {});
    numControlPoints = 0;
    loudness.prepare (sampleRate, maximumBlockSize);
}

void Engine::setPlan (std::unique_ptr<Plan> plan)
{
    plans.give (std::move (plan));
}

void Engine::dropPlans()
{
    plans.give (nullptr);
    plans.freeReplaced();
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
    blocksPlayed.fetch_add (1, std::memory_order_release);
    clock += numSamples;
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
    findControlPoints (plan, span.gridSample, numSamples);

    // Each strip a task, in the plan's order, on the audio thread and the workers; each waits for what it takes in, done
    // in tasks before it, and adds it up in the plan's order, so that the sound is the same to the last bit on one thread
    // or many. A track and the master alone can't play side by side: the audio thread plays them without waking anyone.
    if (plan.strips.size() <= 2)
    {
        for (size_t i = 0; i < plan.strips.size(); ++i)
            playStrip (span, i);

        return;
    }

    workers.run ((int) plan.strips.size(), [] (void* context, int index) noexcept
                 { static_cast<Span*> (context)->engine->playStrip (*static_cast<Span*> (context), (size_t) index); }, &span);
}

void Engine::playStrip (const Span& span, size_t index) noexcept
{
    const auto& plan = *span.plan;
    const auto& strip = plan.strips[index];
    const auto offset = span.offset, numSamples = span.numSamples;
    const auto beat = span.beat, tempo = span.tempo, beatsPerSample = span.beatsPerSample;
    const auto gridSample = span.gridSample;
    const auto getAutomationBeat = [this, &plan, beat] (int latency)
    { return transport.playing && latency > 0 ? plan.tempo.toBeats ((double) (transport.sample - latency)) : beat; };

    // What it takes in, done first: what it adds up, and what its devices' sidechains listen to.
    const auto waitFor = [&plan, &span] (int other)
    {
        const auto& done = plan.strips[(size_t) other].state->donePass;

        while (done.load (std::memory_order_acquire) != span.pass)
            std::this_thread::yield();
    };

    for (const auto& input : strip.inputs)
        waitFor (input.strip);

    for (const auto& device : strip.devices)
        if (device.sidechain >= 0)
            waitFor (device.sidechain);

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

        state.sortEvents();
        playModulators (strip);

        // A channel muted (or soloed out) plays nothing, and its devices start from silence when it is heard again; but one
        // a sidechain listens to plays on, which hears it before its mute.
        if (state.getChannel().isSilenced() && ! strip.feedsSidechain)
        {
            state.wasSilenced = true;
        }
        else
        {
            if (state.wasSilenced)
            {
                for (const auto& device : strip.devices)
                    device.node->getRunner().reset();

                state.wasSilenced = false;
            }

            // Its audio clips play into it before its effects, beside its instrument (after it, which plays into a cleared
            // buffer).
            const auto playAudio = [&]
            {
                if (transport.playing)
                    for (const auto& region : strip.audio)
                        region.play (channels, transport.sample, numSamples, sampleRate, rendering, state.scratch);
            };

            if (! strip.hasInstrument)
                playAudio();

            for (size_t i = 0; i < strip.devices.size(); ++i)
            {
                DeviceRunner::Block block;
                block.tempo = tempo;
                block.beat = beat;
                block.isPlaying = transport.playing;
                block.isRendering = rendering;
                block.beatsPerSample = beatsPerSample;
                block.sample = gridSample;
                block.automationBeat = strip.devices[i].automation.empty() ? beat : getAutomationBeat (strip.devices[i].latency);

                // What modulates it: its strip's modulators, on the block's control points.
                if (auto* modulation = strip.devices[i].modulation.get())
                {
                    for (size_t source = 0; source < modulation->sources.size() && source < strip.modulators.size(); ++source)
                    {
                        modulation->sources[source].points = strip.modulators[source]->getValues();
                        modulation->sources[source].isOn = strip.modulators[source]->isOn();
                    }

                    block.modulation = modulation;
                }

                if (i == 0 && strip.hasInstrument)
                    block.events = std::span (state.events.data(), (size_t) state.numEvents);

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
        if (const auto& event = state.events[(size_t) i]; event.type == devices::NoteEvent::Type::noteOn)
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
