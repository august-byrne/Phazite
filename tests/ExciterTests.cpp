// SPDX-License-Identifier: AGPL-3.0-or-later
// The exciter as DSP (src/devices/saturation/Exciter.h), measured: the sound itself passing untouched and only as late as
// the device says, the band added in phase (a lift that never dips), what Frequency, High Cut, Drive, Colour and Amount
// each do, harmonics without aliases worth hearing, parameters that move without a click, Listen, and all of it at every
// sample rate and block size.
#include "TestHelpers.h"
#include "devices/saturation/Exciter.h"
#include "engine/nodes/DeviceRunner.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <functional>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Exciter;
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr int fftOrder = 16, fftSize = 1 << fftOrder;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Exciter::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

std::vector<float> with (std::initializer_list<std::pair<std::string_view, float>> changes)
{
    auto values = getDefaults();

    for (const auto& [id, value] : changes)
    {
        const auto index = devices::findParameter (Exciter::info, id);
        REQUIRE (index >= 0);
        values[(size_t) index] = value;
    }

    return values;
}

struct Stereo
{
    std::vector<float> left, right;
};

// Plays an exciter that is prepared, in blocks, on a sound (input gives each channel's sample at a time), with the values a
// block starts with (values gives them at the block's first sample).
Stereo play (Exciter& exciter, int numSamples, int blockSize, const std::function<std::pair<float, float> (int)>& input,
             const std::function<std::vector<float> (int)>& values)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    Stereo played;

    for (int done = 0; done < numSamples; done += blockSize)
    {
        const auto length = std::min (blockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
        {
            const auto [left, right] = input (done + i);
            buffer.setSample (0, i, left);
            buffer.setSample (1, i, right);
        }

        const auto now = values (done);
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = now;
        exciter.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

// The same for a mono sound and values that stay, on a new exciter at a rate: its left channel.
std::vector<float> play (const std::vector<float>& values, double rate, int numSamples, const std::function<float (int)>& input,
                         int blockSize = 480)
{
    Exciter exciter;
    exciter.prepare (rate, blockSize);
    return play (
               exciter, numSamples, blockSize,
               [&] (int t)
               {
                   const auto sample = input (t);
                   return std::pair { sample, sample };
               },
               [&] (int) { return values; })
        .left;
}

std::function<float (int)> sine (double frequency, double rate, float amplitude)
{
    return [=] (int t) { return amplitude * (float) std::sin (twoPi * frequency * t / rate); };
}

// A tone's level and phase in a stretch of a sound, by least squares: its sine and cosine parts.
struct Tone
{
    double sinePart = 0.0, cosinePart = 0.0;

    double getLevel() const { return std::sqrt (sinePart * sinePart + cosinePart * cosinePart); }
};

Tone getTone (const float* sound, int from, int numSamples, double frequency, double rate)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = from; i < from + numSamples; ++i)
    {
        const auto s = std::sin (twoPi * frequency * i / rate), c = std::cos (twoPi * frequency * i / rate);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += sound[i] * s;
        yc += sound[i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    return { (ys * cc - yc * sc) / determinant, (yc * ss - ys * sc) / determinant };
}

double toDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}

float getPeak (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// A frequency near the one asked for that is a whole, odd number of FFT bins, which no alias of its harmonics lands on a
// multiple of (as AntialiasingTests.cpp measures).
double onBin (double frequency, double rate)
{
    auto bin = (int) std::lround (frequency * fftSize / rate);

    if (bin % 2 == 0)
        ++bin;

    return bin * rate / fftSize;
}

// In fftSize samples of a sound that a tone on a bin made: how far its loudest harmonic (a multiple of the tone, the tone
// itself aside) and its loudest alias (anything else) are under the tone, in decibels, up to 20 kHz.
struct Spectrum
{
    double harmonicsUnder = 240.0, aliasesUnder = 240.0;
};

Spectrum getSpectrum (const float* sound, double frequency, double rate)
{
    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris);
    std::vector<float> data ((size_t) fftSize * 2, 0.0f);
    std::copy (sound, sound + fftSize, data.begin());
    window.multiplyWithWindowingTable (data.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    const auto bin = (int) std::lround (frequency * fftSize / rate);
    const auto tone = (double) data[(size_t) bin];
    auto harmonic = 1.0e-12, alias = 1.0e-12;

    // The window spreads each line over a few bins either side.
    for (int k = 8; k < fftSize / 2 && k * rate / fftSize < 20000.0; ++k)
    {
        const auto nearest = (int) std::lround ((double) k / bin) * bin;

        if (std::abs (k - nearest) > 6)
            alias = std::max (alias, (double) data[(size_t) k]);
        else if (nearest != bin && k == nearest)
            harmonic = std::max (harmonic, (double) data[(size_t) k]);
    }

    return { toDecibels (tone / harmonic), toDecibels (tone / alias) };
}
} // namespace

TEST_CASE ("The exciter describes itself: an effect with a band, a drive, a colour, an amount and a mix its runner applies")
{
    CHECK (devices::isValid (Exciter::info));
    CHECK (Exciter::info.typeID == "phaziteExciter");
    CHECK (Exciter::info.kind == devices::DeviceKind::effect);
    CHECK (devices::findParameter (Exciter::info, devices::mixParameterID) == Exciter::mix);
    CHECK (Exciter::info.parameters[Exciter::mix].defaultValue == doctest::Approx (1.0f));

    // High Cut is off at first, and the band starts where the presence does.
    CHECK (Exciter::info.parameters[Exciter::highCut].defaultValue == doctest::Approx (Exciter::info.parameters[Exciter::highCut].maximum));
    CHECK (Exciter::info.parameters[Exciter::frequency].defaultValue == doctest::Approx (5000.0f));

    // Its curve passes through nothing as steeply as a straight line, at any colour, and flattens; lopsided once coloured.
    for (const auto colour : { 0.0f, 0.5f, 1.0f })
    {
        CAPTURE (colour);
        CHECK (Exciter::shapeSample (colour, 0.0f) == doctest::Approx (0.0f).epsilon (1.0e-6));
        CHECK ((Exciter::shapeSample (colour, 0.001f) - Exciter::shapeSample (colour, -0.001f)) / 0.002f
               == doctest::Approx (1.0f).epsilon (0.001));
        CHECK (Exciter::shapeSample (colour, 20.0f) == doctest::Approx (Exciter::shapeSample (colour, 10.0f)));
        CHECK (Exciter::shapeSample (colour, 1.0f) < 1.0f);
    }

    CHECK (Exciter::shapeSample (0.0f, 1.0f) == doctest::Approx (-Exciter::shapeSample (0.0f, -1.0f)));
    CHECK (Exciter::shapeSample (1.0f, 1.0f) < -0.8f * Exciter::shapeSample (1.0f, -1.0f));
}

TEST_CASE ("The exciter passes the sound itself untouched, as late as it says, at every sample rate")
{
    for (const auto rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        CAPTURE (rate);
        Exciter exciter;
        exciter.prepare (rate, 512);
        const auto latency = exciter.getLatencySamples();

        // The filter after the curve is 2 ms long, and the curve's own way through its linear-phase filters about one more.
        MESSAGE ("latency at " << rate << " Hz: " << latency << " samples, " << 1000.0 * latency / rate << " ms");
        CHECK (latency >= (int) std::lround (0.002 * rate));
        CHECK (latency <= (int) std::lround (0.0035 * rate));

        // No amount: noise comes out as it went in, sample for sample, whatever the drive.
        juce::Random random (11);
        std::vector<float> noise ((size_t) (rate / 4));

        for (auto& sample : noise)
            sample = random.nextFloat() - 0.5f;

        const auto played = play (
            exciter, (int) noise.size(), 512, [&] (int t) { return std::pair { noise[(size_t) t], -noise[(size_t) t] }; },
            [] (int) { return with ({ { "amount", 0.0f }, { "drive", 36.0f } }); });

        auto same = true;

        for (size_t i = (size_t) latency; i < noise.size(); ++i)
            same = same && juce::exactlyEqual (played.left[i], noise[i - (size_t) latency])
                   && juce::exactlyEqual (played.right[i], -noise[i - (size_t) latency]);

        CHECK (same);
        CHECK (getPeak (played.left, 0, (size_t) latency) == 0.0f);
    }
}

TEST_CASE ("The exciter's runner passes the sound on unchanged at no mix, and silence in gives silence out")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;
    phazite::engine::DeviceRunner runner (Exciter::info, std::make_unique<Exciter>());
    runner.prepare (rate, blockSize);
    const auto latency = runner.getLatencySamples();
    REQUIRE (latency > 0);

    const auto values = with ({ { "mix", 0.0f }, { "amount", 4.0f }, { "drive", 36.0f } });
    juce::Random random (5);
    std::vector<float> noise ((size_t) (40 * blockSize)), played;

    for (auto& sample : noise)
        sample = random.nextFloat() - 0.5f;

    juce::AudioBuffer<float> buffer (2, blockSize);
    phazite::engine::DeviceRunner::Block block;
    block.values = values;

    for (size_t done = 0; done < noise.size(); done += (size_t) blockSize)
    {
        for (int channel = 0; channel < 2; ++channel)
            buffer.copyFrom (channel, 0, noise.data() + done, blockSize);

        runner.process (buffer, 0, blockSize, block);
        played.insert (played.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    // After the first block, which takes the mix down from where it started.
    auto same = true;

    for (auto i = (size_t) (2 * blockSize); i < noise.size(); ++i)
        same = same && juce::exactlyEqual (played[i], noise[i - (size_t) latency]);

    CHECK (same);

    // Silence, at every colour (whose working point is a level inside the curve, which mustn't come out).
    for (const auto colour : { 0.0f, 0.5f, 1.0f })
    {
        CAPTURE (colour);
        const auto silent =
            play (with ({ { "colour", colour }, { "amount", 4.0f }, { "drive", 36.0f } }), rate, 9600, [] (int) { return 0.0f; });
        CHECK (getPeak (silent, 0, silent.size()) < 1.0e-7f);
    }
}

TEST_CASE ("The exciter adds the band in phase: a lift that follows the band's filters, and never a dip")
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        CAPTURE (rate);

        // A quiet tone, which the curve leaves straight, with as much of the band added as the band itself (100 %), and four
        // times that: it comes out as loud as the lift says, and in step with what went in, so that what came out less what
        // went in is the band added, with nothing left over.
        for (const auto amount : { 1.0f, 4.0f })
        {
            for (const auto frequency : { 1000.0f, 3000.0f, 12000.0f })
            {
                const auto values = with ({ { "frequency", frequency }, { "amount", amount }, { "drive", 0.0f } });
                Exciter exciter;
                exciter.prepare (rate, 480);
                const auto latency = exciter.getLatencySamples();

                for (const auto ratio : { 0.25, 0.5, 0.7071, 1.0, 1.4142, 2.0, 4.0 })
                {
                    const auto hertz = frequency * ratio;

                    if (hertz > 0.45 * rate || hertz > 20000.0)
                        continue;

                    const auto length = (int) (rate / 5);
                    const auto played = play (values, rate, length, sine (hertz, rate, 0.001f));
                    const auto from = length / 2, count = length / 2 - 1;
                    const auto out = getTone (played.data(), from, count, hertz, rate);

                    // What went in, as late as the device says.
                    std::vector<float> delayed ((size_t) length, 0.0f);

                    for (int i = latency; i < length; ++i)
                        delayed[(size_t) i] = sine (hertz, rate, 0.001f) (i - latency);

                    const auto in = getTone (delayed.data(), from, count, hertz, rate);
                    const auto lift = Exciter::getLift (values, hertz, rate);

                    CAPTURE (frequency);
                    CAPTURE (amount);
                    CAPTURE (ratio);
                    CHECK (std::abs (toDecibels (out.getLevel() / in.getLevel()) - lift) < 0.05);
                    CHECK (toDecibels (out.getLevel() / in.getLevel()) > -0.05); // never under what went in

                    // In step: the angle between what came out and what went in.
                    const auto angle = std::atan2 (out.cosinePart, out.sinePart) - std::atan2 (in.cosinePart, in.sinePart);
                    CHECK (std::abs (std::remainder (angle, twoPi)) < juce::degreesToRadians (1.0));
                }
            }
        }
    }

    // Driven hard too, where the curve takes much of the band back off: what it takes comes back in step with the band, up
    // to the top, so that a loud tone still comes out in step with what went in.
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto values = with ({ { "frequency", 2000.0f }, { "amount", 2.0f }, { "drive", 24.0f }, { "colour", 0.0f } });
        Exciter exciter;
        exciter.prepare (rate, 480);
        const auto latency = exciter.getLatencySamples();

        for (const auto hertz : { 4000.0, 9000.0, 14000.0, 18000.0 })
        {
            const auto length = (int) (rate / 5), from = length / 2, count = length / 2 - 1;
            const auto played = play (values, rate, length, sine (hertz, rate, 0.25f));
            std::vector<float> delayed ((size_t) length, 0.0f);

            for (int i = latency; i < length; ++i)
                delayed[(size_t) i] = sine (hertz, rate, 0.25f) (i - latency);

            const auto out = getTone (played.data(), from, count, hertz, rate), in = getTone (delayed.data(), from, count, hertz, rate);
            const auto angle = std::atan2 (out.cosinePart, out.sinePart) - std::atan2 (in.cosinePart, in.sinePart);
            CAPTURE (rate);
            CAPTURE (hertz);
            CHECK (std::abs (std::remainder (angle, twoPi)) < juce::degreesToRadians (1.0));
            CHECK (out.getLevel() > in.getLevel());                                                             // lifted,
            CHECK (toDecibels (out.getLevel() / in.getLevel()) < Exciter::getLift (values, hertz, rate) - 3.0); // but held down
        }
    }

    // The band's filters: half of the band's level at the frequency twice over (12 dB down), all of it two octaves up, and
    // 48 dB an octave below, so that at 100 % the top rises 6 dB.
    const auto values = with ({ { "frequency", 4000.0f }, { "amount", 1.0f } });
    CHECK (toDecibels (Exciter::getBandGain (values, 4000.0, 96000.0)) == doctest::Approx (-12.0).epsilon (0.02));
    CHECK (toDecibels (Exciter::getBandGain (values, 16000.0, 96000.0)) > -0.2);
    CHECK (toDecibels (Exciter::getBandGain (values, 2000.0, 96000.0)) < -48.0);
    CHECK (toDecibels (Exciter::getBandGain (values, 1000.0, 96000.0)) < -96.0);
    CHECK (Exciter::getLift (values, 16000.0, 96000.0) == doctest::Approx (6.0).epsilon (0.02));
    CHECK (Exciter::getLift (values, 1000.0, 96000.0) < 0.001);

    // High Cut ends the band the same way.
    const auto cut = with ({ { "frequency", 2000.0f }, { "highCut", 6000.0f }, { "amount", 1.0f } });
    CHECK (toDecibels (Exciter::getBandGain (cut, 6000.0, 96000.0)) == doctest::Approx (-12.0).epsilon (0.05));
    CHECK (toDecibels (Exciter::getBandGain (cut, 12000.0, 96000.0)) < -48.0);
    CHECK (toDecibels (Exciter::getBandGain (cut, 3500.0, 96000.0)) > -4.0);
}

