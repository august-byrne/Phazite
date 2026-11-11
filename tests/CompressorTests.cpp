// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (src/devices/dynamics/Compressor.h): compressing, its drawn curve as it plays, down and up, its attack
// and release times, input and makeup gains, stereo link, cuts, meters and characters, deaf to a sidechain; and ducking, under
// its key or its own sound, its threshold and depth, its attack in a straight line, its hold from the hit and while the key
// stays loud, its release and its curve, a new hit during the release, its key's cuts and Listen, and its meters.
#include "TestHelpers.h"
#include "devices/dsp/Math.h"
#include "devices/dynamics/Compressor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Compressor;
namespace curves = devices::curves;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// A curve's points into the parameters' values, the last one repeated in the places they leave.
void setCurve (std::vector<float>& values, const std::vector<curves::Point>& points)
{
    values[Compressor::points] = (float) points.size();

    for (int point = 0; point < curves::maxPoints; ++point)
        for (int field = 0; field < curves::pointFields; ++field)
            values[(size_t) (Compressor::points + curves::getPointParameter (point, (curves::PointField) field))] =
                points[(size_t) std::min (point, (int) points.size() - 1)][(size_t) field];
}

// How many decibels louder its curve puts out a level (below 0, quieter), once settled.
float getCurveGain (const std::vector<float>& values, float decibels)
{
    const auto out = curves::fromAxis (curves::getAt (Compressor::getCurve (values), curves::axisForDecibels (decibels)));
    return 20.0f * std::log10 (out) - decibels;
}

// Settings that make its timing easy to read: level in, level out, up to -26 dB, then a hard corner to a point at -6 dB in,
// which comes out 15 dB down; a quick attack and makeup at 0 dB.
std::vector<float> getTestValues()
{
    auto values = getDefaults();
    setCurve (values,
              { curves::makePoint (0.0f, 0.0f), curves::makePoint (curves::axisForDecibels (-26.0f), curves::axisForDecibels (-26.0f)),
                curves::makePoint (curves::axisForDecibels (-6.0f), curves::axisForDecibels (-21.0f)),
                curves::makePoint (1.0f, curves::axisForDecibels (-16.5f)) });
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

// Ducking, set so that its timing is easy to read: from -30 dB, 15 dB down, gripping in 2 ms (96 samples), held no longer
// than the key, and back in a straight line over 40 ms (1920 samples).
std::vector<float> getDuckValues()
{
    auto values = getDefaults();
    values[Compressor::mode] = (float) Compressor::Mode::duck;
    values[Compressor::threshold] = -30.0f;
    values[Compressor::depth] = 15.0f;
    values[Compressor::attack] = 2.0f;
    values[Compressor::hold] = 0.0f;
    values[Compressor::release] = 40.0f;
    values[Compressor::releaseCurve] = 0.0f;
    return values;
}

constexpr int duckAttackSamples = 96, duckReleaseSamples = 1920;
constexpr float duckSound = -20.0f; // the sound it ducks, steady

// How far it turned the sound down at each time, in decibels (0 or more), for the steady sound at duckSound.
std::vector<float> getDucked (const std::vector<float>& played)
{
    std::vector<float> ducked;

    for (size_t time = 0; time < played.size(); ++time)
        ducked.push_back (-gainAt (played, (int) time, duckSound));

    return ducked;
}

// Ducks the steady sound under a key, which is loud (-6 dB) where it says so: what it turned the sound down by, sample by
// sample.
template <typename Loud>
std::vector<float> duckUnder (const std::vector<float>& values, int numSamples, Loud loud)
{
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    const auto played = play (
        compressor, values, numSamples, [] (int t, int) { return steady (t, duckSound); },
        [loud] (int t, int) { return loud (t) ? steady (t, -6.0f) : 0.0f; });
    return getDucked (played[0]);
}

// The first time from a time on when the reduction is under a level.
int findUnder (const std::vector<float>& ducked, int from, float decibels)
{
    for (auto time = from; time < (int) ducked.size(); ++time)
        if (ducked[(size_t) time] < decibels)
            return time;

    return -1;
}
} // namespace

