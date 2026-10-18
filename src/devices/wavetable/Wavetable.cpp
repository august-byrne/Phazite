// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Wavetable.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
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

    // The samples a level keeps: at least four for each cycle of its highest harmonic, so that cubic interpolation reads
    // it cleanly, as a power of two, and at least 64.
    int getLevelLength (int harmonics)
    {
        return std::max (64, juce::nextPowerOfTwo (4 * harmonics));
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
    std::shared_ptr<Wavetable> table (new Wavetable());
    table->name = tableName;
    table->numFrames = std::clamp ((int) frames.size(), 1, maxFrames);

    // Each frame's block: its levels one after another, each with its guards.
    auto offset = 0;

    for (int level = 0; level < numLevels; ++level)
    {
        table->levelLengths[(size_t) level] = getLevelLength (harmonicsAtLevel[(size_t) level]);
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
    auto peak = 0.0f;

    for (int frame = 0; frame < table->numFrames; ++frame)
        for (const auto sample : table->getFrameForDrawing (frame))
            peak = std::max (peak, std::abs (sample));

    if (peak > 0.0f)
        for (auto& sample : table->samples)
            sample /= peak;

    return table;
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
