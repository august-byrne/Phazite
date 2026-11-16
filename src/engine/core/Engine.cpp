// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "devices/core/AudioThreads.h"
#include "engine/transport/Click.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace phazite::engine
{
namespace
{
    constexpr int maximumChannels = 64; // the most output channels a block plays into; any more stay silent

    double getMilliseconds() noexcept
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    // What MIDI's Reset All Controllers (RP-015) sets a controller to, as a device takes it, or none for one it leaves as it
    // is: the mod wheel, breath and the foot to none, expression to full, and the pedals (sustain, portamento, sostenuto,
    // soft, legato, hold 2) up.
    std::optional<float> getResetValue (int controller) noexcept
    {
        switch (controller)
        {
            case 1:
            case 2:
            case 4:
            case 64:
            case 65:
            case 66:
            case 67:
            case 68:
            case 69:
                return 0.0f;
            case 11:
                return 1.0f;
            default:
                return std::nullopt;
        }
    }

    // Whether every sample is sound, as the devices take it (devices::soundOf()): a number, and none beyond any level, as a
    // filter gone wrong plays on its way to infinity.
    bool isFinite (const float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            if (! (std::abs (samples[i]) <= devices::loudestSound))
                return false;

        return true;
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
    numClickStarts = 0;
    transport.plan = nullptr;  // a plan for another rate: the playhead stays on its beat
    blockClock = BlockClock(); // the device's blocks timed afresh
    monoSides.setSize (2, maximumBlockSize);

    // The fades at this rate. A jump fading out is taken again as the next block starts (it is still asked for), and fades
    // out afresh.
    fadeLength = std::max (1, (int) std::lround (fadeSeconds * sampleRate));
    jump.pending = jump.thenStop = false;
    fadingIn = 0;
    realign = {};
    leave = {};
    switching = false;
    stopFade = {};

    // A stop asked while no audio thread took it (no device played the engine) is taken at once: nothing fades out after
    // it, nothing having played.
    if (const auto asked = askedTransport.load(); transport.playing && isStopAsked (asked))
    {
        transport.notesOff = true;
        transport.playing = false;
        transport.countIn.left = 0;
        brokeTake = true;
        stopsTaken = asked >> stopsShift;
    }

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
    // which it would wait for, an instrument container's, whose chains share out the notes, and one that plays parts into
    // outputs of their own, which its strip clears as it starts.
    plan.earlyStrips.clear();

    for (size_t i = 0; i < plan.strips.size(); ++i)
    {
        auto& strip = plan.strips[i];
        strip.startsEarly = strip.hasInstrument && ! strip.devices.empty() && strip.devices.front().sidechain < 0
                            && strip.devices.front().parts.empty() && (strip.containers.empty() || strip.containers.front().first > 0)
                            && strip.devices.front().node->getRunner().getProcessor().startsEarly();

        if (strip.startsEarly)
            plan.earlyStrips.push_back ((int) i);
    }
}

void Engine::dropPlans()
{
    // The plan it played goes too, which the audio thread may still point at (it is called while a device plays the
    // engine, once two blocks have played none): counted, so that the audio thread never looks at it again
    // (choosePlan()), writing nothing of the audio thread's here.
    plans.give (nullptr);
    plans.freeReplaced();
    drops.fetch_add (1, std::memory_order_release);
}

void Engine::setRecording (std::unique_ptr<Recording> recording)
{
    recordings.give (std::move (recording));
}

//==============================================================================
void Engine::play (bool countIn)
{
    playing = true;
    askedTransport.store ((stopsAsked << stopsShift) | playBit | (countIn ? countInBit : 0u));
}

void Engine::stop()
{
    playing = false;
    askedTransport.store (++stopsAsked << stopsShift);
}

bool Engine::isStopAsked (std::uint32_t asked) const noexcept
{
    return (asked & playBit) == 0 || (asked >> stopsShift) != stopsTaken;
}

void Engine::locate (double beat)
{
    // Sequentially consistent, all three: a beat read between two equal even counts was written before the first.
    locateCount.fetch_add (1);
    requestedBeat.store (beat);
    locateCount.fetch_add (1);

    // The audio files where it lands, read there first, ahead of everything else, at once (known bug 36: the reading thread
    // learnt of it only as the engine landed, and read it when it next looked): its audio clips', and a version's bounce
    // compared with the song, at the song's beat in the version's time. The plan given last, which this thread gave.
    // At the plan's own rate, which the audio thread doesn't change (prepare() sets the engine's on the device's thread).
    if (const auto* plan = plans.get())
    {
        const auto rate = plan->tempo.getSampleRate();
        const auto sample = std::llround (plan->tempo.toSamples (beat));

        for (const auto& strip : plan->strips)
            for (const auto& region : strip.audio)
                region.want (sample, rate);

        if (plan->comparison.has_value())
            plan->comparison->bounce.want (std::llround (plan->comparison->tempo.toSamples (beat)), rate);
    }
}

double Engine::getPlayheadBeat() const noexcept
{
    // A locate the audio thread hasn't taken yet: where the playhead will be.
    if (getLocatesAsked() != locatesTaken.load())
        return requestedBeat.load();

    // From where the latest block started, by the time since. A block that started later than now (one of several the device
    // took at once, or blocks played faster than real time) shows where it starts.
    if (const auto latest = playheadStamps.getLatest())
        return latest->beat
               + std::clamp (getMilliseconds() - latest->milliseconds, 0.0, 1.5 * latest->length) * latest->beatsPerMillisecond;

    return requestedBeat.load();
}

double Engine::getPlayheadBeatAt (double milliseconds) const noexcept
{
    if (getLocatesAsked() != locatesTaken.load())
        return requestedBeat.load();

    return playheadStamps.getBeatAt (milliseconds).value_or (requestedBeat.load());
}

PlayheadStamps::Place Engine::getPlayheadPlaceBefore (double milliseconds) const noexcept
{
    if (getLocatesAsked() != locatesTaken.load())
        return { requestedBeat.load(), playheadStamps.getLatestRound() };

    // A block that went round the loop in the middle is stamped from where it started (publishPlayhead()).
    const auto* plan = plans.get();
    const auto looped = plan != nullptr && plan->loop.has_value();
    return playheadStamps.getPlaceBefore (getMilliseconds(), milliseconds, looped ? plan->loop->start : 0.0, looped ? plan->loop->end : 0.0)
        .value_or (PlayheadStamps::Place { requestedBeat.load(), playheadStamps.getLatestRound() });
}

void Engine::publishPlayhead (double beat, double beatsPerSecond, double startMilliseconds, double lengthMilliseconds, std::uint32_t round,
                              bool wentRound) noexcept
{
    playheadStamps.publish ({ beat, startMilliseconds, beatsPerSecond / 1000.0, lengthMilliseconds,
                              locatesTaken.load (std::memory_order_relaxed), round, wentRound });
}

//==============================================================================
void Engine::takeTransport (const Plan& plan) noexcept
{
    // The last locate asked for, if it isn't taken yet: read between two equal even counts; and what the transport was
    // asked, with no locate asked meanwhile (a locate() and a play() between the two would start from where the song was).
    // A few tries, else the next part's.
    std::optional<std::pair<double, std::uint32_t>> locate;
    auto asked = askedTransport.load();

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        locate.reset();
        const auto before = locateCount.load();
        const auto beat = requestedBeat.load();
        asked = askedTransport.load();

        if (before % 2 != 0 || locateCount.load() != before)
            continue;

        if (before / 2 != locatesTaken.load (std::memory_order_relaxed))
            locate = std::pair (beat, before / 2);

        break;
    }

    // A stop first, then the jump, then a start: whatever their order was, from where the playhead was asked to be. A stop
    // that comes while a jump fades out waits for the fade's end, where the jump is taken, and stops there (known bug 162:
    // the jump was taken at once, its fade cut short, in a step).
    if (transport.playing && isStopAsked (asked))
    {
        if (jump.pending)
            jump.thenStop = true;
        else
            stopTransport (false);
    }

    stopsTaken = asked >> stopsShift;

    if (locate.has_value() && locate->second != locatesTaken.load (std::memory_order_relaxed)) // not taken by the stop
    {
        // While the song plays (not counting in), what sounds fades out first; the latest beat asked for is taken then. Held
        // at a landing, silent, it is taken at once (and not held there in turn: Landing).
        if (transport.playing && transport.countIn.left == 0 && ! landing.held)
        {
            // From where what follows a jump just before fades in, if it does, rather than from full level (a fade out
            // across a plan realigned, under way too, goes on: the lower of the two is heard).
            if (! jump.pending)
            {
                jump = { true, 0.0, 0, fadingIn > 0 ? std::max (1, fadeLength - fadingIn + 1) : fadeLength };
                fadingIn = 0;
            }

            jump.beat = locate->first;
            jump.count = locate->second;
        }
        else
        {
            takeJump (plan, locate->first, locate->second, clock + partStart);
        }
    }

    if ((asked & playBit) != 0 && ! transport.playing)
        startTransport (plan, (asked & countInBit) != 0);
}

void Engine::startTransport (const Plan& plan, bool countIn) noexcept
{
    // The notes sounding where it starts start too. A count-in: a bar there, on its time signature's beats, at the tempo
    // there.
    if (countIn)
    {
        auto& counting = transport.countIn;
        const auto signature = plan.tempo.getTimeSignatureAt (transport.beat);
        counting.barLength = signature.getBarLength();
        counting.beatLength = signature.getBeatLength();
        counting.samplesPerBeat = plan.tempo.getSamplesPerQuarterNote (transport.beat);
        counting.length = std::llround (counting.barLength * counting.samplesPerBeat);
        counting.left = counting.length;
        counting.firstBeat = transport.beat - counting.barLength;
        counting.barStart = plan.tempo.getBarStart (transport.beat);
        numClickStarts = 0;
    }

    // From a stop, the song starts from silence: tails ringing on from before (a reverb's, a delay's) stop as it starts, as
    // Ableton Live and Bitwig reset plug-ins. Its loop's end doesn't cut them. The audio clips' fade out after the stop
    // before ends there: they start again (AudioRegion fades them in).
    transport.chase = true;
    transport.flush = true;
    transport.playing = true;
    stopFade = {};
}

void Engine::stopTransport (bool fadedOut) noexcept
{
    // The notes sounding end, and what they leave rings on; the audio clips that play, which would stop in a step, fade
    // out from where the song is, unless what goes out has faded out already (a jump's fade, which a stop waits for). The
    // clicks to come don't, but the one sounding rings on.
    if (transport.countIn.left == 0 && ! fadedOut && ! landing.held)
        stopFade = { transport.sample, transport.sample + fadeLength };

    transport.notesOff = true;
    transport.playing = false;
    transport.countIn.left = 0;
    brokeTake = true;
    numClickStarts = 0;
    endHold();
}

void Engine::takeJump (const Plan& plan, double beat, std::uint32_t count, std::int64_t at) noexcept
{
    // A jump while playing: the notes sounding stop, and those where it lands start, from silence, fading in if what
    // sounded faded out. The clicks to come were for where the song was, but the one sounding rings on.
    if (transport.playing)
    {
        transport.notesOff = true;
        transport.flush = true;

        if (jump.pending)
            fadingIn = fadeLength;

        // What the paths held is gone with the flush: nothing is played again for a hold across a leave to keep out.
        realign.hold = 0;
    }

    brokeTake = true;
    transport.chase = true;
    transport.beat = beat;
    transport.sample = std::llround (plan.tempo.toSamples (beat));
    numClickStarts = 0;
    jump.pending = jump.thenStop = false;
    locatesTaken.store (count);

    // Playing on from where it lands, held there while the audio clips at another rate there make their resampling ready
    // (Landing, Engine.h): not during a count-in, nor in a render, nor in a run of jumps.
    const auto heldBefore = landing.held;
    endHold();

    if (! transport.playing || transport.countIn.left > 0)
        return;

    const auto inRun = heldBefore || at - landing.lastJump < (std::int64_t) (runSeconds * sampleRate);
    landing.lastJump = at;

    if (holdsLandings && ! rendering && ! inRun && blockLength <= maximumHoldSeconds * sampleRate
        && needsPreparingAt (plan, transport.sample))
        landing.held = true;
}

bool Engine::needsPreparingAt (const Plan& plan, std::int64_t songSample) const noexcept
{
    for (const auto& strip : plan.strips)
        for (const auto& region : strip.audio)
            if (region.needsPreparing (songSample, sampleRate))
                return true;

    return false;
}

void Engine::prepareLanding (const Plan& plan, int numSamples) noexcept
{
    // A share of the block for each, as much of its file as it plays in one (AudioRegion::prepare()).
    const std::int64_t places[] = { transport.sample };

    for (const auto& strip : plan.strips)
        for (const auto& region : strip.audio)
            region.prepare (transport.sample, false, places, sampleRate, numSamples);
}

const Plan* Engine::choosePlan (const Plan* given) noexcept
{
    // The plans dropped since the last part (dropPlans()): the one it played is gone, and nothing fades across it.
    if (const auto dropped = drops.load (std::memory_order_acquire); dropped != dropsSeen)
    {
        dropsSeen = dropped;
        transport.plan = nullptr;
        realign = {};
        leave = {};
        switching = false;
    }

    // None given (a render has the song's plug-ins): nothing to fade across.
    if (given == nullptr)
    {
        realign = {};
        leave = {};
        switching = false;
        return nullptr;
    }

    // A new plan while the song plays (not counting in, nor jumping, which starts from silence anyway), at the engine's
    // rate, as the plan played is (one for another is never played): nothing of that is read, but its alignment and its
    // departures, kept with it.
    const auto canFade = transport.plan != nullptr && given != transport.plan && transport.playing && transport.countIn.left == 0
                         && ! jump.pending && juce::exactlyEqual (given->tempo.getSampleRate(), sampleRate);

    // Fading on the plan it played (alive: kept in the part before, Handover::Block::keep()); a change of the delay
    // compensation that comes meanwhile dips what goes out too, and paths that go in a plan that comes meanwhile start
    // their own fades, which the plan played is kept for.
    if (isKeeping())
    {
        if (! realign.dipped && canFade && given->alignment != playedAlignment)
            startRealign();

        if (canFade && given->departures != (leave.active ? leave.upTo : playedDepartures))
            startLeave (given->departures);

        // (Stopped, the hold counts down as it was: what the paths hold plays out after a stop, and would play again what
        // the hold keeps out. A jump's landing ends it: takeJump().)
        if (canFade)
            holdAcrossLeave (*given);

        return transport.plan;
    }

    // Faded: the new plan, fading in if what goes out dipped. (Whatever ended the keeping: a dip or a leave is never left
    // behind once nothing keeps the plan played, a hold ended by a stop or a jump at a part's start too.)
    if (std::exchange (switching, false) || realign.dipped || leave.active)
    {
        if (std::exchange (realign.dipped, false))
            fadingIn = fadeLength;

        realign.hold = 0;
        realign.walked = false;
        leave = {};
        return given;
    }

    if (! canFade)
        return given;

    // Its delay compensation changed (a path's delay that goes on playing): what goes out fades out first, from where it
    // is (fading in after another, which it joins). Paths gone: they alone fade out first.
    const auto realigns = given->alignment != playedAlignment;
    const auto departs = given->departures != playedDepartures;

    if (! realigns && ! departs)
        return given;

    if (realigns)
        startRealign();

    if (departs)
        startLeave (given->departures);

    holdAcrossLeave (*given);
    return transport.plan;
}

void Engine::holdAcrossLeave (const Plan& given) noexcept
{
    // A dip across a leave: kept as long as its leaving paths' fades last, and then as long as the given plan's paths read
    // further back than those played did (none on most plans: the walk ends at each path's delay played). Once for each
    // plan that brings a change of either (its alignment or its departures: a plan that brings neither has the same paths'
    // lengths and leaves as the one walked), from where the fades are then; it counts down from there, so that it ends, at
    // most the fade and the largest growth after the last such plan (rather than start again in each part while the
    // given plan's delays, which play only after it, report the same growth). Ended sooner by a jump's landing (takeJump()), by the plans
    // dropped and by none given (realign = {}); a stop lets it count down, as what the paths hold plays out after it.
    if (! realign.dipped || ! leave.active)
        return;

    if (realign.walked && given.alignment == realign.walkedAlignment && given.departures == realign.walkedDepartures)
        return;

    realign.walked = true;
    realign.walkedAlignment = given.alignment;
    realign.walkedDepartures = given.departures;
    auto growth = 0;
    const auto take = [&growth] (const std::shared_ptr<Delay>& delay)
    {
        if (delay != nullptr)
            growth = std::max (growth, delay->getGrowth());
    };

    for (const auto& strip : given.strips)
    {
        for (const auto& input : strip.inputs)
            take (input.delay);

        for (const auto& device : strip.devices)
            take (device.sidechainDelay);

        for (const auto& container : strip.containers)
            for (const auto& chain : container.chains)
                take (chain.delay);
    }

    realign.hold = std::max (realign.hold, leave.left + growth);
}

void Engine::startRealign() noexcept
{
    realign.left = fadingIn > 0 ? std::max (1, fadeLength - fadingIn + 1) : fadeLength;
    realign.dipped = true;
    fadingIn = 0;
}

void Engine::startLeave (std::uint64_t upTo) noexcept
{
    // The paths that go up to these departures each fade out from the next stretch played, the plan played kept as long.
    if (! leave.active)
        leave = { true, 0, playedDepartures, 0 };

    leave.left = fadeLength;
    leave.upTo = upTo;
}

std::pair<float, float> Engine::Span::getPathGains (Delay* delay, std::pair<float, float> gains) const noexcept
{
    if (! leaving || delay == nullptr)
        return gains;

    if (const auto departure = delay->getDeparture(); departure > leavesAfter && departure <= leavesUpTo)
        return delay->leave (engine->fadeLength, numSamples, gains);

    return gains;
}

bool Engine::isFading() const noexcept
{
    return jump.pending || fadingIn > 0 || realign.left > 0 || realign.dipped;
}

void Engine::fadeMaster (float* const* sides, int offset, int numSamples) noexcept
{
    // Down to silence over what is left of a jump's fade out, or of one across a plan realigned (from so far into the part,
    // where it was as the part started), the lower of the two; and up from silence after: each sample a step of
    // fadeLength + 1.
    const auto step = 1.0f / (float) (fadeLength + 1);

    for (int i = 0; i < numSamples; ++i)
    {
        auto gain = 1.0f;

        if (jump.pending)
            gain = (float) std::max (0, jump.left - i) * step;

        // (Silent once faded out, while the plan played is kept.)
        if (realign.left > 0 || realign.dipped)
        {
            gain = std::min (gain, (float) std::max (0, realign.left - offset - i) * step);
        }
        else if (fadingIn > 0 && ! jump.pending)
        {
            gain = (float) (fadeLength - fadingIn + 1) * step;
            --fadingIn;
        }

        sides[0][i] *= gain;
        sides[1][i] *= gain;
    }
}

void Engine::expectAudio (const Plan& plan) noexcept
{
    // Where the audio clips' files play next that what reads them ahead can't see coming (audio/AudioFile.h reads ahead of
    // where a file plays): while the song waits to play, where the playhead is, from where it will start; and the loop's
    // start, which follows the loop's end. Each is read ahead of as a place the file plays is.
    // (Kept for the strips' audio clips at another rate, which make their resampling ready for the first of them that each
    // plays at: AudioRegion::prepare().)
    auto& places = expectedPlaces;
    auto& numPlaces = numExpectedPlaces;
    numPlaces = 0;

    // A jump fading out: where it lands, first (its file read there first already: locate()).
    if (jump.pending)
        places[numPlaces++] = std::llround (plan.tempo.toSamples (jump.beat));

    if (! transport.playing || transport.countIn.left > 0)
        places[numPlaces++] = transport.sample;

    if (plan.loop.has_value() && plan.loop->end > plan.loop->start)
        places[numPlaces++] = std::llround (plan.tempo.toSamples (plan.loop->start));

    for (const auto& strip : plan.strips)
        for (const auto& region : strip.audio)
            for (size_t i = 0; i < numPlaces; ++i)
                region.expect (places[i], sampleRate);

    // A version's bounce compared with the song, the same, at the song's beats in the version's time (known bug 39).
    if (plan.comparison.has_value())
        for (size_t i = 0; i < numPlaces; ++i)
            plan.comparison->bounce.expect (std::llround (plan.comparison->tempo.toSamples (plan.tempo.toBeats ((double) places[i]))),
                                            sampleRate);
}

void Engine::playLive (std::int64_t track, devices::NoteEvent event, bool recordable) noexcept
{
    liveQueue.push ({ track, event, getMilliseconds(), recordable });
}

void Engine::endLive (std::int64_t track) noexcept
{
    liveQueue.push ({ track, {}, getMilliseconds(), true, true });
}

void Engine::dropLiveEvents() noexcept
{
    // What lets go, each once (a note's end by its track and pitch, a controller by its number, what endLive() ends by its
    // track), kept; the rest dropped.
    using Type = devices::NoteEvent::Type;

    for (LiveEvent live; liveQueue.pop (live);)
    {
        const auto& event = live.event;

        if (! live.endsLive && (event.type == Type::noteOn || event.type == Type::portamentoOn || event.type == Type::slide))
            continue;

        const auto same = [&live] (const LiveEvent& waiting)
        {
            return waiting.track == live.track && waiting.endsLive == live.endsLive && waiting.event.type == live.event.type
                   && waiting.event.number == live.event.number;
        };

        if (const auto end = std::find_if (endsWaiting.begin(), endsWaiting.begin() + numEndsWaiting, same);
            end != endsWaiting.begin() + numEndsWaiting)
            *end = live; // the latest value
        else if (numEndsWaiting < (int) endsWaiting.size())
            endsWaiting[(size_t) numEndsWaiting++] = live;
    }

    lastBlockMilliseconds = 0.0; // the next part's live notes are timed from its own start
}

void Engine::followLive (const LiveEvent& live) noexcept
{
    using Type = devices::NoteEvent::Type;
    constexpr int sustainPedal = 64;
    const auto& event = live.event;
    const auto starts = event.type == Type::noteOn || event.type == Type::portamentoOn
                        || (event.type == Type::controller && event.number == sustainPedal && event.value >= 0.5f);

    // Its track's hold; one that holds nothing any more is taken for a track that starts holding.
    LiveHold* hold = nullptr;

    for (int i = 0; i < numLiveHolds && hold == nullptr; ++i)
        if (liveHolds[(size_t) i].track == live.track)
            hold = &liveHolds[(size_t) i];

    if (hold == nullptr && starts)
    {
        for (int i = 0; i < numLiveHolds && hold == nullptr; ++i)
            if (liveHolds[(size_t) i].numNotes == 0 && ! liveHolds[(size_t) i].pedalDown)
                hold = &liveHolds[(size_t) i];

        if (hold == nullptr && numLiveHolds < (int) liveHolds.size())
            hold = &liveHolds[(size_t) numLiveHolds++];

        if (hold != nullptr)
            *hold = { live.track };
    }

    if (hold == nullptr)
        return;

    auto& times = hold->notes[(size_t) std::clamp (event.number, 0, 127)];

    if ((event.type == Type::noteOn || event.type == Type::portamentoOn) && times < 255)
    {
        ++times;
        ++hold->numNotes;
    }
    else if (event.type == Type::noteOff && times > 0)
    {
        --times;
        --hold->numNotes;
    }
    else if (event.type == Type::allNotesOff)
    {
        hold->notes.fill (0);
        hold->numNotes = 0;
    }
    else if (event.type == Type::controller && event.number == sustainPedal)
    {
        hold->pedalDown = event.value >= 0.5f;
    }
}

void Engine::addLiveEvent (LiveEvent live, std::int64_t loopStart, std::int64_t loopEnd) noexcept
{
    // endLive(): each note its track holds live ends, as often as it was started, and its pedal, if it holds it down; as
    // live events of their own, so that a take and Capture record the notes' ends where they came.
    if (live.endsLive)
    {
        using Type = devices::NoteEvent::Type;
        constexpr int sustainPedal = 64;
        const auto held = std::find_if (liveHolds.begin(), liveHolds.begin() + numLiveHolds,
                                        [&live] (const LiveHold& hold) { return hold.track == live.track; });

        if (held == liveHolds.begin() + numLiveHolds)
            return;

        const auto hold = *held; // a copy: the ends below change it

        for (int pitch = 0; pitch < 128; ++pitch)
            for (int times = 0; times < hold.notes[(size_t) pitch]; ++times)
                addLiveEvent ({ live.track, { Type::noteOff, live.event.sample, pitch, 0.0f }, live.milliseconds, live.recordable },
                              loopStart, loopEnd);

        if (hold.pedalDown)
            addLiveEvent ({ live.track, { Type::controller, live.event.sample, sustainPedal, 0.0f }, live.milliseconds, false }, loopStart,
                          loopEnd);

        return;
    }

    if (numLiveEvents >= (int) liveEvents.size())
        return;

    followLive (live);
    liveEvents[(size_t) numLiveEvents++] = live;

    if (! live.recordable)
        return;

    // Where the song was as it came in, in the last part: round the loop, as it plays.
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

void Engine::takeLiveEvents (const Plan& plan, int numSamples, std::int64_t partClock) noexcept
{
    // Each in the block as far as it came after the last block started: a block's delay, the same for every note.
    const auto now = getMilliseconds();
    const auto since = lastBlockMilliseconds > 0.0 ? lastBlockMilliseconds : now;
    lastBlockMilliseconds = now;
    numLiveEvents = 0;
    std::int64_t loopStart = 0, loopEnd = 0;

    if (plan.loop.has_value())
    {
        loopStart = std::llround (plan.tempo.toSamples (plan.loop->start));
        loopEnd = std::llround (plan.tempo.toSamples (plan.loop->end));
    }

    // What the part a hold started in took after where it started, first, at its start (where the hold landed); followed
    // and recorded as it was taken (liveLanding).
    for (int i = 0; i < numLiveLanding && numLiveEvents < (int) liveEvents.size(); ++i)
    {
        liveEvents[(size_t) numLiveEvents] = liveLanding[(size_t) i];
        liveEvents[(size_t) numLiveEvents++].event.sample = 0;
    }

    numLiveLanding = 0;

    // What let go while there was no plan, then, unrecorded (what started it wasn't).
    for (int i = 0; i < numEndsWaiting; ++i)
    {
        auto end = endsWaiting[(size_t) i];
        end.event.sample = 0;
        end.recordable = false;
        addLiveEvent (end, loopStart, loopEnd);
    }

    numEndsWaiting = 0;

    for (LiveEvent live; numLiveEvents < (int) liveEvents.size() && liveQueue.pop (live);)
    {
        live.event.sample = std::clamp ((int) std::lround ((live.milliseconds - since) * sampleRate / 1000.0), 0, numSamples - 1);
        addLiveEvent (live, loopStart, loopEnd);
    }

    lastPart = { partClock, transport.sample, transport.playing && transport.countIn.left == 0 };
}

void Engine::keepLiveForLanding (int from) noexcept
{
    // Never full: it holds what one part took at most, and is emptied as the next part takes its own.
    for (int i = 0; i < numLiveEvents; ++i)
        if (liveEvents[(size_t) i].event.sample >= from && numLiveLanding < (int) liveLanding.size())
            liveLanding[(size_t) numLiveLanding++] = liveEvents[(size_t) i];

    numLiveEvents = 0;
}

void Engine::landLiveEvents (const Plan& plan, int offset, int numSamples, std::int64_t partClock) noexcept
{
    // Those of the rest of the part, from where it lands, as a part of their own would take them; at their places in it.
    takeLiveEvents (plan, numSamples - offset, partClock + offset);

    for (int i = 0; i < numLiveEvents; ++i)
        liveEvents[(size_t) i].event.sample += offset;

    lastPart = { partClock, transport.sample - offset, true };
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
    blockLength = std::max (1, numSamples);
    const auto startTicks = juce::Time::getHighResolutionTicks();
    const auto startMilliseconds = blockClock.stamp (getMilliseconds(), numSamples, sampleRate); // smoothed (PlayheadClock.h)
    numOutputs = std::min (numOutputs, maximumChannels);
    numInputs = inputs != nullptr ? std::clamp (numInputs, 0, maximumInputs) : 0;

    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::clear (outputs[channel], numSamples);

    // A block longer than prepared plays in parts, and so does one longer than its plan was made for: the device's blocks
    // grew (it started again with a larger buffer), and the plan made for them is still being built.
    std::array<float*, maximumChannels> part {};

    for (int done = 0; done < numSamples;)
    {
        // The plan given last, or the one played before while what goes out fades out across a plan realigned, or paths
        // that leave do (said before anything of it is used, so that it isn't freed meanwhile); a fade out ends where a
        // part does.
        const Handover<Plan>::Block held { plans };
        const auto* plan = choosePlan (held.get());
        held.keep (plan);
        const auto longest =
            plan != nullptr && plan->maximumBlockSize > 0 ? std::min (maximumBlockSize, plan->maximumBlockSize) : maximumBlockSize;
        const auto length = std::min ({ numSamples - done, longest, realign.left > 0 ? realign.left : longest,
                                        leave.left > 0 ? leave.left : longest, realign.hold > 0 ? realign.hold : longest });

        for (int channel = 0; channel < numOutputs; ++channel)
            part[(size_t) channel] = outputs[channel] != nullptr ? outputs[channel] + done : nullptr;

        numPartInputs = numInputs;

        for (int channel = 0; channel < numInputs; ++channel)
            partInputs[(size_t) channel] = inputs[channel] != nullptr ? inputs[channel] + done : nullptr;

        const Handover<Recording>::Block heldRecording { recordings };
        auto* recording = heldRecording.get();
        partStart = done;

        // A plan for another rate (the device's changed, and the new one is still being built) plays nothing.
        if (plan != nullptr && juce::exactlyEqual (plan->tempo.getSampleRate(), sampleRate))
        {
            // A new plan, perhaps of another tempo or rate: the playhead stays on its beat.
            if (transport.plan != plan)
            {
                transport.sample = std::llround (plan->tempo.toSamples (transport.beat));
                transport.plan = plan;
                playedAlignment = plan->alignment;
                playedDepartures = plan->departures;
            }

            takeTransport (*plan);

            // Held where a jump landed (Landing), the live notes wait in their queue for where it lands (landLiveEvents()): from
            // here, as they came in now.
            if (landing.held)
            {
                numLiveEvents = 0;
                lastBlockMilliseconds = getMilliseconds();
                lastPart = { clock + done, transport.sample, true };
            }
            else
            {
                takeLiveEvents (*plan, length, clock + done);
            }
            expectAudio (*plan);
            auto startBeat = transport.beat;
            auto startRound = loopRounds; // the times round the loop as it started (or after a jump taken in it)
            auto counted = 0, jumpedAt = 0;

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
                findCountInClicks (*plan, counted);
                transport.countIn.left -= counted;
            }

            if (! transport.playing)
            {
                playSpan (*plan, part.data(), numOutputs, 0, length);
            }
            else
            {
                auto offset = counted;

                auto preparedLanding = false;

                while (offset < length && transport.playing)
                {
                    // Held where a jump landed (Landing): silent, in whole blocks, while the audio clips at another rate there
                    // make their resampling ready, a share of each block; then on from there, at the place in the block it
                    // landed in, as it would have played at once.
                    if (landing.held)
                    {
                        if (! std::exchange (preparedLanding, true))
                            prepareLanding (*plan, numSamples);

                        const auto maximumHeld = (int) (maximumHoldSeconds * sampleRate) / blockLength * blockLength;

                        if (landing.heldFor > 0 && landing.heldFor % blockLength == 0
                            && (landing.heldFor >= maximumHeld || ! needsPreparingAt (*plan, transport.sample)))
                        {
                            endHold();
                            jumpedAt = offset;
                            startBeat = transport.beat;
                            startRound = loopRounds;
                            landLiveEvents (*plan, offset, length, clock + done);
                            continue;
                        }

                        const auto silent = std::min (length - offset, blockLength - landing.heldFor % blockLength);
                        landing.heldFor += silent;
                        offset += silent;
                        continue;
                    }

                    auto span = length - offset;
                    std::int64_t loopStart = 0, loopEnd = 0;

                    if (plan->loop.has_value())
                    {
                        loopStart = std::llround (plan->tempo.toSamples (plan->loop->start));
                        loopEnd = std::llround (plan->tempo.toSamples (plan->loop->end));

                        // Past the loop's end (it was moved before the playhead), playback starts again at its start, the
                        // notes sounding ending and those there starting, as a loop round does; it plays as far as its end
                        // at most.
                        if (loopEnd > loopStart)
                        {
                            if (transport.sample >= loopEnd)
                            {
                                transport.sample = loopStart;
                                ++loopRounds;
                                transport.notesOff = true;
                                transport.chase = true;
                            }

                            span = (int) std::min<std::int64_t> (span, loopEnd - transport.sample);
                        }
                    }

                    // A jump fading out: as far as its fade's end, where it is taken.
                    if (jump.pending)
                        span = std::min (span, jump.left);

                    // At the loop's end, the notes sounding end on its last sample, and those at its start start again.
                    endsAtLoop = loopEnd > loopStart && transport.sample + span >= loopEnd;
                    playSpan (*plan, part.data(), numOutputs, offset, span);
                    endsAtLoop = false;

                    // What comes in meanwhile, into the take, which starts here if it hasn't yet: not while a jump fades
                    // out, which ends the take as it was asked for (a take given after it starts where it lands).
                    if (recording != nullptr && ! jump.pending)
                        recording->record (partInputs.data(), numPartInputs, offset, span, transport.sample, clock + done + offset);

                    transport.sample += span;
                    offset += span;

                    if (loopEnd > loopStart && transport.sample >= loopEnd)
                    {
                        transport.sample = loopStart;
                        transport.chase = true;
                        ++loopRounds;
                    }

                    // The jump, its fade out done; the take ends with it. The live notes taken in the next part, from
                    // this one's start, are where the song is from here.
                    if (jump.pending && (jump.left -= span) <= 0)
                    {
                        const auto stops = jump.thenStop;
                        takeJump (*plan, jump.beat, jump.count, clock + done + offset);

                        if (recording != nullptr)
                            recording->end();

                        brokeTake = false;
                        jumpedAt = offset;
                        startBeat = transport.beat;
                        startRound = loopRounds;
                        lastPart.sample = transport.sample - offset;

                        // A stop that waited for the jump's fade: where it landed, with nothing left to fade out.
                        if (stops)
                            stopTransport (true);

                        // Held there: the live notes this part took for after here, where it lands.
                        if (landing.held)
                            keepLiveForLanding (offset);
                    }
                }

                transport.beat = plan->tempo.toBeats ((double) transport.sample);

                // The rest of the part after such a stop, stopped.
                if (offset < length)
                    playSpan (*plan, part.data(), numOutputs, offset, length - offset);
            }

            // Where this part started, and when: the block's start on its smoothed clock and the samples before the part;
            // or after a jump taken in it, where it landed and when.
            // (Held where a jump landed, it waits there.)
            const auto beatsPerSecond = transport.playing && transport.countIn.left == 0 && ! landing.held
                                            ? sampleRate / plan->tempo.getSamplesPerQuarterNote (startBeat)
                                            : 0.0;
            publishPlayhead (startBeat, beatsPerSecond, startMilliseconds + (done + jumpedAt) * 1000.0 / sampleRate,
                             (length - jumpedAt) * 1000.0 / sampleRate, startRound, loopRounds != startRound);
        }
        else
        {
            transport.plan = nullptr; // freed once replaced: never compared with the next

            // No plan (a render has the song's plug-ins, or a plan for the device's new rate is still being built): live
            // notes don't wait to sound as one burst when one comes, but what lets go does; and the transport is taken
            // as last asked when one comes.
            dropLiveEvents();
        }

        // The clicks that start in it, or ring on into it.
        playClicks (part.data(), numOutputs, length);
        done += length;

        // The fades on the plan played (across a plan realigned, of paths that leave), done: the new plan from the next part.
        const auto wasKeeping = isKeeping();
        realign.left = std::max (0, realign.left - length);
        leave.left = std::max (0, leave.left - length);
        realign.hold = std::max (0, realign.hold - length);

        if (wasKeeping && ! isKeeping())
            switching = true;
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

        // The master's sound goes out (on the first two channels), or while a version is compared with the song, the one of
        // the two that is heard.
        auto& master = *plan.strips.back().state;

        // Around a jump while playing, and a plan realigned, what goes out fades out and in (the meters before it don't see
        // it).
        if (isFading())
        {
            float* sides[] = { master.buffer.getWritePointer (0, offset), master.buffer.getWritePointer (1, offset) };
            fadeMaster (sides, offset, numSamples);
        }

        // The last line of defence: a stretch with a sample that is no finite number (a device gone wrong, a plug-in's bad
        // output that nothing before caught) goes out as silence, rather than to the audio device, and to the loudness
        // meter, whose filters would keep it.
        if (! isFinite (master.buffer.getReadPointer (0, offset), numSamples)
            || ! isFinite (master.buffer.getReadPointer (1, offset), numSamples))
        {
            juce::FloatVectorOperations::clear (master.buffer.getWritePointer (0, offset), numSamples);
            juce::FloatVectorOperations::clear (master.buffer.getWritePointer (1, offset), numSamples);
        }

        const float* out[] = { master.buffer.getReadPointer (0, offset), master.buffer.getReadPointer (1, offset) };

        // One output alone (a mono device, or one channel chosen in the settings) plays both sides, each at half, rather
        // than the left alone.
        const auto mono = numOutputs == 1 && outputs[0] != nullptr;

        if (plan.comparison.has_value() && numOutputs > 0)
        {
            float* heard[] = { outputs[0] != nullptr ? outputs[0] + offset : nullptr,
                               numOutputs > 1 && outputs[1] != nullptr ? outputs[1] + offset : nullptr };

            if (mono)
            {
                monoSides.clear (0, numSamples);
                heard[0] = monoSides.getWritePointer (0);
                heard[1] = monoSides.getWritePointer (1);
            }

            const auto& comparison = *plan.comparison;
            comparison.state->process (comparison.bounce, plan.tempo, comparison.tempo, out, heard, mono ? 2 : std::min (numOutputs, 2),
                                       transport.sample, numSamples, transport.playing && transport.countIn.left == 0);

            // The version's bounce, a file's samples, the same.
            for (auto* side : heard)
                if (side != nullptr && ! isFinite (side, numSamples))
                    juce::FloatVectorOperations::clear (side, numSamples);

            if (mono)
                for (const auto* side : heard)
                    juce::FloatVectorOperations::addWithMultiply (outputs[0] + offset, side, 0.5f, numSamples);
        }
        else if (mono)
        {
            for (const auto* side : out)
                juce::FloatVectorOperations::addWithMultiply (outputs[0] + offset, side, 0.5f, numSamples);
        }
        else
        {
            for (int channel = 0; channel < std::min (numOutputs, 2); ++channel)
                if (outputs[channel] != nullptr)
                    juce::FloatVectorOperations::add (outputs[channel] + offset, out[channel], numSamples);
        }

        // How loud the song is.
        loudness.process (out, 2, numSamples);
    }

    transport.notesOff = false;
    transport.chase = false;
    transport.flush = false;

    // The audio clips' fade out after a stop moves on.
    if (! transport.playing && stopFade.sample < stopFade.end)
        stopFade.sample = std::min (stopFade.end, stopFade.sample + numSamples);

    if (plan.metronome && transport.playing)
        findClicks (plan, offset, numSamples);
}

void Engine::playStrips (const Plan& plan, int offset, int numSamples) noexcept
{
    // Where the song is for the stretch of the block. Where automation is: moving with the song while it plays; at the
    // playhead while the song is stopped. The control points it and modulation move on are the song's while it plays,
    // time's while it is stopped.
    Span span { this, &plan, offset, numSamples };
    span.songSample = transport.sample;
    span.beat = plan.tempo.toBeats ((double) transport.sample);
    span.tempo = plan.tempo.getTempoAt (span.beat);
    span.timeSignature = plan.tempo.getTimeSignatureAt (span.beat);
    span.bars = plan.tempo.getBarsAt (span.beat);
    span.barStart = plan.tempo.getBarStart (span.beat);

    if (transport.playing && numSamples > 0)
    {
        const auto endBeat = plan.tempo.toBeats ((double) (transport.sample + numSamples));
        span.beatsPerSample = (endBeat - span.beat) / numSamples;
        span.barsPerSample = (plan.tempo.getBarsAt (endBeat) - span.bars) / numSamples;
    }

    span.gridSample = transport.playing ? transport.sample : clock + partStart + offset;
    span.pass = ++passes;
    span.flush = transport.flush;

    // Paths that leave fade out alone (Leave), each from the first stretch it is heard to leave in.
    span.leaving = leave.active;
    span.leavesAfter = leave.after;
    span.leavesUpTo = leave.upTo;

    // The audio clips: while the song plays, faded out before the loop's end, which cuts them (as much as half the loop);
    // after a stop, for as long as they fade out from where it stopped.
    if (transport.playing)
    {
        span.playsAudio = true;
        span.audioSample = transport.sample;

        if (plan.loop.has_value())
        {
            const auto loopStart = std::llround (plan.tempo.toSamples (plan.loop->start));
            const auto loopEnd = std::llround (plan.tempo.toSamples (plan.loop->end));
            span.cut = { loopEnd, (int) std::min<std::int64_t> (fadeLength, (loopEnd - loopStart) / 2) };
        }
    }
    else if (stopFade.sample < stopFade.end)
    {
        span.playsAudio = true;
        span.audioSample = stopFade.sample;
        span.cut = { stopFade.end, fadeLength, true };
    }
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
    // Its notes, what is played live into it and its modulators, then its instrument handed the block (muted too: its
    // channel silences what comes out); its own task takes what it played (playStrip()).
    const auto& strip = span.plan->strips[index];
    auto& state = *strip.state;

    if (span.flush)
        flushStrip (strip);

    takeEvents (span, strip);
    const auto& instrument = strip.devices.front();
    auto block = getDeviceBlock (span, strip, 0);
    block.events = std::span (state.events.data(), (size_t) state.numEvents);
    instrument.node->start (state.buffer, span.offset, span.numSamples, block, instrument.automation);
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

void Engine::resetContainers (const Plan::Strip& strip) noexcept
{
    for (const auto& container : strip.containers)
    {
        container.state->reset();

        for (const auto& chain : container.chains)
        {
            chain.state->reset();

            if (chain.delay != nullptr)
                chain.delay->reset();
        }
    }
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

    // What it writes into its taps, which the paths from it read: from silence too.
    for (const auto& tap : strip.taps)
        tap.ring->reset();

    resetContainers (strip);
}

DeviceRunner::Block Engine::getDeviceBlock (const Span& span, const Plan::Strip& strip, size_t index) noexcept
{
    const auto& device = strip.devices[index];
    DeviceRunner::Block block;
    block.tempo = span.tempo;
    block.beat = span.beat;
    block.bars = span.bars;
    block.songSample = span.songSample;
    block.timeSignatureNumerator = span.timeSignature.numerator;
    block.timeSignatureDenominator = span.timeSignature.denominator;
    block.barLength = span.timeSignature.getBarLength();
    block.barStart = span.barStart;
    block.isPlaying = transport.playing;
    block.isRendering = rendering || playingAsRender;
    block.beatsPerSample = span.beatsPerSample;
    block.barsPerSample = span.barsPerSample;
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

    return block;
}

bool Engine::hasInstrumentContainer (const Plan::Strip& strip) noexcept
{
    return ! strip.containers.empty() && strip.containers.front().first == 0 && strip.containers.front().instrument;
}

void Engine::playDevice (const Span& span, const Plan::Strip& strip, size_t index, juce::AudioBuffer<float>& buffer,
                         std::span<devices::NoteEvent> events) noexcept
{
    const auto& plan = *span.plan;
    const auto& device = strip.devices[index];
    const auto offset = span.offset, numSamples = span.numSamples;
    auto block = getDeviceBlock (span, strip, index);
    block.events = events;

    block.partOutputs = device.parts;
    block.outputOffset = offset;

    // What its sidechain listens to: another strip after its fader and before its mute, lined up with its sound.
    if (device.sidechain >= 0)
    {
        const auto& heard = plan.strips[(size_t) device.sidechain].state->beforeMute;
        const float* from[] = { heard.getReadPointer (0, offset), heard.getReadPointer (1, offset) };
        float* into[] = { buffer.getWritePointer (2, offset), buffer.getWritePointer (3, offset) };
        juce::FloatVectorOperations::clear (into[0], numSamples);
        juce::FloatVectorOperations::clear (into[1], numSamples);
        const auto [startGain, endGain] = span.getPathGains (device.sidechainDelay.get(), { 1.0f, 1.0f });
        (device.sidechainDelay != nullptr ? *device.sidechainDelay : noDelay).addDelayed (from, into, numSamples, startGain, endGain);
        block.hasSidechain = true;
    }

    device.node->process (buffer, offset, numSamples, block, device.automation);
}

void Engine::playContainer (const Span& span, const Plan::Strip& strip, const Plan::Container& container, float* const* channels) noexcept
{
    const auto offset = span.offset, numSamples = span.numSamples;
    auto& kept = *container.state;
    const auto count = std::min (container.chains.size(), (size_t) maximumChains);

    // What comes in, kept to mix with what the chains put out (an instrument container's chains start from silence).
    if (! container.instrument)
        kept.keepDry (channels, numSamples);

    // Each chain's part of it in its buffer; an instrument container's, its instrument's notes, those in its ranges.
    std::array<ChainPart, maximumChains> parts {};

    for (size_t c = 0; c < count; ++c)
    {
        const auto& chain = container.chains[c];
        auto& state = *chain.state;
        parts[c] = { &state, chain.crossover.get(), {} };

        if (container.split == Split::bands && ! chain.crossoverModulation.empty())
            parts[c].crossoverModulation = getFaderModulation (strip, chain.crossoverModulation, state.crossoverModulation);

        if (container.instrument)
        {
            juce::FloatVectorOperations::clear (state.buffer.getWritePointer (0, offset), numSamples);
            juce::FloatVectorOperations::clear (state.buffer.getWritePointer (1, offset), numSamples);
            state.numEvents = 0;

            for (int e = 0; e < strip.state->numEvents; ++e)
                if (const auto& event = strip.state->events[(size_t) e]; state.takes (event) && state.numEvents < (int) state.events.size())
                    state.events[(size_t) state.numEvents++] = event;
        }
    }

    if (! container.instrument)
        splitIntoParts (container.split, channels, offset, numSamples, std::span (parts.data(), count),
                        { span.beat, span.beatsPerSample, span.gridSample });

    // Each through its devices and its fader, lined up with the latest and added up. One muted (or soloed out) is silenced
    // by its fader, its devices playing on, as a track's are (known bug 78).
    juce::FloatVectorOperations::clear (channels[0], numSamples);
    juce::FloatVectorOperations::clear (channels[1], numSamples);

    for (size_t c = 0; c < count; ++c)
    {
        const auto& chain = container.chains[c];
        auto& state = *chain.state;
        auto& fader = state.getFader();

        for (int d = chain.first; d < chain.first + chain.count; ++d)
        {
            const auto events = container.instrument && d == chain.first ? std::span (state.events.data(), (size_t) state.numEvents)
                                                                         : std::span<devices::NoteEvent>();
            playDevice (span, strip, (size_t) d, state.buffer, events);
        }

        float* sides[] = { state.buffer.getWritePointer (0, offset), state.buffer.getWritePointer (1, offset) };
        Channel::Automation automation;
        automation.volume = chain.volume.get();
        automation.pan = chain.pan.get();
        automation.beatsPerSample = span.beatsPerSample;
        automation.sample = span.gridSample;
        automation.beat =
            chain.volume != nullptr || chain.pan != nullptr
                ? (transport.playing && chain.latency > 0 ? span.plan->tempo.toBeats ((double) (transport.sample - chain.latency))
                                                          : span.beat)
                : span.beat;
        automation.volumeModulation = getFaderModulation (strip, chain.volumeModulation, state.volumeModulation);
        automation.panModulation = getFaderModulation (strip, chain.panModulation, state.panModulation);
        fader.process (sides, 2, numSamples, automation);
        const auto [startGain, endGain] = span.getPathGains (chain.delay.get(), { 1.0f, 1.0f });
        (chain.delay != nullptr ? *chain.delay : noDelay).addDelayed (sides, channels, numSamples, startGain, endGain);
    }

    if (! container.instrument)
        kept.mix (channels, numSamples);
}

void Engine::playStrip (const Span& span, size_t index) noexcept
{
    const auto& plan = *span.plan;
    const auto& strip = plan.strips[index];
    const auto offset = span.offset, numSamples = span.numSamples;

    // Its devices' parts' own outputs, silent until they play into them: a device that doesn't play in a block (asleep)
    // would leave what it played before for a part's track to add again.
    for (const auto& device : strip.devices)
        for (const auto& part : device.partOutputs)
            part->clear (offset, numSamples);

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
            const auto& sound = input.beforeFader ? from.beforeFader : (input.beforeMute ? from.beforeMute : from.buffer);
            const float* sides[] = { input.part != nullptr ? input.part->getChannel (0) + offset : sound.getReadPointer (0, offset),
                                     input.part != nullptr ? input.part->getChannel (1) + offset : sound.getReadPointer (1, offset) };
            const auto [startGain, endGain] =
                span.getPathGains (input.delay.get(), input.level != nullptr ? input.level->moveOn() : std::pair (1.0f, 1.0f));
            (input.delay != nullptr ? *input.delay : noDelay).addDelayed (sides, channels, numSamples, startGain, endGain);
        }

        // Its events and modulators, unless they were taken as its instrument was handed its block.
        if (! startedEarly)
            takeEvents (span, strip);

        // What it plays, muted (or soloed out) too: its channel silences what comes out, and its instrument and effects play
        // on, as in Ableton Live and most DAWs, so that a note held, a reverb's tail and a delay's echoes are heard at once as
        // it is heard again (known bug 78: it stopped playing, and the notes it held stayed silent until its next).
        {
            // Its audio clips play into it before its effects, beside its instrument (after it, which plays into a cleared
            // buffer).
            const auto playAudio = [&]
            {
                if (span.playsAudio)
                    for (const auto& region : strip.audio)
                        region.play (channels, span.audioSample, numSamples, sampleRate, rendering || playingAsRender, state.scratch,
                                     span.cut);
            };

            auto audioPlayed = false;

            if (! strip.hasInstrument)
            {
                addInput (strip, channels, offset, numSamples);
                playAudio();
                audioPlayed = true;
            }

            // Its devices in their order, a container's chains side by side in buffers of their own; its instrument (or its
            // instrument container) first, given its notes, and its audio clips after it.
            const auto instrumentContainer = hasInstrumentContainer (strip);
            auto container = strip.containers.begin();

            for (size_t i = 0;;)
            {
                if (container != strip.containers.end() && container->first == (int) i)
                {
                    playContainer (span, strip, *container, channels);
                    i += (size_t) container->count;
                    ++container;
                }
                else if (i < strip.devices.size())
                {
                    const auto isInstrument = i == 0 && strip.hasInstrument && ! instrumentContainer;
                    playDevice (span, strip, i, state.buffer,
                                isInstrument ? std::span (state.events.data(), (size_t) state.numEvents) : std::span<devices::NoteEvent>());
                    ++i;
                }
                else
                {
                    break;
                }

                if (! audioPlayed)
                {
                    playAudio();
                    audioPlayed = true;
                }
            }

            if (! audioPlayed)
                playAudio();

            // Those of files at another rate made ready, a little a block, for where they play from next without playing
            // on to there (known bug 85: the loop's wrap started every one's resampler from nothing, in one block); while
            // a jump fades out, for where it lands first, and where it lands, held there until they are (Landing).
            for (const auto& region : strip.audio)
                region.prepare (span.songSample + numSamples, transport.playing && ! jump.pending,
                                std::span (expectedPlaces.data(), numExpectedPlaces), sampleRate, numSamples);
        }

        // What it sends before its fader, muted too (its sends' cuts say whether each sends: SendLevel).
        if (strip.feedsBeforeFader)
            for (int channel = 0; channel < 2; ++channel)
                juce::FloatVectorOperations::copy (state.beforeFader.getWritePointer (channel, offset), channels[channel], numSamples);

        Channel::Automation faderAutomation;
        faderAutomation.volume = strip.volume.get();
        faderAutomation.pan = strip.pan.get();
        faderAutomation.beatsPerSample = beatsPerSample;
        faderAutomation.sample = gridSample;
        faderAutomation.beat = strip.volume != nullptr || strip.pan != nullptr ? getAutomationBeat (strip.latency) : beat;
        faderAutomation.volumeModulation = getFaderModulation (strip, strip.volumeModulation, state.volumeModulation);
        faderAutomation.panModulation = getFaderModulation (strip, strip.panModulation, state.panModulation);
        // Its sound before its mute, for a sidechain (which hears a muted track, as its design says) and for its sends after
        // the fader (whose cut says whether they send: SendLevel).
        float* beforeMute[] = { state.beforeMute.getWritePointer (0, offset), state.beforeMute.getWritePointer (1, offset) };
        const auto keepsBeforeMute = strip.feedsSidechain || strip.feedsBeforeMute;
        state.getChannel().process (channels, 2, numSamples, faderAutomation, keepsBeforeMute ? beforeMute : nullptr);

        // What comes out of it at each tap, into the tap's ring, for the paths from there, which read it once this pass is
        // done (each waits for it): once, however many paths read it.
        for (const auto& tap : strip.taps)
        {
            using Point = Plan::Strip::TapPoint;
            const auto& from = tap.point == Point::beforeFader  ? state.beforeFader
                               : tap.point == Point::beforeMute ? state.beforeMute
                                                                : state.buffer;
            const float* sides[] = { tap.part != nullptr ? tap.part->getChannel (0) + offset : from.getReadPointer (0, offset),
                                     tap.part != nullptr ? tap.part->getChannel (1) + offset : from.getReadPointer (1, offset) };
            tap.ring->start();
            tap.ring->write (sides, numSamples);
        }

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
        point.place = { songSample / sampleRate, beat, plan.tempo.getTempoAt (beat), plan.tempo.getBarLength (beat),
                        plan.tempo.getBarsAt (beat) };
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

    if (! transport.playing)
        return;

    // None now (the song changed as it played: the track's clips gone, or their notes): what the notes before sounded ends.
    if (notes == nullptr)
    {
        if (state.sequence != nullptr)
        {
            for (size_t pitch = 0; pitch < 128; ++pitch)
                if (state.sounding[pitch])
                    state.addEvent ({ Type::noteOff, 0, (int) pitch, 0.0f });

            state.sounding.reset();
            state.sequence = nullptr;
        }

        return;
    }

    const auto from = (double) transport.sample;
    const auto fromBeat = plan.tempo.toBeats (from);

    // As far as the stretch goes, and where it ends at the loop's end, no further than the loop's end in beats: the loop's
    // last sample can end a little past that beat (a beat falls between two samples), and a note that starts on it is the
    // next bar's, which the loop never plays.
    auto toBeat = plan.tempo.toBeats (from + numSamples);

    if (endsAtLoop && plan.loop.has_value())
        toBeat = std::min (toBeat, plan.loop->end);

    // The notes sounding here: those that started before it and end after its first sample. A note that ends where the
    // span starts ends on that sample, however its beat rounds to it, so that a song played from where one note ends
    // and the next starts plays the next alone. Found by the sequence's index of them, not by a look through every note
    // before here (known bug 85).
    const auto firstSampleEnd = std::max (fromBeat, plan.tempo.toBeats (from + 0.5));
    const auto forEachSounding = [notes, fromBeat, firstSampleEnd] (auto&& action)
    { notes->forEachSounding (fromBeat, firstSampleEnd, action); };

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

        // Where playback starts, jumps or goes round the loop, the controllers, the bend and the channel's pressure as the
        // track's clips have them there, before the notes sounding start, as Cubase's and Ableton Live's chase does (a
        // plug-in with the mod wheel on its filter, a note started in the middle of a bend). Each the last value before here
        // where the clips hold one; where they hold none before here, the bend's middle, no pressure, and the controllers
        // that MIDI's Reset All Controllers resets (RP-015: the mod wheel, breath, the foot, expression at full and the
        // pedals) to that; any other controller the clips change only later (a volume, a pan) is left as it is, having no
        // value of its own to go back to. One that changes where playback starts (on its first sample) isn't chased: its own
        // change plays there, once. Not where the song changed as it played (a new sequence): its controllers are as they
        // were. A search through its changes for each one the track changes, nothing allocated.
        const auto chasesControls = transport.chase;

        if (chasesControls)
        {
            notes->forEachControlAt (fromBeat, firstSampleEnd,
                                     [&state] (const NoteSequence::ControlAt& control)
                                     {
                                         if (control.type == Type::aftertouch)
                                             return;

                                         if (control.held)
                                         {
                                             state.addEvent ({ control.type, 0, control.number, control.value });
                                             return;
                                         }

                                         if (control.type != Type::controller)
                                         {
                                             state.addEvent ({ control.type, 0, control.number, 0.0f }); // the middle, none
                                             return;
                                         }

                                         if (const auto reset = getResetValue (control.number))
                                             state.addEvent ({ control.type, 0, control.number, *reset });
                                     });
        }

        // The notes sounding here start, each from where it started.
        std::array<double, 128> startedAt;
        startedAt.fill (-1.0);

        forEachSounding (
            [&state, &startedAt] (const NoteSequence::Note& note)
            {
                if (! state.sounding[(size_t) note.pitch])
                {
                    state.addEvent ({ Type::noteOn, 0, note.pitch, note.velocity });
                    state.sounding.set ((size_t) note.pitch);
                    startedAt[(size_t) note.pitch] = note.start;
                }
            });

        // And a note's own pressure, where it changed since that note started.
        if (chasesControls)
            notes->forEachControlAt (fromBeat, firstSampleEnd,
                                     [&state, &startedAt] (const NoteSequence::ControlAt& control)
                                     {
                                         const auto pitch = (size_t) std::clamp (control.number, 0, 127);

                                         if (control.type == Type::aftertouch && control.held && startedAt[pitch] >= 0.0
                                             && control.beat >= startedAt[pitch])
                                             state.addEvent ({ control.type, 0, control.number, control.value });
                                     });

        state.mustChase = false;
    }

    // Each event in this stretch, at its sample.
    const auto& events = notes->getEvents();

    for (; state.nextEvent < events.size() && events[state.nextEvent].beat < toBeat; ++state.nextEvent)
    {
        const auto& event = events[state.nextEvent];
        const auto at = std::llround (plan.tempo.toSamples (event.beat)) - transport.sample;

        // Its beat is in this stretch's last half sample, and its sample, the nearest, the next stretch's first: the next
        // plays it there, so that it is on the same sample wherever a block ends. At the loop's end there is no next.
        if (at >= numSamples && ! endsAtLoop)
            break;

        const auto sample = (int) std::clamp<std::int64_t> (at, 0, numSamples - 1);

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

void Engine::continueClick (float* const* outputs, int numOutputs, int from, int to) noexcept
{
    // The click playing on, as far as a sample of the part.
    if (clicking == nullptr)
        return;

    const auto length = std::min (to - from, clicking->getNumSamples() - clickPosition);
    const auto* samples = clicking->getReadPointer (0, clickPosition);

    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::add (outputs[channel] + from, samples, length);

    clickPosition += length;

    if (clickPosition >= clicking->getNumSamples())
        clicking = nullptr;
}

void Engine::addClickStart (const Plan& plan, int start, bool accent) noexcept
{
    // As late as the song's sound comes out of the master. A click past the 64 waiting (a delay of seconds at a fast tempo)
    // is dropped.
    if (numClickStarts < (int) clickStarts.size())
        clickStarts[(size_t) ((firstClickStart + numClickStarts++) % (int) clickStarts.size())] = {
            clock + partStart + start + getOutputLatency (plan), accent
        };
}

void Engine::playClicks (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    // Each click that starts in the part, the one sounding playing on until then (a click that should have started before
    // starts at once).
    const auto partClock = clock + partStart;
    auto done = 0;

    for (; numClickStarts > 0; --numClickStarts)
    {
        const auto& next = clickStarts[(size_t) firstClickStart];

        if (next.clock >= partClock + numSamples)
            break;

        const auto start = (int) std::max<std::int64_t> (done, next.clock - partClock);
        continueClick (outputs, numOutputs, done, start);
        done = start;
        clicking = next.accent ? &accentClick : &click;
        clickPosition = 0;
        firstClickStart = (firstClickStart + 1) % (int) clickStarts.size();
    }

    continueClick (outputs, numOutputs, done, numSamples);
}

void Engine::findCountInClicks (const Plan& plan, int numSamples) noexcept
{
    // The time signature's beats in the bar before where the song starts, as long as a bar there at the tempo there, each
    // counted from the bar the playhead is in: each starts a click, a bar's first the accented one.
    const auto& countIn = transport.countIn;
    const auto from = (double) (countIn.length - countIn.left);

    // From the first beat after the sample before this stretch, as the song's clicks are (playClicks()).
    const auto earliest = countIn.firstBeat + std::max (0.0, from - 1.0) / countIn.samplesPerBeat;

    for (auto count = std::ceil ((earliest - countIn.barStart) / countIn.beatLength - 1.0e-9);; count += 1.0)
    {
        const auto beat = countIn.barStart + count * countIn.beatLength;
        const auto at = std::ceil ((beat - countIn.firstBeat) * countIn.samplesPerBeat - 1.0e-6) - from;

        if (at >= numSamples || beat >= countIn.firstBeat + countIn.barLength - 1.0e-9)
            break;

        if (at < 0.0)
            continue; // the stretch before played it

        const auto inBar = (beat - countIn.barStart) / countIn.barLength;
        addClickStart (plan, (int) at, std::abs (inBar - std::round (inBar)) < 1.0e-6);
    }
}

void Engine::findClicks (const Plan& plan, int offset, int numSamples) noexcept
{
    // Each of the time signature's beats starts a click at the first sample at or after it, a bar's first the accented one.
    // A beat that falls between the sample before this stretch and its first is first heard on its first, so the beats are
    // looked for from that sample on: from this stretch's own, such a beat is past already, and the stretch before, which
    // ends there, leaves its click to this one.
    const auto from = (double) transport.sample;
    const auto to = from + numSamples;

    for (auto beat = plan.tempo.getBeatAtOrAfter (plan.tempo.toBeats (std::max (0.0, from - 1.0)));; beat = plan.tempo.getNextBeat (beat))
    {
        const auto at = std::ceil (plan.tempo.toSamples (beat) - 1.0e-6);

        if (at >= to)
            break;

        if (at < from)
            continue; // the stretch before played it

        addClickStart (plan, offset + (int) (at - from), std::abs (plan.tempo.getBarStart (beat) - beat) < 1.0e-6);
    }
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