TEST_CASE ("The compressor's description holds together: its mode first, its curve's points after its knobs, at first 4:1 over -18 dB")
{
    REQUIRE (devices::isValid (Compressor::info));
    CHECK (Compressor::info.parameters.size() == (size_t) Compressor::mix + 1);
    CHECK (devices::findParameter (Compressor::info, "mode") == 0);                          // first: what it does, compress or duck
    CHECK (devices::findParameter (Compressor::info, "character") == 1);                     // then what kind of compressor it is
    CHECK (devices::findParameter (Compressor::info, "inputGain") == Compressor::inputGain); // then as the sound goes
    CHECK (devices::findParameter (Compressor::info, "threshold") == Compressor::threshold); // Duck's, which the curve isn't
    CHECK (devices::findParameter (Compressor::info, "depth") == Compressor::depth);
    CHECK (devices::findParameter (Compressor::info, "hold") == Compressor::hold);
    CHECK (devices::findParameter (Compressor::info, "releaseCurve") == Compressor::releaseCurve);
    CHECK (devices::findParameter (Compressor::info, "drive") == Compressor::drive);
    CHECK (devices::findParameter (Compressor::info, "keyLowCut") == Compressor::keyLowCut);
    CHECK (devices::findParameter (Compressor::info, "keyHighCut") == Compressor::keyHighCut);
    CHECK (devices::findParameter (Compressor::info, "points") == Compressor::points);
    CHECK (devices::findParameter (Compressor::info, "point8Type")
           == Compressor::points + curves::getPointParameter (curves::maxPoints - 1, curves::pointType));
    CHECK (devices::findParameter (Compressor::info, "mix") == Compressor::mix); // which its thin plug-in applies

    // Its modes, saved by their names, Compress at first; and Duck's settings at first: from -24 dB, 12 dB down, held no
    // longer than the key, back in a straight line.
    const auto& mode = Compressor::info.parameters[(size_t) Compressor::mode];
    CHECK (mode.unit == devices::Unit::choice);
    CHECK (devices::getChoiceName (mode, 0) == "Compress");
    CHECK (devices::getChoiceName (mode, 1) == "Duck");
    CHECK (Compressor::getMode (mode.defaultValue) == Compressor::Mode::compress);
    const auto values = getDefaults();
    CHECK (values[Compressor::threshold] == doctest::Approx (-24.0f));
    CHECK (values[Compressor::depth] == doctest::Approx (12.0f));
    CHECK (values[Compressor::hold] == doctest::Approx (0.0f));
    CHECK (values[Compressor::releaseCurve] == doctest::Approx (0.0f));

    for (const auto& [id, low, high] : { std::tuple ("threshold", -60.0f, 0.0f), std::tuple ("depth", 0.0f, 60.0f),
                                         std::tuple ("hold", 0.0f, 1000.0f), std::tuple ("releaseCurve", -1.0f, 1.0f) })
    {
        CAPTURE (id);
        const auto& parameter = Compressor::info.parameters[(size_t) devices::findParameter (Compressor::info, id)];
        CHECK (parameter.minimum == doctest::Approx (low));
        CHECK (parameter.maximum == doctest::Approx (high));
    }

    // At first, as a compressor with a threshold at -18 dB and a ratio of 4:1 would be set.
    const auto expected = curves::makeCompression (-18.0f, 4.0f);
    REQUIRE (values[Compressor::points] == doctest::Approx ((float) expected.size()));

    for (int point = 0; point < curves::maxPoints; ++point)
    {
        CAPTURE (point);

        for (int field = 0; field < curves::pointFields; ++field)
            CHECK (values[(size_t) (Compressor::points + curves::getPointParameter (point, (curves::PointField) field))]
                   == doctest::Approx (expected[(size_t) std::min (point, (int) expected.size() - 1)][(size_t) field]).epsilon (1.0e-5));
    }
}

TEST_CASE ("The compressor turns each level as its curve says once settled: down, up, or not at all")
{
    // Downward: 4:1 over -18 dB, 18 dB over coming out 4.5 dB over exactly at 0 dB, and nearly 4:1 between.
    auto values = getDefaults();
    CHECK (getCurveGain (values, -30.0f) == doctest::Approx (0.0f).epsilon (1.0e-4));
    CHECK (getCurveGain (values, 0.0f) == doctest::Approx (-13.5f).epsilon (0.001));
    CHECK (std::abs (getCurveGain (values, -9.0f) + 6.75f) < 0.1f); // nearly 4:1 between the points, its line bent

    // Each shape as it plays, once settled, at levels around it: what its curve says, to a twentieth of a decibel (its table
    // of gains is a little off the curve between its entries).
    for (const auto* name : { "Compress 4:1 over -18 dB", "Lift under -30 dB", "Expand under -24 dB", "Limit at 0 dB" })
    {
        CAPTURE (name);
        REQUIRE (curves::findShape (name) != nullptr);
        values = getTestValues();
        setCurve (values, curves::findShape (name)->points);

        for (const auto level : { -45.0f, -35.0f, -26.0f, -12.0f, -3.0f, 3.0f })
        {
            CAPTURE (level);
            Compressor compressor;
            compressor.prepare (testRate, testBlockSize);
            const auto played = play (compressor, values, 24000, [level] (int t, int) { return steady (t, level); });

            for (const auto& channel : played)
                CHECK (std::abs (gainAt (channel, 23999, level) - getCurveGain (values, level)) < 0.05f);
        }
    }

    // Upward: a quiet level comes out louder, and the meters say how far up.
    values = getTestValues();
    setCurve (values, curves::findShape ("Lift under -30 dB")->points);
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    play (compressor, values, 24000, [] (int t, int) { return steady (t, -45.0f); });
    CHECK (getCurveGain (values, -45.0f) > 3.0f);
    CHECK (compressor.getBoostMeter().take() == doctest::Approx (getCurveGain (values, -45.0f)).epsilon (0.01));
    CHECK (compressor.getReductionMeter().take() == doctest::Approx (0.0f));

    // A gate: under it, silence, or nearly.
    setCurve (values, curves::findShape ("Gate under -30 dB")->points);
    Compressor gate;
    gate.prepare (testRate, testBlockSize);
    const auto played = play (gate, values, 24000, [] (int t, int) { return steady (t, -36.0f); });
    CHECK (gainAt (played[0], 23999, -36.0f) < -60.0f);
}

