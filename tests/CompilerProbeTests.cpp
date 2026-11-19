// SPDX-License-Identifier: AGPL-3.0-or-later
// Small sums a compiler must get right, each run on its own and guarded, so that a build whose compiler gets one wrong
// says which. CI's Windows release build (MSVC 19.52) made the part of a clip from 0 to 8 that lies in a range from 2 to
// 6 end at 2 (known bug 204), and crashed in a test of such sums, where MSVC 19.51, GCC and Clang do neither. What a
// probe finds is said as a warning, not a failure: Phazite's own code is checked by its own tests, and a compiler's
// fault isn't fixed here.
#include "TestHelpers.h"
#include "model/base/Beats.h"

#include <algorithm>
#include <cmath>
#include <string>

#if JUCE_WINDOWS
 #include <excpt.h>
#endif

namespace
{
using phazite::BeatPosition;
using phazite::BeatRange;

// The numbers every probe works with, read through volatiles so that no sum is worked out as it is compiled:
// a clip from 0 to 8, a range from 2 to 6.
volatile double numbers[] = { 0.0, 8.0, 2.0, 6.0 };

#if JUCE_WINDOWS
 #define PHAZITE_NOINLINE __declspec (noinline)
#else
 #define PHAZITE_NOINLINE __attribute__ ((noinline))
#endif

PHAZITE_NOINLINE BeatRange getClip()
{
    return { BeatPosition::fromBeats (numbers[0]), BeatPosition::fromBeats (numbers[1]) };
}

PHAZITE_NOINLINE BeatRange getRange()
{
    return { BeatPosition::fromBeats (numbers[2]), BeatPosition::fromBeats (numbers[3]) };
}

PHAZITE_NOINLINE double getNumber (int index)
{
    return numbers[index];
}

// A value of a class of one double with nothing constexpr about it.
struct Plain
{
    double value;
    bool operator< (Plain other) const { return value < other.value; }
};

PHAZITE_NOINLINE Plain getPlain (int index)
{
    return { numbers[index] };
}

PHAZITE_NOINLINE std::string getText (int index)
{
    return std::string ((size_t) numbers[index] + 1, 'a'); // 1, 9, 3 or 7 letters
}

// Each probe's sum.
PHAZITE_NOINLINE double minOfNamedPositions()
{
    const auto a = getClip().getEnd(), b = getRange().getEnd();
    return std::min (a, b).inBeats();
}

PHAZITE_NOINLINE double minOfReturnedPositions()
{
    const auto clip = getClip(), range = getRange();
    return std::min (clip.getEnd(), range.getEnd()).inBeats();
}

PHAZITE_NOINLINE double maxOfReturnedPositions()
{
    const auto clip = getClip(), range = getRange();
    return std::max (clip.getStart(), range.getStart()).inBeats();
}

PHAZITE_NOINLINE double minKeptThenRead()
{
    const auto clip = getClip(), range = getRange();
    const auto least = std::min (clip.getEnd(), range.getEnd());
    return least.inBeats();
}

PHAZITE_NOINLINE double endOfRangeOfMaxAndMin()
{
    const auto clip = getClip(), range = getRange();
    const BeatRange made (std::max (clip.getStart(), range.getStart()), std::min (clip.getEnd(), range.getEnd()));
    return made.getEnd().inBeats();
}

PHAZITE_NOINLINE double startOfRangeOfMaxAndMin()
{
    const auto clip = getClip(), range = getRange();
    const BeatRange made (std::max (clip.getStart(), range.getStart()), std::min (clip.getEnd(), range.getEnd()));
    return made.getStart().inBeats();
}

PHAZITE_NOINLINE double endOfRangeOfNamedMaxAndMin()
{
    const auto clip = getClip(), range = getRange();
    const auto start = std::max (clip.getStart(), range.getStart());
    const auto end = std::min (clip.getEnd(), range.getEnd());
    return BeatRange (start, end).getEnd().inBeats();
}

PHAZITE_NOINLINE double endOfIntersection()
{
    return getClip().getIntersectionWith (getRange()).getEnd().inBeats();
}

PHAZITE_NOINLINE double endOfUnion()
{
    return getRange().getUnionWith (getClip()).getEnd().inBeats();
}

PHAZITE_NOINLINE double maxOfMaxAndMin()
{
    const auto clip = getClip(), range = getRange();
    const auto a = clip.getStart(), b = range.getStart(), c = clip.getEnd(), d = range.getEnd();
    return std::max (std::max (a, b), std::min (c, d)).inBeats();
}

PHAZITE_NOINLINE double minOfLengths()
{
    const auto clip = getClip(), range = getRange();
    return std::min (clip.getLength(), range.getLength()).inBeats();
}

PHAZITE_NOINLINE double minOfReturnedPlains()
{
    return std::min (getPlain (1), getPlain (3)).value;
}

PHAZITE_NOINLINE double addPlains (Plain a, Plain b)
{
    return a.value + b.value;
}

PHAZITE_NOINLINE double sumOfMaxAndMinOfPlains()
{
    return addPlains (std::max (getPlain (0), getPlain (2)), std::min (getPlain (1), getPlain (3)));
}

PHAZITE_NOINLINE double minOfReturnedDoubles()
{
    return std::min (getNumber (1), getNumber (3));
}

PHAZITE_NOINLINE double addDoubles (double a, double b)
{
    return a + b;
}

PHAZITE_NOINLINE double sumOfMaxAndMinOfDoubles()
{
    return addDoubles (std::max (getNumber (0), getNumber (2)), std::min (getNumber (1), getNumber (3)));
}

PHAZITE_NOINLINE double lengthOfMinOfReturnedTexts()
{
    return (double) std::min (getText (1), getText (3)).size(); // the shorter is the lesser: all the same letter
}

// Classes of one double, each BeatPosition's shape but for one thing, to find what that compiler trips over. The three
// sums it got wrong with BeatPosition (CI's windows-profile job, 2026-11-19), for each.
class AsBeatPosition // BeatPosition's own shape: constexpr, a private double, compared by value
{
public:
    constexpr AsBeatPosition() noexcept = default;
    static constexpr AsBeatPosition from (double v) noexcept { return AsBeatPosition (v); }
    constexpr double get() const noexcept { return value; }
    constexpr bool operator< (AsBeatPosition other) const noexcept { return value < other.value; }

private:
    explicit constexpr AsBeatPosition (double v) noexcept
        : value (v)
    {
    }

