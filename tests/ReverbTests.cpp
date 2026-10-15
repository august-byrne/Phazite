// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's reverb (src/devices/reverb/Reverb.h): its decay time, damping, pre-delay, width and low cut, its level, and
// that it plays the same every time.
#include "TestHelpers.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Random.h"
#include "devices/reverb/Reverb.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Reverb;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 512;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Reverb::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

struct Stereo
{
    std::vector<float> left, right;
};

// Plays a new reverb in blocks; input (time in samples) gives both channels.
template <typename Input>
Stereo play (const std::vector<float>& values, int numSamples, Input input)
{
    Reverb reverb;
    reverb.prepare (testRate, testBlockSize);
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    Stereo played;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, input (done + i));

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        reverb.process (block);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

float impulse (int t)
{
    return t == 0 ? 1.0f : 0.0f;
}

// The time it takes to fade by 60 dB, from the slope of its energy decay curve between -5 and -35 dB (Schroeder's
// backward integration, as ISO 3382 measures a room's T30).
double measureDecay (const Stereo& response)
{
    std::vector<double> remaining (response.left.size() + 1, 0.0);

    for (auto i = response.left.size(); i-- > 0;)
        remaining[i] = remaining[i + 1] + (double) response.left[i] * response.left[i] + (double) response.right[i] * response.right[i];

    const auto timeAt = [&remaining] (double decibels)
    {
        const auto level = remaining[0] * std::pow (10.0, decibels / 10.0);

        for (size_t at = 0; at < remaining.size(); ++at)
            if (remaining[at] <= level)
                return (double) at / testRate;

        return (double) remaining.size() / testRate;
    };

    return 2.0 * (timeAt (-35.0) - timeAt (-5.0));
}

double energyOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto energy = 0.0;

    for (auto i = from; i < to && i < sound.size(); ++i)
        energy += (double) sound[i] * sound[i];

    return energy;
}

// The sound through a filter of a shape, at a frequency.
std::vector<float> filtered (const std::vector<float>& sound, phazite::devices::dsp::FilterShape shape, double frequency)
{
    const auto coefficients = phazite::devices::dsp::makeFilter (shape, frequency, testRate, 0.70710678);
    phazite::devices::dsp::Filter filter;
    std::vector<float> out;

    for (const auto sample : sound)
        out.push_back (filter.process (sample, coefficients));

    return out;
}
} // namespace

TEST_CASE ("The reverb's description holds together, and its tail lasts its decay")
{
    REQUIRE (devices::isValid (Reverb::info));
    CHECK (devices::findParameter (Reverb::info, "decay") == Reverb::decay);
    CHECK (devices::findParameter (Reverb::info, "mix") == Reverb::mix);
    CHECK (Reverb::info.parameters[Reverb::mix].defaultValue == doctest::Approx (0.25f)); // on an insert

    auto values = getDefaults();
    values[Reverb::decay] = 3.0f;
    values[Reverb::predelay] = 50.0f;
    Reverb reverb;
    reverb.prepare (testRate, testBlockSize);
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = testBlockSize;
    block.parameters = values;
    reverb.process (block);
    CHECK (reverb.getTailSeconds() == doctest::Approx (3.05));
}

TEST_CASE ("The reverb fades by 60 dB over its decay time")
{
    auto values = getDefaults();
    values[Reverb::damping] = 20000.0f; // the treble fades about as fast as the rest
    values[Reverb::lowCut] = 20.0f;

    for (const auto decay : { 1.0f, 3.0f })
    {
        CAPTURE (decay);
        values[Reverb::decay] = decay;
        const auto measured = measureDecay (play (values, (int) (testRate * decay * 1.5), impulse));
        CHECK (measured == doctest::Approx (decay).epsilon (0.2));
    }
}

TEST_CASE ("The reverb's damping makes the treble fade sooner")
{
    auto values = getDefaults();
    values[Reverb::decay] = 2.0f;

    // How much quieter the treble is than the bass in the tail, from a third of a second to two thirds, in decibels.
    const auto trebleUnderBass = [&values] (float damping)
    {
        values[Reverb::damping] = damping;
        const auto tail = play (values, (int) testRate, impulse);
        const auto from = (size_t) (testRate / 3), to = (size_t) (2 * testRate / 3);
        const auto bass = energyOf (filtered (tail.left, phazite::devices::dsp::FilterShape::lowPass, 1000.0), from, to);
        const auto treble = energyOf (filtered (tail.left, phazite::devices::dsp::FilterShape::highPass, 4000.0), from, to);
        return 10.0 * std::log10 (bass / treble);
    };

    CHECK (trebleUnderBass (2000.0f) > trebleUnderBass (20000.0f) + 10.0);
}

