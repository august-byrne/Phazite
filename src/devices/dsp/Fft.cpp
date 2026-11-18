// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace phazite::devices::dsp
{
void Fft::prepare (int newOrder)
{
    order = std::clamp (newOrder, 1, maxOrder);
    size = 1 << order;
    twiddles.resize ((size_t) (size / 2));

    for (int k = 0; k < size / 2; ++k)
    {
        const auto angle = -2.0 * std::numbers::pi * (double) k / (double) size;
        twiddles[(size_t) k] = { (float) std::cos (angle), (float) std::sin (angle) };
    }

    reversed.resize ((size_t) size);

    for (int i = 0; i < size; ++i)
    {
        std::uint32_t r = 0;

        for (int bit = 0; bit < order; ++bit)
            if ((i >> bit) & 1)
                r |= 1u << (order - 1 - bit);

        reversed[(size_t) i] = r;
    }
}

void Fft::perform (const Complex* in, Complex* out, bool inverse) const noexcept
{
    if (size == 0)
        return;

    // The points in bit-reversed order.
    if (in == out)
    {
        for (int i = 0; i < size; ++i)
            if (const auto j = (int) reversed[(size_t) i]; i < j)
                std::swap (out[i], out[j]);
    }
    else
    {
        for (int i = 0; i < size; ++i)
            out[reversed[(size_t) i]] = in[i];
    }

    auto* data = reinterpret_cast<float*> (out);
    const auto* turns = reinterpret_cast<const float*> (twiddles.data());
    const auto sign = inverse ? -1.0f : 1.0f;

    // The first stage's butterflies turn nothing.
    for (int i = 0; i < 2 * size; i += 4)
    {
        const auto ar = data[i], ai = data[i + 1], br = data[i + 2], bi = data[i + 3];
        data[i] = ar + br;
        data[i + 1] = ai + bi;
        data[i + 2] = ar - br;
        data[i + 3] = ai - bi;
    }

    for (int half = 2; half < size; half *= 2)
    {
        const auto step = size / (2 * half);

        for (int start = 0; start < size; start += 2 * half)
        {
            auto* a = data + 2 * start;
            auto* b = a + 2 * half;

            for (int j = 0; j < half; ++j)
            {
                const auto wr = turns[2 * j * step], wi = sign * turns[2 * j * step + 1];
                const auto xr = b[2 * j], xi = b[2 * j + 1];
                const auto tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                const auto ur = a[2 * j], ui = a[2 * j + 1];
                a[2 * j] = ur + tr;
                a[2 * j + 1] = ui + ti;
                b[2 * j] = ur - tr;
                b[2 * j + 1] = ui - ti;
            }
        }
    }

    if (inverse)
    {
        const auto scale = 1.0f / (float) size;

        for (int i = 0; i < 2 * size; ++i)
            data[i] *= scale;
    }
}
} // namespace phazite::devices::dsp
