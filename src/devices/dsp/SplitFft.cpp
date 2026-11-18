// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SplitFft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace phazite::devices::dsp
{
void SplitFft::prepare (int order, bool inverse)
{
    order = std::max (2, order);
    const auto order1 = order / 2, order2 = order - order1;
    size = 1 << order;
    n1 = 1 << order1;
    n2 = 1 << order2;
    isInverse = inverse;
    columns.prepare (order1);
    rows.prepare (order2);
    twiddles.resize ((size_t) size);
    const auto sign = inverse ? 1.0 : -1.0;

    for (int column = 0; column < n2; ++column)
        for (int k = 0; k < n1; ++k)
        {
            const auto angle = sign * 2.0 * std::numbers::pi * (double) column * (double) k / (double) size;
            twiddles[(size_t) (column * n1 + k)] = { (float) std::cos (angle), (float) std::sin (angle) };
        }

    from.assign ((size_t) std::max (n1, n2), {});
    to.assign ((size_t) std::max (n1, n2), {});
}

int SplitFft::getPartCost (int part) const noexcept
{
    const auto log2 = [] (int n)
    {
        auto bits = 0;

        while ((1 << bits) < n)
            ++bits;

        return bits;
    };

    return part < n2 ? n1 * log2 (n1) + n1 : n2 * log2 (n2);
}

int SplitFft::getCost() const noexcept
{
    auto cost = 0;

    for (int part = 0; part < getNumParts(); ++part)
        cost += getPartCost (part);

    return cost;
}

void SplitFft::perform (int part, const Complex* in, Complex* work, Complex* out) noexcept
{
    if (part < n2)
    {
        // Column `part`: the points part, part + n2, part + 2 n2...; transformed, turned, and kept as the column of work's
        // rows of n2.
        const auto column = part;

        for (int k = 0; k < n1; ++k)
            from[(size_t) k] = in[k * n2 + column];

        columns.perform (from.data(), to.data(), isInverse);
        const auto* turn = twiddles.data() + column * n1;

        for (int k = 0; k < n1; ++k)
            work[k * n2 + column] = to[(size_t) k] * turn[k];

        return;
    }

    // Row k1: its n2 points transformed, out at k1, k1 + n1, k1 + 2 n1...
    const auto row = part - n2;
    rows.perform (work + row * n2, to.data(), isInverse);

    for (int k = 0; k < n2; ++k)
        out[row + k * n1] = to[(size_t) k];
}

void SplitFft::performAll (const Complex* in, Complex* work, Complex* out) noexcept
{
    for (int part = 0; part < getNumParts(); ++part)
        perform (part, in, work, out);
}
} // namespace phazite::devices::dsp