TEST_CASE ("The compressor's attack and release take their times")
{
    auto values = getTestValues();
    values[Compressor::attack] = 10.0f;
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);

    // From silence to 20 dB over where its curve starts, which settles 15 dB down: after the attack time it has gone 63 % of the
    // way, as an analog compressor's time constant does.
    const auto onset = 4800;
    auto played = play (compressor, values, onset + 9600, [] (int t, int) { return t < onset ? 0.0f : steady (t, -6.0f); });
    const auto attackSamples = (int) (0.010 * testRate);
    CHECK (gainAt (played[0], onset + attackSamples - 1, -6.0f) == doctest::Approx (-15.0f * (1.0f - std::exp (-1.0f))).epsilon (0.01));
    CHECK (gainAt (played[0], onset + 9599, -6.0f) == doctest::Approx (-15.0f).epsilon (0.001));

    // Down to 20 dB under where its curve starts, which isn't turned down: after the release time 37 % of the reduction is left.
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

    // A new compressor starts at its makeup gain, and stays there under where its curve starts.
    auto played = play (compressor, values, 4800, [] (int t, int) { return steady (t, -46.0f); });
    CHECK (gainAt (played[0], 0, -46.0f) == doctest::Approx (6.0f).epsilon (0.001));
    CHECK (gainAt (played[0], 4799, -46.0f) == doctest::Approx (6.0f).epsilon (0.001));

    // A change glides rather than jumps.
    values[Compressor::makeup] = 12.0f;
    played = play (compressor, values, 9600, [] (int t, int) { return steady (t, -46.0f); });
    CHECK (gainAt (played[0], 0, -46.0f) < 6.1f);
    CHECK (gainAt (played[0], 9599, -46.0f) == doctest::Approx (12.0f).epsilon (0.001));
}

TEST_CASE ("The compressor's input gain drives the sound into it and into its detector; compressing, it doesn't hear a sidechain")
{
    // 10 dB under where its curve starts: untouched at 0 dB in; 16 dB more goes 6 dB over, which it turns down.
    auto values = getTestValues();
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    auto played = play (compressor, values, 12000, [] (int t, int) { return steady (t, -36.0f); });
    CHECK (std::abs (gainAt (played[0], 11999, -36.0f)) < 0.001f);

    values[Compressor::inputGain] = 16.0f;
    played = play (compressor, values, 24000, [] (int t, int) { return steady (t, -36.0f); });
    CHECK (gainAt (played[0], 0, -36.0f) < 1.0f); // it glides there, as the makeup does
    CHECK (getCurveGain (values, -20.0f) < -1.0f);
    CHECK (gainAt (played[0], 23999, -36.0f) == doctest::Approx (16.0f + getCurveGain (values, -20.0f)).epsilon (0.003));

    // A loud sidechain changes nothing while it compresses: it hears its own sound, quiet, and plays it as it would without
    // one, sample for sample, its key kept for when it ducks.
    values[Compressor::inputGain] = 6.0f;
    Compressor keyed, alone;
    keyed.prepare (testRate, testBlockSize);
    alone.prepare (testRate, testBlockSize);
    const auto quiet = [] (int t, int) { return steady (t, -46.0f); };
    played = play (keyed, values, 12000, quiet, [] (int t, int) { return steady (t, -6.0f); });
    CHECK (gainAt (played[0], 11999, -46.0f) == doctest::Approx (6.0f).epsilon (0.001));
    CHECK (played == play (alone, values, 12000, quiet));
}

