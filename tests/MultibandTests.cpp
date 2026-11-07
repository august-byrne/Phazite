// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's multiband (src/devices/dynamics/Multiband.h), in Maximus's image: its description, its curves' axis and shapes,
// what it does to quiet and loud sound, its band modes and solo, LMH Mix, the master band driven by the mid band, its
// saturation, stereo separation and releases.
#include "TestHelpers.h"
#include "devices/dynamics/Multiband.h"

#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Multiband;
namespace curves = devices::curves;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 480;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Multiband::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, std::string_view id, float value)
{
    const auto index = devices::findParameter (Multiband::info, id);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

// A stereo sound: its two channels.
struct Stereo
{
    std::vector<float> left, right;
};

// Plays the multiband in blocks of 10 ms on a sound (input gives each channel's sample at a time) and returns what comes out.
template <typename Input>
Stereo play (Multiband& multiband, const std::vector<float>& values, int numSamples, Input input)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    Stereo played;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
        {
            const auto [left, right] = input (done + i);
            buffer.setSample (0, i, left);
            buffer.setSample (1, i, right);
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = values;
        multiband.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

auto sine (double frequency, float decibels)
{
    const auto amplitude = juce::Decibels::decibelsToGain (decibels);
    return [frequency, amplitude] (int t)
    {
        const auto value = amplitude * (float) std::sin (twoPi * frequency * t / testRate);
        return std::pair { value, value };
    };
}

// A sine's level from its RMS, in decibels of its peak, over the second half of a sound.
float getLevel (const std::vector<float>& sound)
{
    auto sum = 0.0;

    for (auto i = sound.size() / 2; i < sound.size(); ++i)
        sum += (double) sound[i] * sound[i];

    return 20.0f * std::log10 (std::max (1.0e-9f, (float) std::sqrt (2.0 * sum / (double) (sound.size() - sound.size() / 2))));
}

float getPeak (const std::vector<float>& sound, size_t from)
{
    auto peak = 0.0f;

    for (auto i = from; i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// The level a sine at a frequency and level comes out at, for some parameters.
float levelOut (const std::vector<float>& values, double frequency, float decibels)
{
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    return getLevel (play (multiband, values, 48000, sine (frequency, decibels)).left);
}

// A band's curve set to limit at a level, as a flat line from it to the end.
void limitAt (std::vector<float>& values, const char* band, float decibels)
{
    const auto place = curves::axisForDecibels (decibels);
    const auto prefix = juce::String (band);
    set (values, (prefix + "Point2X").toStdString(), place);
    set (values, (prefix + "Point2Y").toStdString(), place);
    set (values, (prefix + "Point3Y").toStdString(), place);
}
} // namespace

TEST_CASE ("The multiband describes itself: global parameters, then each band's, its curve's points last")
{
    REQUIRE (devices::isValid (Multiband::info));
    CHECK (Multiband::info.parameters.size() == (size_t) Multiband::getBandParameter (Multiband::numBands, Multiband::mode));
    CHECK (devices::findParameter (Multiband::info, "lmhMix") == Multiband::lmhMix);
    CHECK (devices::findParameter (Multiband::info, "midPre") == Multiband::getBandParameter (Multiband::mid, Multiband::pre));
    CHECK (devices::findParameter (Multiband::info, "masterRelease2")
           == Multiband::getBandParameter (Multiband::master, Multiband::release2));
    CHECK (devices::findParameter (Multiband::info, "highPoint8Type")
           == Multiband::getPointParameter (Multiband::high, curves::maxPoints - 1, curves::pointType));
    CHECK (devices::findParameter (Multiband::info, devices::mixParameterID) < 0); // LMH Mix, before the master band, is its mix

    // Its latency is the most both look-aheads take, whatever they are set to, and the two saturations' at twice the rate.
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    const auto saturation = (int) std::lround (phazite::devices::dsp::AntialiasedShaper::getLatencySamples());
    CHECK (saturation > 0);
    CHECK (saturation < 8);
    CHECK (multiband.getLatencySamples() == 2 * (int) std::lround (Multiband::mostLookaheadSeconds * testRate) + 2 * saturation);
}

TEST_CASE ("The multiband's curves: Maximus's axis, a limiter at first, and each line's shape")
{
    // Silence at one end, 0 dB in the middle, +12 dB at the other.
    CHECK (curves::toAxis (0.0f) == doctest::Approx (0.0f));
    CHECK (curves::toAxis (1.0f) == doctest::Approx (0.5f));
    CHECK (curves::toAxis (4.0f) == doctest::Approx (1.0f));
    CHECK (curves::fromAxis (curves::toAxis (0.3f)) == doctest::Approx (0.3f));
    CHECK (curves::axisForDecibels (0.0f) == doctest::Approx (0.5f));

    // At first: level in, level out, up to 0 dB; then 0 dB.
    auto values = getDefaults();
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.3f) == doctest::Approx (0.3f));
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.8f) == doctest::Approx (0.5f));

    // A tension above 0 bends the line before a point up; a hold keeps the level before it; stairs step.
    set (values, "lowPoint2Tension", 0.5f);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.25f) > 0.3f);
    set (values, "lowPoint2Type", (float) curves::PointType::hold);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.25f) == doctest::Approx (0.0f));
    set (values, "lowPoint2Type", (float) curves::PointType::stairs);
    set (values, "lowPoint2Tension", -1.0f); // the fewest steps: two
    CHECK (curves::getStairs (-1.0f) == 2);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.2f) == doctest::Approx (0.0f));
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.3f) == doctest::Approx (0.5f));

    // A double curve passes through the middle of its line, as an S.
    set (values, "lowPoint2Type", (float) curves::PointType::doubleCurve);
    set (values, "lowPoint2Tension", 0.8f);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.25f) == doctest::Approx (0.25f).epsilon (0.01));
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.1f) < 0.1f);
    CHECK (curves::getAt (Multiband::getCurve (values, Multiband::low), 0.4f) > 0.4f);
}

