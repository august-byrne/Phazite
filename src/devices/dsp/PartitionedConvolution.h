// SPDX-License-Identifier: AGPL-3.0-or-later
// A convolution of one or two sides with long kernels, uniformly partitioned, overlap-save, in the frequency domain (the EQ's
// linear phase): each partition of P samples, one FFT of the last 2 P samples of each side in, whose spectra are kept for as
// many partitions as a kernel has; each side out the sum, over the partitions, of each kernel's partition's spectrum times
// the spectrum of what came in that many partitions before; and one inverse FFT for each side out (each side transformed
// alone, so that a loud side's rounding never reaches a quiet one). Its sound comes P samples late, beside the kernel's own
// delay. The partitions are counted
// from its reset, and what it plays doesn't depend on where they fall but for rounding (a convolution is the same however it
// is cut).
//
// What it plays through is a set of kernels (Kernels): up to four responses, each from a side in to a side out (left to
// left, right to left, left to right, right to right), a kernel shared by several, and one that is a delay alone played as
// such; two sets at once while one glides to the other, each from the same spectra of what came in. Real-time safe once
// prepared.
#pragma once

#include "devices/dsp/Fft.h"

#include <array>
#include <complex>
#include <vector>

namespace phazite::devices::dsp
{
class PartitionedConvolution
{
public:
    using Complex = std::complex<float>;

    // The responses a set plays: each kernel's partitions' spectra (P + 1 points each, its taps' transform at 2 P points,
    // whose other half mirrors it), and which kernel plays from each side in to each side out.
    struct Kernels
    {
        enum class Kind
        {
            none,  // nothing: skipped
            delay, // a delay alone, of the partitions given to prepare(): what came in so many partitions before, as it is
            filter
        };

        std::array<Kind, 4> kinds {};
        std::array<std::vector<Complex>, 4> spectra {};

        // At [out * 2 + in], the kernel from side in to side out, or -1 for none. On a mono channel only the first plays.
        std::array<int, 4> routes { -1, -1, -1, -1 };

        // Whether its sides are the middle and the side, (left + right) / 2 and (left - right) / 2, taken apart from the left's
        // and the right's spectra: what it plays out is then the middle and the side, for its caller to put back together.
        bool midSide = false;
    };

    // For partitions of partitionSize samples (a power of two), kernels of so many partitions, and a delay kernel that is
    // so many partitions long. Allocates.
    void prepare (int partitionSize, int numPartitions, int delayPartitions);

    int getPartitionSize() const noexcept { return partitionSize; }
    int getNumPartitions() const noexcept { return numPartitions; }

    // Room for a set's kernels. Allocates.
    void allocate (Kernels&) const;

    // Silence in its history and in what it plays next; its partitions counted from here. The spectra of what came in before
    // are never read again, and nothing of them is written (the EQ's at 48 kHz are 0.25 MiB, 1 MiB at 192 kHz, and every Play
    // and every jump resets every device: ClearedSamples.h): it clears its last 2 P samples and what it plays next alone.
    void reset() noexcept;

    // The set it plays, and the one it glides to (nullptr for none); the part of a partition already under way plays
    // through it from here, worked out again from the spectra it holds.
    void setPlayed (const Kernels*) noexcept;
    void setGliding (const Kernels*) noexcept;
    bool isGliding() const noexcept { return gliding != nullptr; }

    // The set it glides to becomes the one it plays.
    void endGlide() noexcept;

    // Plays so many samples of one or two sides: in, out of what it plays, and out of what it glides to (only while it
    // glides). In and played may be the same.
    void process (const float* const* in, int numChannels, int length, float* const* played, float* const* glided) noexcept;

    // The real multiply-adds a set takes for each sample it plays, on so many channels.
    static int getMultiplyAddsPerSample (const Kernels&, int numChannels, int partitionSize, int numPartitions) noexcept;

    // Two partitions of real taps, the first at real and the second at imaginary, transformed together (2 P points) into
    // their spectra: each side's P + 1 points, taken apart.
    static void takeApart (const Complex* together, int partitionSize, Complex* first, Complex* second) noexcept;

private:
    int partitionSize = 0, numPartitions = 0, delayPartitions = 0, bins = 0;
    Fft fft;
    std::array<std::vector<float>, 2> frames;   // the last 2 P samples of each side
    std::vector<Complex> frame, spectrum;       // a side's frame, as complex numbers, and its transform
    std::array<std::vector<Complex>, 4> inputs; // the spectra (P + 1 points) of the left, the right, the middle and the side,
                                                // numPartitions of each, the newest at `newest`
    int newest = 0, position = 0, channelsIn = 2;
    int transformed = 0; // partitions transformed since the reset, up to numPartitions: the spectra of older ones are silence
    std::vector<Complex> sums, spectrumOut, timeOut; // each side's sum, and a side's whole spectrum for the inverse FFT
    std::array<std::array<std::vector<float>, 2>, 2> outputs;
    const Kernels* played = nullptr;
    const Kernels* gliding = nullptr;

    void transformPartition() noexcept;
    void convolve (const Kernels&, std::array<std::vector<float>, 2>& output) noexcept;
};
} // namespace phazite::devices::dsp