TEST_CASE ("The exciter's drive turns the band into harmonics without making it louder, and its colour picks even or odd ones")
{
    constexpr double rate = 96000.0;
    constexpr int length = 48000, from = 24000, count = 19200; // a whole number of the tone's cycles
    constexpr double hertz = 5000.0;
    const auto input = sine (hertz, rate, 0.1f); // -20 dBFS, in the band from 2 kHz

    const auto measure = [&] (float drive, float colour, float amount = 1.0f)
    {
        const auto played =
            play (with ({ { "frequency", 2000.0f }, { "drive", drive }, { "colour", colour }, { "amount", amount } }), rate, length, input);
        struct Levels
        {
            double first, second, third, fourth;
        };

        return Levels { toDecibels (getTone (played.data(), from, count, hertz, rate).getLevel() / 0.1),
                        toDecibels (getTone (played.data(), from, count, 2.0 * hertz, rate).getLevel() / 0.1),
                        toDecibels (getTone (played.data(), from, count, 3.0 * hertz, rate).getLevel() / 0.1),
                        toDecibels (getTone (played.data(), from, count, 4.0 * hertz, rate).getLevel() / 0.1) };
    };

    // Without drive the tone only rises by the lift (5.8 dB here, at 100 %), with little of a second harmonic (at -20 dBFS it
    // is a tenth of the way to the curve's bend) and no third to speak of.
    const auto clean = measure (0.0f, 0.5f);
    CHECK (clean.first
           == doctest::Approx (Exciter::getLift (with ({ { "frequency", 2000.0f }, { "amount", 1.0f } }), hertz, rate)).epsilon (0.01));
    CHECK (clean.second < -35.0);
    CHECK (clean.third < -60.0);

    // Driven, harmonics come, more of them the harder; and the tone itself rises less, held down by the curve, never more.
    const auto driven = measure (18.0f, 0.5f), hard = measure (30.0f, 0.5f);
    CHECK (driven.second > clean.second + 15.0);
    CHECK (driven.third > -45.0);
    CHECK (hard.third > driven.third + 8.0);
    CHECK (driven.first < clean.first);
    CHECK (hard.first < driven.first);
    CHECK (hard.first > 0.0);

    // Colour: at 0 the curve is even-sided, and makes odd harmonics alone; at 100 % the second leads, and at a gentle drive
    // the third is all but gone.
    const auto odd = measure (18.0f, 0.0f), even = measure (18.0f, 1.0f), gentle = measure (10.0f, 1.0f);
    CHECK (odd.second < -90.0);
    CHECK (odd.fourth < -90.0);
    CHECK (odd.third > -40.0);
    CHECK (even.second > odd.third);
    CHECK (even.second > even.third + 12.0);
    CHECK (gentle.second > gentle.third + 30.0);

    // Amount is how much of all that is added: twice as much, 6 dB more of each harmonic.
    const auto twice = measure (18.0f, 0.5f, 2.0f);
    CHECK (twice.second == doctest::Approx (driven.second + 6.02).epsilon (0.01));
    CHECK (twice.third == doctest::Approx (driven.third + 6.02).epsilon (0.01));

    // And a note below the band stays out of the curve: no harmonics of it, however hard the band is driven.
    const auto low =
        play (with ({ { "frequency", 4000.0f }, { "drive", 36.0f }, { "amount", 4.0f } }), rate, length, sine (500.0, rate, 0.5f));
    CHECK (toDecibels (getTone (low.data(), from, count, 1000.0, rate).getLevel() / 0.5) < -100.0);
    CHECK (toDecibels (getTone (low.data(), from, count, 1500.0, rate).getLevel() / 0.5) < -100.0);
    CHECK (std::abs (toDecibels (getTone (low.data(), from, count, 500.0, rate).getLevel() / 0.5)) < 0.01);
}