TEST_CASE ("The compressor turns both channels alike, compressing and ducking; ducking, it hears its key when one feeds it")
{
    SUBCASE ("Ducking: a quiet sound turned down by a loud key, both sides alike")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played = play (
            compressor, getDuckValues(), 12000, [] (int t, int) { return steady (t, -46.0f); },
            [] (int t, int) { return steady (t, -6.0f); });

        for (const auto& channel : played)
            CHECK (gainAt (channel, 11999, -46.0f) == doctest::Approx (-15.0f).epsilon (0.001));
    }

    SUBCASE ("Ducking: a loud sound with a silent key isn't turned down")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played =
            play (compressor, getDuckValues(), 12000, [] (int t, int) { return steady (t, -6.0f); }, [] (int, int) { return 0.0f; });
        CHECK (std::abs (gainAt (played[0], 11999, -6.0f)) < 0.001f);
    }

    SUBCASE ("Ducking without a key: a loud sound turned down under itself")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played = play (compressor, getDuckValues(), 12000, [] (int t, int) { return steady (t, -6.0f); });
        CHECK (gainAt (played[0], 11999, -6.0f) == doctest::Approx (-15.0f).epsilon (0.001));
    }

    SUBCASE ("Compressing, loud on the left only: both sides go down alike")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played =
            play (compressor, getTestValues(), 12000, [] (int t, int side) { return steady (t, side == 0 ? -6.0f : -46.0f); });
        CHECK (gainAt (played[0], 11999, -6.0f) == doctest::Approx (-15.0f).epsilon (0.001));
        CHECK (gainAt (played[1], 11999, -46.0f) == doctest::Approx (-15.0f).epsilon (0.001));
    }

    SUBCASE ("Ducking, loud on the left only: both sides go down alike")
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto played = play (
            compressor, getDuckValues(), 12000, [] (int t, int) { return steady (t, duckSound); },
            [] (int t, int side) { return side == 0 ? steady (t, -6.0f) : 0.0f; });
        CHECK (gainAt (played[0], 11999, duckSound) == doctest::Approx (-15.0f).epsilon (0.001));
        CHECK (gainAt (played[1], 11999, duckSound) == doctest::Approx (-15.0f).epsilon (0.001));
    }
}

TEST_CASE ("The compressor's key low cut keeps bass from pushing it, and its meters show what it did")
{
    auto values = getTestValues();
    const auto bass = [] (int t, int) { return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 30.0 * t / testRate)); };

    // The most it turned a 30 Hz sine down by, over its last half second.
    const auto mostReduction = [&] (float lowCut)
    {
        values[Compressor::keyLowCut] = lowCut;
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        play (compressor, values, 24000, bass);
        compressor.getReductionMeter().take();
        play (compressor, values, 24000, bass);
        return compressor.getReductionMeter().take();
    };

    CHECK (mostReduction (20.0f) > 10.0f);
    CHECK (mostReduction (300.0f) < 0.1f);

    // Compressing, the high cut is Duck's and changes nothing: a bass heard through a 100 Hz high cut is turned down as far.
    values[Compressor::keyHighCut] = 100.0f;
    CHECK (mostReduction (20.0f) > 10.0f);
    values[Compressor::keyHighCut] = 20000.0f;

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

TEST_CASE ("Ducking, the compressor's key high cut lets a kick duck the sound, and not hats on the same track")
{
    // A key of two parts: a 50 Hz kick for the first half second, then 8 kHz hats as loud.
    auto values = getDuckValues();
    const auto key = [] (int t, int)
    {
        const auto hertz = t < 24000 ? 50.0 : 8000.0;
        return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * hertz * t / testRate));
    };

    const auto reductions = [&] (float highCut)
    {
        values[Compressor::keyHighCut] = highCut;
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        play (compressor, values, 24000, [] (int t, int) { return steady (t, -30.0f); }, key);
        const auto kick = compressor.getReductionMeter().take();
        play (
            compressor, values, 24000, [] (int t, int) { return steady (t, -30.0f); },
            [&key] (int t, int c) { return key (t + 24000, c); });
        compressor.getReductionMeter().take(); // the kick's release
        play (
            compressor, values, 24000, [] (int t, int) { return steady (t, -30.0f); },
            [&key] (int t, int c) { return key (t + 24000, c); });
        return std::pair { kick, compressor.getReductionMeter().take() };
    };

    // Off at first: both duck it. At 150 Hz, the kick does and the hats don't.
    const auto [kickOff, hatsOff] = reductions (20000.0f);
    CHECK (kickOff > 6.0f);
    CHECK (hatsOff > 6.0f);
    const auto [kickCut, hatsCut] = reductions (150.0f);
    CHECK (kickCut > 6.0f);
    CHECK (hatsCut < 1.0f);
}