TEST_CASE ("The multiband passes quiet sound as it came in, at any frequency, and limits loud sound at 0 dB")
{
    const auto values = getDefaults();

    for (const auto frequency : { 50.0, 120.0, 1000.0, 5000.0, 12000.0 })
    {
        INFO (frequency);
        CHECK (levelOut (values, frequency, -20.0f) == doctest::Approx (-20.0f).epsilon (0.02));
    }

    // 6 dB over: each band and the master limit at 0 dB.
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    const auto loud = play (multiband, values, 48000, sine (1000.0, 6.0f));
    CHECK (getLevel (loud.left) == doctest::Approx (0.0f).epsilon (0.5));
    CHECK (getPeak (loud.left, 24000) < juce::Decibels::decibelsToGain (1.0f));
}

TEST_CASE ("The multiband's bands: muted, off (the mid band taking its room) and soloed")
{
    auto values = getDefaults();

    // Muting the low band takes the bass away, and leaves the middle.
    set (values, "lowMode", (float) Multiband::Mode::muted);
    CHECK (levelOut (values, 50.0, -20.0f) < -45.0f);
    CHECK (levelOut (values, 1000.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));

    // With the low band off, the mid band covers the bass: its pre gain raises it.
    values = getDefaults();
    set (values, "midPre", 6.0f);
    CHECK (levelOut (values, 50.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));
    set (values, "lowMode", (float) Multiband::Mode::off);
    CHECK (levelOut (values, 50.0, -20.0f) == doctest::Approx (-14.0f).epsilon (0.05));

    // Soloing the high band leaves the bass out.
    values = getDefaults();
    set (values, "highSolo", 1.0f);
    CHECK (levelOut (values, 50.0, -20.0f) < -45.0f);
    CHECK (levelOut (values, 12000.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));
}

