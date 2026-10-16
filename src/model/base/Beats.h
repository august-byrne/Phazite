// SPDX-License-Identifier: AGPL-3.0-or-later
// Places and lengths in the song in beats from its start (a beat is a quarter note in 4/4), as the model's public
// functions and the UI pass them: Phazite's own, so that code above the model needs no Tracktion (docs/design.md,
// section 24, rule 4). They are numbers with names; the model turns them into Tracktion's (model/base/Tracktion.h).
// Nothing here compares two doubles with ==, which GCC's -Wfloat-equal would refuse: equal means neither is less.
#pragma once

#include <algorithm>

namespace phazite
{
// A length in beats; negative for one that goes back.
class BeatDuration
{
public:
    constexpr BeatDuration() noexcept = default;
    static constexpr BeatDuration fromBeats (double beats) noexcept { return BeatDuration (beats); }
    constexpr double inBeats() const noexcept { return value; }

    constexpr BeatDuration operator-() const noexcept { return BeatDuration (-value); }
    constexpr BeatDuration operator+ (BeatDuration other) const noexcept { return BeatDuration (value + other.value); }
    constexpr BeatDuration operator- (BeatDuration other) const noexcept { return BeatDuration (value - other.value); }
    constexpr BeatDuration operator* (double factor) const noexcept { return BeatDuration (value * factor); }
    constexpr BeatDuration operator/ (double divisor) const noexcept { return BeatDuration (value / divisor); }
    constexpr double operator/ (BeatDuration other) const noexcept { return value / other.value; }
    constexpr BeatDuration& operator+= (BeatDuration other) noexcept { return *this = *this + other; }
    constexpr BeatDuration& operator-= (BeatDuration other) noexcept { return *this = *this - other; }

    constexpr bool operator< (BeatDuration other) const noexcept { return value < other.value; }
    constexpr bool operator> (BeatDuration other) const noexcept { return other < *this; }
    constexpr bool operator<= (BeatDuration other) const noexcept { return ! (other < *this); }
    constexpr bool operator>= (BeatDuration other) const noexcept { return ! (*this < other); }
    constexpr bool operator== (BeatDuration other) const noexcept { return *this <= other && other <= *this; }
    constexpr bool operator!= (BeatDuration other) const noexcept { return ! (*this == other); }

private:
    explicit constexpr BeatDuration (double beats) noexcept
        : value (beats)
    {
    }

    double value = 0.0;
};

// A place in the song, in beats from its start.
class BeatPosition
{
public:
    constexpr BeatPosition() noexcept = default;
    static constexpr BeatPosition fromBeats (double beats) noexcept { return BeatPosition (beats); }
    constexpr double inBeats() const noexcept { return value; }

    constexpr BeatPosition operator+ (BeatDuration d) const noexcept { return BeatPosition (value + d.inBeats()); }
    constexpr BeatPosition operator- (BeatDuration d) const noexcept { return BeatPosition (value - d.inBeats()); }
    constexpr BeatDuration operator- (BeatPosition other) const noexcept { return BeatDuration::fromBeats (value - other.value); }
    constexpr BeatPosition& operator+= (BeatDuration d) noexcept { return *this = *this + d; }
    constexpr BeatPosition& operator-= (BeatDuration d) noexcept { return *this = *this - d; }

    constexpr bool operator< (BeatPosition other) const noexcept { return value < other.value; }
    constexpr bool operator> (BeatPosition other) const noexcept { return other < *this; }
    constexpr bool operator<= (BeatPosition other) const noexcept { return ! (other < *this); }
    constexpr bool operator>= (BeatPosition other) const noexcept { return ! (*this < other); }
    constexpr bool operator== (BeatPosition other) const noexcept { return *this <= other && other <= *this; }
    constexpr bool operator!= (BeatPosition other) const noexcept { return ! (*this == other); }

private:
    explicit constexpr BeatPosition (double beats) noexcept
        : value (beats)
    {
    }

    double value = 0.0;
};

// From a start to an end, which is never before it.
class BeatRange
{
public:
    constexpr BeatRange() noexcept = default;
    constexpr BeatRange (BeatPosition s, BeatPosition e) noexcept
        : start (s),
          end (std::max (s, e))
    {
    }
    constexpr BeatRange (BeatPosition s, BeatDuration length) noexcept
        : BeatRange (s, s + length)
    {
    }

    static constexpr BeatRange between (BeatPosition a, BeatPosition b) noexcept { return { std::min (a, b), std::max (a, b) }; }

    constexpr BeatPosition getStart() const noexcept { return start; }
    constexpr BeatPosition getEnd() const noexcept { return end; }
    constexpr BeatDuration getLength() const noexcept { return end - start; }
    constexpr BeatPosition getCentre() const noexcept { return start + getLength() / 2.0; }
    constexpr bool isEmpty() const noexcept { return end <= start; }

    // Whether a position is in it: at its start or later, and before its end.
    constexpr bool contains (BeatPosition p) const noexcept { return start <= p && p < end; }
    constexpr bool overlaps (BeatRange other) const noexcept { return other.start < end && start < other.end; }

    constexpr BeatRange withStart (BeatPosition s) const noexcept { return { s, end }; }
    constexpr BeatRange withEnd (BeatPosition e) const noexcept { return { start, e }; }
    constexpr BeatRange movedBy (BeatDuration d) const noexcept { return { start + d, end + d }; }
    constexpr BeatRange getIntersectionWith (BeatRange other) const noexcept
    {
        return { std::max (start, other.start), std::max (std::max (start, other.start), std::min (end, other.end)) };
    }

    constexpr bool operator== (BeatRange other) const noexcept { return start == other.start && end == other.end; }
    constexpr bool operator!= (BeatRange other) const noexcept { return ! (*this == other); }

private:
    BeatPosition start, end;
};
} // namespace phazite
