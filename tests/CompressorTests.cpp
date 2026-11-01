// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's compressor (src/devices/dynamics/Compressor.h): its curve, as it reports it and as it plays, its attack and
// release times, input and makeup gains, sidechain, stereo link, low cut and meters.
#include "TestHelpers.h"
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
    CHECK (devices::findParameter (Compressor::info, "character") == 0);                     // first: what kind of compressor it is
    CHECK (devices::findParameter (Compressor::info, "inputGain") == Compressor::inputGain); // then as the sound goes
    CHECK (devices::findParameter (Compressor::info, "drive") == Compressor::drive);
    CHECK (devices::findParameter (Compressor::info, "threshold") == Compressor::threshold);
    CHECK (devices::findParameter (Compressor::info, "keyLowCut") == Compressor::keyLowCut);
    CHECK (devices::findParameter (Compressor::info, "keyHighCut") == Compressor::keyHighCut);
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

TEST_CASE ("The compressor's input gain drives the sound into it, and into its detector unless a sidechain feeds it")
{
    // 10 dB under the threshold: untouched at 0 dB in; 16 dB more goes 6 dB over, which 4:1 turns down by 4.5 dB.
    auto values = getTestValues();
    Compressor compressor;
    compressor.prepare (testRate, testBlockSize);
    auto played = play (compressor, values, 12000, [] (int t, int) { return steady (t, -36.0f); });
    CHECK (std::abs (gainAt (played[0], 11999, -36.0f)) < 0.001f);

    values[Compressor::inputGain] = 16.0f;
    played = play (compressor, values, 24000, [] (int t, int) { return steady (t, -36.0f); });
    CHECK (gainAt (played[0], 0, -36.0f) < 1.0f); // it glides there, as the makeup does
    CHECK (gainAt (played[0], 23999, -36.0f) == doctest::Approx (16.0f - 4.5f).epsilon (0.001));

    // With a sidechain, the sound goes in louder, and the key, which says how far down, is as it was.
    values[Compressor::inputGain] = 6.0f;
    Compressor ducking;
    ducking.prepare (testRate, testBlockSize);
    played = play (ducking, values, 12000, [] (int t, int) { return steady (t, -46.0f); }, [] (int t, int) { return steady (t, -6.0f); });
    CHECK (gainAt (played[0], 11999, -46.0f) == doctest::Approx (6.0f - 15.0f).epsilon (0.001));
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

TEST_CASE ("The compressor's key high cut lets a kick duck the sound, and not hats on the same track")
{
    // A key of two parts: a 50 Hz kick for the first half second, then 8 kHz hats as loud.
    auto values = getTestValues();
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

TEST_CASE ("The compressor plays its key while its editor listens, through its cuts, and stops half a second after it last asks")
{
    auto values = getTestValues();
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

TEST_CASE ("The compressor's characters: a tube's ratio grows over 15 dB, an Opto's knee is soft, and the curve reports them")
{
    auto values = getDefaults(); // -18 dB, 4:1, a 6 dB knee
    const auto slope = 0.75f;

    // Tube: nothing at the threshold, the reduction growing as the square of how far over it is, then as the ratio set.
    values[Compressor::character] = (float) Compressor::Character::tube;
    CHECK (Compressor::getReduction (values, -18.0f) == doctest::Approx (0.0f));
    CHECK (Compressor::getReduction (values, -20.0f) == doctest::Approx (0.0f)); // no knee below the threshold
    CHECK (Compressor::getReduction (values, -10.5f) == doctest::Approx (slope * 7.5f * 7.5f / 30.0f));
    CHECK (Compressor::getReduction (values, -3.0f) == doctest::Approx (slope * 7.5f));
    CHECK (Compressor::getReduction (values, 0.0f) == doctest::Approx (slope * 10.5f));

    // Opto: a knee at least 12 dB wide, so it starts 6 dB under the threshold however hard the knee is set.
    values[Compressor::character] = (float) Compressor::Character::opto;
    values[Compressor::knee] = 0.0f;
    CHECK (Compressor::getReduction (values, -24.5f) == doctest::Approx (0.0f));
    CHECK (Compressor::getReduction (values, -18.0f) == doctest::Approx (slope * 36.0f / 24.0f));
    CHECK (Compressor::getReduction (values, -6.0f) == doctest::Approx (slope * 12.0f));
}

TEST_CASE ("The compressor's characters hear differently: VCA the average, FET what it puts out, Opto letting go slowly")
{
    // How far each turns down: a millisecond's burst 10 dB over the threshold every 1000 samples, and a steady level 12 dB
    // over.
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

    // At a steady level 12 dB over the threshold, 4:1 settles 9 dB down; listening to what it puts out, FET settles where
    // what comes out is as far over as the curve allows, less far.
    const auto level = [] (int time, int) { return steady (time, -14.0f); };
    CHECK (reductionOf (Compressor::Character::clean, level, 24000) == doctest::Approx (9.0f).epsilon (0.02));
    const auto fet = reductionOf (Compressor::Character::fet, level, 24000);
    CHECK (fet > 1.0f);
    CHECK (fet < 8.0f);

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

TEST_CASE ("The compressor's drive saturates what it puts out in its character's colour, and leaves quiet sound be")
{
    // A sine at a level, through a compressor that turns nothing down (a threshold at the top), driven all the way.
    const auto drive = [] (Compressor::Character character, float decibels)
    {
        Compressor compressor;
        compressor.prepare (testRate, testBlockSize);
        auto values = getDefaults();
        values[Compressor::character] = (float) character;
        values[Compressor::threshold] = 0.0f;
        values[Compressor::knee] = 0.0f;
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
