// SPDX-License-Identifier: AGPL-3.0-or-later
// Stereo's DSP (src/devices/stereo/Stereo.h): nothing changed at its defaults, width through mid and side, a spread that
// decorrelates without touching the mono sum, the bass kept in mono, polarity and gain, the balance, Mono, the sides
// swapped and the DC filter, each switched without a click, a mono channel, and how alike the two sides are, coming in and
// going out.
#include "TestHelpers.h"
#include "devices/dsp/DcBlocker.h"
#include "devices/stereo/Stereo.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
using phazite::devices::Stereo;

constexpr double rate = 48000.0;
constexpr int blockSize = 512;

// How far the device's gains move in a sample at most, of the way to where they go: its 20 ms glide.
const auto glideStep = 1.0 - std::exp (-1.0 / (0.02 * rate));

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Stereo::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, const char* id, float value)
{
    values[(size_t) phazite::devices::findParameter (Stereo::info, id)] = value;
}

// Plays a buffer through a fresh Stereo, in blocks of 512, with the values given; from the block at switchAt on, with the
// values after (if any). Its first channel alone, as a mono channel, if asked.
juce::AudioBuffer<float> play (const juce::AudioBuffer<float>& in, const std::vector<float>& values, Stereo* kept = nullptr,
                               const std::vector<float>* after = nullptr, int switchAt = 0, bool mono = false)
{
    Stereo own;
    auto& stereo = kept != nullptr ? *kept : own;
    stereo.prepare (rate, blockSize);
    auto out = in;

    for (int start = 0; start < out.getNumSamples(); start += blockSize)
    {
        const auto length = std::min (blockSize, out.getNumSamples() - start);
        float* channels[] = { out.getWritePointer (0, start), mono ? nullptr : out.getWritePointer (1, start) };
        phazite::devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = mono ? 1 : 2;
        block.numSamples = length;
        block.parameters = after != nullptr && start >= switchAt ? *after : values;
        stereo.process (block);
    }

    return out;
}

// The values with one parameter set.
std::vector<float> with (std::vector<float> values, const char* id, float value)
{
    set (values, id, value);
    return values;
}

// A buffer of one value on each side, all through.
juce::AudioBuffer<float> makeConstant (int numSamples, float left, float right)
{
    juce::AudioBuffer<float> buffer (2, numSamples);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), left, numSamples);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (1), right, numSamples);
    return buffer;
}

// The largest change from one sample to the next, on either side.
float largestJump (const juce::AudioBuffer<float>& buffer)
{
    auto largest = 0.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int i = 1; i < buffer.getNumSamples(); ++i)
            largest = std::max (largest, std::abs (buffer.getSample (channel, i) - buffer.getSample (channel, i - 1)));

    return largest;
}

// Whether a side of two buffers holds the same samples, bit for bit, from a sample on.
bool sameSamples (const juce::AudioBuffer<float>& a, int channelOfA, const juce::AudioBuffer<float>& b, int channelOfB, int from = 0)
{
    for (int i = from; i < a.getNumSamples(); ++i)
        if (std::memcmp (a.getReadPointer (channelOfA, i), b.getReadPointer (channelOfB, i), sizeof (float)) != 0)
            return false;

    return true;
}

// The level of a side over its second half, in decibels.
double levelOf (const juce::AudioBuffer<float>& buffer, int channel)
{
    const auto half = buffer.getNumSamples() / 2;
    return juce::Decibels::gainToDecibels ((double) buffer.getRMSLevel (channel, half, buffer.getNumSamples() - half), -300.0);
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

TEST_CASE ("Stereo changes nothing at its defaults, and has no mix")
{
    const auto in = makeNoise (48000, false);
    const auto out = play (in, getDefaults());

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < in.getNumSamples(); ++i)
            REQUIRE (out.getSample (channel, i) == doctest::Approx (in.getSample (channel, i)).epsilon (1.0e-6));

    CHECK (phazite::devices::findParameter (Stereo::info, phazite::devices::mixParameterID) < 0);
}

TEST_CASE ("Stereo's width goes from mono to twice as wide, and never changes the mono sum")
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