TEST_CASE ("The exciter keeps what the curve makes below the band out: two tones' difference isn't added")
{
    // Two tones in the band, 700 Hz apart: a lopsided curve makes their difference, 700 Hz, and their sum. The filter after
    // the curve keeps the difference out (the band starts at 3 kHz) and passes the sum.
    constexpr double rate = 48000.0;
    constexpr int length = 48000, from = 24000, count = 19200;
    const auto two = [&] (int t) { return 0.1f * (float) (std::sin (twoPi * 6000.0 * t / rate) + std::sin (twoPi * 6700.0 * t / rate)); };
    const auto played =
        play (with ({ { "frequency", 3000.0f }, { "drive", 24.0f }, { "colour", 1.0f }, { "amount", 1.0f } }), rate, length, two);
    const auto sum = toDecibels (getTone (played.data(), from, count, 12700.0, rate).getLevel() / 0.1);
    const auto difference = toDecibels (getTone (played.data(), from, count, 700.0, rate).getLevel() / 0.1);
    CHECK (sum > -30.0);
    CHECK (difference < sum - 48.0); // the filter after the curve at 700 Hz, (700 / 3000) to the fourth: 50 dB
}

TEST_CASE ("The exciter's harmonics don't fold back as aliases worth hearing, at 44.1 and 48 kHz")
{
    for (const auto rate : { 44100.0, 48000.0 })
    {
        // Loud tones up the band, driven hard into the curve at its most lopsided and added whole.
        for (const auto hertz : { 2500.0, 5000.0, 7000.0, 9000.0, 13000.0 })
        {
            const auto frequency = onBin (hertz, rate);
            const auto played = play (with ({ { "frequency", 2000.0f }, { "drive", 24.0f }, { "colour", 1.0f }, { "amount", 1.0f } }), rate,
                                      fftSize + 9600, sine (frequency, rate, 0.5f));
            const auto spectrum = getSpectrum (played.data() + 9600, frequency, rate);
            CAPTURE (rate);
            CAPTURE (hertz);
            MESSAGE ("harmonics " << spectrum.harmonicsUnder << " dB under the tone, aliases " << spectrum.aliasesUnder << " dB");
            CHECK (spectrum.aliasesUnder > 66.0);

            if (3.0 * hertz < 20000.0)
                CHECK (spectrum.harmonicsUnder < 30.0); // the harmonics are there
        }
    }
}

