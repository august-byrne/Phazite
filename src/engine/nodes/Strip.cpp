// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Strip.h"

#include <algorithm>
#include <utility>

namespace phazite::engine
{
void StripState::prepare (double sampleRate, int maximumBlockSize)
{
    buffer.setSize (4, maximumBlockSize);
    buffer.clear();
    beforeFader.setSize (2, maximumBlockSize);
    beforeFader.clear();
    beforeMute.setSize (2, maximumBlockSize);
    beforeMute.clear();
    scratch.setSize (2, 9 * maximumBlockSize + 16);
    noteStarts.reserve ((size_t) DeviceRunner::maximumEvents);
    volumeModulation.assign ((size_t) (maximumBlockSize / controlPointSpacing + 3), 0.0f);
    panModulation.assign (volumeModulation.size(), 0.0f);
    sendGains.assign ((size_t) maximumBlockSize, 0.0f);
    sendModulation.assign (volumeModulation.size(), 0.0f);
    channel.prepare (sampleRate);
    sequence = nullptr;
    nextEvent = 0;
    sounding.reset();
    mustChase = true;
    numEvents = 0;
}

void StripState::addEvent (devices::NoteEvent event) noexcept
{
    if (numEvents < (int) events.size())
        events[(std::size_t) numEvents++] = event;
}

void StripState::sortEvents() noexcept
{
    // By sample, keeping the order they were added in at a sample: an insertion sort, as they come mostly in order.
    for (int i = 1; i < numEvents; ++i)
    {
        const auto event = events[(std::size_t) i];
        auto j = i;

        for (; j > 0 && events[(std::size_t) j - 1].sample > event.sample; --j)
            events[(std::size_t) j] = events[(std::size_t) j - 1];

        events[(std::size_t) j] = event;
    }
}

//==============================================================================
DelayLine::DelayLine (int size, std::shared_ptr<DelayLine> takesFrom, bool allocateNow)
    : capacity (std::max (1, size)),
      old (std::move (takesFrom))
{
    if (allocateNow)
        allocate();
}

DelayLine::~DelayLine()
{
    // The rings it holds, each holding the one before, let go of one after another rather than each in the last's
    // destructor: plans that come faster than blocks make a long chain of them.
    for (auto next = std::move (old); next != nullptr && next.use_count() == 1;)
        next = std::move (next->old);
}

void DelayLine::allocate()
{
    if (isAllocated())
        return;

    samples.setSize (2, capacity);
    samples.clear();
    clearedSamples.fetch_add (capacity, std::memory_order_relaxed);
}

void DelayLine::letGoOfTheOld() noexcept
{
    // The audio thread reads the rings before it only until it is done: once it says so, it never reads them again.
    if (old != nullptr && done.load (std::memory_order_acquire))
        old = nullptr;
}

void DelayLine::start() noexcept
{
    if (started.load (std::memory_order_relaxed))
        return;

    // The latest ring before it that played (no ring that never played is ever played after a later one is, and none that
    // did is written to again): what it held then, its own and what it read further back, is the past this one reads
    // until it holds its own. Walking back through rings that never played, which nothing lets go of while this one holds
    // them.
    const auto* ring = old.get();

    while (ring != nullptr && ! ring->started.load (std::memory_order_relaxed))
        ring = ring->old.get();

    source = ring;
    inherited = ring != nullptr ? ring->getReach() : 0;
    started.store (true, std::memory_order_relaxed);
}

void DelayLine::reset() noexcept
{
    // Nothing of its own, and silence before it (getReach()): its memory as it was, never read (forEachHeld()).
    written = 0;
    silentBefore = true;
    source = nullptr;
    inherited = 0;
    started.store (true, std::memory_order_relaxed);
    done.store (true, std::memory_order_release);
}

void DelayLine::write (const float* const* inputs, int numSamples) noexcept
{
    for (int at = 0; at < numSamples;)
    {
        const auto part = std::min (numSamples - at, capacity - position);

        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::copy (samples.getWritePointer (channel, position), inputs[channel] + at, part);

        position = (position + part) % capacity;
        at += part;
    }

    written = std::min (capacity, written + numSamples);

    // Holding all it has room for itself, it never reads the ring before again, and holds no silence of a reset any more.
    if (written == capacity)
    {
        silentBefore = false;

        if (! done.load (std::memory_order_relaxed))
            done.store (true, std::memory_order_release);
    }
}

Delay::Delay (int samples)
    : length (std::max (0, samples))
{
    // None: nothing to keep (the engine's delay of none, which every strip may play through at once, writes nothing).
    if (length > 0)
    {
        line = std::make_shared<DelayLine> (length);
        line->reset(); // silence, heard as what came before
    }

    started = true;
}

Delay::Delay (std::shared_ptr<DelayLine> ring, int samples, int fade, bool isNew, bool isTapped)
    : length (std::max (0, samples)),
      fadeSamples (std::max (0, fade)),
      line (std::move (ring)),
      tapped (isTapped),
      newPath (isNew)
{
    jassert (line != nullptr && line->getCapacity() >= length);
}

Delay::~Delay()
{
    // Plans that come faster than blocks make a long chain of them.
    for (auto next = std::move (previous); next != nullptr && next.use_count() == 1;)
        next = std::move (next->previous);
}

void Delay::letGoOfThePrevious() noexcept
{
    // The audio thread reads the delays before only until it has played.
    if (previous != nullptr && played.load (std::memory_order_acquire))
        previous = nullptr;
}

int Delay::getGrowth() const noexcept
{
    // (Only the audio thread says it played, so while it hasn't, the message thread holds the delays before as they are.)
    if (played.load (std::memory_order_relaxed))
        return 0;

    auto marked = readsLeaving.load (std::memory_order_relaxed);

    for (const auto* before = previous.get(); before != nullptr; before = before->previous.get())
    {
        if (before->played.load (std::memory_order_relaxed))
            return marked ? std::max (0, length - before->length) : 0;

        marked = marked || before->readsLeaving.load (std::memory_order_relaxed);
    }

    return 0;
}

std::pair<float, float> Delay::leave (int fadeLength, int numSamples, std::pair<float, float> gains) noexcept
{
    if (leaving < 0)
        leaving = fadeLength;

    const auto step = 1.0f / (float) (fadeLength + 1);
    const auto from = (float) leaving * step, to = (float) std::max (0, leaving - numSamples) * step;
    leaving = std::max (0, leaving - numSamples);
    return { gains.first * from, gains.second * to };
}

void Delay::addDelayed (const float* const* inputs, float* const* outputs, int numSamples, float startGain, float endGain,
                        const float* gains) noexcept
{
    // At a gain: a straight line from one to the other over the block, times each sample's own if given; and through its
    // fade in, if it makes one, from so far into it.
    const auto add = [this, numSamples, startGain, endGain, gains] (float* to, const float* from, int offset, int count, int faded)
    {
        if (gains == nullptr && juce::exactlyEqual (startGain, 1.0f) && juce::exactlyEqual (endGain, 1.0f) && faded < 0)
        {
            juce::FloatVectorOperations::add (to, from, count);
            return;
        }

        const auto step = (endGain - startGain) / (float) numSamples;

        if (gains != nullptr)
        {
            for (int i = 0; i < count; ++i)
            {
                const auto fade = faded >= 0 && faded + i < fadeSamples ? (float) (faded + i + 1) / (float) (fadeSamples + 1) : 1.0f;
                to[i] += from[i] * gains[offset + i] * (startGain + step * (float) (offset + i)) * fade;
            }

            return;
        }

        for (int i = 0; i < count; ++i)
        {
            const auto fade = faded >= 0 && faded + i < fadeSamples ? (float) (faded + i + 1) / (float) (fadeSamples + 1) : 1.0f;
            to[i] += from[i] * (startGain + step * (float) (offset + i)) * fade;
        }
    };

    if (! played.load (std::memory_order_relaxed))
        played.store (true, std::memory_order_release);

    // Without a ring it writes nothing of its own but that: the engine's one for what needs no delay (Engine::noDelay)
    // joins for every strip, on every worker at once, and a count set here was two threads' write (ThreadSanitizer, CI,
    // 2026-11-17). Its count stays the 0 it starts with.
    if (line == nullptr)
    {
        for (int channel = 0; channel < 2; ++channel)
            add (outputs[channel], inputs[channel], 0, numSamples, -1);

        return;
    }

    touched = 0;
    auto& ring = *line;
    ring.start();

    // A path new while the song plays fades in from its first sample, though it has no delay and its ring (a tap others
    // read too) holds the past, rather than cut in.
    if (! std::exchange (started, true) && newPath && fadeSamples > 0)
        fadedIn = 0;

    // Where the ring doesn't hold what it reaches back to, it plays nothing, and fades in from the first it does.
    const auto read = [&] (int ago, int done, int part)
    {
        const auto missing = std::clamp (ago - ring.getReach(), 0, part);

        if (missing > 0 && fadeSamples > 0)
            fadedIn = 0;

        const auto faded = fadedIn;
        ring.forEachHeld (ago, part,
                          [&] (int offset, const float* left, const float* right, int count)
                          {
                              const auto fadeFrom = faded >= 0 ? faded + offset - missing : -1;
                              add (outputs[0] + done + offset, left, done + offset, count, fadeFrom);
                              add (outputs[1] + done + offset, right, done + offset, count, fadeFrom);
                              touched += count;
                          });

        if (fadedIn >= 0 && (fadedIn += part - missing) >= fadeSamples)
            fadedIn = -1;
    };

    // A strip's tap, which holds the stretch already: as far back from its end.
    if (tapped)
    {
        read (numSamples + length, 0, numSamples);
        return;
    }

    // Its own, with none: what comes in goes out, and into the ring, for when the path comes to need a delay.
    if (length == 0)
    {
        for (int channel = 0; channel < 2; ++channel)
            add (outputs[channel], inputs[channel], 0, numSamples, fadedIn);

        if (fadedIn >= 0 && (fadedIn += numSamples) >= fadeSamples)
            fadedIn = -1;

        ring.write (inputs, numSamples);
        touched = numSamples;
        return;
    }

    // Its own: no further than it reaches back, so that what it reads came in before this stretch, never in it; what came
    // in so many samples ago goes out, and what comes in goes in.
    for (int done = 0; done < numSamples;)
    {
        const auto part = std::min (numSamples - done, length);
        read (length, done, part);
        const float* in[] = { inputs[0] + done, inputs[1] + done };
        ring.write (in, part);
        touched += part;
        done += part;
    }
}

void Delay::reset() noexcept
{
    if (line != nullptr && ! tapped)
        line->reset();

    if (! played.load (std::memory_order_relaxed))
        played.store (true, std::memory_order_release);

    fadedIn = -1;
    started = true;
}

//==============================================================================
namespace
{
    float toGain (float decibels) noexcept
    {
        return decibels <= Channel::silenceDb ? 0.0f : juce::Decibels::decibelsToGain (decibels, Channel::silenceDb);
    }
} // namespace

void SendLevel::setDecibels (float decibels) noexcept
{
    gain.store (toGain (decibels), std::memory_order_relaxed);
    position.store (decibelsToFaderPosition (decibels), std::memory_order_relaxed);
}

SendLevel::Gains SendLevel::moveOn (const Automation& automation, std::span<float> gains) noexcept
{
    // Where it is set: by hand, or by its automation where the song is (but what is written by hand now); then moved by its
    // modulation on a control point, as parts of the fader's travel.
    const auto room = ! gains.empty();
    const auto* curve = writing.load (std::memory_order_relaxed) || ! room ? nullptr : automation.curve;
    const auto& offsets = room ? automation.modulation : std::span<const float>();
    const auto follows = ! offsets.empty() || (automation.beatsPerSample > 0.0 && curve != nullptr);
    const auto positionAt = [this, curve] (double beat)
    { return curve != nullptr ? std::clamp (curve->getValueAt (beat), 0.0f, 1.0f) : position.load (std::memory_order_relaxed); };
    const auto modulate = [&offsets] (float at, size_t point)
    { return offsets.empty() ? at : std::clamp (at + offsets[std::min (point, offsets.size() - 1)], 0.0f, 1.0f); };

    // Cut or sending again: over the block, from where it was.
    const auto heardNow = isCut() ? 0.0f : 1.0f;
    const auto heardFrom = heard < 0.0f ? heardNow : heard;
    heard = heardNow;

    if (! follows)
    {
        // Set: from where it is, over the block; at once as it first plays. By hand at the gain set, so that a send that
        // follows nothing sends as it always has.
        const auto at = positionAt (automation.beat);
        const auto target = curve != nullptr ? toGain (faderPositionToDecibels (at)) : getGain();
        playedPosition.store (at, std::memory_order_relaxed);
        playedBasePosition.store (at, std::memory_order_relaxed);

        // Just done following: eased there over 15 ms at least, from where the line left it, sample by sample, toward where
        // it goes now in each.
        if (std::exchange (followedLast, false) && room && level >= 0.0f)
            easing = std::max ((int) gains.size(), automation.easeSamples);

        if (! room)
            easing = 0;

        if (easing > 0)
        {
            for (auto& sample : gains)
            {
                if (easing > 0)
                    level += (target - level) / (float) easing--;
                else
                    level = target;

                sample = level;
            }

            return { heardFrom, heardNow, true };
        }

        const auto from = level < 0.0f ? target : level;
        level = target;
        return { from * heardFrom, target * heardNow, false };
    }

    // Its automation and modulation: a straight line to its value on each control point, the first the song's next, from
    // where the line was as the block started, whatever the block's size; at once as it first plays.
    followedLast = true;
    easing = 0;

    if (level < 0.0f)
        level = toGain (faderPositionToDecibels (modulate (positionAt (automation.beat), 0)));

    const auto numSamples = (int) gains.size();
    const auto toControlPoint =
        controlPointSpacing - (int) (((automation.sample % controlPointSpacing) + controlPointSpacing) % controlPointSpacing);
    auto base = positionAt (automation.beat), moved = modulate (base, 0);

    for (int done = 0, stretch = 0; done < numSamples; ++stretch)
    {
        const auto toPoint = done == 0 ? toControlPoint : controlPointSpacing;
        const auto length = std::min (numSamples - done, toPoint);
        base = positionAt (automation.beat + (done + toPoint) * automation.beatsPerSample);
        moved = modulate (base, (size_t) stretch + 1);
        const auto end = toGain (faderPositionToDecibels (moved));
        const auto step = (end - level) / (float) toPoint;

        for (int i = done; i < done + length; ++i)
            gains[(size_t) i] = (level += step);

        if (length == toPoint)
            level = end;

        done += length;
    }

    playedPosition.store (moved, std::memory_order_relaxed);
    playedBasePosition.store (base, std::memory_order_relaxed);
    return { heardFrom, heardNow, true };
}

} // namespace phazite::engine
