// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Wavetable.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // Each level's harmonics: maxHarmonics / sqrt (2)^level, rounded down.
    constexpr std::array<int, Wavetable::numLevels> harmonicsAtLevel = { 1024, 724, 512, 362, 256, 181, 128, 90, 64, 45, 32,
                                                                         22,   16,  11,  8,   5,   4,   2,   2,  1,  1 };
    static_assert (harmonicsAtLevel[0] == Wavetable::maxHarmonics);

    constexpr int guardBefore = 1, guardAfter = 2; // the samples around each level's cycle, for interpolation

    // The samples a level keeps: at least so many for each cycle of its highest harmonic (four for a wavetable), so that
    // cubic interpolation reads it cleanly, as a power of two, and at least 64.
    int getLevelLength (int harmonics, int samplesPerCycle)
    {
        return std::max (64, juce::nextPowerOfTwo (samplesPerCycle * harmonics));
    }

    int getOrder (int size)
    {
        auto order = 0;

        while ((1 << order) < size)
            ++order;

        return order;
    }

    // FFTs of the sizes a table needs, made as they are asked for, each with the gains it gives either way, measured
    // once: FFT engines scale differently.
    class Transforms
    {
    public:
        struct Transform
        {
            explicit Transform (int order)
                : fft (order),
                  size (1 << order)
            {
                // Forward: a cosine of amplitude 1 at the first harmonic gives this.
                std::vector<float> data ((size_t) size * 2, 0.0f);

                for (int n = 0; n < size; ++n)
                    data[(size_t) n] = (float) std::cos (juce::MathConstants<double>::twoPi * n / size);

                fft.performRealOnlyForwardTransform (data.data(), true);
                forwardGain = data[2];

                // Inverse: the first harmonic alone, at 1, gives a cosine of this amplitude.
                std::fill (data.begin(), data.end(), 0.0f);
                data[2] = 1.0f;
                fft.performRealOnlyInverseTransform (data.data());
                inverseGain = data[0];
            }

            juce::dsp::FFT fft;
            int size = 0;
            float forwardGain = 1.0f, inverseGain = 1.0f;
        };

        Transform& get (int size)
        {
            const auto order = getOrder (size);

            for (auto& transform : made)
                if (transform->size == (1 << order))
                    return *transform;

            return *made.emplace_back (std::make_unique<Transform> (order));
        }

    private:
        std::vector<std::unique_ptr<Transform>> made;
    };

    // A frame's harmonics from a cycle of its samples, of any length: resampled to a power of two first if it isn't one.
    Wavetable::Harmonics analyse (std::span<const float> cycle, Transforms& transforms)
    {
        const auto length = (int) cycle.size();

        if (length < 2)
            return {};

        auto& transform = transforms.get (std::max (length, 4));
        const auto size = transform.size;
        std::vector<float> data ((size_t) size * 2, 0.0f);

        if (size == length)
        {
            std::copy (cycle.begin(), cycle.end(), data.begin());
        }
        else
        {
            // Around the cycle, with cubic interpolation.
            for (int n = 0; n < size; ++n)
            {
                const auto place = (double) n * length / size;
                const auto index = (int) place;
                const auto t = (float) (place - index);
                const auto at = [&cycle, length] (int i) { return cycle[(size_t) ((i % length + length) % length)]; };
                const auto a = at (index - 1), b = at (index), c = at (index + 1), d = at (index + 2);
                data[(size_t) n] = b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
            }
        }

        transform.fft.performRealOnlyForwardTransform (data.data(), true);

        // Below the transform's own Nyquist frequency, whose phase is lost.
        const auto count = std::min (Wavetable::maxHarmonics, size / 2 - 1);
        Wavetable::Harmonics harmonics ((size_t) count);

        for (int h = 1; h <= count; ++h)
            harmonics[(size_t) h - 1] = std::complex<float> (data[(size_t) h * 2], data[(size_t) h * 2 + 1]) / transform.forwardGain;

        return harmonics;
    }
} // namespace

std::shared_ptr<const Wavetable> Wavetable::fromHarmonics (const juce::String& tableName, const std::vector<Harmonics>& frames)
{
    return make (tableName, frames, true, 4);
}

std::shared_ptr<const Wavetable> Wavetable::forOscillator (const juce::String& tableName, const Harmonics& frame)
{
    return make (tableName, { frame }, false, 32);
}