TEST_CASE ("Stereo's spread makes stereo of a mono sound, unrelated sides, but not of its bass")
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

TEST_CASE ("Stereo keeps the bass in mono below its frequency, and the mono sum's level")
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

TEST_CASE ("Stereo flips each side's polarity first, then sets the gain")
{
    auto values = getDefaults();
    set (values, "invertLeft", 1.0f);
    set (values, "gain", 6.0206f);
    const auto in = makeNoise (48000, false);
    const auto out = play (in, values);
    CHECK (out.getSample (0, 30000) == doctest::Approx (-2.0f * in.getSample (0, 30000)).epsilon (0.001));
    CHECK (out.getSample (1, 30000) == doctest::Approx (2.0f * in.getSample (1, 30000)).epsilon (0.001));
}

TEST_CASE ("Stereo tells how alike its sides are, coming in and going out")
{
    Stereo stereo;
    auto values = getDefaults();

    play (makeNoise (48000, true), values, &stereo);
    CHECK (stereo.getOutputCorrelation().get() == doctest::Approx (1.0f).epsilon (0.01));

    play (makeSine (500.0, 48000, -1.0f), values, &stereo);
    CHECK (stereo.getInputCorrelation().get() == doctest::Approx (-1.0f).epsilon (0.01));

    play (makeNoise (96000, false), values, &stereo);
    CHECK (std::abs (stereo.getOutputCorrelation().get()) < 0.1f);

    // Silence: no reading.
    juce::AudioBuffer<float> silence (2, 96000);
    silence.clear();
    play (silence, values, &stereo);
    CHECK (stereo.getOutputCorrelation().get() == doctest::Approx (Stereo::noReading));

    // Width 0 makes the sides the same.
    set (values, "width", 0.0f);
    play (makeNoise (48000, false), values, &stereo);
    CHECK (stereo.getOutputCorrelation().get() == doctest::Approx (1.0f).epsilon (0.01));

    // What goes out is read after Mono: the sides the same.
    play (makeNoise (48000, false), with (getDefaults(), "mono", 1.0f), &stereo);
    CHECK (stereo.getOutputCorrelation().get() == doctest::Approx (1.0f).epsilon (0.01));
    CHECK (std::abs (stereo.getInputCorrelation().get()) < 0.1f);
}

TEST_CASE ("Stereo's balance fades the side it is turned away from and leaves the other as it was, without a click")
{
    const auto in = makeNoise (48000, false);
    const auto plain = play (in, getDefaults());

    // Turned all the way right: the left silent once it has glided there, the right as it was, bit for bit.
    auto out = play (in, with (getDefaults(), "balance", 1.0f));
    CHECK (out.getMagnitude (0, 24000, 24000) < 1.0e-5f * in.getMagnitude (0, 24000, 24000)); // -100 dB
    CHECK (sameSamples (out, 1, plain, 1));

    // All the way left: the mirror.
    out = play (in, with (getDefaults(), "balance", -1.0f));
    CHECK (out.getMagnitude (1, 24000, 24000) < 1.0e-5f * in.getMagnitude (1, 24000, 24000));
    CHECK (sameSamples (out, 0, plain, 0));

    // Half way right: the left at cos (pi / 4) of what it was.
    out = play (in, with (getDefaults(), "balance", 0.5f));
    CHECK (std::abs (levelOf (out, 0) - levelOf (plain, 0)
                     - juce::Decibels::gainToDecibels (std::cos (0.25 * juce::MathConstants<double>::pi)))
           < 0.01);
    CHECK (sameSamples (out, 1, plain, 1));

    // Back at the centre after it was turned: what it plays at its defaults, bit for bit, once it has glided back; that is,
    // what came in (the defaults' case above).
    const auto centred = getDefaults();
    const auto turned = with (getDefaults(), "balance", 1.0f);
    out = play (in, turned, nullptr, &centred, 10 * blockSize);
    CHECK_FALSE (sameSamples (out, 0, plain, 0));
    CHECK (sameSamples (out, 0, plain, 0, 10 * blockSize + 20000));
    CHECK (sameSamples (out, 1, plain, 1));

    // A step from the centre to all the way right, on a steady sound: no jump larger than its glide's.
    const auto steady = makeConstant (48000, 0.5f, 0.5f);
    out = play (steady, centred, nullptr, &turned, 10 * blockSize);
    CHECK (largestJump (out) <= 0.5f * (float) glideStep * 1.01f);
    CHECK (out.getSample (0, 47999) < 1.0e-5f);
}

