// SPDX-License-Identifier: AGPL-3.0-or-later
// Whole numbers made of floating-point ones that may be no finite number: a value from a plug-in, a file, an audio device,
// a setting or a division by what can be 0. A cast to an integer of a NaN, an infinity or a number past the integer's range
// is undefined behaviour in C++, and juce::roundToInt() makes a meaningless number of one (known bug 148). Each of these
// is the conversion it names for a finite number within the type's range, the type's end for one past it (an infinity
// too), and `otherwise` for what is no number at all.
#pragma once

#include "Json.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>

namespace phazite
{
// A value, which a caller has made whole (std::floor(), std::ceil(), std::trunc()) or not, as the integer it truncates to.
template <typename Int, typename Float>
Int toWhole (Float value, Int otherwise = 0) noexcept
{
    static_assert (std::is_integral_v<Int> && std::is_floating_point_v<Float>);

    if (std::isnan (value))
        return otherwise;

    // The type's ends as doubles: the lowest exactly, the largest a power of two past it for 64 bits, hence >=.
    constexpr auto lowest = (double) std::numeric_limits<Int>::lowest();
    constexpr auto largest = (double) std::numeric_limits<Int>::max();

    if ((double) value <= lowest)
        return std::numeric_limits<Int>::lowest();

    if ((double) value >= largest)
        return std::numeric_limits<Int>::max();

    return (Int) value;
}

// The nearest whole number, halves away from 0, as std::round() and std::lround() give it.
template <typename Int, typename Float>
Int roundToWhole (Float value, Int otherwise = 0) noexcept
{
    return toWhole<Int> (std::round (value), otherwise);
}

// A juce::var's whole number, as `(Int) value` reads it, but a double's as toWhole() makes it: JUCE casts a double held in
// a var with a plain cast (juce_Variant.cpp), which JSON's "1e300" or "nan" makes undefined behaviour.
template <typename Int>
Int toWhole (const juce::var& value, Int otherwise = 0) noexcept
{
    if (value.isDouble())
        return toWhole<Int> ((double) value, otherwise);

    // A whole number held within the type's range, as a double's is: not cut down to its lower bits (5e9 to 705032704).
    const auto whole = (juce::int64) value;

    if constexpr (std::is_signed_v<Int>)
    {
        if (whole <= (juce::int64) std::numeric_limits<Int>::lowest())
            return std::numeric_limits<Int>::lowest();
    }
    else if (whole <= 0)
    {
        return 0;
    }

    if (whole > 0 && (std::uint64_t) whole >= (std::uint64_t) std::numeric_limits<Int>::max())
        return std::numeric_limits<Int>::max();

    return (Int) whole;
}

// A number as a float: held within a float's range (a cast of a double past it is undefined behaviour too), and `otherwise`
// for what is no finite number. And a juce::var's, as a preset's JSON holds one (a double such as 1e300, or a text, read
// without the overflow of JUCE's reader: Json.h, readFloat()).
inline float toFloat (double value, float otherwise = 0.0f) noexcept
{
    constexpr auto largest = (double) std::numeric_limits<float>::max();
    return std::isfinite (value) ? (float) juce::jlimit (-largest, largest, value) : otherwise;
}

inline float toFloat (const juce::var& value, float otherwise = 0.0f)
{
    return value.isString() ? readFloat (value.toString(), otherwise) : toFloat ((double) value, otherwise);
}

// A number, or none for what is no finite number: a value typed ("1e999" reads as an infinity).
inline std::optional<double> keepFinite (double value) noexcept
{
    return std::isfinite (value) ? std::optional (value) : std::nullopt;
}

// The nearest int as juce::roundToInt() gives it (halves to the even one), for what it is given in place of that.
template <typename Float>
int roundToNearestInt (Float value, int otherwise = 0) noexcept
{
    if (std::isnan (value))
        return otherwise;

    return juce::roundToInt (
        juce::jlimit ((double) std::numeric_limits<int>::lowest(), (double) std::numeric_limits<int>::max(), (double) value));
}
} // namespace phazite
