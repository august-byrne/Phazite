// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TruePeak.h"

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // ITU-R BS.1770-5, Annex 2: the interpolating filter's 48 coefficients, as four points' filters of 12, each weighing the
    // newest sample first. The last two points' filters are the first two's backwards.
    // clang-format off
    constexpr std::array<std::array<float, TruePeak::taps>, 4> points { {
        {  0.0017089843750f,  0.0109863281250f, -0.0196533203125f,  0.0332031250000f, -0.0594482421875f,  0.1373291015625f,
           0.9721679687500f, -0.1022949218750f,  0.0476074218750f, -0.0266113281250f,  0.0148925781250f, -0.0083007812500f },
        { -0.0291748046875f,  0.0292968750000f, -0.0517578125000f,  0.0891113281250f, -0.1665039062500f,  0.4650878906250f,
           0.7797851562500f, -0.2003173828125f,  0.1015625000000f, -0.0582275390625f,  0.0330810546875f, -0.0189208984375f },
        { -0.0189208984375f,  0.0330810546875f, -0.0582275390625f,  0.1015625000000f, -0.2003173828125f,  0.7797851562500f,
           0.4650878906250f, -0.1665039062500f,  0.0891113281250f, -0.0517578125000f,  0.0292968750000f, -0.0291748046875f },
        { -0.0083007812500f,  0.0148925781250f, -0.0266113281250f,  0.0476074218750f, -0.1022949218750f,  0.9721679687500f,
           0.1373291015625f, -0.0594482421875f,  0.0332031250000f, -0.0196533203125f,  0.0109863281250f,  0.0017089843750f },
    } };
    // clang-format on
} // namespace

void TruePeak::reset() noexcept
{
    history.fill (0.0f);
    newest = taps - 1;
}

float TruePeak::push (float sample) noexcept
{
    newest = (newest + 1) % taps;
    history[(size_t) newest] = history[(size_t) (newest + taps)] = sample;

    // The newest sample at newest + taps, the oldest of the twelve at newest + 1.
    const auto* last = history.data() + newest + taps;
    auto loudest = 0.0f;

    for (const auto& weights : points)
    {
        auto point = 0.0f;

        for (int tap = 0; tap < taps; ++tap)
            point += weights[(size_t) tap] * last[-tap];

        loudest = std::max (loudest, std::abs (point));
    }

    return loudest;
}
} // namespace phazite::devices