    double value = 0.0;
};

class ComparedByReference // as AsBeatPosition, operator< taking a reference
{
public:
    constexpr ComparedByReference() noexcept = default;
    static constexpr ComparedByReference from (double v) noexcept { return ComparedByReference (v); }
    constexpr double get() const noexcept { return value; }
    constexpr bool operator< (const ComparedByReference& other) const noexcept { return value < other.value; }

private:
    explicit constexpr ComparedByReference (double v) noexcept
        : value (v)
    {
    }

    double value = 0.0;
};

class NotConstexpr // as AsBeatPosition, nothing constexpr
{
public:
    NotConstexpr() noexcept = default;
    static NotConstexpr from (double v) noexcept { return NotConstexpr (v); }
    double get() const noexcept { return value; }
    bool operator< (NotConstexpr other) const noexcept { return value < other.value; }

private:
    explicit NotConstexpr (double v) noexcept
        : value (v)
    {
    }

    double value = 0.0;
};

class NotInitialized // as AsBeatPosition, the double without its "= 0.0"
{
public:
    constexpr NotInitialized() noexcept
        : value (0.0)
    {
    }
    static constexpr NotInitialized from (double v) noexcept { return NotInitialized (v); }
    constexpr double get() const noexcept { return value; }
    constexpr bool operator< (NotInitialized other) const noexcept { return value < other.value; }

private:
    explicit constexpr NotInitialized (double v) noexcept
        : value (v)
    {
    }

    double value;
};

class ComparedByFriend // as AsBeatPosition, operator< a friend of two values
{
public:
    constexpr ComparedByFriend() noexcept = default;
    static constexpr ComparedByFriend from (double v) noexcept { return ComparedByFriend (v); }
    constexpr double get() const noexcept { return value; }
    friend constexpr bool operator< (ComparedByFriend a, ComparedByFriend b) noexcept { return a.value < b.value; }

private:
    explicit constexpr ComparedByFriend (double v) noexcept
        : value (v)
    {
    }

    double value = 0.0;
};

template <typename Type>
PHAZITE_NOINLINE Type makeValue (int index)
{
    return Type::from (numbers[index]);
}

template <>
PHAZITE_NOINLINE BeatPosition makeValue<BeatPosition> (int index)
{
    return BeatPosition::fromBeats (numbers[index]);
}

inline double read (BeatPosition p) noexcept
{
    return p.inBeats();
}

template <typename Type>
double read (Type value) noexcept
{
    return value.get();
}

// The clip's start and end, the range's start and end: 0, 8, 2, 6.
template <typename Type>
PHAZITE_NOINLINE double greatestOfGreatestAndLeast()
{
    const auto a = makeValue<Type> (0), b = makeValue<Type> (2), c = makeValue<Type> (1), d = makeValue<Type> (3);
    return read (std::max (std::max (a, b), std::min (c, d)));
}

template <typename Type>
PHAZITE_NOINLINE double leastKeptThenRead()
{
    const auto least = std::min (makeValue<Type> (1), makeValue<Type> (3));
    return read (least);
}

// As BeatRange's constructor: the end never before the start.
template <typename Type>
struct RangeOf
{
    RangeOf (Type s, Type e) noexcept
        : start (s),
          end (std::max (s, e))
    {
    }

