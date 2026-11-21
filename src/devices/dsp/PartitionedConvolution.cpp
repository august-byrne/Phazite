// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PartitionedConvolution.h"
#include "ClearedSamples.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace phazite::devices::dsp
{
void PartitionedConvolution::prepare (int newPartitionSize, int newNumPartitions, int newDelayPartitions)
{
    partitionSize = newPartitionSize;
    numPartitions = std::max (1, newNumPartitions);
    delayPartitions = std::clamp (newDelayPartitions, 0, numPartitions - 1);
    bins = partitionSize + 1;
    const auto size = (size_t) (2 * partitionSize);
    auto order = 0;

    while ((1 << order) < (int) size)
        ++order;

    fft.prepare (order);

    for (auto& side : frames)
        side.assign (size, 0.0f);

    frame.assign (size, {});
    spectrum.assign (size, {});

    for (auto& side : inputs)
        side.assign ((size_t) (numPartitions * bins), {});

    sums.assign ((size_t) (2 * bins), {});
    spectrumOut.assign (size, {});
    timeOut.assign (size, {});

    for (auto& set : outputs)
        for (auto& side : set)
            side.assign ((size_t) partitionSize, 0.0f);

    played = gliding = nullptr;
    reset();
}

void PartitionedConvolution::allocate (Kernels& kernels) const
{
    for (auto& spectra : kernels.spectra)
        spectra.assign ((size_t) (numPartitions * bins), {});
}

void PartitionedConvolution::reset() noexcept
{
    for (auto& side : frames)
        ClearedSamples::clear (std::span (side));

    for (auto& set : outputs)
        for (auto& side : set)
            ClearedSamples::clear (std::span (side));

    newest = position = transformed = 0;
}

void PartitionedConvolution::setPlayed (const Kernels* kernels) noexcept
{
    played = kernels;

    if (played != nullptr)
        convolve (*played, outputs[0]);
    else
        for (auto& side : outputs[0])
            std::fill (side.begin(), side.end(), 0.0f);
}

void PartitionedConvolution::setGliding (const Kernels* kernels) noexcept
{
    gliding = kernels;

    if (gliding != nullptr)
        convolve (*gliding, outputs[1]);
}

void PartitionedConvolution::endGlide() noexcept
{
    if (gliding == nullptr)
        return;

    played = std::exchange (gliding, nullptr);
    std::swap (outputs[0], outputs[1]);
}

void PartitionedConvolution::process (const float* const* in, int numChannels, int length, float* const* playedOut,
                                      float* const* glidedOut) noexcept
{
    numChannels = std::clamp (numChannels, 1, 2);

    for (int done = 0; done < length;)
    {
        const auto n = std::min (partitionSize - position, length - done);

        // What comes in first, as what goes out may be where it came from.
        for (int channel = 0; channel < numChannels; ++channel)
            std::copy (in[channel] + done, in[channel] + done + n, frames[(size_t) channel].begin() + partitionSize + position);

        if (numChannels < 2)
            std::fill (frames[1].begin() + partitionSize + position, frames[1].begin() + partitionSize + position + n, 0.0f);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto& out = outputs[0][(size_t) channel];
            std::copy (out.begin() + position, out.begin() + position + n, playedOut[channel] + done);

            if (gliding != nullptr && glidedOut != nullptr)
            {
                const auto& glided = outputs[1][(size_t) channel];
                std::copy (glided.begin() + position, glided.begin() + position + n, glidedOut[channel] + done);
            }
        }

        channelsIn = numChannels;
        position += n;
        done += n;

        if (position == partitionSize)
        {
            transformPartition();
            position = 0;
        }
    }
}

void PartitionedConvolution::takeApart (const Complex* together, int size, Complex* first, Complex* second) noexcept
{
    // z = a + i b, a and b real: A[k] = (Z[k] + conj Z[N - k]) / 2, B[k] = (Z[k] - conj Z[N - k]) / 2i.
    const auto n = 2 * size;

    for (int k = 0; k <= size; ++k)
    {
        const auto z = together[k];
        const auto mirrored = std::conj (together[(n - k) % n]);
        const auto sum = z + mirrored, difference = z - mirrored;
        first[k] = { 0.5f * sum.real(), 0.5f * sum.imag() };
        second[k] = { 0.5f * difference.imag(), -0.5f * difference.real() };
    }
}