std::shared_ptr<const Wavetable> Wavetable::make (const juce::String& tableName, const std::vector<Harmonics>& frames, bool peakAtOne,
                                                  int samplesPerCycle)
{
    std::shared_ptr<Wavetable> table (new Wavetable());
    table->name = tableName;
    table->numFrames = std::clamp ((int) frames.size(), 1, maxFrames);

    // Each frame's block: its levels one after another, each with its guards.
    auto offset = 0;

    for (int level = 0; level < numLevels; ++level)
    {
        table->levelLengths[(size_t) level] = getLevelLength (harmonicsAtLevel[(size_t) level], samplesPerCycle);
        table->levelOffsets[(size_t) level] = offset + guardBefore;
        offset += guardBefore + table->levelLengths[(size_t) level] + guardAfter;
    }

    table->frameStride = offset;
    table->samples.assign ((size_t) table->frameStride * (size_t) table->numFrames, 0.0f);

    Transforms transforms;
    std::vector<float> data;

    for (int frame = 0; frame < table->numFrames && frame < (int) frames.size(); ++frame)
    {
        const auto& harmonics = frames[(size_t) frame];

        for (int level = 0; level < numLevels; ++level)
        {
            const auto length = table->levelLengths[(size_t) level];
            const auto count = std::min (harmonicsAtLevel[(size_t) level], (int) harmonics.size());
            auto& transform = transforms.get (length);
            data.assign ((size_t) length * 2, 0.0f);

            for (int h = 1; h <= count; ++h)
            {
                const auto c = harmonics[(size_t) h - 1] / transform.inverseGain;
                data[(size_t) h * 2] = c.real();
                data[(size_t) h * 2 + 1] = c.imag();
            }

            transform.fft.performRealOnlyInverseTransform (data.data());

            auto* cycle = table->samples.data() + (size_t) frame * (size_t) table->frameStride + table->levelOffsets[(size_t) level];
            std::copy (data.begin(), data.begin() + length, cycle);
            cycle[-1] = cycle[length - 1];
            cycle[length] = cycle[0];
            cycle[length + 1] = cycle[1];
        }
    }

    // The loudest frame's peak at 1.
    auto scale = 1.0f;

    if (peakAtOne)
    {
        auto peak = 0.0f;

        for (int frame = 0; frame < table->numFrames; ++frame)
            for (const auto sample : table->getFrameForDrawing (frame))
                peak = std::max (peak, std::abs (sample));

        if (peak > 0.0f)
        {
            scale = 1.0f / peak;

            for (auto& sample : table->samples)
                sample /= peak;
        }
    }

    table->findAreasAndMeans (frames, scale);
    return table;
}

namespace
{
    // For each bend b that the bent means are kept at, and each harmonic h: G (h) = the integral from 0 to 1 of
    // e^(i 2 pi h p) b / (b - (b - 1) p)^2 dp. The mean of a wave read with its places bent, p (t) = b t / (1 + (b - 1) t), is
    // the integral over t of w (p (t)), which is the integral over p of w (p) / p' (t) = w (p) b / (b - (b - 1) p)^2: for a
    // wave that is the sum of Re (c_h e^(i 2 pi h p)), the sum of Re (c_h G (h)), exactly, for any level's harmonics. Each
    // bend's G at every harmonic from one FFT of the weight, by the trapezoids of 8192 samples of it (its ends together, as
    // e^(i 2 pi h) is 1). The weight's ends don't meet (from 1 / b to b), so each G is off by its aliases at whole multiples
    // of 8192 from h, about pi (b - 1 / b) h / (6 8192^2) each: summed over a saw's harmonics, at a bend of 8, under
    // 0.00005 of the wave's peak. Worked out once, the first time a table is made, off the audio thread (a few ms).
    struct BendWeights
    {
        static constexpr int stride = (Wavetable::numBends + 7) / 8 * 8; // the bends, padded for the vector registers

        // Harmonic by harmonic, from the first, each bend's G: its real part, and its imaginary part negated, so that
        // Re (c_h G (h)) is c_h's real part times the one plus its imaginary part times the other.
        std::vector<float> real = std::vector<float> ((size_t) (stride * Wavetable::maxHarmonics));
        std::vector<float> imagNegated = std::vector<float> ((size_t) (stride * Wavetable::maxHarmonics));
    };

