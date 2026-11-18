// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own FFT (src/devices/dsp/Fft.h), the FFT done in parts (SplitFft.h) and the partitioned convolution
// (PartitionedConvolution.h) that the EQ's linear phase plays through: each against the sum it stands for, written out.
#include "TestHelpers.h"
#include "devices/dsp/Fft.h"
#include "devices/dsp/PartitionedConvolution.h"
#include "devices/dsp/SplitFft.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

namespace
{
namespace dsp = phazite::devices::dsp;
using Complex = std::complex<float>;
using Kernels = dsp::PartitionedConvolution::Kernels;

// Numbers from -1 to 1, the same every time on every system.
struct Numbers
{
    std::uint32_t state;

    float next()
    {
        state = state * 1664525u + 1013904223u;
        return (float) ((double) (state >> 8) / (double) (1u << 23) - 1.0);
    }
};

std::vector<Complex> makeNumbers (int size, std::uint32_t seed)
{
    Numbers numbers { seed };
    std::vector<Complex> points ((size_t) size);

    for (auto& point : points)
    {
        const auto re = numbers.next();
        point = { re, numbers.next() };
    }

    return points;
}

// The DFT written out, in double precision: forward, or inverse scaled by 1 / N.
std::vector<std::complex<double>> dft (const std::vector<Complex>& in, bool inverse)
{
    const auto size = in.size();
    std::vector<std::complex<double>> out (size);
    const auto sign = inverse ? 1.0 : -1.0;

    for (size_t k = 0; k < size; ++k)
    {
        std::complex<double> sum;

        for (size_t n = 0; n < size; ++n)
        {
            const auto angle = sign * 2.0 * std::numbers::pi * (double) ((k * n) % size) / (double) size;
            sum += std::complex<double> (in[n]) * std::complex<double> (std::cos (angle), std::sin (angle));
        }

        out[k] = inverse ? sum / (double) size : sum;
    }

    return out;
}

double largestOf (const std::vector<std::complex<double>>& points)
{
    auto largest = 0.0;

    for (const auto& point : points)
        largest = std::max (largest, std::abs (point));

    return largest;
}

int orderOf (int size)
{
    auto order = 0;

    while ((1 << order) < size)
        ++order;

    return order;
}

// A kernel's taps into a set's partitions' spectra, as the convolution keeps them: each partition of P taps, followed by P
// zeros, transformed, its first P + 1 points kept.
void setKernel (const dsp::PartitionedConvolution& convolution, Kernels& kernels, int index, const std::vector<float>& taps)
{
    const auto partition = convolution.getPartitionSize();
    const dsp::Fft fft (orderOf (2 * partition));
    std::vector<Complex> frame ((size_t) (2 * partition)), spectrum ((size_t) (2 * partition));

    for (int p = 0; p < convolution.getNumPartitions(); ++p)
    {
        std::fill (frame.begin(), frame.end(), Complex {});

        for (int i = 0; i < partition; ++i)
            if (const auto tap = (size_t) (p * partition + i); tap < taps.size())
                frame[(size_t) i] = taps[tap];

        fft.perform (frame.data(), spectrum.data(), false);
        std::copy (spectrum.begin(), spectrum.begin() + partition + 1, kernels.spectra[(size_t) index].begin() + p * (partition + 1));
    }

    kernels.kinds[(size_t) index] = Kernels::Kind::filter;
}

// Taps that fade as they go, so that every partition counts.
std::vector<float> makeTaps (int numTaps, std::uint32_t seed)
{
    Numbers numbers { seed };
    std::vector<float> taps ((size_t) numTaps);

    for (int i = 0; i < numTaps; ++i)
        taps[(size_t) i] = numbers.next() * (float) std::exp (-3.0 * i / numTaps);

    return taps;
}

// A direct convolution written out, in double precision, as late as `late` samples.
std::vector<double> convolveDirectly (const std::vector<float>& in, const std::vector<float>& taps, int late)
{
    std::vector<double> out (in.size(), 0.0);

    for (size_t n = 0; n < in.size(); ++n)
    {
        auto sum = 0.0;

        for (size_t k = 0; k < taps.size(); ++k)
            if (const auto from = (std::int64_t) n - late - (std::int64_t) k; from >= 0)
                sum += (double) taps[k] * in[(size_t) from];

        out[n] = sum;
    }

    return out;
}
} // namespace

