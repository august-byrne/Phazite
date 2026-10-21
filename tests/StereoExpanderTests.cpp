// SPDX-License-Identifier: AGPL-3.0-or-later
// The stereo expander's DSP (src/devices/stereo/StereoExpander.h): nothing changed at its defaults, width through mid and
// side, a spread that decorrelates without touching the mono sum, the bass kept in mono, polarity and gain, and how alike
// the two sides are, coming in and going out.
#include "TestHelpers.h"
#include "devices/stereo/StereoExpander.h"

#include <vector>

namespace
{
using phazite::devices::StereoExpander;

constexpr double rate = 48000.0;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : StereoExpander::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, const char* id, float value)
{
    values[(size_t) phazite::devices::findParameter (StereoExpander::info, id)] = value;
}

// Plays a buffer through a fresh expander, in blocks of 512.
juce::AudioBuffer<float> play (const juce::AudioBuffer<float>& in, const std::vector<float>& values, StereoExpander* kept = nullptr)
{
    StereoExpander own;
    auto& expander = kept != nullptr ? *kept : own;
    expander.prepare (rate, 512);
    auto out = in;

    for (int start = 0; start < out.getNumSamples(); start += 512)
    {
        const auto length = std::min (512, out.getNumSamples() - start);
        float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
        phazite::devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        expander.process (block);
    }

    return out;
}

juce::AudioBuffer<float> makeNoise (int numSamples, bool mono, int seed = 1)
{
    juce::AudioBuffer<float> buffer (2, numSamples);
    juce::Random random (seed);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto left = random.nextFloat() * 2.0f - 1.0f;
        buffer.setSample (0, i, 0.25f * left);
        buffer.setSample (1, i, 0.25f * (mono ? left : random.nextFloat() * 2.0f - 1.0f));
    }

    return buffer;
}

juce::AudioBuffer<float> makeSine (double hertz, int numSamples, float rightSign = 1.0f)
{
    juce::AudioBuffer<float> buffer (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto sample = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * i / rate);
        buffer.setSample (0, i, sample);
        buffer.setSample (1, i, rightSign * sample);
    }

    return buffer;
}

// The correlation of the two sides over the second half.
double correlationOf (const juce::AudioBuffer<float>& buffer)
{
    double lr = 0.0, ll = 0.0, rr = 0.0;

    for (int i = buffer.getNumSamples() / 2; i < buffer.getNumSamples(); ++i)
    {
        const auto l = (double) buffer.getSample (0, i), r = (double) buffer.getSample (1, i);
        lr += l * r;
        ll += l * l;
        rr += r * r;
    }

    return lr / std::sqrt (ll * rr);
}

// The level of the mono sum over the second half, in decibels.
double monoLevel (const juce::AudioBuffer<float>& buffer)
{
    double sum = 0.0;

    for (int i = buffer.getNumSamples() / 2; i < buffer.getNumSamples(); ++i)
    {
        const auto mono = 0.5 * (buffer.getSample (0, i) + buffer.getSample (1, i));
        sum += mono * mono;
    }

    return juce::Decibels::gainToDecibels (std::sqrt (sum / (buffer.getNumSamples() / 2)));
}
} // namespace

TEST_CASE ("The stereo expander changes nothing at its defaults, and has no mix")
{
    const auto in = makeNoise (48000, false);
    const auto out = play (in, getDefaults());

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < in.getNumSamples(); ++i)
            REQUIRE (out.getSample (channel, i) == doctest::Approx (in.getSample (channel, i)).epsilon (1.0e-6));

    CHECK (phazite::devices::findParameter (StereoExpander::info, phazite::devices::mixParameterID) < 0);
}