    const BendWeights& getBendWeights()
    {
        static const BendWeights weights = []
        {
            constexpr int order = 13, size = 1 << order;
            static_assert (size >= 4 * Wavetable::maxHarmonics);
            BendWeights made;
            juce::dsp::FFT fft (order);
            std::vector<float> data ((size_t) size * 2);

            for (int index = 0; index < Wavetable::numBends; ++index)
            {
                const auto bend = std::exp2 ((double) (index - (Wavetable::numBends - 1) / 2) / Wavetable::bendsPerOctave);
                const auto weight = [bend] (double p) { return bend / ((bend - (bend - 1.0) * p) * (bend - (bend - 1.0) * p)); };
                std::fill (data.begin(), data.end(), 0.0f);
                data[0] = (float) (0.5 * (weight (0.0) + weight (1.0)));

                for (int n = 1; n < size; ++n)
                    data[(size_t) n] = (float) weight ((double) n / size);

                // Its forward transform is the sum of x_n e^(-i 2 pi h n / N): G (h) is its conjugate, over N.
                fft.performRealOnlyForwardTransform (data.data(), true);

                for (int h = 1; h <= Wavetable::maxHarmonics; ++h)
                {
                    const auto at = (size_t) ((h - 1) * BendWeights::stride + index);
                    made.real[at] = data[(size_t) h * 2] / (float) size;
                    made.imagNegated[at] = data[(size_t) h * 2 + 1] / (float) size;
                }
            }

            return made;
        }();

        return weights;
    }

} // namespace

void Wavetable::findAreasAndMeans (const std::vector<Harmonics>& frames, float scale)
{
    static_assert (areaPoints == 128);
    constexpr int stride = areaPoints + 1;
    areas.assign ((size_t) (numFrames * numLevels * stride), 0.0f);
    bentMeans.assign ((size_t) (numFrames * numLevels * numBends), 0.0f);
    const auto& weights = getBendWeights();
    juce::dsp::FFT areaTransform (7);
    std::array<std::complex<float>, areaPoints> folded {}, atPoints {};

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const auto* harmonics = frame < (int) frames.size() ? frames[(size_t) frame].data() : nullptr;
        const auto count = harmonics != nullptr ? std::min ((int) frames[(size_t) frame].size(), maxHarmonics) : 0;

        // Its levels' areas, from its harmonics: for a wave that is the sum of Re (c_h e^(i 2 pi h t)), the area to x is the
        // sum of Re (d_h (e^(i 2 pi h x) - 1)) with d_h = c_h / (i 2 pi h), exactly. At the places j / areaPoints, e^(i 2 pi h x)
        // repeats as h goes up by areaPoints, so the d_h fold onto areaPoints of them and one transform of that many gives all
        // the places (the forward one of their conjugates, whose real part is the same): each level from the harmonics it
        // keeps, from the fewest, folded in turn.
        folded.fill ({});
        auto folds = 0;

        for (int level = numLevels - 1; level >= 0; --level)
        {
            auto* area = areas.data() + (size_t) ((frame * numLevels + level) * stride);
            const auto upTo = std::min (harmonicsAtLevel[(size_t) level], count);

            if (upTo == folds && level < numLevels - 1)
            {
                std::copy (area + stride, area + 2 * stride, area); // the same harmonics as the level after it
                continue;
            }

            for (; folds < upTo; ++folds)
            {
                const auto h = folds + 1;
                // c / (i a), written out: the compiler's division of complex numbers warns of a divisor it can't see is not
                // zero (C4723, in an optimised build), and a is 2 pi h, with h from 1.
                const auto a = juce::MathConstants<float>::twoPi * (float) h;
                const std::complex<float> d (harmonics[folds].imag() / a, -harmonics[folds].real() / a);
                folded[(size_t) (h % areaPoints)] += std::conj (d);
            }

            areaTransform.perform (folded.data(), atPoints.data(), false);

            for (int point = 0; point < areaPoints; ++point)
                area[point] = (atPoints[(size_t) point].real() - atPoints[0].real()) * scale;

            area[areaPoints] = 0.0f; // the whole cycle's
        }

        // Its bent means, from its harmonics (getBendWeights()): each level's the sum over the harmonics it keeps, which
        // the levels take from the first in turn, so that one running sum for each bend gives them all, from the fewest.
        // Harmonic by harmonic, every bend at once, which the compiler does in vector registers.
        std::array<float, BendWeights::stride> sums {};
        auto done = 0;

        for (int level = numLevels - 1; level >= 0; --level)
        {
            for (const auto upTo = std::min (harmonicsAtLevel[(size_t) level], count); done < upTo; ++done)
            {
                const auto re = harmonics[done].real(), im = harmonics[done].imag();
                const auto* real = weights.real.data() + (size_t) (done * BendWeights::stride);
                const auto* imagNegated = weights.imagNegated.data() + (size_t) (done * BendWeights::stride);

                for (size_t index = 0; index < sums.size(); ++index)
                    sums[index] += re * real[index] + im * imagNegated[index];
            }

            auto* means = bentMeans.data() + (size_t) ((frame * numLevels + level) * numBends);

            for (int index = 0; index < numBends; ++index)
                means[index] = sums[(size_t) index] * scale;
        }
    }
}