TEST_CASE ("Stereo's Mono plays the middle on both sides, and switches without a click")
{
    const auto in = makeNoise (48000, false);
    const auto out = play (in, with (getDefaults(), "mono", 1.0f));
    auto largest = 0.0f;

    for (int i = 0; i < in.getNumSamples(); ++i)
    {
        REQUIRE (juce::exactlyEqual (out.getSample (0, i), out.getSample (1, i)));
        largest = std::max (largest, std::abs (out.getSample (0, i) - 0.5f * (in.getSample (0, i) + in.getSample (1, i))));
    }

    CHECK (largest < 1.0e-6f);

    // Switched on under the side alone: it glides to silence.
    const auto side = makeConstant (48000, 0.5f, -0.5f);
    const auto monoOn = with (getDefaults(), "mono", 1.0f);
    const auto steps = play (side, getDefaults(), nullptr, &monoOn, 10 * blockSize);
    CHECK (steps.getSample (0, 100) == doctest::Approx (0.5f));
    CHECK (largestJump (steps) <= 0.5f * (float) glideStep * 1.01f);
    CHECK (std::abs (steps.getSample (0, 47999)) < 1.0e-5f);
}

TEST_CASE ("Stereo swaps its sides, each with its own polarity, and switches without a click")
{
    const auto in = makeNoise (48000, false);
    const auto plain = play (in, getDefaults());

    // Swapped: each side what the other plays at the defaults, exactly; that is, what came in on the other.
    auto out = play (in, with (getDefaults(), "swap", 1.0f));

    for (int i = 0; i < in.getNumSamples(); ++i)
    {
        REQUIRE (juce::exactlyEqual (out.getSample (0, i), plain.getSample (1, i)));
        REQUIRE (juce::exactlyEqual (out.getSample (1, i), plain.getSample (0, i)));
        REQUIRE (out.getSample (0, i) == doctest::Approx (in.getSample (1, i)).epsilon (1.0e-6));
    }

    // A flip comes before the swap: Flip Left flips what came in on the left, which goes out on the right.
    auto values = with (getDefaults(), "swap", 1.0f);
    set (values, "invertLeft", 1.0f);
    out = play (in, values);
    CHECK (out.getSample (0, 30000) == doctest::Approx (in.getSample (1, 30000)).epsilon (1.0e-5));
    CHECK (out.getSample (1, 30000) == doctest::Approx (-in.getSample (0, 30000)).epsilon (1.0e-5));

    // Both flipped and swapped: each comes out on the other side, upside down.
    set (values, "invertRight", 1.0f);
    out = play (in, values);
    CHECK (out.getSample (0, 30000) == doctest::Approx (-in.getSample (1, 30000)).epsilon (1.0e-5));
    CHECK (out.getSample (1, 30000) == doctest::Approx (-in.getSample (0, 30000)).epsilon (1.0e-5));

    // Switched on and off a steady sound, its sides apart: each side glides to the other's.
    const auto steady = makeConstant (48000, 0.5f, -0.5f);
    const auto swapped = with (getDefaults(), "swap", 1.0f);
    out = play (steady, getDefaults(), nullptr, &swapped, 10 * blockSize);
    CHECK (largestJump (out) <= (float) glideStep * 1.01f);
    CHECK (out.getSample (0, 47999) == doctest::Approx (-0.5f));
    const auto defaults = getDefaults();
    out = play (steady, swapped, nullptr, &defaults, 10 * blockSize);
    CHECK (largestJump (out) <= (float) glideStep * 1.01f);
    CHECK (out.getSample (0, 47999) == doctest::Approx (0.5f));
}

