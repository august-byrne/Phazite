// SPDX-License-Identifier: AGPL-3.0-or-later
// The vocoder's DSP (src/devices/vocoder/Vocoder.h): its bands following the voice, the voice's tone on the carrier and
// Formant moving it, silence for silence, a track's sound as the carrier only when one feeds it, the consonants' noise, a
// level about the voice's, Stereo keeping the mono sum, the same at each render, and a change of its bands while it plays.
#include "TestHelpers.h"
#include "devices/dsp/Filters.h"
#include "devices/vocoder/Vocoder.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Vocoder;

constexpr double rate = 48000.0;
constexpr int blockSize = 480;
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr double c3 = 130.8127826502993;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Vocoder::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void setCarrier (std::vector<float>& values, Vocoder::Carrier carrier)
{
    values[Vocoder::carrier] = (float) carrier;
}

// Plays a voice (the same on both sides) through a vocoder in blocks of 10 ms, with a sidechain if given; `beforeBlock` is
// called with each block's number first.
juce::AudioBuffer<float> play (Vocoder& vocoder, const std::vector<float>& values, const std::vector<float>& voice,
                               const std::vector<float>* sidechain = nullptr, const std::function<void (int)>& beforeBlock = {})
{
    const auto numSamples = (int) voice.size();
    juce::AudioBuffer<float> out (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            out.setSample (channel, i, voice[(size_t) i]);

    for (int start = 0, number = 0; start < numSamples; start += blockSize, ++number)
    {
        if (beforeBlock)
            beforeBlock (number);

        const auto length = std::min (blockSize, numSamples - start);
        float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
        const float* key[] = { sidechain != nullptr ? sidechain->data() + start : nullptr,
                               sidechain != nullptr ? sidechain->data() + start : nullptr };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;

        if (sidechain != nullptr)
        {
            block.sidechain = key;
            block.numSidechainChannels = 2;
        }

        vocoder.process (block);
    }

    return out;
}

juce::AudioBuffer<float> play (const std::vector<float>& values, const std::vector<float>& voice,
                               const std::vector<float>* sidechain = nullptr)
{
    Vocoder vocoder;
    vocoder.prepare (rate, blockSize);
    return play (vocoder, values, voice, sidechain);
}

std::vector<float> makeNoise (int numSamples, float level = 0.5f, int seed = 1)
{
    std::vector<float> sound ((size_t) numSamples);
    juce::Random random (seed);

    for (auto& sample : sound)
        sample = level * (random.nextFloat() * 2.0f - 1.0f);

    return sound;
}

// Pink noise (Paul Kellet's filters over white noise), at an RMS level in decibels.
std::vector<float> makePinkNoise (int numSamples, float rmsDecibels)
{
    auto white = makeNoise (numSamples, 1.0f, 5);
    std::vector<float> pink ((size_t) numSamples);
    auto b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0, sum = 0.0;

    for (size_t i = 0; i < white.size(); ++i)
    {
        const auto w = (double) white[i];
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        pink[i] = (float) (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
        b6 = w * 0.115926;
        sum += (double) pink[i] * pink[i];
    }

    const auto scale = juce::Decibels::decibelsToGain (rmsDecibels) / std::sqrt (sum / numSamples);

    for (auto& sample : pink)
        sample = (float) (sample * scale);

    return pink;
}

std::vector<float> makeSine (double hertz, int numSamples, float level = 0.5f)
{
    std::vector<float> sound ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
        sound[(size_t) i] = level * (float) std::sin (twoPi * hertz * i / rate);

    return sound;
}

std::vector<float> makeSaw (double hertz, int numSamples, float level = 0.5f)
{
    std::vector<float> sound ((size_t) numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto phase = hertz * i / rate;
        sound[(size_t) i] = level * (float) (2.0 * (phase - std::floor (phase)) - 1.0);
    }

    return sound;
}

// A sound through a filter of a shape.
std::vector<float> filtered (std::vector<float> sound, phazite::devices::dsp::FilterShape shape, double hertz, double q, double gain = 0.0)
{
    const auto coefficients = phazite::devices::dsp::makeFilter (shape, hertz, rate, q, gain);
    phazite::devices::dsp::Filter filter;

    for (auto& sample : sound)
        sample = filter.process (sample, coefficients);

    return sound;
}

double rmsDecibels (const juce::AudioBuffer<float>& sound, int channel, int from, int to)
{
    auto sum = 0.0;

    for (int i = from; i < to; ++i)
        sum += (double) sound.getSample (channel, i) * sound.getSample (channel, i);

    return juce::Decibels::gainToDecibels (std::sqrt (sum / std::max (1, to - from)), -200.0);
}

// The left side's level around a frequency over the second half, in decibels: through a narrow band-pass.
double levelNear (const juce::AudioBuffer<float>& sound, double hertz)
{
    std::vector<float> left ((size_t) sound.getNumSamples());

    for (int i = 0; i < sound.getNumSamples(); ++i)
        left[(size_t) i] = sound.getSample (0, i);

    left = filtered (left, phazite::devices::dsp::FilterShape::bandPass, hertz, 8.0);
    left = filtered (left, phazite::devices::dsp::FilterShape::bandPass, hertz, 8.0);
    auto sum = 0.0;

    for (auto i = left.size() / 2; i < left.size(); ++i)
        sum += (double) left[i] * left[i];

    return juce::Decibels::gainToDecibels (std::sqrt (sum / (double) (left.size() / 2)), -200.0);
}

// How strong one frequency is in the left side over the second half (its amplitude), from one bin of a Fourier transform.
double amplitudeAt (const juce::AudioBuffer<float>& sound, double hertz)
{
    const auto half = sound.getNumSamples() / 2;
    const auto length = std::max (1, (int) std::lround (std::floor (half * hertz / rate) * rate / hertz));
    auto re = 0.0, im = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto sample = (double) sound.getSample (0, half + i);
        re += sample * std::cos (twoPi * hertz * (half + i) / rate);
        im += sample * std::sin (twoPi * hertz * (half + i) / rate);
    }

    return 2.0 * std::sqrt (re * re + im * im) / length;
}
} // namespace