TEST_CASE ("Ducking, the compressor plays its key while its editor listens, through its cuts, and stops half a second after it last asks")
{
    auto values = getDuckValues();
    values[Compressor::keyLowCut] = 300.0f;
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    CHECK_FALSE (compressor.overridesMix());

    // The sound is a tone; the key is a 1 kHz sine with a 30 Hz rumble, which the low cut takes away.
    const auto sound = [] (int t, int) { return steady (t, -30.0f); };
    const auto key = [] (int t, int)
    {
        const auto at = juce::MathConstants<double>::twoPi * t / testRate;
        return (float) (0.25 * std::sin (1000.0 * at) + 0.5 * std::sin (30.0 * at));
    };

    compressor.listenToKey();
    CHECK (compressor.isPlayingKey());
    const auto heard = play (compressor, values, 4800, sound, key);
    CHECK (compressor.overridesMix());
    auto loudest = 0.0f;

    for (int i = 2400; i < 4800; ++i)
        loudest = std::max (loudest, std::abs (heard[0][(size_t) i]));

    CHECK (loudest == doctest::Approx (0.25f).epsilon (0.1)); // the 1 kHz sine, and not the rumble

    // Half a second on by the clock, unless asked again, it stops, whether it played meanwhile or not; and fades back to the
    // sound.
    juce::Thread::sleep (600);
    CHECK_FALSE (compressor.isPlayingKey());
    play (compressor, values, 4800, sound, key);
    CHECK_FALSE (compressor.overridesMix());
}

TEST_CASE ("The compressor's characters hear differently: VCA the average, Opto letting go slowly, and each settles on the curve")
{
    // How far each turns down: a millisecond's burst 10 dB over where the curve starts every 1000 samples.
    const auto reductionOf = [] (Compressor::Character character, auto input, int numSamples)
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        auto values = getTestValues();
        values[Compressor::character] = (float) character;
        play (compressor, values, numSamples, input);
        return compressor.getReductionMeter().take();
    };

    const auto clicks = [] (int time, int) { return time % 1000 < 48 ? steady (time, -16.0f) : 0.0f; };
    const auto clean = reductionOf (Compressor::Character::clean, clicks, 10000);
    const auto vca = reductionOf (Compressor::Character::vca, clicks, 10000);
    CHECK (clean > 1.0f);
    CHECK (vca < 0.5f * clean); // a burst is short: its average is lower than its peaks

    // Each hears what comes in, so that once settled each follows the curve, whatever its character: a steady sine (whose
    // average an RMS ear hears as loud as its peaks) 20 dB over where the curve starts settles 15 dB down.
    const auto sine = [] (int time, int)
    { return toGain (-6.0f) * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * time / testRate); };

    for (const auto character : { Compressor::Character::clean, Compressor::Character::vca, Compressor::Character::fet,
                                  Compressor::Character::opto, Compressor::Character::tube })
    {
        CAPTURE ((int) character);
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        auto values = getTestValues();
        values[Compressor::character] = (float) character;
        values[Compressor::release] = 500.0f; // so that a peak ear's reduction barely ripples between the sine's peaks
        play (compressor, values, 48000, sine);
        compressor.getReductionMeter().take();
        play (compressor, values, 4800, sine);
        CHECK (compressor.getReductionMeter().take() == doctest::Approx (15.0f).epsilon (0.02));
    }

    // Opto lets go at the release set for the first half of the way, then slower: half a second after a loud part, it is
    // still further down than Clean.
    const auto releaseAfter = [] (Compressor::Character character)
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        auto values = getTestValues();
        values[Compressor::character] = (float) character;
        play (compressor, values, 24000, [] (int time, int) { return steady (time, -6.0f); });
        const auto played = play (compressor, values, 24000, [] (int time, int) { return steady (time, -40.0f); });
        return -gainAt (played[0], 23999, -40.0f);
    };

    CHECK (releaseAfter (Compressor::Character::clean) == doctest::Approx (0.0f).epsilon (0.05)); // 10 release times later
    CHECK (releaseAfter (Compressor::Character::opto) > 0.5f);
}

TEST_CASE ("FET and Tube colour the sound the further they turn it down, FET with odd harmonics and Tube with even ones")
{
    // A 200 Hz sine at a level through each, limited at 0 dB with no drive set: its loudest and quietest samples, once settled.
    const auto peaksOf = [] (Compressor::Character character, float decibels)
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        auto values = getTestValues();
        values[Compressor::character] = (float) character;
        values[Compressor::release] = 500.0f;
        setCurve (values, curves::findShape ("Limit at 0 dB")->points);
        const auto sine = [decibels] (int time, int)
        { return toGain (decibels) * (float) std::sin (juce::MathConstants<double>::twoPi * 200.0 * time / testRate); };
        auto played = play (compressor, values, 48000, sine)[0];
        played.erase (played.begin(), played.begin() + 36000);
        const auto [lowest, highest] = std::minmax_element (played.begin(), played.end());
        return std::pair { *lowest, *highest };
    };

    // Turned 12 dB down: Clean's sine as it was, only quieter; FET's flattened alike both ways; Tube's lopsided.
    const auto [cleanLow, cleanHigh] = peaksOf (Compressor::Character::clean, 12.0f);
    CHECK (-cleanLow == doctest::Approx (cleanHigh).epsilon (0.01));
    const auto [fetLow, fetHigh] = peaksOf (Compressor::Character::fet, 12.0f);
    CHECK (fetHigh < 0.97f * cleanHigh);
    CHECK (-fetLow == doctest::Approx (fetHigh).epsilon (0.01));
    const auto [tubeLow, tubeHigh] = peaksOf (Compressor::Character::tube, 12.0f);
    CHECK (std::abs (-tubeLow - tubeHigh) > 0.02f * tubeHigh);

    // Not turned down, none of them colours it.
    const auto [quietLow, quietHigh] = peaksOf (Compressor::Character::fet, -36.0f);
    const auto [quietCleanLow, quietCleanHigh] = peaksOf (Compressor::Character::clean, -36.0f);
    CHECK (quietHigh == doctest::Approx (quietCleanHigh).epsilon (0.001));
}

