// SPDX-License-Identifier: AGPL-3.0-or-later
// A complex FFT of Phazite's own, radix 2, forward and inverse (the inverse scaled by 1 / N, so that one after the other gives
// back what went in), for sizes from 2 to 32768 points: the EQ's linear phase convolves and designs with it on the audio
// thread. Its twiddles and its bit-reversal table are made when it is prepared, in double precision and kept as floats; a
// transform allocates nothing, locks nothing and writes nothing of its own, so one object serves any number of callers at
// once. The same arithmetic on every system, where juce::dsp::FFT takes a lock and computes with another library on some.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace phazite::devices::dsp
{
class Fft
{
public:
    using Complex = std::complex<float>;
    static constexpr int maxOrder = 15;

    Fft() = default;
    explicit Fft (int ofOrder) { prepare (ofOrder); }

    // For transforms of 2^order points, order from 1 to maxOrder (clamped). Allocates.
    void prepare (int newOrder);

    int getSize() const noexcept { return size; }
    int getOrder() const noexcept { return order; }

    // getSize() points of in transformed into out; in and out may be the same (in place). The inverse is scaled by 1 / N.
    void perform (const Complex* in, Complex* out, bool inverse) const noexcept;

private:
    int order = 0, size = 0;
    std::vector<Complex> twiddles;       // e^(-2 pi i k / N), k from 0 to N / 2 - 1
    std::vector<std::uint32_t> reversed; // each index with its bits reversed
};
} // namespace phazite::devices::dsp
