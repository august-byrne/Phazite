// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DynamicsCurve.h"
#include "devices/dsp/Math.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices::curves
{
namespace
{
    constexpr float axisDecibels = topDecibels - bottomDecibels; // how many decibels the axes span
    constexpr float quietest = -120.0f;                          // decibels: no quieter gains than this
    constexpr float loudest = 48.0f;                             // and no louder
    constexpr float justOverBottom = 1.0e-4f; // the place whose gain the levels under the axes' bottom take: 0.007 dB over it

    // A line rising (or falling) from 0 to 1 over a fraction s of its way, bent up by a tension above 0 or down below it:
    // (e^(-a s) - 1) / (e^(-a) - 1), the bend a = 8 tension (as the synths' envelopes bend, devices/dsp/Envelopes.h).
    float bendUp (float s, float bend) noexcept
    {
        if (std::abs (bend) < 1.0e-4f)
            return s;

        return (float) (std::expm1 (-(double) bend * s) / std::expm1 (-(double) bend));
    }

    float getLine (PointType type, float tension, float s, float from, float to) noexcept
    {
        // Bending "up" means faster first for a rising line, slower first for a falling one.
        const auto bend = 8.0f * tension * (to >= from ? 1.0f : -1.0f);

        switch (type)
        {
            case PointType::doubleCurve:
            {
                // An S: the first half slow then fast, the second its mirror, the tension how steep its middle is.
                const auto middle = 0.5f * (from + to);

                if (s < 0.5f)
                    return from + (middle - from) * bendUp (2.0f * s, -bend);

                return middle + (to - middle) * (1.0f - bendUp (2.0f - 2.0f * s, -bend));
            }
            case PointType::hold:
                return s < 1.0f ? from : to;
            case PointType::stairs:
            {
                const auto steps = getStairs (tension);
                const auto step = std::min ((int) std::floor (s * (float) steps), steps - 1);
                return from + (to - from) * (float) step / (float) (steps - 1);
            }
            case PointType::smoothStairs:
            {
                const auto steps = getStairs (tension);
                const auto along = s * (float) steps;
                const auto step = std::floor (along);
                const auto rounded = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (along - step));
                return from + (to - from) * std::min (1.0f, (step + rounded) / (float) steps);
            }
            case PointType::singleCurve:
            default:
                return from + (to - from) * bendUp (s, bend);
        }
    }
} // namespace

float axisForDecibels (float decibels) noexcept
{
    return std::max (0.0f, (decibels - bottomDecibels) / axisDecibels); // not a number, 0
}

float decibelsForAxis (float place) noexcept
{
    return bottomDecibels + place * axisDecibels;
}

float toAxis (float amplitude) noexcept
{
    return amplitude > 0.0f ? axisForDecibels (dsp::gainToDecibels (amplitude)) : 0.0f;
}

float fromAxis (float place) noexcept
{
    return place > 0.0f ? dsp::decibelsToGain (decibelsForAxis (place)) : 0.0f;
}

int getStairs (float tension) noexcept
{
    return 2 + (int) std::lround ((juce::jlimit (-1.0f, 1.0f, tension) + 1.0f) * 7.0f);
}

float getAt (std::span<const float> curve, float place) noexcept
{
    const auto value = [curve] (int point, PointField field) { return curve[(size_t) getPointParameter (point, field)]; };
    const auto count = juce::jlimit (2, maxPoints, (int) std::lround (curve[0]));

    // Each point no further left than the one before it.
    auto lastX = 0.0f, lastY = value (0, pointY);

    if (place <= value (0, pointX))
        return lastY;

    lastX = std::max (0.0f, value (0, pointX));

    for (int i = 1; i < count; ++i)
    {
        const auto x = std::max (lastX, value (i, pointX)), y = value (i, pointY);

        if (place <= x)
        {
            const auto s = x > lastX ? (place - lastX) / (x - lastX) : 1.0f;
            const auto type = (PointType) juce::jlimit (0, (int) std::size (pointTypeNames) - 1, (int) std::lround (value (i, pointType)));
            return getLine (type, value (i, pointTension), s, lastY, y);
        }

        lastX = x;
        lastY = y;
    }

    return lastY;
}