TEST_CASE ("The multiband's LMH Mix, its master band driven by the mid band, its saturation and stereo separation")
{
    // LMH Mix at 0: the bands' gains no longer reach what comes out; the master's still do.
    auto values = getDefaults();
    set (values, "lowPre", 12.0f);
    set (values, "midPre", 12.0f);
    set (values, "highPre", 12.0f);
    set (values, "lmhMix", 0.0f);
    CHECK (levelOut (values, 1000.0, -20.0f) == doctest::Approx (-20.0f).epsilon (0.05));
    set (values, "masterPost", -6.0f);
    CHECK (levelOut (values, 1000.0, -20.0f) == doctest::Approx (-26.0f).epsilon (0.05));

    // The master limiting at -30 dB turns a bass note down, unless the mid band, silent, drives it.
    values = getDefaults();
    limitAt (values, "master", -30.0f);
    CHECK (levelOut (values, 50.0, -10.0f) < -25.0f);
    set (values, "midDrivesMaster", 1.0f);
    CHECK (levelOut (values, 50.0, -10.0f) == doctest::Approx (-10.0f).epsilon (0.05));

    // Saturation: turned all the way, the master's rounds a loud sine's peaks off under its ceiling, -6 dB.
    values = getDefaults();
    limitAt (values, "master", 12.0f); // no limiting: the curve straight to the end
    limitAt (values, "low", 12.0f);
    limitAt (values, "mid", 12.0f);
    limitAt (values, "high", 12.0f);
    set (values, "masterSaturation", -1.0f);
    set (values, "masterCeiling", -6.0f);
    Multiband multiband;
    multiband.prepare (testRate, testBlockSize);
    const auto saturated = play (multiband, values, 48000, sine (1000.0, -3.0f));
    CHECK (getPeak (saturated.left, 24000) <= juce::Decibels::decibelsToGain (-6.0f) * 1.001f);
    CHECK (getPeak (saturated.left, 24000) > juce::Decibels::decibelsToGain (-12.0f));

    // Stereo separation at -1: mono, a sound in the left channel only coming out on both alike.
    values = getDefaults();
    set (values, "masterStereo", -1.0f);
    multiband.reset();
    const auto left = sine (1000.0, -20.0f);
    const auto mono = play (multiband, values, 24000, [&left] (int t) { return std::pair { left (t).first, 0.0f }; });
    CHECK (getLevel (mono.left) == doctest::Approx (getLevel (mono.right)).epsilon (0.01));
    CHECK (getLevel (mono.left) == doctest::Approx (-26.0f).epsilon (0.05)); // half of it each side
}

TEST_CASE ("The multiband's LMH Mix meets the bands in phase: half and half, a tone at either crossover keeps its level")
{
    // The bands add up to the sound through an all-pass at each crossover, half a turn there at 24 dB: had the dry sound not
    // gone through the same, half of each would cancel out there.
    for (const auto slope : { 0.0f, 1.0f })
    {
        auto values = getDefaults();
        set (values, "lmhMix", 0.5f);
        set (values, "lowSlope", slope);
        set (values, "highSlope", slope);

        for (const auto frequency : { 120.0, 1000.0, 5000.0 })
        {
            INFO (slope << " " << frequency);
            CHECK (levelOut (values, frequency, -20.0f) == doctest::Approx (-20.0f).epsilon (0.01));
        }
    }
}

TEST_CASE ("The multiband's releases: the first speeds up and is done in its time, the second slows into the gain")
{
    // A loud burst into the master's limiter at -20 dB, then quiet: how far down the gain still is a while later.
    const auto reductionAfter = [] (float release, float release2, int samplesAfter, float curve = 1.0f)
    {
        auto values = getDefaults();
        limitAt (values, "master", -20.0f);
        set (values, "masterRelease", release);
        set (values, "masterRelease2", release2);
        set (values, "masterCurve", curve);
        Multiband multiband;
        multiband.prepare (testRate, testBlockSize);
        play (multiband, values, 9600, sine (1000.0, -6.0f));
        const auto quiet = play (multiband, values, samplesAfter + 1200, sine (1000.0, -40.0f));
        auto peak = 0.0f;

        for (auto i = (size_t) samplesAfter; i < (size_t) samplesAfter + 1200; ++i)
            peak = std::max (peak, std::abs (quiet.left[i]));

        return -40.0f - 20.0f * std::log10 (std::max (peak, 1.0e-9f));
    };

    // The first release alone, 100 ms: still down halfway through (it starts slowly), back after it.
    CHECK (reductionAfter (100.0f, 0.0f, 2400) > 3.0f);
    CHECK (reductionAfter (100.0f, 0.0f, 7200) == doctest::Approx (0.0f).epsilon (0.5));

    // The second alone, 100 ms, through one stage (curve 1): further back at the same time, as it starts fast and slows.
    CHECK (reductionAfter (0.0f, 100.0f, 2400, 0.0f) < reductionAfter (100.0f, 0.0f, 2400) - 1.0f);
}