TEST_CASE ("The stereo expander's width goes from mono to twice as wide, and never changes the mono sum")
{
    const auto in = makeNoise (48000, false);
    auto values = getDefaults();

    set (values, "width", 0.0f); // mono: both sides alike
    auto out = play (in, values);
    CHECK (out.getSample (0, 30000) == doctest::Approx (out.getSample (1, 30000)).epsilon (1.0e-5));

    set (values, "width", 2.0f); // twice the side
    out = play (in, values);
    const auto sideIn = in.getSample (0, 30000) - in.getSample (1, 30000);
    CHECK (out.getSample (0, 30000) - out.getSample (1, 30000) == doctest::Approx (2.0f * sideIn).epsilon (0.001));

    // Whatever the width and the spread, the left plus the right stays what came in.
    for (const auto width : { 0.0f, 0.5f, 2.0f })
    {
        for (const auto spread : { 0.0f, 1.0f })
        {
            CAPTURE (width);
            CAPTURE (spread);
            set (values, "width", width);
            set (values, "spread", spread);
            out = play (in, values);
            auto largest = 0.0f;

            for (int i = 0; i < in.getNumSamples(); ++i)
                largest = std::max (largest,
                                    std::abs ((out.getSample (0, i) + out.getSample (1, i)) - (in.getSample (0, i) + in.getSample (1, i))));

            CHECK (largest < 1.0e-5f);
        }
    }
}

TEST_CASE ("The stereo expander's spread makes stereo of a mono sound, unrelated sides, but not of its bass")
{
    auto values = getDefaults();
    set (values, "spread", 1.0f);

    const auto noise = play (makeNoise (96000, true), values);
    CHECK (correlationOf (noise) > -0.15);
    CHECK (correlationOf (noise) < 0.25);

    // A 1 kHz tone (at -6 dB) spreads; a 30 Hz one, two octaves under the bass mono frequency, stays in the middle.
    const auto sideOf = [&values] (double hertz)
    {
        const auto out = play (makeSine (hertz, 96000), values);
        double side = 0.0;

        for (int i = 48000; i < 96000; ++i)
            side = std::max (side, (double) std::abs (out.getSample (0, i) - out.getSample (1, i)));

        return juce::Decibels::gainToDecibels (side);
    };

    CHECK (sideOf (1000.0) > -20.0);
    CHECK (sideOf (30.0) < -36.0);
}

TEST_CASE ("The stereo expander keeps the bass in mono below its frequency, and the mono sum's level")
{
    auto values = getDefaults();
    set (values, "bassMono", 1.0f);

    // Sines in mono keep their level, below, at and above the frequency.
    for (const auto hertz : { 50.0, 120.0, 1000.0 })
    {
        CAPTURE (hertz);
        const auto in = makeSine (hertz, 96000);
        CHECK (monoLevel (play (in, values)) == doctest::Approx (monoLevel (in)).epsilon (0.006)); // about 0.05 dB
    }

    // A side's bass goes: 40 Hz with the right upside down cancels; 4 kHz stays.
    const auto low = play (makeSine (40.0, 96000, -1.0f), values);
    CHECK (low.getMagnitude (0, 48000, 48000) < 0.1f);
    const auto high = play (makeSine (4000.0, 96000, -1.0f), values);
    CHECK (high.getMagnitude (0, 48000, 48000) > 0.45f);
}

TEST_CASE ("The stereo expander flips each side's polarity first, then sets the gain")
{
    auto values = getDefaults();
    set (values, "invertLeft", 1.0f);
    set (values, "gain", 6.0206f);
    const auto in = makeNoise (48000, false);
    const auto out = play (in, values);
    CHECK (out.getSample (0, 30000) == doctest::Approx (-2.0f * in.getSample (0, 30000)).epsilon (0.001));
    CHECK (out.getSample (1, 30000) == doctest::Approx (2.0f * in.getSample (1, 30000)).epsilon (0.001));
}

TEST_CASE ("The stereo expander tells how alike its sides are, coming in and going out")
{
    StereoExpander expander;
    auto values = getDefaults();

    play (makeNoise (48000, true), values, &expander);
    CHECK (expander.getOutputCorrelation().get() == doctest::Approx (1.0f).epsilon (0.01));

    play (makeSine (500.0, 48000, -1.0f), values, &expander);
    CHECK (expander.getInputCorrelation().get() == doctest::Approx (-1.0f).epsilon (0.01));

    play (makeNoise (96000, false), values, &expander);
    CHECK (std::abs (expander.getOutputCorrelation().get()) < 0.1f);

    // Silence: no reading.
    juce::AudioBuffer<float> silence (2, 96000);
    silence.clear();
    play (silence, values, &expander);
    CHECK (expander.getOutputCorrelation().get() == doctest::Approx (StereoExpander::noReading));

    // Width 0 makes the sides the same.
    set (values, "width", 0.0f);
    play (makeNoise (48000, false), values, &expander);
    CHECK (expander.getOutputCorrelation().get() == doctest::Approx (1.0f).epsilon (0.01));
}
