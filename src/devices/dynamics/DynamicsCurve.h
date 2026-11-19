// SPDX-License-Identifier: AGPL-3.0-or-later
// A dynamics curve, as FL Studio's Maximus draws its compressors' (the owner, 2026-11-01: "i like this interface and
// interaction with how i can use it as an upwards/downwards compressor/expander, so i want this graph to also be used the
// same in the compressor plugin"): level in across, level out up, through up to eight points, each line to a point bent by
// its tension or shaped as an S, a hold or stairs. By its shape alone, with no threshold or ratio of its own, it compresses
// or expands, downward or upward, limits or gates. The multiband's bands and the compressor each have one, as parameters:
// how many points it has, then each point's place across, its place up, its line's tension and its line's type (PointField),
// from left to right, the last one repeated in the places it leaves.
//
// Its axes: a level in decibels, the same across and up, evenly from -60 dB at 0 (bottomDecibels) to 20 log10 4 dB, about
// +12.04, at 1 (topDecibels: where the curves' end was before their axes were decibels, so that an end means what it meant).
// What its editor shows is what plays: its lines are worked out on these axes, so that a straight line is a constant ratio
// (from -18 dB to the top, rising 7.5 dB, is 4:1), as the transfer curves of FabFilter's, Ableton's and Bitwig's compressors
// are drawn, and a bent one stays in the box its two points span. The bottom, a place of 0, says two things:
// - Out, it is silence: a point at the bottom puts out nothing (a gate's floor), and a line leaving it rises from -60 dB.
// - In, it is -60 dB and every level under it, which come out with the gain the curve gives just over -60 dB: unchanged under
//   a line that rises from the corner (No Compression's), silent under one that stays at the bottom (a gate's floor), and as
//   much louder or quieter as at -60 dB under one that starts higher or lower (Lift's).
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace phazite::devices::curves
{
inline constexpr int maxPoints = 8;

enum PointField
{
    pointX, // across, from 0 to 1 (the first point at 0, the last at 1)
    pointY,
    pointTension, // from -1 to 1: how the line to it bends (up for 1)
    pointType,    // a PointType: how the line to it goes
    pointFields
};

enum class PointType
{
    singleCurve, // bent by its tension
    doubleCurve, // an S, bent by its tension
    hold,        // the level before it held until it
    stairs,      // steps, as many as its tension says
    smoothStairs // the same, each step rounded
};

inline constexpr std::string_view pointTypeNames[] = { "Single Curve", "Double Curve", "Hold", "Stairs", "Smooth Stairs" };

// How many parameters a curve has, and where a point's field is among them.
inline constexpr int parameterCount = 1 + maxPoints * pointFields;

constexpr int getPointParameter (int point, PointField field) noexcept
{
    return 1 + point * pointFields + field;
}

// The axes' ends, in decibels.
inline constexpr float bottomDecibels = -60.0f;
inline constexpr float topDecibels = 12.0411998f; // 20 log10 4

// The axes: where a level in decibels is (0 at -60 dB and under, past 1 over the top), and the level at a place (-60 dB at
// 0); an amplitude's place, and the amplitude at a place, which is silence at 0 (out at the bottom is no sound).
float axisForDecibels (float decibels) noexcept;
float decibelsForAxis (float place) noexcept;
float toAxis (float amplitude) noexcept;
float fromAxis (float place) noexcept;

// How many steps a stairs line takes, for a tension.
int getStairs (float tension) noexcept;

// Where a curve puts out (from 0 to 1) what comes in at a place across (from 0 to 1), from its parameters' values.
float getAt (std::span<const float> curve, float place) noexcept;

// A point by its fields, in PointField's order.
using Point = std::array<float, pointFields>;

constexpr Point makePoint (float across, float up) noexcept
{
    return { across, up, 0.0f, (float) PointType::singleCurve };
}

// A compressor's curve as one with a threshold, a ratio and a soft knee centred on the threshold sets it (Giannoulis,
// Massberg and Reiss, 2012): level in equalling level out up to the knee, then coming out 1 / ratio as far over the
// threshold as it goes in (a ratio under 1 expands upward). A straight line on its axes is a ratio, so its points (at the
// axes' start, where the knee starts, where it ends, at the axes' end, and where an upward ratio reaches the top) and its
// straight lines are exact; only the knee's line, bent as near the knee's curve as its tension takes it, is off it:
// within about 0.15 dB for knees of 6 to 12 dB at 2:1 to 10:1 (0.2 dB at 24 dB), for a threshold whose knee starts
// above the axis's bottom and ends under its top.
std::vector<Point> makeCompression (float thresholdDecibels, float ratio, float kneeDecibels = 0.0f);

// The shapes a curve starts from, as Maximus's manual shows its curves, by name.
struct Shape
{
    std::string_view name;
    std::vector<Point> points;
};

const std::vector<Shape>& getShapes();
const Shape* findShape (std::string_view name);

// A curve's gains, for a compressor: how many decibels each level comes out louder (below 0, quieter), a table of them every
// quarter of a decibel or so from -120 dB (under the axes' bottom, the gain just over it) to the axes' end. Made again only
// when the curve's values change, without allocating.
class Gains
{
public:
    static constexpr int tableSize = 528;
    static constexpr float lowestDecibels = -120.0f;

    bool update (std::span<const float> curve) noexcept; // whether it made its table again
    void reset() noexcept { hasTable = false; }          // made again at the next update

    // For a level heard (an amplitude); past the axes' end, out as loud as the curve's end.
    float getDecibels (float level) const noexcept;

    // How far apart, in decibels, the most its curve turns a level up and the most it turns one down: as far as its gain can
    // move, from one level to another.
    float getSpan() const noexcept { return span; }

private:
    std::array<float, tableSize + 1> gains {};
    std::array<float, parameterCount> shownCurve {}; // the values the table was made from
    bool hasTable = false;
    float lastGain = 1.0f;      // the amplitude the curve puts out at the axes' end
    float endDecibels = 12.0f;  // the axes' end
    float stepDecibels = 0.25f; // between the table's entries
    float span = 0.0f;
};
} // namespace phazite::devices::curves