TEST_CASE ("The compressor's drive saturates what it puts out in its character's colour, and leaves quiet sound be")
{
    // A sine at a level, through a compressor that turns nothing down (a straight curve), driven all the way.
    const auto drive = [] (Compressor::Character character, float decibels)
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        auto values = getDefaults();
        values[Compressor::character] = (float) character;
        setCurve (values, curves::findShape ("No Compression")->points);
        values[Compressor::drive] = 1.0f;
        const auto sine = [decibels] (int time, int)
        { return toGain (decibels) * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * time / testRate); };
        auto played = play (compressor, values, 24000, sine)[0];
        played.erase (played.begin(), played.begin() + 12000); // after the DC blocker settles
        const auto [lowest, highest] = std::minmax_element (played.begin(), played.end());
        return std::pair { *lowest, *highest };
    };

    // Quiet, it passes nearly as it came: the curve is straight there and drive takes back its own gain.
    const auto [quietLow, quietHigh] = drive (Compressor::Character::clean, -40.0f);
    CHECK (quietHigh == doctest::Approx (toGain (-40.0f)).epsilon (0.02));

    // Loud, its peaks round off: lower than they came in, the same both ways for FET's odd harmonics...
    const auto [fetLow, fetHigh] = drive (Compressor::Character::fet, -3.0f);
    CHECK (fetHigh < 0.9f * toGain (-3.0f));
    CHECK (-fetLow == doctest::Approx (fetHigh).epsilon (0.02));

    // ...and lopsided for a tube's even ones, driven less hard, where they show most.
    const auto [tubeLow, tubeHigh] = drive (Compressor::Character::tube, -20.0f);
    CHECK (std::abs (-tubeLow - tubeHigh) > 0.1f * tubeHigh);
}

TEST_CASE ("A curve made as a threshold, a ratio and a knee set it follows them, though a straight line on its axes isn't a ratio")
{
    struct Setting
    {
        float threshold, ratio, knee, within;
    };

    // Exact at its points, and between them as near as its lines' tensions bend them: within a tenth of a decibel for the
    // presets' settings and the default, a fifth for 8:1 over -30 dB; a lower corner, further from the middle of Maximus's
    // axes, less closely.
    for (const auto& setting :
         { Setting { -18.0f, 4.0f, 0.0f, 0.1f }, Setting { -14.0f, 2.0f, 12.0f, 0.1f }, Setting { -24.0f, 10.0f, 6.0f, 0.1f },
           Setting { -20.0f, 3.0f, 10.0f, 0.1f }, Setting { -30.0f, 8.0f, 0.0f, 0.2f }, Setting { -36.0f, 4.0f, 0.0f, 0.6f } })
    {
        CAPTURE (setting.threshold);
        CAPTURE (setting.ratio);
        CAPTURE (setting.knee);
        auto values = getDefaults();
        setCurve (values, curves::makeCompression (setting.threshold, setting.ratio, setting.knee));
        const auto slope = 1.0f - 1.0f / setting.ratio;

        for (auto in = setting.threshold - setting.knee - 6.0f; in <= 12.0f; in += 0.25f)
        {
            CAPTURE (in);
            const auto over = in - setting.threshold;
            const auto out = 2.0f * over <= -setting.knee ? in
                             : 2.0f * over < setting.knee
                                 ? in - slope * (over + 0.5f * setting.knee) * (over + 0.5f * setting.knee) / (2.0f * setting.knee)
                                 : in - slope * over;
            CHECK (std::abs (in + getCurveGain (values, in) - out) < setting.within);
        }
    }

    // A straight line would be a decibel and more off at -36 dB.
    auto straight = curves::makeCompression (-36.0f, 4.0f);

    for (auto& point : straight)
        point[curves::pointTension] = 0.0f;

    auto values = getDefaults();
    setCurve (values, straight);
    CHECK (std::abs (getCurveGain (values, -18.0f) + 13.5f) > 1.0f);
}