TEST_CASE ("Phazite's FFT is the DFT written out, forward and inverse, and gives back what went in")
{
    for (const auto size : { 16, 64, 1024 })
    {
        CAPTURE (size);
        const dsp::Fft fft (orderOf (size));
        REQUIRE (fft.getSize() == size);
        const auto in = makeNumbers (size, (std::uint32_t) size);

        for (const auto inverse : { false, true })
        {
            CAPTURE (inverse);
            std::vector<Complex> out ((size_t) size);
            fft.perform (in.data(), out.data(), inverse);
            const auto expected = dft (in, inverse);
            auto apart = 0.0;

            for (size_t k = 0; k < (size_t) size; ++k)
                apart = std::max (apart, std::abs (std::complex<double> (out[k]) - expected[k]));

            MESSAGE (size << " points" << std::string (inverse ? ", inverse" : "") << ": " << apart / largestOf (expected)
                          << " of the largest bin from the DFT at most");
            CHECK (apart <= 1.0e-4 * largestOf (expected));

            // In place, the same to the bit.
            auto inPlace = in;
            fft.perform (inPlace.data(), inPlace.data(), inverse);

            auto sameBits = true;

            for (size_t k = 0; k < (size_t) size; ++k)
                sameBits = sameBits && juce::exactlyEqual (inPlace[k].real(), out[k].real())
                           && juce::exactlyEqual (inPlace[k].imag(), out[k].imag());

            CHECK (sameBits);
        }

        // Parseval: the energy of the spectrum is N times the signal's.
        std::vector<Complex> spectrum ((size_t) size);
        fft.perform (in.data(), spectrum.data(), false);
        auto energyIn = 0.0, energyOut = 0.0;

        for (size_t i = 0; i < (size_t) size; ++i)
        {
            energyIn += std::norm (std::complex<double> (in[i]));
            energyOut += std::norm (std::complex<double> (spectrum[i]));
        }

        CHECK (energyOut == doctest::Approx (energyIn * size).epsilon (1.0e-5));
    }

    // Forward then inverse, at every size it takes.
    for (int order = 4; order <= dsp::Fft::maxOrder; ++order)
    {
        CAPTURE (order);
        const dsp::Fft fft (order);
        const auto in = makeNumbers (fft.getSize(), (std::uint32_t) order);
        std::vector<Complex> spectrum (in.size()), back (in.size());
        fft.perform (in.data(), spectrum.data(), false);
        fft.perform (spectrum.data(), back.data(), true);
        auto apart = 0.0f;

        for (size_t i = 0; i < in.size(); ++i)
            apart = std::max (apart, std::abs (back[i] - in[i]));

        CHECK (apart < 1.0e-5f);
    }
}

TEST_CASE ("The FFT done in parts is the FFT of the same size")
{
    for (const auto order : { 4, 9, 13 })
        for (const auto inverse : { false, true })
        {
            CAPTURE (order);
            CAPTURE (inverse);
            dsp::SplitFft split;
            split.prepare (order, inverse);
            const dsp::Fft fft (order);
            const auto in = makeNumbers (fft.getSize(), 7);
            std::vector<Complex> work (in.size()), out (in.size()), plain (in.size());
            split.performAll (in.data(), work.data(), out.data());
            fft.perform (in.data(), plain.data(), inverse);
            auto apart = 0.0f, largest = 0.0f;

            for (size_t k = 0; k < in.size(); ++k)
            {
                apart = std::max (apart, std::abs (out[k] - plain[k]));
                largest = std::max (largest, std::abs (plain[k]));
            }

            CHECK (apart <= 1.0e-5f * largest);
        }
}