std::vector<Point> makeCompression (float thresholdDecibels, float ratio, float kneeDecibels)
{
    // What the compressor puts out, in decibels, for what comes in.
    const auto slope = 1.0f - 1.0f / std::max (0.01f, ratio);
    const auto wanted = [thresholdDecibels, slope, kneeDecibels] (float in)
    {
        const auto over = in - thresholdDecibels;

        if (2.0f * over <= -kneeDecibels)
            return in;

        if (2.0f * over < kneeDecibels)
            return in - slope * (over + 0.5f * kneeDecibels) * (over + 0.5f * kneeDecibels) / (2.0f * kneeDecibels);

        return in - slope * over;
    };
    const auto place = [] (float decibels) { return juce::jlimit (0.0f, 1.0f, axisForDecibels (decibels)); };

    // Where it starts (level in level out, unless the knee or the ratio start under the axes' bottom), where the knee starts
    // and ends, and the end: where an upward ratio reaches the top, then along it.
    const auto start = thresholdDecibels - 0.5f * kneeDecibels, kneeEnd = thresholdDecibels + 0.5f * kneeDecibels;
    std::vector<Point> points { makePoint (0.0f, place (wanted (bottomDecibels))) };
    auto kneeLine = -1;

    if (start > bottomDecibels && start < topDecibels)
        points.push_back (makePoint (place (start), place (start)));

    if (kneeDecibels > 0.0f && kneeEnd > bottomDecibels && kneeEnd < topDecibels)
    {
        kneeLine = (int) points.size();
        points.push_back (makePoint (place (kneeEnd), place (wanted (kneeEnd))));
    }

    if (const auto atTop = thresholdDecibels + (topDecibels - thresholdDecibels) * ratio;
        wanted (topDecibels) > topDecibels && atTop > std::max (bottomDecibels, kneeEnd) && atTop < topDecibels)
        points.push_back (makePoint (place (atTop), 1.0f));

    points.push_back (makePoint (1.0f, place (wanted (topDecibels))));

    // The knee's line: the tension at which it is nearest the knee's curve where it is furthest from it, sampled evenly (a
    // golden-section search: the error falls, then rises, as the tension grows).
    if (kneeLine > 0 && points[(size_t) kneeLine][pointX] > points[(size_t) kneeLine - 1][pointX])
    {
        const auto& from = points[(size_t) kneeLine - 1];
        const auto& to = points[(size_t) kneeLine];
        const auto worstError = [&] (float tension)
        {
            auto worst = 0.0f;

            for (int k = 1; k < 32; ++k)
            {
                const auto s = (float) k / 32.0f;
                const auto in = decibelsForAxis (from[pointX] + s * (to[pointX] - from[pointX]));
                const auto out = getLine (PointType::singleCurve, tension, s, from[pointY], to[pointY]);
                worst = std::max (worst, std::abs (decibelsForAxis (out) - wanted (in)));
            }

            return worst;
        };

        constexpr auto ratioOfGold = 0.618034f;
        auto a = -1.0f, b = 1.0f;

        for (int step = 0; step < 40; ++step)
        {
            const auto c = b - ratioOfGold * (b - a), d = a + ratioOfGold * (b - a);

            if (worstError (c) < worstError (d))
                b = d;
            else
                a = c;
        }

        points[(size_t) kneeLine][pointTension] = 0.5f * (a + b);
    }

    return points;
}