TEST_CASE ("The reverb waits its pre-delay, plays the same every time, and spreads as wide as its width")
{
    auto values = getDefaults();
    values[Reverb::predelay] = 100.0f;

    // Nothing before the pre-delay, then the tail.
    const auto response = play (values, (int) testRate, impulse);
    const auto first = std::find_if (response.left.begin(), response.left.end(), [] (float sample) { return std::abs (sample) > 1.0e-5f; });
    const auto firstTime = (double) std::distance (response.left.begin(), first) / testRate;
    CHECK (firstTime >= 0.1);
    CHECK (firstTime < 0.2);

    // The same sound twice, bit for bit, from a burst of noise.
    phazite::devices::dsp::Random random (7);
    std::vector<float> burst (4800);

    for (auto& sample : burst)
        sample = 0.5f * random.nextBipolar();

    const auto noise = [&burst] (int t) { return t < (int) burst.size() ? burst[(size_t) t] : 0.0f; };

    const auto once = play (values, 48000, noise), twice = play (values, 48000, noise);
    CHECK (once.left == twice.left);
    CHECK (once.right == twice.right);

    // Wide, the sides differ; at no width, they are the same.
    const auto correlation = [] (const Stereo& sound)
    {
        auto both = 0.0, left = 0.0, right = 0.0;

        for (size_t i = 4800; i < sound.left.size(); ++i)
        {
            both += (double) sound.left[i] * sound.right[i];
            left += (double) sound.left[i] * sound.left[i];
            right += (double) sound.right[i] * sound.right[i];
        }

        return both / std::sqrt (left * right);
    };

    CHECK (std::abs (correlation (once)) < 0.5);
    values[Reverb::width] = 0.0f;
    const auto narrow = play (values, 48000, noise);
    CHECK (narrow.left == narrow.right);
}

TEST_CASE ("The reverb's tail sits a little under the sound going in, without rumble below its low cut")
{
    auto values = getDefaults();

    // Steady noise: at the default 2 s, the tail settles about 6 dB under the noise.
    phazite::devices::dsp::Random random (3);
    std::vector<float> noise ((size_t) testRate * 2);

    for (auto& sample : noise)
        sample = 0.25f * random.nextBipolar();

    const auto steady = play (values, (int) noise.size(), [&noise] (int t) { return noise[(size_t) t]; });
    const auto from = noise.size() / 2;
    const auto ratio = 10.0 * std::log10 (energyOf (steady.left, from, noise.size()) / energyOf (noise, from, noise.size()));
    CHECK (ratio > -9.0);
    CHECK (ratio < -3.0);

    // A 30 Hz hum mostly stays out of the tail with the low cut at 300 Hz.
    const auto hum = [] (int t) { return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 30.0 * t / testRate)); };
    values[Reverb::lowCut] = 20.0f;
    const auto open = play (values, (int) testRate, hum);
    values[Reverb::lowCut] = 300.0f;
    const auto cut = play (values, (int) testRate, hum);
    const auto half = (size_t) testRate / 2;
    CHECK (10.0 * std::log10 (energyOf (open.left, half, open.left.size()) / energyOf (cut.left, half, cut.left.size())) > 20.0);
}

TEST_CASE ("The reverb reports the bass's decay as its decay, and a shorter one for the treble it damps")
{
    auto values = getDefaults();
    values[Reverb::decay] = 3.0f;
    values[Reverb::damping] = 2000.0f;
    CHECK (Reverb::getDecayAt (values, 50.0, testRate) == doctest::Approx (3.0).epsilon (0.01));
    CHECK (Reverb::getDecayAt (values, 8000.0, testRate) < 0.5 * 3.0);

    // Even at its brightest, the damping takes a little treble on each pass.
    const auto damped = Reverb::getDecayAt (values, 8000.0, testRate);
    values[Reverb::damping] = 20000.0f;
    CHECK (Reverb::getDecayAt (values, 8000.0, testRate) > 0.6 * 3.0);
    CHECK (Reverb::getDecayAt (values, 8000.0, testRate) > 3.0 * damped);
}
