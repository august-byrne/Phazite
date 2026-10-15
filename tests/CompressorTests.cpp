// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (src/devices/dynamics/Compressor.h): its curve, as it reports it and as it plays, its attack and
// release times, makeup gain, sidechain, stereo link, low cut and meters.
#include "TestHelpers.h"
#include "devices/dynamics/Compressor.h"

#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Compressor;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// Settings that make its timing easy to read: a hard knee, 20 dB under -6 dB, a quick attack and makeup at 0 dB.
std::vector<float> getTestValues()
{
    auto values = getDefaults();
    values[Compressor::threshold] = -26.0f;
    values[Compressor::ratio] = 4.0f;
    values[Compressor::knee] = 0.0f;
    values[Compressor::attack] = 1.0f;
    values[Compressor::release] = 50.0f;
    return values;
}

float toGain (float decibels)
{
    return std::pow (10.0f, decibels / 20.0f);
}

// A steady level for the detector: the highest frequency there is, which its low cut lets through whole.
float steady (int time, float decibels)
{
    return time % 2 == 0 ? toGain (decibels) : -toGain (decibels);
}

// Plays the compressor in blocks and returns what comes out of each channel: input (time in samples, channel) gives
// the sound and, when given, sidechain gives the sidechain's.
template <typename Input, typename Sidechain = std::nullptr_t>
std::array<std::vector<float>, 2> play (Compressor& compressor, const std::vector<float>& values, int numSamples, Input input,
                                        [[maybe_unused]] Sidechain sidechain = nullptr)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize), side (2, testBlockSize);
    std::array<std::vector<float>, 2> played;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < length; ++i)
            {
                buffer.setSample (channel, i, input (done + i, channel));

                if constexpr (! std::is_null_pointer_v<Sidechain>)
                    side.setSample (channel, i, sidechain (done + i, channel));
            }
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        [[maybe_unused]] const float* sides[] = { side.getReadPointer (0), side.getReadPointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;

        if constexpr (! std::is_null_pointer_v<Sidechain>)
        {
            block.sidechain = sides;
            block.numSidechainChannels = 2;
        }

        compressor.process (block);

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < length; ++i)
                played[(size_t) channel].push_back (buffer.getSample (channel, i));
    }

    return played;
}

// The gain at a time, in decibels, for a steady input at a level.
float gainAt (const std::vector<float>& played, int time, float inputDecibels)
{
    return 20.0f * std::log10 (std::abs (played[(size_t) time]) / toGain (inputDecibels));
}
} // namespace

TEST_CASE ("The compressor's description holds together")
{
    REQUIRE (devices::isValid (Compressor::info));
    CHECK (Compressor::info.parameters.size() == (size_t) Compressor::mix + 1);
    CHECK (devices::findParameter (Compressor::info, "threshold") == Compressor::threshold);
    CHECK (devices::findParameter (Compressor::info, "sidechainLowCut") == Compressor::sidechainLowCut);
    CHECK (devices::findParameter (Compressor::info, "mix") == Compressor::mix); // which its thin plug-in applies
}

TEST_CASE ("The compressor turns levels over the threshold down by its ratio, easing in across the knee")
{
    auto values = getDefaults();
    values[Compressor::threshold] = -20.0f;
    values[Compressor::ratio] = 4.0f;
    values[Compressor::knee] = 0.0f;

    CHECK (Compressor::getReduction (values, -30.0f) == doctest::Approx (0.0f));
    CHECK (Compressor::getReduction (values, -20.0f) == doctest::Approx (0.0f));
    CHECK (Compressor::getReduction (values, -8.0f) == doctest::Approx (9.0f)); // 12 dB over comes out 3 dB over

    // A 12 dB knee: from 6 dB under the threshold to 6 dB over, where it meets the line.
    values[Compressor::knee] = 12.0f;
    CHECK (Compressor::getReduction (values, -26.0f) == doctest::Approx (0.0f));
    CHECK (Compressor::getReduction (values, -20.0f) == doctest::Approx (0.75f * 12.0f / 8.0f));
    CHECK (Compressor::getReduction (values, -14.0f) == doctest::Approx (0.75f * 6.0f));
    CHECK (Compressor::getReduction (values, -8.0f) == doctest::Approx (9.0f));

    // It never turns a louder level down less, nor bends more steeply than the ratio.
    auto last = 0.0f;

    for (auto level = -40.0f; level <= 0.0f; level += 0.25f)
    {
        const auto reduction = Compressor::getReduction (values, level);
        CHECK (reduction >= last);
        CHECK (reduction - last <= 0.75f * 0.25f + 1.0e-4f);
        last = reduction;
    }

    values[Compressor::ratio] = 1.0f;
    CHECK (Compressor::getReduction (values, 0.0f) == doctest::Approx (0.0f));

    // As it plays, once settled: its curve, with a soft knee.
    values = getTestValues();
    values[Compressor::knee] = 6.0f;

    for (const auto level : { -40.0f, -29.0f, -26.0f, -24.0f, -12.0f, 0.0f })
    {
        CAPTURE (level);
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played = play (compressor, values, 12000, [level] (int t, int) { return steady (t, level); });

        for (const auto& channel : played)
            CHECK (gainAt (channel, 11999, level) == doctest::Approx (-Compressor::getReduction (values, level)).epsilon (0.002));
    }
}