TEST_CASE ("Stereo's DC filter takes off an offset, keeps 20 Hz, plays nothing while off, and switches without a click")
{
    const auto filterOn = with (getDefaults(), "dcFilter", 1.0f);

    // A 1 kHz tone over an offset, for two seconds: none of the offset in the second, at -80 dB.
    constexpr float offset = 0.3f;
    auto in = makeSine (1000.0, 96000);

    for (int channel = 0; channel < 2; ++channel)
        juce::FloatVectorOperations::add (in.getWritePointer (channel), offset, in.getNumSamples());

    auto out = play (in, filterOn);

    for (int channel = 0; channel < 2; ++channel)
    {
        double sum = 0.0;

        for (int i = 48000; i < 96000; ++i)
            sum += out.getSample (channel, i);

        CHECK (std::abs (sum / 48000.0) < offset * 1.0e-4);
    }

    // 20 Hz keeps its level within 0.1 dB.
    const auto low = makeSine (20.0, 96000);
    out = play (low, filterOn);
    CHECK (std::abs (levelOf (out, 0) - levelOf (low, 0)) < 0.1);
    CHECK (std::abs (levelOf (out, 1) - levelOf (low, 1)) < 0.1);

    // Switched off after a second, once it has faded out: what it plays at its defaults, bit for bit.
    const auto noise = makeNoise (96000, false);
    const auto plain = play (noise, getDefaults());
    const auto defaults = getDefaults();
    out = play (noise, filterOn, nullptr, &defaults, 94 * blockSize);
    CHECK_FALSE (sameSamples (out, 0, plain, 0));
    CHECK (sameSamples (out, 0, plain, 0, 94 * blockSize + 480));
    CHECK (sameSamples (out, 1, plain, 1, 94 * blockSize + 480));

    // Switched on and off a steady offset: no jump larger than its fade's over 5 ms.
    const auto steady = makeConstant (96000, 0.5f, 0.5f);
    const auto allowed = 0.5f / (float) (phazite::devices::dsp::DcBlocker::blendSeconds * rate) * 1.01f + 1.0e-6f;
    out = play (steady, getDefaults(), nullptr, &filterOn, 10 * blockSize);
    CHECK (largestJump (out) <= allowed);
    CHECK (std::abs (out.getSample (0, 95999)) < 1.0e-4f);
    out = play (steady, filterOn, nullptr, &defaults, 94 * blockSize);
    CHECK (largestJump (out) <= allowed);
    CHECK (out.getSample (0, 95999) == doctest::Approx (0.5f));
}

TEST_CASE ("Stereo on a mono channel: its left's polarity, the DC filter and the gain, and nothing else")
{
    const auto in = makeNoise (48000, false);
    const auto plain = play (in, getDefaults(), nullptr, nullptr, 0, true);

    // Everything that works on two sides set, and the right's flip: the same, bit for bit.
    auto values = getDefaults();

    for (const auto& [id, value] :
         { std::pair { "width", 0.0f }, std::pair { "spread", 1.0f }, std::pair { "balance", 1.0f }, std::pair { "mono", 1.0f },
           std::pair { "swap", 1.0f }, std::pair { "bassMono", 1.0f }, std::pair { "invertRight", 1.0f } })
        set (values, id, value);

    auto out = play (in, values, nullptr, nullptr, 0, true);
    CHECK (sameSamples (out, 0, plain, 0));

    // The left's flip and the gain.
    set (values, "invertLeft", 1.0f);
    set (values, "gain", 6.0206f);
    out = play (in, values, nullptr, nullptr, 0, true);
    CHECK (out.getSample (0, 30000) == doctest::Approx (-2.0f * in.getSample (0, 30000)).epsilon (0.001));

    // The DC filter takes an offset off.
    auto offset = makeSine (1000.0, 96000);
    juce::FloatVectorOperations::add (offset.getWritePointer (0), 0.3f, offset.getNumSamples());
    out = play (offset, with (getDefaults(), "dcFilter", 1.0f), nullptr, nullptr, 0, true);
    double sum = 0.0;

    for (int i = 48000; i < 96000; ++i)
        sum += out.getSample (0, i);

    CHECK (std::abs (sum / 48000.0) < 0.3 * 1.0e-4);

    // What came in on the right is left as it was.
    CHECK (sameSamples (out, 1, offset, 1));
}