//==============================================================================
TEST_CASE ("Ducking, the compressor turns the sound down by its depth once its key is 6 dB over its threshold, less the less it is over")
{
    // Under a steady key at each level, once settled: nothing under the threshold, the whole depth 6 dB over it and more, and
    // a straight share between (to a twentieth of a decibel: the key's low cut passes it a hair louder). Its meters: the key's
    // level, and how far down.
    for (const auto& [key, expected] : { std::pair (-40.0f, 0.0f), std::pair (-31.0f, 0.0f), std::pair (-29.0f, 2.5f),
                                         std::pair (-27.0f, 7.5f), std::pair (-24.0f, 15.0f), std::pair (-6.0f, 15.0f) })
    {
        CAPTURE (key);
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        const auto values = getDuckValues();
        const auto played = play (
            compressor, values, 12000, [] (int t, int) { return steady (t, duckSound); }, [key] (int t, int) { return steady (t, key); });
        CHECK (std::abs (-gainAt (played[0], 11999, duckSound) - expected) < 0.05f);
        compressor.getLevelMeter().take();
        compressor.getReductionMeter().take();
        play (compressor, values, 4800, [] (int t, int) { return steady (t, duckSound); }, [key] (int t, int) { return steady (t, key); });
        CHECK (std::abs (compressor.getLevelMeter().take() - key) < 0.05f);
        CHECK (std::abs (compressor.getReductionMeter().take() - expected) < 0.05f);
    }

    // A deeper depth goes as deep; and none of what only compressing uses (its curve, character, input gain, drive and makeup)
    // changes a sample of it.
    auto deeper = getDuckValues();
    deeper[Compressor::depth] = 36.0f;
    CHECK (duckUnder (deeper, 12000, [] (int) { return true; })[11999] == doctest::Approx (36.0f).epsilon (0.002));

    auto coloured = getDuckValues();
    setCurve (coloured, curves::findShape ("Limit at 0 dB")->points);
    coloured[Compressor::character] = (float) Compressor::Character::tube;
    coloured[Compressor::inputGain] = 12.0f;
    coloured[Compressor::drive] = 1.0f;
    coloured[Compressor::makeup] = 9.0f;
    const auto burst = [] (int t) { return t % 9600 < 2400; };
    CHECK (duckUnder (coloured, 24000, burst) == duckUnder (getDuckValues(), 24000, burst));
}

TEST_CASE ("Ducking, the compressor grips in a straight line in decibels, its whole depth over its attack, at once with none")
{
    // The key from silence to 24 dB over the threshold at a time: the reduction rises by the same step each sample, the
    // whole 15 dB over the 96 samples of its 2 ms attack.
    constexpr int onset = 4800;
    const auto ducked = duckUnder (getDuckValues(), onset + 4800, [] (int t) { return t >= onset; });
    CHECK (ducked[onset - 1] == doctest::Approx (0.0f));

    for (const auto steps : { 1, 24, 48, 72, duckAttackSamples })
    {
        CAPTURE (steps);
        CHECK (ducked[(size_t) (onset + steps - 1)] == doctest::Approx (15.0f * (float) steps / duckAttackSamples).epsilon (0.001));
    }

    CHECK (ducked[onset + duckAttackSamples + 100] == doctest::Approx (15.0f).epsilon (0.001));

    // Its shortest attack, a tenth of a millisecond, is all but at once.
    auto quick = getDuckValues();
    quick[Compressor::attack] = 0.1f;
    CHECK (duckUnder (quick, onset + 480, [] (int t) { return t >= onset; })[onset + 4] == doctest::Approx (15.0f).epsilon (0.001));
}

TEST_CASE ("Ducking, the compressor holds for its hold from the hit, and as long as its key stays over its threshold")
{
    constexpr int hit = 4800;
    constexpr int window = Compressor::keyPeakMilliseconds * 48 + 48; // how long the key's level takes to fall, at most

    // A short kick, 5 ms, with a hold of 100 ms: down until 100 ms after the hit, then letting go: a duck of a set length.
    auto values = getDuckValues();
    values[Compressor::hold] = 100.0f;
    auto ducked = duckUnder (values, hit + 9600, [] (int t) { return t >= hit && t < hit + 240; });
    CHECK (ducked[hit + 4799] == doctest::Approx (15.0f).epsilon (0.001));
    CHECK (findUnder (ducked, hit + duckAttackSamples, 14.999f) == hit + 4800);

    // Without one, it lets go as the kick's level falls under the threshold.
    values[Compressor::hold] = 0.0f;
    ducked = duckUnder (values, hit + 9600, [] (int t) { return t >= hit && t < hit + 240; });
    const auto letGo = findUnder (ducked, hit + duckAttackSamples, 14.999f);
    CHECK (letGo > hit + 240);
    CHECK (letGo <= hit + 240 + window);

    // A key held longer than the hold keeps it down for as long as it stays over the threshold: 300 ms here.
    values[Compressor::hold] = 100.0f;
    ducked = duckUnder (values, hit + 24000, [] (int t) { return t >= hit && t < hit + 14400; });
    CHECK (ducked[hit + 14399] == doctest::Approx (15.0f).epsilon (0.001));
    const auto heldUntil = findUnder (ducked, hit + duckAttackSamples, 14.999f);
    CHECK (heldUntil > hit + 14400);
    CHECK (heldUntil <= hit + 14400 + window);
}