TEST_CASE ("The compressor's attack and release take their times")
{
    auto values = getTestValues();
    values[Compressor::attack] = 10.0f;
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);

    // From silence to 20 dB over the threshold, which settles 15 dB down: after the attack time it has gone 63 % of the
    // way, as an analog compressor's time constant does.
    const auto onset = 4800;
    auto played = play (compressor, values, onset + 9600, [] (int t, int) { return t < onset ? 0.0f : steady (t, -6.0f); });
    const auto attackSamples = (int) (0.010 * testRate);
    CHECK (gainAt (played[0], onset + attackSamples - 1, -6.0f) == doctest::Approx (-15.0f * (1.0f - std::exp (-1.0f))).epsilon (0.01));
    CHECK (gainAt (played[0], onset + 9599, -6.0f) == doctest::Approx (-15.0f).epsilon (0.001));

    // Down to 20 dB under the threshold, which isn't turned down: after the release time 37 % of the reduction is left.
    played = play (compressor, values, 4800, [] (int t, int) { return steady (t, -46.0f); });
    const auto releaseSamples = (int) (0.050 * testRate);
    CHECK (gainAt (played[0], releaseSamples - 1, -46.0f) == doctest::Approx (-15.0f * std::exp (-1.0f)).epsilon (0.01));

    // reset() lets go of what it was doing.
    play (compressor, values, 4800, [] (int t, int) { return steady (t, -6.0f); });
    compressor.reset();
    played = play (compressor, values, 1, [] (int t, int) { return steady (t, -46.0f); });
    CHECK (std::abs (gainAt (played[0], 0, -46.0f)) < 0.01f);
}

TEST_CASE ("The compressor's makeup gain moves smoothly")
{
    auto values = getTestValues();
    values[Compressor::makeup] = 6.0f;
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);

    // A new compressor starts at its makeup gain, and stays there under the threshold.
    auto played = play (compressor, values, 4800, [] (int t, int) { return steady (t, -46.0f); });
    CHECK (gainAt (played[0], 0, -46.0f) == doctest::Approx (6.0f).epsilon (0.001));
    CHECK (gainAt (played[0], 4799, -46.0f) == doctest::Approx (6.0f).epsilon (0.001));

    // A change glides rather than jumps.
    values[Compressor::makeup] = 12.0f;
    played = play (compressor, values, 9600, [] (int t, int) { return steady (t, -46.0f); });
    CHECK (gainAt (played[0], 0, -46.0f) < 6.1f);
    CHECK (gainAt (played[0], 9599, -46.0f) == doctest::Approx (12.0f).epsilon (0.001));
}

TEST_CASE ("The compressor listens to its sidechain when one feeds it, and to both channels alike")
{
    const auto values = getTestValues();

    SUBCASE ("Ducking: a quiet sound turned down by a loud sidechain")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played =
            play (compressor, values, 12000, [] (int t, int) { return steady (t, -46.0f); }, [] (int t, int) { return steady (t, -6.0f); });

        for (const auto& channel : played)
            CHECK (gainAt (channel, 11999, -46.0f) == doctest::Approx (-15.0f).epsilon (0.001));
    }

    SUBCASE ("A loud sound with a silent sidechain isn't turned down")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played = play (compressor, values, 12000, [] (int t, int) { return steady (t, -6.0f); }, [] (int, int) { return 0.0f; });
        CHECK (std::abs (gainAt (played[0], 11999, -6.0f)) < 0.001f);
    }

    SUBCASE ("Loud on the left only: both sides go down alike")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played = play (compressor, values, 12000, [] (int t, int side) { return steady (t, side == 0 ? -6.0f : -46.0f); });
        CHECK (gainAt (played[0], 11999, -6.0f) == doctest::Approx (-15.0f).epsilon (0.001));
        CHECK (gainAt (played[1], 11999, -46.0f) == doctest::Approx (-15.0f).epsilon (0.001));
    }
}

TEST_CASE ("The compressor's low cut keeps bass from pushing it, and its meters show what it did")
{
    auto values = getTestValues();
    const auto bass = [] (int t, int) { return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 30.0 * t / testRate)); };

    // The most it turned a 30 Hz sine down by, over its last half second.
    const auto mostReduction = [&] (float lowCut)
    {
        values[Compressor::sidechainLowCut] = lowCut;
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        play (compressor, values, 24000, bass);
        compressor.getReductionMeter().take();
        play (compressor, values, 24000, bass);
        return compressor.getReductionMeter().take();
    };

    CHECK (mostReduction (20.0f) > 10.0f);
    CHECK (mostReduction (300.0f) < 0.1f);

    // The meters: how loud the detector heard it and how far it went down, until taken. (Their first readings hold the
    // low cut's overshoot as the sound starts.)
    values = getTestValues();
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    play (compressor, values, 12000, [] (int t, int) { return steady (t, -6.0f); });
    CHECK (compressor.getLevelMeter().take() > -6.1f);
    compressor.getReductionMeter().take();
    play (compressor, values, 4800, [] (int t, int) { return steady (t, -6.0f); });
    CHECK (compressor.getLevelMeter().take() == doctest::Approx (-6.0f).epsilon (0.001));
    CHECK (compressor.getReductionMeter().take() == doctest::Approx (15.0f).epsilon (0.001));
    CHECK (compressor.getLevelMeter().take() == doctest::Approx (-120.0f));
    CHECK (compressor.getReductionMeter().take() == doctest::Approx (0.0f));
}