const std::vector<Shape>& getShapes()
{
    // Each as it says, exactly: a straight line a ratio, a gate a floor of silence under its level, with a 1 dB edge.
    const auto zero = axisForDecibels (0.0f);
    static const std::vector<Shape> shapes {
        { "Limit at 0 dB", { makePoint (0.0f, 0.0f), makePoint (zero, zero), makePoint (1.0f, zero) } },
        { "No Compression", { makePoint (0.0f, 0.0f), makePoint (1.0f, 1.0f) } },
        { "Compress 2:1 over -12 dB", makeCompression (-12.0f, 2.0f) },
        { "Compress 4:1 over -18 dB", makeCompression (-18.0f, 4.0f) },
        { "Gate under -30 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-31.0f), 0.0f),
            makePoint (axisForDecibels (-30.0f), axisForDecibels (-30.0f)), makePoint (1.0f, 1.0f) } },
        { "Gate under -40 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-41.0f), 0.0f),
            makePoint (axisForDecibels (-40.0f), axisForDecibels (-40.0f)), makePoint (1.0f, 1.0f) } },
        { "Gate under -50 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-51.0f), 0.0f),
            makePoint (axisForDecibels (-50.0f), axisForDecibels (-50.0f)), makePoint (1.0f, 1.0f) } },
        // 1:2 under -24 dB, down to -60 dB out at -42 dB in; silent under that, as a gate, since the axis ends there.
        { "Expand under -24 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-42.0f), 0.0f),
            makePoint (axisForDecibels (-24.0f), axisForDecibels (-24.0f)), makePoint (1.0f, 1.0f) } },
        // 2:1 upward under -30 dB: -60 dB, and what is quieter, 15 dB louder.
        { "Lift under -30 dB",
          { makePoint (0.0f, axisForDecibels (-45.0f)), makePoint (axisForDecibels (-30.0f), axisForDecibels (-30.0f)),
            makePoint (1.0f, 1.0f) } },
        // 2.5:1 upward under -30 dB, and over it 30:16 to 0 dB and 4:1 above.
        { "Squeeze Both Ways",
          { makePoint (0.0f, axisForDecibels (-42.0f)), makePoint (axisForDecibels (-30.0f), axisForDecibels (-30.0f)),
            makePoint (zero, axisForDecibels (-14.0f)), makePoint (1.0f, axisForDecibels (-11.0f)) } },
    };
    return shapes;
}

const Shape* findShape (std::string_view name)
{
    for (const auto& shape : getShapes())
        if (shape.name == name)
            return &shape;

    return nullptr;
}

bool Gains::update (std::span<const float> curve) noexcept
{
    if (hasTable && std::equal (shownCurve.begin(), shownCurve.end(), curve.begin()))
        return false;

    std::copy (curve.begin(), curve.begin() + parameterCount, shownCurve.begin());
    hasTable = true;

    // The gain for each level in: how far out the curve puts it, against how far in it is; silence out at the bottom, and
    // under the axes' bottom the gain just over it.
    endDecibels = topDecibels;
    stepDecibels = (endDecibels - lowestDecibels) / (float) tableSize;

    for (int i = 0; i <= tableSize; ++i)
    {
        const auto place = std::max (justOverBottom, axisForDecibels (lowestDecibels + (float) i * stepDecibels));
        const auto out = getAt (curve, place);
        gains[(size_t) i] = out > 0.0f ? juce::jlimit (quietest, loudest, decibelsForAxis (out) - decibelsForAxis (place)) : quietest;
    }

    const auto [lowest, highest] = std::minmax_element (gains.begin(), gains.end());
    span = *highest - *lowest;
    lastGain = fromAxis (getAt (curve, 1.0f));
    return true;
}

float Gains::getDecibels (float level) const noexcept
{
    const auto decibels = dsp::gainToDecibels (level);

    if (decibels >= endDecibels)
        return juce::jlimit (quietest, loudest, dsp::gainToDecibels (lastGain) - decibels);

    // Whatever it is asked, it reads inside its table: what is no level at all (not a number) as silence.
    const auto along = decibels > lowestDecibels ? (decibels - lowestDecibels) / stepDecibels : 0.0f;
    const auto index = std::clamp ((int) along, 0, tableSize - 1);
    return gains[(size_t) index] + (along - (float) index) * (gains[(size_t) index + 1] - gains[(size_t) index]);
}
} // namespace phazite::devices::curves
