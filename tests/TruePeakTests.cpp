// SPDX-License-Identifier: AGPL-3.0-or-later
// The true peak as ITU-R BS.1770-5 finds it (src/devices/dynamics/TruePeak.h): sines like EBU Tech 3341's true-peak test
// signals, whose samples miss their peaks, read within the tolerance Tech 3341 gives a meter (+0.2 dB, -0.4 dB); bright
// sines read within a quarter of a decibel; and the loudness meter, which shows it, reads what the limiter holds.
#include "TestHelpers.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/TruePeak.h"
#include "engine/nodes/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

// The loudest a sine (its amplitude, its frequency in cycles a sample, its phase in degrees) reads, in decibels: of the
// points between its samples alone, or with the samples too, as the loudness meter reads it. Read once the filter holds
// nothing but the sine: its start out of silence is a step, which the filter rings at.
double readSine (double amplitude, double cyclesPerSample, double degrees, bool withSamples, int numSamples = 48000)
{
    devices::TruePeak truePeak;
    auto loudest = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto sample = (float) (amplitude * std::sin (twoPi * cyclesPerSample * i + degrees * twoPi / 360.0));
        const auto point = truePeak.push (sample);

        if (i < 2 * devices::TruePeak::taps)
            continue;

        loudest = std::max (loudest, point);

        if (withSamples)
            loudest = std::max (loudest, std::abs (sample));
    }

    return 20.0 * std::log10 (std::max (loudest, 1.0e-9f));
}
} // namespace

TEST_CASE ("The true peak reads sines whose samples miss their peaks as EBU Tech 3341 asks of a meter")
{
    // A quarter, a sixth and an eighth of the sample rate, phased so that no sample (or none but the first case's) falls on
    // a peak, each with a true peak of -6 dBTP; and the 45 degree one pushed to +3 dBTP. A meter reads each within +0.2 dB
    // and -0.4 dB of it.
    struct Case
    {
        double cyclesPerSample, degrees, amplitude, decibels;
    };

    const Case cases[] = { { 0.25, 0.0, 0.5, -6.02 },
                           { 0.25, 45.0, 0.5, -6.02 },
                           { 1.0 / 6.0, 60.0, 0.5, -6.02 },
                           { 0.125, 67.5, 0.5, -6.02 },
                           { 0.25, 45.0, std::sqrt (2.0), 3.01 } };

    for (const auto& sine : cases)
    {
        CAPTURE (sine.cyclesPerSample);
        CAPTURE (sine.degrees);
        const auto reading = readSine (sine.amplitude, sine.cyclesPerSample, sine.degrees, true);
        CHECK (reading <= sine.decibels + 0.2);
        CHECK (reading >= sine.decibels - 0.4);
    }

    // The samples alone miss the 45 degree sine's peaks by 3 dB.
    auto loudestSample = 0.0;

    for (int i = 0; i < 4800; ++i)
        loudestSample = std::max (loudestSample, std::abs (0.5 * std::sin (twoPi * 0.25 * i + twoPi / 8.0)));

    CHECK (20.0 * std::log10 (loudestSample) == doctest::Approx (-9.03).epsilon (0.01));
}

TEST_CASE ("The true peak reads a bright sine's peak within a quarter of a decibel, up to 19 kHz at 44.1 kHz and 20 kHz at 48 kHz")
{
    // The points alone, without the samples, which a long sine also brings near its peak. Not at a frequency whose cycle is a
    // whole number of samples or two (a third of the rate, say): four points a sample then meet its peak at the same places
    // every cycle, and can miss it by up to 0.3 dB, as any four times oversampled meter does.
    for (const auto rate : { 44100.0, 48000.0 })
    {
        for (const auto hertz : { 997.0, 5011.0, 10007.0, 12011.0, 15013.0, 16013.0, 17011.0, 18013.0, 19009.0, 20011.0 })
        {
            if (rate < 48000.0 && hertz > 19000.0)
                continue;

            CAPTURE (rate);
            CAPTURE (hertz);
            const auto reading = readSine (1.0, hertz / rate, 30.0, false);
            CHECK (std::abs (reading) < 0.25);
        }
    }

    // A single sample's click: at least the sample, and with it the filter's ringing on either side.
    devices::TruePeak truePeak;
    auto loudest = 0.0f;

    for (int i = 0; i < 32; ++i)
        loudest = std::max (loudest, truePeak.push (i == 10 ? 0.5f : 0.0f));

    CHECK (loudest > 0.45f);
    CHECK (loudest < 0.5f);
    CHECK (truePeak.getSample (devices::TruePeak::taps - 1) == 0.0f);
}

TEST_CASE ("The loudness meter reads the true peak the limiter holds to its ceiling, for a sine whose samples miss its peaks")
{
    constexpr double rate = 44100.0;
    constexpr int blockSize = 441;
    devices::Limiter limiter;
    limiter.prepare (rate, blockSize);
    phazite::engine::LoudnessMeter meter;
    meter.prepare (rate, blockSize);

    std::vector<float> values;

    for (const auto& parameter : devices::Limiter::info.parameters)
        values.push_back (parameter.defaultValue);

    values[devices::Limiter::gain] = 3.0f; // over the -1 dB ceiling by 4 dB
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < 200; ++block)
    {
        for (int i = 0; i < blockSize; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, (float) std::sin (twoPi * 17000.0 * (block * blockSize + i) / rate + 1.0));

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = blockSize;
        context.parameters = values;
        limiter.process (context);

        if (block == 50) // once the limiter has settled
            meter.reset();

        meter.process (buffer.getArrayOfReadPointers(), 2, blockSize);
    }

    const auto truePeak = meter.getReadings().truePeak;
    REQUIRE (truePeak.has_value());
    CHECK (*truePeak <= -1.0f + 0.01f);
    CHECK (*truePeak > -1.5f);
}
