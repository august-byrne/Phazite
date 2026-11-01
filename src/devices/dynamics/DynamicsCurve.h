// SPDX-License-Identifier: AGPL-3.0-or-later
// A dynamics curve, as FL Studio's Maximus draws its compressors' (the owner, 2026-11-01: "i like this interface and
// interaction with how i can use it as an upwards/downwards compressor/expander, so i want this graph to also be used the
// same in the compressor plugin"): level in across, level out up, through up to eight points, each line to a point bent by
// its tension or shaped as an S, a hold or stairs. By its shape alone, with no threshold or ratio of its own, it compresses
// or expands, downward or upward, limits or gates. The multiband's bands and the compressor each have one, as parameters:
// how many points it has, then each point's place across, its place up, its line's tension and its line's type (PointField),
// from left to right, the last one repeated in the places it leaves.
//
// Its axes: an amplitude a (1 at 0 dB) sits at ln (1 + 2a) / ln 9 across, from silence at 0 through 0 dB at the middle to
// +12 dB at 1, as Maximus draws its curves; a line between two points is straight on those axes.
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

// The axes: an amplitude's place across, from 0 to 1, and back; and where a level in decibels is.
float toAxis (float amplitude) noexcept;
float fromAxis (float place) noexcept;
float axisForDecibels (float decibels) noexcept;

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
// threshold as it goes in (a ratio under 1 expands upward). It has a point where the knee starts, one where it ends, one at
// 0 dB and one at the axes' end, which are exact, and the lines between them bent as near the compressor's curve as their
// tensions take them, since a straight line on Maximus's axes isn't a ratio: within a tenth of a decibel at 4:1 over -18 dB,
// half a decibel at 4:1 over -36 dB.
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
// quarter of a decibel or so from -120 dB to the axes' end, so that quiet levels, which Maximus's axes crowd together, are
// as exact as loud ones. Made again only when the curve's values change, without allocating.
class Gains
{
public:
    static constexpr int tableSize = 528;
    static constexpr float lowestDecibels = -120.0f;

    void update (std::span<const float> curve) noexcept;
    void reset() noexcept { hasTable = false; } // made again at the next update

    // For a level heard (an amplitude); past the axes' end, out as loud as the curve's end.
    float getDecibels (float level) const noexcept;

private:
    std::array<float, tableSize + 1> gains {};
    std::array<float, parameterCount> shownCurve {}; // the values the table was made from
    bool hasTable = false;
    float lastGain = 1.0f;      // the amplitude the curve puts out at the axes' end
    float endDecibels = 12.0f;  // the axes' end
    float stepDecibels = 0.25f; // between the table's entries
};
} // namespace phazite::devices::curves