TEST_CASE ("The exciter's parameters move without a click, and Listen plays what is added alone")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 64, length = 48000;

    // A low tone, under the band, while everything jumps every block or two (1.3 ms), the colour and the band's edges too,
    // with the band added four times over: nothing of the curve's working point or of the filters' moves comes through (they
    // glide), and the tone stays as it was, within 75 dB.
    {
        Exciter exciter;
        exciter.prepare (rate, blockSize);
        const auto latency = exciter.getLatencySamples();
        const auto tone = sine (100.0, rate, 0.5f);
        const auto played = play (
            exciter, length, blockSize,
            [&] (int t)
            {
                const auto sample = tone (t);
                return std::pair { sample, sample };
            },
            [] (int t)
            {
                const auto step = (t / blockSize) % 4;
                return with ({ { "frequency", step < 2 ? 2000.0f : 9000.0f },
                               { "drive", step % 2 == 0 ? 0.0f : 36.0f },
                               { "colour", step == 1 ? 1.0f : 0.0f },
                               { "amount", step == 3 ? 0.0f : 4.0f },
                               { "highCut", step == 2 ? 6000.0f : 20000.0f } });
            });

        auto worst = 0.0f;

        for (int i = latency + 4800; i < length; ++i)
            worst = std::max (worst, std::abs (played.left[(size_t) i] - tone (i - latency)));

        CHECK (toDecibels (worst / 0.5) < -75.0);
    }

    // A tone in the band while the amount jumps from nothing to 100 % and back: it glides there, never a step from one
    // sample to the next larger than the tone's own.
    {
        Exciter exciter;
        exciter.prepare (rate, blockSize);
        const auto tone = sine (6000.0, rate, 0.25f);
        const auto played = play (
            exciter, length, blockSize,
            [&] (int t)
            {
                const auto sample = tone (t);
                return std::pair { sample, sample };
            },
            [] (int t)
            { return with ({ { "frequency", 2000.0f }, { "drive", 12.0f }, { "amount", (t / 4800) % 2 == 0 ? 0.0f : 1.0f } }); });

        auto largestStep = 0.0f;

        for (int i = 4800; i < length; ++i)
            largestStep = std::max (largestStep, std::abs (played.left[(size_t) i] - played.left[(size_t) i - 1]));

        // A 6 kHz tone at twice 0.25 (the tone and as much again) moves 2 * 0.25 * 2 sin (pi * 6000 / 48000) a sample at most.
        CHECK (largestStep < 2.0f * 0.25f * 2.0f * (float) std::sin (juce::MathConstants<double>::pi * 6000.0 / rate) * 1.05f);
    }

    // Listen: what is added alone, which its runner then plays whole, and the sound again half a second after it was last
    // asked for.
    {
        Exciter exciter;
        exciter.prepare (rate, blockSize);
        const auto values = with ({ { "frequency", 2000.0f }, { "drive", 0.0f }, { "amount", 1.0f } });
        const auto both = [&] (int t)
        { return (float) (0.25 * std::sin (twoPi * 200.0 * t / rate) + 0.01 * std::sin (twoPi * 8000.0 * t / rate)); };
        const auto run = [&]
        {
            return play (
                       exciter, 9600, blockSize,
                       [&] (int t)
                       {
                           const auto sample = both (t);
                           return std::pair { sample, sample };
                       },
                       [&] (int) { return values; })
                .left;
        };

        const auto whole = run();
        CHECK_FALSE (exciter.overridesMix());
        CHECK (getTone (whole.data(), 4800, 4800, 200.0, rate).getLevel() == doctest::Approx (0.25).epsilon (0.01));

        exciter.listenToAdded();
        CHECK (exciter.isPlayingAdded());
        const auto added = run();
        CHECK (exciter.overridesMix());
        CHECK (getTone (added.data(), 4800, 4800, 200.0, rate).getLevel() < 0.25 * 1.0e-4); // the low tone is gone
        CHECK (getTone (added.data(), 4800, 4800, 8000.0, rate).getLevel()
               == doctest::Approx (0.01 * Exciter::getBandGain (values, 8000.0, rate)).epsilon (0.02)); // the band, as much as is added
    }
}