TEST_CASE ("The vocoder describes itself: a sidechain for its carrier, and its bands log-spaced from low to high")
{
    CHECK (devices::isValid (Vocoder::info));
    CHECK (Vocoder::info.hasSidechain);
    CHECK (devices::findParameter (Vocoder::info, devices::mixParameterID) >= 0);

    auto values = getDefaults();
    REQUIRE (Vocoder::getNumBands (values) == 16);
    CHECK (Vocoder::getBandFrequency (values, 0) == doctest::Approx (100.0));
    CHECK (Vocoder::getBandFrequency (values, 15) == doctest::Approx (10000.0));
    CHECK (Vocoder::getBandFrequency (values, 1) / Vocoder::getBandFrequency (values, 0)
           == doctest::Approx (Vocoder::getBandFrequency (values, 9) / Vocoder::getBandFrequency (values, 8)));

    // Formant moves where the carrier's bands are, not the voice's.
    values[Vocoder::formant] = 12.0f;
    CHECK (Vocoder::getCarrierBandFrequency (values, 3, rate) == doctest::Approx (2.0 * Vocoder::getBandFrequency (values, 3)));
    values[Vocoder::bands] = 5.0f;
    CHECK (Vocoder::getNumBands (values) == 40);
}

TEST_CASE ("The vocoder's bands follow the voice: up as it speaks, down as it stops, and only near what it says")
{
    auto values = getDefaults();
    setCarrier (values, Vocoder::Carrier::noise);

    // A 1 kHz tone for 200 ms, then silence.
    std::vector<float> voice (48000, 0.0f);

    for (int i = 0; i < 9600; ++i)
        voice[(size_t) i] = 0.3f * (float) std::sin (twoPi * 1000.0 * i / rate);

    const auto out = play (values, voice);
    const auto speaking = rmsDecibels (out, 0, 4800, 9600);
    CHECK (speaking > -40.0);
    CHECK (rmsDecibels (out, 0, 9600 + 4800, 9600 + 9600) < speaking - 30.0); // 100 ms after: five releases of 20 ms

    // Near 1 kHz, much louder than two octaves away, either way.
    std::vector<float> held (48000);

    for (int i = 0; i < 48000; ++i)
        held[(size_t) i] = 0.3f * (float) std::sin (twoPi * 1000.0 * i / rate);

    const auto steady = play (values, held);
    CHECK (levelNear (steady, 1000.0) > levelNear (steady, 4000.0) + 25.0);
    CHECK (levelNear (steady, 1000.0) > levelNear (steady, 250.0) + 25.0);
}