    Type start, end;
};

template <typename Type>
PHAZITE_NOINLINE double endOfRangeOfNamed()
{
    const auto start = std::max (makeValue<Type> (0), makeValue<Type> (2));
    const auto end = std::min (makeValue<Type> (1), makeValue<Type> (3));
    return read (RangeOf<Type> (start, end).end);
}

#define PHAZITE_PROBES_OF(Type)                                                                                                            \
    { #Type ": std::max (std::max (a, b), std::min (c, d)) of named values", greatestOfGreatestAndLeast<Type>, 6.0 },                      \
        { #Type ": std::min of made values, kept in a local, then read", leastKeptThenRead<Type>, 6.0 },                                   \
        { #Type ": the end of a range of a named std::max and a named std::min", endOfRangeOfNamed<Type>, 6.0 }

struct Probe
{
    const char* what;
    double (*run)();
    double expected;
};

// Runs a probe; false if it crashed (Windows: a structured exception, such as an access violation). No object with a
// destructor may live in a function that uses __try, hence one of its own.
bool runGuarded (double (*run)(), double& result)
{
#if JUCE_WINDOWS
    __try
    {
        result = run();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    result = run();
    return true;
#endif
}
} // namespace

TEST_CASE ("Compiler probes: sums of least and greatest come out as the language says, each run on its own")
{
    const Probe probes[] = {
        { "std::min of two named positions", minOfNamedPositions, 6.0 },
        { "std::min of two positions as getters return them", minOfReturnedPositions, 6.0 },
        { "std::max of two positions as getters return them", maxOfReturnedPositions, 2.0 },
        { "std::min of returned positions, kept in a local, then read", minKeptThenRead, 6.0 },
        { "the end of BeatRange (std::max (getters), std::min (getters))", endOfRangeOfMaxAndMin, 6.0 },
        { "the start of BeatRange (std::max (getters), std::min (getters))", startOfRangeOfMaxAndMin, 2.0 },
        { "the end of BeatRange (start, end), each a named std::max or std::min of getters", endOfRangeOfNamedMaxAndMin, 6.0 },
        { "the end of a range's intersection with another", endOfIntersection, 6.0 },
        { "the end of a range's union with another", endOfUnion, 8.0 },
        { "std::max (std::max (a, b), std::min (c, d)) of named positions", maxOfMaxAndMin, 6.0 },
        { "std::min of two lengths as getters return them", minOfLengths, 4.0 },
        { "std::min of two returned values of a plain class of one double", minOfReturnedPlains, 6.0 },
        { "a call given std::max and std::min of returned values of that class", sumOfMaxAndMinOfPlains, 8.0 },
        { "std::min of two returned doubles", minOfReturnedDoubles, 6.0 },
        { "a call given std::max and std::min of returned doubles", sumOfMaxAndMinOfDoubles, 8.0 },
        { "the length of std::min of two returned strings", lengthOfMinOfReturnedTexts, 7.0 },
        PHAZITE_PROBES_OF (BeatPosition),
        PHAZITE_PROBES_OF (AsBeatPosition),
        PHAZITE_PROBES_OF (ComparedByReference),
        PHAZITE_PROBES_OF (NotConstexpr),
        PHAZITE_PROBES_OF (NotInitialized),
        PHAZITE_PROBES_OF (ComparedByFriend),
    };

    int wrong = 0;

    for (const auto& probe : probes)
    {
        double result = -1.0;
        const auto ran = runGuarded (probe.run, result);
        const auto right = ran && std::abs (result - probe.expected) < 1.0e-9;
        wrong += right ? 0 : 1;
        const auto line = std::string (right ? "right: " : "WRONG: ") + probe.what + (ran ? " = " : " CRASHED, ") + std::to_string (result)
                          + " (should be " + std::to_string (probe.expected) + ")";
        MESSAGE (line);
    }

    // Said, not failed: see the top of this file.
    WARN (wrong == 0);
}
