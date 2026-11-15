// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlayheadClock.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace phazite::engine
{
double BlockClock::stamp (double milliseconds, int numSamples, double sampleRate) noexcept
{
    if (numSamples <= 0 || sampleRate <= 0.0)
        return milliseconds;

    const auto nominal = 1000.0 / sampleRate; // milliseconds a sample
    const auto length = numSamples * nominal;

    // A loop of the second order whose bandwidth is taken over a block's length: the time a block came corrects when the
    // next one should start by a part of its error, and how fast blocks come by a smaller part, so that one block's lateness
    // moves the times by little and a steady difference is followed.
    if (running && juce::exactlyEqual (sampleRate, rate) && std::abs (milliseconds - next) <= farthest * length
        && std::abs (perSample / nominal - 1.0) < 0.05)
    {
        const auto omega = juce::MathConstants<double>::twoPi * bandwidth * length / 1000.0;
        const auto error = milliseconds - next;
        const auto start = next;
        next = start + numSamples * perSample + juce::MathConstants<double>::sqrt2 * omega * error;
        perSample += omega * omega * error / numSamples;
        return start;
    }

    running = true;
    rate = sampleRate;
    perSample = nominal;
    next = milliseconds + length;
    return milliseconds;
}

//==============================================================================
void PlayheadStamps::publish (const Stamp& stamp) noexcept
{
    const auto number = written.load (std::memory_order_relaxed);
    auto& slot = slots[(size_t) (number % size)];

    slot.sequence.store (2 * number + 1, std::memory_order_relaxed); // odd: being written
    std::atomic_thread_fence (std::memory_order_release);
    slot.beat.store (stamp.beat, std::memory_order_relaxed);
    slot.milliseconds.store (stamp.milliseconds, std::memory_order_relaxed);
    slot.beatsPerMillisecond.store (stamp.beatsPerMillisecond, std::memory_order_relaxed);
    slot.length.store (stamp.length, std::memory_order_relaxed);
    slot.run.store (stamp.run, std::memory_order_relaxed);
    slot.round.store (stamp.round, std::memory_order_relaxed);
    slot.wentRound.store (stamp.wentRound, std::memory_order_relaxed);
    slot.sequence.store (2 * number + 2, std::memory_order_release); // written
    written.store (number + 1, std::memory_order_release);
}

bool PlayheadStamps::read (std::uint64_t number, Stamp& stamp) const noexcept
{
    const auto& slot = slots[(size_t) (number % size)];
    const auto whole = 2 * number + 2;

    if (slot.sequence.load (std::memory_order_acquire) != whole)
        return false; // being written, or written over by a later stamp

    stamp.beat = slot.beat.load (std::memory_order_relaxed);
    stamp.milliseconds = slot.milliseconds.load (std::memory_order_relaxed);
    stamp.beatsPerMillisecond = slot.beatsPerMillisecond.load (std::memory_order_relaxed);
    stamp.length = slot.length.load (std::memory_order_relaxed);
    stamp.run = slot.run.load (std::memory_order_relaxed);
    stamp.round = slot.round.load (std::memory_order_relaxed);
    stamp.wentRound = slot.wentRound.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    return slot.sequence.load (std::memory_order_relaxed) == whole;
}

std::optional<PlayheadStamps::Stamp> PlayheadStamps::getLatest() const noexcept
{
    // The latest stamp stays whole until the writer comes round to its slot again, size stamps later; a few tries at most.
    for (int tries = 0; tries < 4; ++tries)
    {
        const auto count = written.load (std::memory_order_acquire);

        if (count == 0)
            return std::nullopt;

        if (Stamp stamp; read (count - 1, stamp))
            return stamp;
    }

    return std::nullopt;
}

std::optional<double> PlayheadStamps::getBeatAt (double milliseconds) const noexcept
{
    // From the latest stamp back, a slot short of all of them, which the writer may be filling.
    const auto count = written.load (std::memory_order_acquire);
    std::optional<Stamp> later;

    for (std::uint64_t back = 0; back < std::min (count, size - 1); ++back)
    {
        Stamp stamp;

        if (! read (count - 1 - back, stamp) || (later.has_value() && stamp.run != later->run))
            break; // written over meanwhile, or from before a locate: no further back

        if (stamp.milliseconds <= milliseconds)
        {
            const auto reach = back == 0 ? 1.5 * stamp.length : stamp.length;
            return stamp.beat + std::clamp (milliseconds - stamp.milliseconds, 0.0, reach) * stamp.beatsPerMillisecond;
        }

        later = stamp;
    }

    if (later.has_value())
        return later->beat;

    return std::nullopt;
}

std::optional<PlayheadStamps::Place> PlayheadStamps::getPlaceBefore (double now, double milliseconds, double loopStart,
                                                                     double loopEnd) const noexcept
{
    const auto count = written.load (std::memory_order_acquire);
    std::optional<Stamp> later;
    std::optional<Place> place;
    auto wentRound = false; // in the block the place is in
    auto left = std::max (0.0, milliseconds);

    for (std::uint64_t back = 0; back < std::min (count, size - 1) && ! place.has_value(); ++back)
    {
        Stamp stamp;

        if (! read (count - 1 - back, stamp) || (later.has_value() && stamp.run != later->run))
            break;

        // How much of it has played: of the latest, as far as the time since it started (as getBeatAt() reaches); of
        // each before it, all of it.
        const auto played = back == 0 ? std::clamp (now - stamp.milliseconds, 0.0, 1.5 * stamp.length) : stamp.length;

        if (left <= played)
        {
            place = Place { stamp.beat + (played - left) * stamp.beatsPerMillisecond, stamp.round };
            wentRound = stamp.wentRound;
        }

        left -= played;
        later = stamp;
    }

    if (! place.has_value() && later.has_value())
        place = Place { later->beat, later->round };

    // Past the loop's end in a block that went round it: as far into the loop as it went, the round after (the next stamp's,
    // however many times a loop shorter than a block went round in it). In one that didn't, the speed it started at ran
    // ahead of the playhead (the tempo slowing): at the end, the same round.
    if (place.has_value() && loopEnd > loopStart && place->beat >= loopEnd)
    {
        if (wentRound)
        {
            place->beat = loopStart + std::fmod (place->beat - loopStart, loopEnd - loopStart);
            ++place->round;
        }
        else
        {
            place->beat = loopEnd;
        }
    }

    return place;
}

std::uint32_t PlayheadStamps::getLatestRound() const noexcept
{
    const auto latest = getLatest();
    return latest.has_value() ? latest->round : 0;
}
} // namespace phazite::engine