TEST_CASE ("The vocoder puts the voice's tone on the buzz, and Formant moves it up")
{
    auto values = getDefaults();
    values[Vocoder::unvoiced] = 0.0f; // the buzz alone

    // A voice of noise with a strong peak at 2 kHz: the buzz comes out louder near 2 kHz than near 500 Hz, in its harmonics.
    const auto voice = filtered (makeNoise (96000, 0.2f), phazite::devices::dsp::FilterShape::bell, 2000.0, 2.0, 18.0);
    const auto out = play (values, voice);
    CHECK (levelNear (out, 2000.0) > levelNear (out, 500.0) + 10.0);
    CHECK (amplitudeAt (out, 15.0 * c3) > 0.01); // a harmonic of C3, near 2 kHz

    // An octave up: the peak near 4 kHz.
    values[Vocoder::formant] = 12.0f;
    const auto moved = play (values, voice);
    CHECK (levelNear (moved, 4000.0) > levelNear (moved, 2000.0) + 6.0);
    CHECK (levelNear (moved, 4000.0) > levelNear (out, 4000.0) + 6.0);
}

TEST_CASE ("The vocoder is silent for silence, whatever its carrier, and with Track until a track feeds it")
{
    const std::vector<float> silence (48000, 0.0f);
    const auto saw = makeSaw (220.0, 48000);

    for (int carrier = 0; carrier <= (int) Vocoder::Carrier::track; ++carrier)
    {
        CAPTURE (carrier);
        auto values = getDefaults();
        values[Vocoder::carrier] = (float) carrier;
        const auto out = play (values, silence, &saw);
        CHECK (out.getMagnitude (0, 48000) < 1.0e-5f);
    }

    // Track, with a voice but no sidechain: nothing.
    auto values = getDefaults();
    setCarrier (values, Vocoder::Carrier::track);
    CHECK (play (values, makeNoise (48000)).getMagnitude (0, 48000) < 1.0e-5f);
}

TEST_CASE ("The vocoder plays the track that feeds its sidechain, at that track's pitch")
{
    auto values = getDefaults();
    setCarrier (values, Vocoder::Carrier::track);
    values[Vocoder::unvoiced] = 0.0f; // the track alone
    const auto saw = makeSaw (220.0, 96000);
    const auto out = play (values, makeNoise (96000, 0.3f), &saw);

    // The saw's harmonics, and nothing of the buzz's C3.
    const auto harmonics = amplitudeAt (out, 440.0) + amplitudeAt (out, 880.0);
    const auto buzz = amplitudeAt (out, 4.0 * c3) + amplitudeAt (out, 7.0 * c3);
    CHECK (juce::Decibels::gainToDecibels (harmonics / std::max (buzz, 1.0e-9)) > 20.0);
}

TEST_CASE ("The vocoder's Unvoiced lets an \"s\" through on a dull carrier, and leaves a vowel alone")
{
    // A low sine as the carrier: it has nothing up where an "s" is.
    auto values = getDefaults();
    setCarrier (values, Vocoder::Carrier::track);
    const auto dull = makeSine (110.0, 48000);
    const auto ess = filtered (filtered (makeNoise (48000, 0.3f), phazite::devices::dsp::FilterShape::highPass, 5000.0, 0.7),
                               phazite::devices::dsp::FilterShape::highPass, 5000.0, 0.7);
    const auto vowel = makeSaw (180.0, 48000, 0.3f);

    const auto level = [&values, &dull] (const std::vector<float>& voice, float unvoiced)
    {
        values[Vocoder::unvoiced] = unvoiced;
        const auto out = play (values, voice, &dull);
        return rmsDecibels (out, 0, 24000, 48000);
    };

    CHECK (level (ess, 1.0f) > level (ess, 0.0f) + 6.0);
    CHECK (std::abs (level (vowel, 1.0f) - level (vowel, 0.0f)) < 1.0);
}