TEST_CASE ("The exciter plays the same in blocks of any size, also longer than it was prepared for")
{
    constexpr double rate = 44100.0;
    constexpr int length = 8192;
    const auto values = with ({ { "frequency", 3000.0f }, { "drive", 24.0f }, { "amount", 1.0f }, { "highCut", 12000.0f } });
    juce::Random random (3);
    std::vector<float> noise ((size_t) length);

    for (auto& sample : noise)
        sample = 0.5f * (random.nextFloat() - 0.5f);

    const auto playIn = [&] (int blockSize, int preparedFor)
    {
        Exciter exciter;
        exciter.prepare (rate, preparedFor);
        return play (
                   exciter, length, blockSize, [&] (int t) { return std::pair { noise[(size_t) t], noise[(size_t) t] }; },
                   [&] (int) { return values; })
            .left;
    };

    const auto reference = playIn (512, 512);

    for (const auto& [blockSize, preparedFor] : { std::pair { 1, 512 }, { 17, 512 }, { 64, 64 }, { 4096, 512 }, { 4096, 4096 } })
    {
        CAPTURE (blockSize);
        CAPTURE (preparedFor);
        const auto played = playIn (blockSize, preparedFor);
        auto worst = 0.0f;

        for (size_t i = 0; i < reference.size(); ++i)
            worst = std::max (worst, std::abs (played[i] - reference[i]));

        CHECK (worst < 1.0e-5f);
    }

    // And nothing but numbers, with the loudest sound driven as hard as it goes.
    const auto loud = play (with ({ { "frequency", 1000.0f }, { "drive", 36.0f }, { "colour", 1.0f }, { "amount", 4.0f } }), rate, length,
                            [&] (int t) { return 8.0f * noise[(size_t) t]; });
    auto finite = true;

    for (const auto sample : loud)
        finite = finite && std::isfinite (sample);

    CHECK (finite);
}
