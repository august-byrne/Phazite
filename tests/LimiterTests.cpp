// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's limiter (src/devices/dynamics/Limiter.h): that it leaves quiet sound alone but for its delay, keeps loud
// sound under its ceiling between samples too, lets go at its release time, and raises the sound by its gain.
#include "TestHelpers.h"
#include "devices/dynamics/Limiter.h"

#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Limiter;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 480;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Limiter::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

float toDecibels (float gain)
{
    return 20.0f * std::log10 (std::max (gain, 1.0e-9f));
}

// Plays the limiter in blocks of 10 ms and returns its left channel; input (time in samples) gives both channels.
// After each block, block (its index) is told how far the limiter turned the sound down in it, in decibels.
template <typename Input, typename Block>
std::vector<float> play (Limiter& limiter, const std::vector<float>& values, int numSamples, Input input, Block block)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    std::vector<float> played;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, input (done + i));

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = values;
        limiter.process (context);
        block (done / testBlockSize, limiter.getReductionMeter().take());

        for (int i = 0; i < length; ++i)
            played.push_back (buffer.getSample (0, i));
    }

    return played;
}

template <typename Input>
std::vector<float> play (Limiter& limiter, const std::vector<float>& values, int numSamples, Input input)
{
    return play (limiter, values, numSamples, input, [] (int, float) {});
}

// The true peak of a sound, in decibels: its highest point, looked for between its samples too (at quarters, as
// ITU-R BS.1770 asks), with a much longer interpolation than the limiter's own.
float getTruePeak (const std::vector<float>& sound, size_t from)
{
    constexpr int halfWidth = 32;
    auto peak = 0.0;

    for (auto k = std::max (from, (size_t) halfWidth); k + halfWidth < sound.size(); ++k)
    {
        for (const auto fraction : { 0.0, 0.25, 0.5, 0.75 })
        {
            auto point = 0.0;

            for (int m = -halfWidth + 1; m <= halfWidth; ++m)
            {
                const auto distance = m - fraction;
                const auto sinc = std::abs (distance) < 1.0e-9 ? 1.0
                                                               : std::sin (juce::MathConstants<double>::pi * distance)
                                                                     / (juce::MathConstants<double>::pi * distance);
                const auto window = 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * distance / halfWidth)
                                    + 0.08 * std::cos (twoPi * distance / halfWidth); // Blackman
                point += sound[(size_t) ((int) k + m)] * sinc * window;
            }

            peak = std::max (peak, std::abs (point));
        }
    }

    return toDecibels ((float) peak);
}

float getSamplePeak (const std::vector<float>& sound, size_t from)
{
    auto peak = 0.0f;

    for (auto i = from; i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}
} // namespace

TEST_CASE ("The limiter's description holds together, and it reports its look-ahead as latency")
{
    REQUIRE (devices::isValid (Limiter::info));
    CHECK (devices::findParameter (Limiter::info, "ceiling") == Limiter::ceiling);
    CHECK (devices::findParameter (Limiter::info, "mix") == Limiter::mix);

    Limiter limiter;
    limiter.prepare (testRate, testBlockSize);
    CHECK (limiter.getLatencySamples() == 96 + 3); // 2 ms, and half its interpolation
}

TEST_CASE ("The limiter leaves sound under its ceiling as it was, only later")
{
    Limiter limiter;
    limiter.prepare (testRate, testBlockSize);
    const auto latency = limiter.getLatencySamples();
    const auto sine = [] (int t) { return (float) (0.25 * std::sin (twoPi * 1000.0 * t / testRate)); };
    const auto played = play (limiter, getDefaults(), 9600, sine);

    auto differs = 0.0f;

    for (int t = latency; t < (int) played.size(); ++t)
        differs = std::max (differs, std::abs (played[(size_t) t] - sine (t - latency)));

    CHECK (differs < 1.0e-6f);
}

TEST_CASE ("The limiter keeps loud sound under its ceiling, between samples too")
{
    auto values = getDefaults();
    values[Limiter::gain] = 12.0f;

    SUBCASE ("A full-scale sine pushed 12 dB")
    {
        Limiter limiter;
        limiter.prepare (testRate, testBlockSize);
        auto reduction = 0.0f;
        const auto played = play (
            limiter, values, 48000, [] (int t) { return (float) std::sin (twoPi * 997.0 * t / testRate); },
            [&reduction] (int, float most) { reduction = most; });

        CHECK (getSamplePeak (played, 0) <= juce::Decibels::decibelsToGain (-1.0f) * 1.000001f);
        CHECK (getTruePeak (played, 4800) < -1.0f + 0.2f);
        CHECK (getTruePeak (played, 4800) > -1.0f - 0.5f); // not much more than it must
        CHECK (reduction == doctest::Approx (13.0f).epsilon (0.03));
    }

    SUBCASE ("A sine whose peaks all fall between its samples: a quarter of the sample rate, 45 degrees on")
    {
        values[Limiter::gain] = 0.0f;
        Limiter limiter;
        limiter.prepare (testRate, testBlockSize);
        const auto played = play (limiter, values, 48000,
                                  [] (int t) { return (float) std::sin (twoPi * 0.25 * t + juce::MathConstants<double>::pi / 4.0); });

        // Its samples are 3 dB under its true peak of 0 dB, under the ceiling, but its true peak isn't.
        CHECK (getTruePeak (played, 4800) < -1.0f + 0.2f);
        CHECK (getTruePeak (played, 4800) > -1.0f - 0.5f);
    }
}

TEST_CASE ("The limiter lets go at its release time, and its gain raises quiet sound")
{
    auto values = getDefaults();
    values[Limiter::gain] = 12.0f;
    Limiter limiter;
    limiter.prepare (testRate, testBlockSize);

    // Pushed 13 dB over the ceiling for a fifth of a second, then quiet: 100 ms later, the gain has come 63 % of the
    // way back up (counted as a gain, not in decibels).
    std::vector<float> reductions;
    const auto burst = 9600;
    play (
        limiter, values, 48000,
        [burst] (int t) { return t < burst ? (float) std::sin (twoPi * 997.0 * t / testRate) : (t % 2 == 0 ? 0.01f : -0.01f); },
        [&reductions] (int, float most) { reductions.push_back (most); });

    const auto blocksAfter = [burst] (double seconds) { return (size_t) ((burst + seconds * testRate) / testBlockSize); };
    const auto expected = 1.0 - (1.0 - std::pow (10.0, -13.0 / 20.0)) * std::exp (-1.0);
    CHECK (reductions[blocksAfter (0.1)] == doctest::Approx (-20.0 * std::log10 (expected)).epsilon (0.15));
    CHECK (reductions.back() < 0.01f);

    // A quiet sine, 6 dB up, comes out 6 dB louder once the gain has settled.
    values[Limiter::gain] = 6.0f;
    Limiter quiet;
    quiet.prepare (testRate, testBlockSize);
    const auto played = play (quiet, values, 9600, [] (int t) { return (float) (0.1 * std::sin (twoPi * 1000.0 * t / testRate)); });
    CHECK (toDecibels (getSamplePeak (played, 4800)) == doctest::Approx (-20.0f + 6.0f).epsilon (0.005));
}