TEST_CASE ("The vocoder is about as loud as the voice, with any carrier")
{
    const auto voice = makePinkNoise (96000, -18.0f);

    for (int carrier = 0; carrier <= (int) Vocoder::Carrier::voice; ++carrier)
    {
        CAPTURE (carrier);
        auto values = getDefaults();
        values[Vocoder::carrier] = (float) carrier;
        const auto out = play (values, voice);
        CHECK (std::abs (rmsDecibels (out, 0, 48000, 96000) + 18.0) < 3.0);
    }

    // A quiet track as the carrier is evened out too, and so is a lone tone, which no band raises around it.
    auto values = getDefaults();
    setCarrier (values, Vocoder::Carrier::track);
    const auto quietSaw = makeSaw (110.0, 96000, 0.02f);
    CHECK (std::abs (rmsDecibels (play (values, voice, &quietSaw), 0, 48000, 96000) + 18.0) < 4.0);
    const auto tone = makeSine (1000.0, 96000);
    CHECK (rmsDecibels (play (values, voice, &tone), 0, 48000, 96000) < -18.0);
}

TEST_CASE ("The vocoder's Stereo sends its bands left and right in turn, its mono sum as it was")
{
    auto values = getDefaults();
    const auto voice = makePinkNoise (48000, -18.0f);
    const auto narrow = play (values, voice);
    values[Vocoder::stereo] = 1.0f;
    const auto wide = play (values, voice);

    auto largest = 0.0f;

    for (int i = 0; i < 48000; ++i)
        largest = std::max (
            largest, std::abs ((wide.getSample (0, i) + wide.getSample (1, i)) - (narrow.getSample (0, i) + narrow.getSample (1, i))));

    CHECK (largest < 1.0e-4f);
    CHECK (std::abs (rmsDecibels (wide, 0, 24000, 48000) - rmsDecibels (wide, 1, 24000, 48000)) < 6.0);
    CHECK (rmsDecibels (narrow, 0, 0, 48000) == doctest::Approx (rmsDecibels (narrow, 1, 0, 48000)));
}

TEST_CASE ("The vocoder renders the same each time, and changes its bands while it plays without a jump")
{
    auto values = getDefaults();
    setCarrier (values, Vocoder::Carrier::noise);
    const auto voice = makePinkNoise (48000, -12.0f);
    const auto first = play (values, voice), second = play (values, voice);

    for (int channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (first.getReadPointer (channel), second.getReadPointer (channel), sizeof (float) * 48000) == 0);

    // From 16 bands to 40, a quarter of a second in: it fades out and in, and stays finite.
    Vocoder vocoder;
    vocoder.prepare (rate, blockSize);
    const auto changed = play (vocoder, values, voice, nullptr,
                               [&values] (int block)
                               {
                                   if (block == 25)
                                       values[Vocoder::bands] = 5.0f;
                               });

    for (int i = 0; i < 48000; ++i)
        REQUIRE (std::isfinite (changed.getSample (0, i)));

    CHECK (std::abs (changed.getSample (0, 12000 + (int) (0.01 * rate) - 1)) < 0.05f); // faded out as the bands change
    CHECK (rmsDecibels (changed, 0, 36000, 48000) > -30.0);                            // and back
    CHECK (vocoder.getBandLevel (39).get() > 0.0f);
}

TEST_CASE ("The vocoder keeps its bands when their number changes and changes back before the fade is over")
{
    auto values = getDefaults();
    Vocoder vocoder;
    vocoder.prepare (rate, blockSize);
    const auto voice = makePinkNoise (48000, -12.0f);
    juce::AudioBuffer<float> buffer (2, 100);

    // Blocks of 100 samples: 16 bands, 40 for one block (the fade, 480 samples, still going), then 16 again.
    for (int block = 0; block < 400; ++block)
    {
        values[Vocoder::bands] = block == 100 ? 5.0f : 2.0f;

        for (int channel = 0; channel < 2; ++channel)
            buffer.copyFrom (channel, 0, voice.data() + (block % 400) * 100, 100);

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = 100;
        context.parameters = values;
        vocoder.process (context);
    }

    CHECK (vocoder.getBandLevel (15).get() > 0.0f);
    CHECK (vocoder.getBandLevel (16).get() == doctest::Approx (0.0f));
}
