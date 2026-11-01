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
    constexpr float log9 = 2.1972245773f; // the axes: ln 9
    constexpr float quietest = -120.0f;   // decibels: no quieter gains than this
    constexpr float loudest = 48.0f;      // and no louder

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

float toAxis (float amplitude) noexcept
{
    return std::log1p (2.0f * std::max (0.0f, amplitude)) / log9;
}

float fromAxis (float place) noexcept
{
    return 0.5f * std::expm1 (place * log9);
}

float axisForDecibels (float decibels) noexcept
{
    return toAxis (dsp::decibelsToGain (decibels));
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

    const auto end = dsp::gainToDecibels (fromAxis (1.0f));
    const auto start = thresholdDecibels - 0.5f * kneeDecibels;
    std::vector<Point> points { makePoint (0.0f, 0.0f), makePoint (place (start), place (start)) };

    if (const auto kneeEnd = thresholdDecibels + 0.5f * kneeDecibels; kneeDecibels > 0.0f && kneeEnd < 0.0f)
        points.push_back (makePoint (place (kneeEnd), place (wanted (kneeEnd))));

    if (start < 0.0f)
        points.push_back (makePoint (0.5f, place (wanted (0.0f))));

    points.push_back (makePoint (1.0f, place (wanted (end))));

    // Each line after the knee's start: the tension at which it is nearest the compressor's curve where it is furthest from
    // it, sampled evenly in decibels (a golden-section search: the error falls, then rises, as the tension grows).
    for (size_t i = 2; i < points.size(); ++i)
    {
        const auto& from = points[i - 1];
        const auto& to = points[i];

        if (to[pointX] <= from[pointX])
            continue;

        const auto low = dsp::gainToDecibels (fromAxis (from[pointX])), high = dsp::gainToDecibels (fromAxis (to[pointX]));
        const auto worstError = [&] (float tension)
        {
            auto worst = 0.0f;

            for (int k = 1; k < 32; ++k)
            {
                const auto in = low + (high - low) * (float) k / 32.0f;
                const auto s = (axisForDecibels (in) - from[pointX]) / (to[pointX] - from[pointX]);
                const auto out = getLine (PointType::singleCurve, tension, s, from[pointY], to[pointY]);
                worst = std::max (worst, std::abs (dsp::gainToDecibels (fromAxis (out)) - wanted (in)));
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

        points[i][pointTension] = 0.5f * (a + b);
    }

    return points;
}

const std::vector<Shape>& getShapes()
{
    static const std::vector<Shape> shapes {
        { "Limit at 0 dB", { makePoint (0.0f, 0.0f), makePoint (0.5f, 0.5f), makePoint (1.0f, 0.5f) } },
        { "No Compression", { makePoint (0.0f, 0.0f), makePoint (1.0f, 1.0f) } },
        { "Compress 2:1 over -12 dB", makeCompression (-12.0f, 2.0f) },
        { "Compress 4:1 over -18 dB", makeCompression (-18.0f, 4.0f) },
        { "Gate under -30 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-31.0f), 0.0f),
            makePoint (axisForDecibels (-30.0f), axisForDecibels (-30.0f)), makePoint (1.0f, 1.0f) } },
        { "Expand under -24 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-40.0f), axisForDecibels (-56.0f)),
            makePoint (axisForDecibels (-24.0f), axisForDecibels (-24.0f)), makePoint (1.0f, 1.0f) } },
        { "Lift under -30 dB",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-60.0f), axisForDecibels (-45.0f)),
            makePoint (axisForDecibels (-30.0f), axisForDecibels (-30.0f)), makePoint (1.0f, 1.0f) } },
        { "Squeeze Both Ways",
          { makePoint (0.0f, 0.0f), makePoint (axisForDecibels (-60.0f), axisForDecibels (-42.0f)),
            makePoint (axisForDecibels (-30.0f), axisForDecibels (-30.0f)), makePoint (0.5f, axisForDecibels (-14.0f)),
            makePoint (1.0f, axisForDecibels (-11.0f)) } },
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

void Gains::update (std::span<const float> curve) noexcept
{
    if (hasTable && std::equal (shownCurve.begin(), shownCurve.end(), curve.begin()))
        return;

    std::copy (curve.begin(), curve.begin() + parameterCount, shownCurve.begin());
    hasTable = true;

    // The gain for each level in: how far out the curve puts it, against how far in it is.
    endDecibels = dsp::gainToDecibels (fromAxis (1.0f));
    stepDecibels = (endDecibels - lowestDecibels) / (float) tableSize;

    for (int i = 0; i <= tableSize; ++i)
    {
        const auto decibels = lowestDecibels + (float) i * stepDecibels;
        const auto out = fromAxis (getAt (curve, axisForDecibels (decibels)));
        gains[(size_t) i] = juce::jlimit (quietest, loudest, dsp::gainToDecibels (out) - decibels);
    }

    lastGain = fromAxis (getAt (curve, 1.0f));
}

float Gains::getDecibels (float level) const noexcept
{
    const auto decibels = dsp::gainToDecibels (level);

    if (decibels >= endDecibels)
        return juce::jlimit (quietest, loudest, dsp::gainToDecibels (lastGain) - decibels);

    const auto along = (decibels - lowestDecibels) / stepDecibels;
    const auto index = std::min ((int) along, tableSize - 1);
    return gains[(size_t) index] + (along - (float) index) * (gains[(size_t) index + 1] - gains[(size_t) index]);
}
} // namespace phazite::devices::curves