float Wavetable::getArea (int frame, int level, float place) const noexcept
{
    if (areas.empty())
        return 0.0f;

    frame = std::clamp (frame, 0, numFrames - 1);
    level = std::clamp (level, 0, numLevels - 1);
    const auto at = std::clamp (place, 0.0f, 1.0f) * (float) areaPoints;
    const auto index = std::min ((int) at, areaPoints - 1);
    const auto* area = areas.data() + (size_t) ((frame * numLevels + level) * (areaPoints + 1) + index);
    return area[0] + (at - (float) index) * (area[1] - area[0]);
}

Wavetable::Bend Wavetable::findBend (float bend) noexcept
{
    const auto at = std::clamp ((std::log2 (std::max (bend, 1.0e-6f)) + 3.0f) * (float) bendsPerOctave, 0.0f, (float) (numBends - 1));
    const auto index = std::min ((int) at, numBends - 2);
    return { index, at - (float) index };
}

float Wavetable::getBentMean (int frame, int level, Bend bend) const noexcept
{
    if (bentMeans.empty())
        return 0.0f;

    frame = std::clamp (frame, 0, numFrames - 1);
    level = std::clamp (level, 0, numLevels - 1);
    const auto* mean = bentMeans.data() + (size_t) ((frame * numLevels + level) * numBends + bend.index);
    return mean[0] + bend.fraction * (mean[1] - mean[0]);
}

std::shared_ptr<const Wavetable> Wavetable::fromFrames (const juce::String& tableName, const std::vector<std::vector<float>>& frames)
{
    Transforms transforms;
    std::vector<Harmonics> harmonics;

    for (const auto& frame : frames)
    {
        if ((int) harmonics.size() == maxFrames)
            break;

        harmonics.push_back (analyse (frame, transforms));
    }

    return fromHarmonics (tableName, harmonics);
}

std::shared_ptr<const Wavetable> Wavetable::fromSound (const juce::String& tableName, std::span<const float> sound, int samplesPerFrame)
{
    const auto length = (int) sound.size();
    auto perFrame = samplesPerFrame;

    if (perFrame <= 1 || perFrame > length)
    {
        if (length > 0 && length % frameSize == 0)
            perFrame = frameSize;
        else if (length < 2 * frameSize)
            perFrame = length; // a single cycle
        else
            perFrame = frameSize;
    }

    std::vector<std::vector<float>> frames;

    for (int start = 0; perFrame > 0 && start + perFrame <= length && (int) frames.size() < maxFrames; start += perFrame)
        frames.emplace_back (sound.begin() + start, sound.begin() + start + perFrame);

    return fromFrames (tableName, frames);
}

std::span<const float> Wavetable::getFrameForDrawing (int frame) const noexcept
{
    const auto level = getLevel (frame, 0);
    return { level.samples, (size_t) level.length };
}

Wavetable::Level Wavetable::getLevel (int frame, int level) const noexcept
{
    frame = std::clamp (frame, 0, numFrames - 1);
    level = std::clamp (level, 0, numLevels - 1);
    return { samples.data() + (size_t) frame * (size_t) frameStride + levelOffsets[(size_t) level], levelLengths[(size_t) level] };
}

int Wavetable::getHarmonics (int level) noexcept
{
    return harmonicsAtLevel[(size_t) std::clamp (level, 0, numLevels - 1)];
}

int Wavetable::chooseLevel (double highestHarmonic) noexcept
{
    for (int level = 0; level < numLevels - 1; ++level)
        if ((double) harmonicsAtLevel[(size_t) level] <= highestHarmonic)
            return level;

    return numLevels - 1;
}
} // namespace phazite::devices