void PartitionedConvolution::transformPartition() noexcept
{
    // Each side's last 2 P samples transformed on their own: packed as one complex frame, the rounding of a loud side would
    // reach a quiet one, whose kernel from the other side may be far quieter than the other's own.
    const auto size = 2 * partitionSize;
    newest = (newest + 1) % numPartitions;

    for (int side = 0; side < channelsIn; ++side)
    {
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = { frames[(size_t) side][(size_t) i], 0.0f };

        fft.perform (frame.data(), spectrum.data(), false);
        std::copy (spectrum.begin(), spectrum.begin() + bins, inputs[(size_t) side].begin() + newest * bins);
    }

    for (int side = channelsIn; side < 2; ++side)
        std::fill (inputs[(size_t) side].begin() + newest * bins, inputs[(size_t) side].begin() + (newest + 1) * bins, Complex {});

    // The middle's and the side's, from the left's and the right's: where the two sides are alike to the bit, so are their
    // spectra, and the side's is nothing.
    const auto* left = inputs[0].data() + newest * bins;
    const auto* right = inputs[1].data() + newest * bins;
    auto* middle = inputs[2].data() + newest * bins;
    auto* difference = inputs[3].data() + newest * bins;

    for (int k = 0; k < bins; ++k)
    {
        middle[k] = (left[k] + right[k]) * 0.5f;
        difference[k] = (left[k] - right[k]) * 0.5f;
    }

    transformed = std::min (transformed + 1, numPartitions);

    for (auto& side : frames)
        std::copy (side.begin() + partitionSize, side.end(), side.begin());

    if (played != nullptr)
        convolve (*played, outputs[0]);

    if (gliding != nullptr)
        convolve (*gliding, outputs[1]);
}

void PartitionedConvolution::convolve (const Kernels& kernels, std::array<std::vector<float>, 2>& output) noexcept
{
    const auto sides = channelsIn > 1 ? 2 : 1;
    const auto firstInput = kernels.midSide && sides > 1 ? 2 : 0;

    // The partitions transformed since the reset alone: what came in before it is silence, which adds nothing to a sum (a sum
    // from +0 is never -0, so that leaving out a product that is 0 or -0 leaves it the same, to the bit).
    const auto heard = std::min (numPartitions, transformed);

    for (int out = 0; out < sides; ++out)
    {
        auto* sum = reinterpret_cast<float*> (sums.data() + out * bins);
        std::fill (sum, sum + 2 * bins, 0.0f);

        for (int in = 0; in < sides; ++in)
        {
            const auto route = kernels.routes[(size_t) (out * 2 + in)];

            if (route < 0)
                continue;

            switch (kernels.kinds[(size_t) route])
            {
                case Kernels::Kind::none:
                    break;

                case Kernels::Kind::delay:
                {
                    if (delayPartitions >= heard)
                        break;

                    const auto slot = (newest - delayPartitions + numPartitions) % numPartitions;
                    const auto* x = reinterpret_cast<const float*> (inputs[(size_t) (firstInput + in)].data() + slot * bins);

                    for (int k = 0; k < 2 * bins; ++k)
                        sum[k] += x[k];

                    break;
                }

                case Kernels::Kind::filter:
                {
                    const auto* kernel = reinterpret_cast<const float*> (kernels.spectra[(size_t) route].data());

                    for (int partition = 0; partition < heard; ++partition)
                    {
                        const auto slot = (newest - partition + numPartitions) % numPartitions;
                        const auto* x = reinterpret_cast<const float*> (inputs[(size_t) (firstInput + in)].data() + slot * bins);
                        const auto* h = kernel + 2 * partition * bins;

                        for (int k = 0; k < 2 * bins; k += 2)
                        {
                            sum[k] += h[k] * x[k] - h[k + 1] * x[k + 1];
                            sum[k + 1] += h[k] * x[k + 1] + h[k + 1] * x[k];
                        }
                    }

                    break;
                }
            }
        }
    }

    // Each side out through an inverse transform of its own, of the whole spectrum its half gives (a real signal's).
    const auto size = 2 * partitionSize;

    for (int out = 0; out < sides; ++out)
    {
        const auto* sum = sums.data() + out * bins;

        for (int k = 0; k <= partitionSize; ++k)
        {
            spectrumOut[(size_t) k] = k == 0 || k == partitionSize ? Complex { sum[k].real(), 0.0f } : sum[k];

            if (k > 0 && k < partitionSize)
                spectrumOut[(size_t) (size - k)] = std::conj (sum[k]);
        }

        fft.perform (spectrumOut.data(), timeOut.data(), true);

        for (int i = 0; i < partitionSize; ++i)
            output[(size_t) out][(size_t) i] = timeOut[(size_t) (partitionSize + i)].real();
    }
}

int PartitionedConvolution::getMultiplyAddsPerSample (const Kernels& kernels, int numChannels, int size, int partitions) noexcept
{
    const auto sides = numChannels > 1 ? 2 : 1;
    auto perPartition = 0;

    for (int out = 0; out < sides; ++out)
        for (int in = 0; in < sides; ++in)
            if (const auto route = kernels.routes[(size_t) (out * 2 + in)]; route >= 0)
            {
                if (kernels.kinds[(size_t) route] == Kernels::Kind::filter)
                    perPartition += 4 * partitions * (size + 1);
                else if (kernels.kinds[(size_t) route] == Kernels::Kind::delay)
                    perPartition += 2 * (size + 1);
            }

    return (perPartition + size - 1) / size;
}
} // namespace phazite::devices::dsp