TEST_CASE ("Ducking, the compressor lets go over its release, straight in decibels at its curve's middle, slow at first above it")
{
    // A short kick; from the sample it lets go, the release's share of the way back after each sample, eased by the curve.
    const auto releaseWith = [] (float curve)
    {
        auto values = getDuckValues();
        values[Compressor::releaseCurve] = curve;
        return duckUnder (values, 24000, [] (int t) { return t >= 4800 && t < 5040; });
    };

    for (const auto curve : { 0.0f, 0.6f, -0.6f })
    {
        CAPTURE (curve);
        const auto ducked = releaseWith (curve);
        const auto start = findUnder (ducked, 4800 + duckAttackSamples, 14.999f);
        REQUIRE (start > 0);

        for (const auto share : { 0.25, 0.5, 0.75 })
        {
            CAPTURE (share);
            const auto steps = (int) std::lround (share * duckReleaseSamples);
            const auto expected = 15.0f * (1.0f - (float) devices::dsp::getEasedProgress (share, curve));
            CHECK (ducked[(size_t) (start + steps - 1)] == doctest::Approx (expected).epsilon (0.002));
        }

        // Back whole after the release's 40 ms, not before.
        CHECK (ducked[(size_t) (start + duckReleaseSamples - 2)] > 0.0f);
        CHECK (std::abs (ducked[(size_t) (start + duckReleaseSamples - 1)]) < 0.001f);
    }

    // Straight in decibels at 0: half way back half way through. Slow at first above it (staying down, then springing back),
    // quick at first below it.
    const auto halfWay = [&releaseWith] (float curve)
    {
        const auto ducked = releaseWith (curve);
        const auto start = findUnder (ducked, 4800 + duckAttackSamples, 14.999f);
        return ducked[(size_t) (start + duckReleaseSamples / 2 - 1)];
    };

    CHECK (halfWay (0.0f) == doctest::Approx (7.5f).epsilon (0.002));
    CHECK (halfWay (0.6f) > 10.0f);
    CHECK (halfWay (-0.6f) < 5.0f);
}

TEST_CASE ("Ducking, a new hit during the compressor's release grips again from where it is")
{
    // A kick, then another half way through its release: from where the release had got to, up by the attack's step, to the
    // whole depth again, and held.
    auto values = getDuckValues();
    const auto first = duckUnder (values, 24000, [] (int t) { return t >= 4800 && t < 5040; });
    const auto start = findUnder (first, 4800 + duckAttackSamples, 14.999f);
    const auto again = start + duckReleaseSamples / 2;
    const auto ducked = duckUnder (values, 24000, [again] (int t) { return (t >= 4800 && t < 5040) || (t >= again && t < again + 240); });
    const auto from = ducked[(size_t) (again - 1)];
    CHECK (from == doctest::Approx (7.5f).epsilon (0.01));
    CHECK (ducked[(size_t) (again + 23)] == doctest::Approx (from + 24.0f * 15.0f / duckAttackSamples).epsilon (0.002));
    CHECK (ducked[(size_t) (again + duckAttackSamples)] == doctest::Approx (15.0f).epsilon (0.001));

    // Then it lets go again, over its whole release.
    const auto secondStart = findUnder (ducked, again + duckAttackSamples, 14.999f);
    REQUIRE (secondStart > again + 240);
    CHECK (ducked[(size_t) (secondStart + duckReleaseSamples / 2 - 1)] == doctest::Approx (7.5f).epsilon (0.002));
}

TEST_CASE ("Ducking, a kick's low notes don't let the compressor go between their cycles")
{
    // A 40 Hz kick, held 200 ms: its level, the loudest over the last 10 ms, stays over the threshold as its wave crosses 0, so
    // the sound stays down the whole time, without a ripple.
    auto values = getDuckValues();
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    const auto played = play (
        compressor, values, 24000, [] (int t, int) { return steady (t, duckSound); },
        [] (int t, int) { return t < 9600 ? 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 40.0 * t / testRate) : 0.0f; });
    const auto ducked = getDucked (played[0]);

    for (int t = 200; t < 9600; t += 7)
    {
        CAPTURE (t);
        CHECK (ducked[(size_t) t] == doctest::Approx (15.0f).epsilon (0.001));
    }
}
