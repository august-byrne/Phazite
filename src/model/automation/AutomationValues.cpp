// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AutomationValues.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace phazite
{
std::vector<AutomationPoint> makeAutomationShape (AutomationShape shape, BeatRange range, BeatDuration cycle, float from, float to)
{
    const auto length = range.getLength();

    if (shape == AutomationShape::line || length <= BeatDuration() || cycle <= BeatDuration())
        return { { range.getStart(), from }, { range.getEnd(), to } };

    const auto cycles = std::max (1, (int) std::round (length / cycle));
    const auto each = length / (double) cycles;
    std::vector<AutomationPoint> points;

    const auto add = [&points, &range, each] (int n, double phase, float value)
    { points.push_back ({ range.getStart() + each * ((double) n + phase), value }); };

    for (int n = 0; n < cycles; ++n)
    {
        switch (shape)
        {
            case AutomationShape::sine:
            {
                constexpr int pointsPerCycle = 16;

                for (int i = 0; i < pointsPerCycle; ++i)
                {
                    const auto phase = (double) i / pointsPerCycle;
                    add (n, phase, from + (to - from) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * phase)));
                }

                break;
            }
            case AutomationShape::triangle:
                add (n, 0.0, from);
                add (n, 0.5, to);
                break;
            case AutomationShape::square:
                add (n, 0.0, from);
                add (n, 0.5, from);
                add (n, 0.5, to);
                add (n, 1.0, to);
                break;
            case AutomationShape::sawUp:
                add (n, 0.0, from);
                add (n, 1.0, to);
                break;
            case AutomationShape::sawDown:
                add (n, 0.0, to);
                add (n, 1.0, from);
                break;
            case AutomationShape::line:
            default:
                break;
        }
    }

    // The shapes that swing back end where they began.
    if (shape == AutomationShape::sine || shape == AutomationShape::triangle)
        points.push_back ({ range.getEnd(), from });

    return points;
}
} // namespace phazite
