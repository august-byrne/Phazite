// SPDX-License-Identifier: AGPL-3.0-or-later
// A complex FFT done in small parts, for work spread evenly over many blocks (the EQ's linear-phase kernels, designed
// while it plays): a transform of N = N1 x N2 points as N2 transforms of N1 points, each turned by its twiddles, then N1
// transforms of N2 points (the "four-step" FFT). Each part is a unit of work of its own, done whenever its caller has the
// time; together they give the transform dsp::Fft gives, to a float's rounding (the inverse scaled by 1 / N). Made, with its
// tables, off the audio thread; perform() allocates nothing and locks nothing.
#pragma once

#include "devices/dsp/Fft.h"

#include <complex>
#include <vector>

namespace phazite::devices::dsp
{
class SplitFft
{
public:
    using Complex = std::complex<float>;

    // For a transform of 2^order points (order from 2 to 30), forward or inverse. Allocates.
    void prepare (int order, bool inverse);

    int getSize() const noexcept { return size; }
    int getNumParts() const noexcept { return n1 + n2; } // n2 transforms of n1 points, then n1 of n2

    // The work of a part, in units of about five floating-point operations (a butterfly's share of a point), and of all.
    int getPartCost (int part) const noexcept;
    int getCost() const noexcept;

    // A part of the transform of `in` (getSize() points) into `out` (as many), through `work` (as many): the parts in order,
    // from 0 to getNumParts() - 1, `in` as it was until the first n2 are done.
    void perform (int part, const Complex* in, Complex* work, Complex* out) noexcept;

    // All of them, in order.
    void performAll (const Complex* in, Complex* work, Complex* out) noexcept;

private:
    int size = 0, n1 = 1, n2 = 1;
    bool isInverse = false;
    Fft columns, rows;             // of n1 and n2 points
    std::vector<Complex> twiddles; // e^(-+2 pi i n2 k1 / N), at [n2 * n1 + k1]
    std::vector<Complex> from, to; // one transform's points, gathered and transformed
};
} // namespace phazite::devices::dsp