TEST_CASE ("The partitioned convolution is the convolution written out, however its blocks fall, and glides as two crossfaded")
{
    constexpr int partition = 64, numSamples = 6000;

    for (const auto numPartitions : { 3, 31 })
        for (const auto numKernels : { 1, 4 })
        {
            CAPTURE (numPartitions);
            CAPTURE (numKernels);
            dsp::PartitionedConvolution convolution;
            convolution.prepare (partition, numPartitions, 0);
            const auto numTaps = numPartitions * partition - 5; // the last partition not whole
            Kernels first, second;

            for (auto* kernels : { &first, &second })
            {
                convolution.allocate (*kernels);
                kernels->kinds.fill (Kernels::Kind::none);
            }

            // One kernel for both sides, or four: [out * 2 + in].
            std::vector<std::vector<float>> firstTaps, secondTaps;

            for (int k = 0; k < numKernels; ++k)
            {
                firstTaps.push_back (makeTaps (numTaps, (std::uint32_t) (11 + k)));
                secondTaps.push_back (makeTaps (numTaps, (std::uint32_t) (31 + k)));
                setKernel (convolution, first, k, firstTaps.back());
                setKernel (convolution, second, k, secondTaps.back());
            }

            first.routes = second.routes = numKernels == 1 ? std::array { 0, -1, -1, 0 } : std::array { 0, 1, 2, 3 };

            std::array<std::vector<float>, 2> in;

            for (int side = 0; side < 2; ++side)
            {
                Numbers numbers { (std::uint32_t) (101 + side) };
                in[(size_t) side].resize ((size_t) numSamples);

                for (auto& x : in[(size_t) side])
                    x = 0.5f * numbers.next();
            }

            // What each set gives, written out: each side out the sum of its kernels from each side in, P samples late.
            const auto direct = [&] (const std::vector<std::vector<float>>& taps)
            {
                std::array<std::vector<double>, 2> out;

                for (int side = 0; side < 2; ++side)
                {
                    out[(size_t) side].assign ((size_t) numSamples, 0.0);

                    for (int from = 0; from < 2; ++from)
                    {
                        const auto route = numKernels == 1 ? (from == side ? 0 : -1) : side * 2 + from;

                        if (route < 0)
                            continue;

                        const auto part = convolveDirectly (in[(size_t) from], taps[(size_t) route], partition);

                        for (size_t i = 0; i < part.size(); ++i)
                            out[(size_t) side][i] += part[i];
                    }
                }

                return out;
            };

            const auto expectedFirst = direct (firstTaps), expectedSecond = direct (secondTaps);
            auto largest = 0.0;

            for (const auto& side : expectedFirst)
                for (const auto x : side)
                    largest = std::max (largest, std::abs (x));

            // Played in blocks: the first set, then a glide to the second from a sample in the middle of a partition, over so
            // many samples, crossfaded in a straight line as the linear phase does, then the second.
            constexpr int glideFrom = 2000 + 37, glideLength = 1024;

            for (const auto blockSize : { 1, 7, 300 })
            {
                CAPTURE (blockSize);
                convolution.reset();
                convolution.setPlayed (&first);
                std::array<std::vector<float>, 2> played { std::vector<float> ((size_t) numSamples),
                                                           std::vector<float> ((size_t) numSamples) };
                std::array<std::vector<float>, 2> glided { std::vector<float> ((size_t) numSamples),
                                                           std::vector<float> ((size_t) numSamples) };
                auto apart = 0.0;

                for (int start = 0; start < numSamples;)
                {
                    auto length = std::min (blockSize, numSamples - start);

                    for (const auto edge : { glideFrom, glideFrom + glideLength })
                        if (start < edge && start + length > edge)
                            length = edge - start;

                    if (start == glideFrom)
                        convolution.setGliding (&second);

                    if (start == glideFrom + glideLength)
                        convolution.endGlide();

                    const float* ins[] = { in[0].data() + start, in[1].data() + start };
                    float* outs[] = { played[0].data() + start, played[1].data() + start };
                    float* glides[] = { glided[0].data() + start, glided[1].data() + start };
                    convolution.process (ins, 2, length, outs, glides);

                    for (int side = 0; side < 2; ++side)
                        for (int i = start; i < start + length; ++i)
                        {
                            auto out = (double) played[(size_t) side][(size_t) i];
                            auto expected = expectedFirst[(size_t) side][(size_t) i];

                            if (i >= glideFrom + glideLength)
                            {
                                expected = expectedSecond[(size_t) side][(size_t) i];
                            }
                            else if (i >= glideFrom)
                            {
                                const auto t = (double) (i - glideFrom + 1) / glideLength;
                                out += ((double) glided[(size_t) side][(size_t) i] - out) * t;
                                expected += (expectedSecond[(size_t) side][(size_t) i] - expected) * t;
                            }

                            apart = std::max (apart, std::abs (out - expected));
                        }

                    start += length;
                }

                MESSAGE (numPartitions << " partitions, " << numKernels << " kernels, blocks of " << blockSize << ": " << apart / largest
                                       << " of the largest sample from the direct convolution at most");
                CHECK (apart <= 1.0e-5 * largest);
            }
        }
}
