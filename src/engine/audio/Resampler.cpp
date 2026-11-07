// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Resampler.h"

#include <juce_core/juce_core.h>

// r8brain-free-src's code is third-party, so its warnings are off: every warning a build prints stays Phazite's own.
// GCC and Clang take its folder as a system one (CMakeLists.txt). MSVC says its C++ is older than C++11 (__cplusplus,
// unless asked otherwise), so r8brain includes Windows.h there, without its min and max macros.
#if defined(_MSC_VER)
 #pragma warning(push, 0)
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN 1
 #endif
#endif

#include <CDSPResampler.h>

#if defined(_MSC_VER)
 #pragma warning(pop)
#endif

#include <algorithm>

namespace phazite::engine
{
namespace
{
    // The filter: its transition band 3 % of the lower rate's spectrum, so flat to 0.95 of its Nyquist frequency (20.9 kHz
    // at 44.1 kHz; r8brain's own advice is 2 to 3 % for most uses), and what it leaves of the aliases 136 dB down, under
    // what any converter plays (the 16-bit setting of r8brain's, 6 dB a bit and 40 dB more for loud and quiet passages).
    constexpr double transitionBand = 3.0;
    constexpr double stopbandDecibels = 136.45;
} // namespace

Resampler::Resampler (double from, double to, int maximum)
    : fromRate (from),
      toRate (to),
      maximumInput (std::max (1, maximum)),
      resampler (std::make_unique<r8b::CDSPResampler> (fromRate, toRate, maximumInput, transitionBand, stopbandDecibels))
{
}

Resampler::~Resampler() = default;

int Resampler::getMaximumOutput() const noexcept
{
    return resampler->getMaxOutLen (maximumInput);
}

void Resampler::reset() noexcept
{
    resampler->clear();
}

int Resampler::process (const double* input, int numInput, const double*& output) noexcept
{
    jassert (numInput >= 0 && numInput <= maximumInput);

    // r8brain reads its input without changing it, and keeps what it gives out in its own buffers.
    double* given = nullptr;
    const auto count = resampler->process (const_cast<double*> (input), std::clamp (numInput, 0, maximumInput), given);
    output = given;
    return count;
}
} // namespace phazite::engine
