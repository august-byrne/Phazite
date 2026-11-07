// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Saturation.h"

namespace phazite::devices::dsp
{
namespace
{
    // The tube curve bends between x = -7.11 and 2.11 (where x + x^2 / 5 reaches 3, softClip's 1); outside, it is 1. Its
    // integral is kept on steps of 1/512 across that, with its slope (the curve), and read between them by cubic Hermite
    // interpolation, which meets both at every step, so that what ADAA works out from two readings is the curve's average.
    constexpr double tableFrom = -7.25, tableTo = 2.25, tableStep = 1.0 / 512.0;
    constexpr int tableSteps = 4864; // (tableTo - tableFrom) / tableStep

    double tube (double x) noexcept
    {
        const auto g = x + 0.2 * x * x;

        if (g >= 3.0)
            return 1.0;

        if (g <= -3.0)
            return -1.0;

        return g * (27.0 + g * g) / (27.0 + 9.0 * g * g);
    }

    struct TubeTable
    {
        std::vector<double> integrals, slopes;

        TubeTable()
        {
            integrals.resize ((size_t) tableSteps + 1);
            slopes.resize ((size_t) tableSteps + 1);
            auto sum = 0.0;

            // Simpson's rule on 16 panels a step, from the table's start.
            for (int i = 0; i <= tableSteps; ++i)
            {
                const auto x = tableFrom + i * tableStep;
                slopes[(size_t) i] = tube (x);

                if (i > 0)
                {
                    constexpr int panels = 16;
                    const auto h = tableStep / panels;
                    auto step = 0.0;

                    for (int p = 0; p < panels; ++p)
                    {
                        const auto a = x - tableStep + p * h;
                        step += h / 6.0 * (tube (a) + 4.0 * tube (a + 0.5 * h) + tube (a + h));
                    }

                    sum += step;
                }

                integrals[(size_t) i] = sum;
            }

            // From 0, as every curve's integral is.
            const auto atZero = integrals[(size_t) std::lround (-tableFrom / tableStep)];

            for (auto& integral : integrals)
                integral -= atZero;
        }
    };

    const TubeTable& getTubeTable()
    {
        static const TubeTable table;
        return table;
    }
} // namespace

void TubeCurve::prepare()
{
    getTubeTable();
}

double TubeCurve::integral (double x) noexcept
{
    const auto& table = getTubeTable();

    // Outside the table the curve is 1: the integral goes on in a straight line.
    if (x <= tableFrom)
        return table.integrals.front() + (x - tableFrom);

    if (x >= tableTo)
        return table.integrals.back() + (x - tableTo);

    const auto place = (x - tableFrom) / tableStep;
    const auto i = std::min ((int) place, tableSteps - 1);
    const auto t = place - i;
    const auto f0 = table.integrals[(size_t) i], f1 = table.integrals[(size_t) i + 1];
    const auto d0 = table.slopes[(size_t) i] * tableStep, d1 = table.slopes[(size_t) i + 1] * tableStep;
    const auto t2 = t * t, t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * f0 + (t3 - 2.0 * t2 + t) * d0 + (-2.0 * t3 + 3.0 * t2) * f1 + (t3 - t2) * d1;
}
} // namespace phazite::devices::dsp
