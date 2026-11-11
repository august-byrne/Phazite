// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's reverb (src/devices/reverb/Reverb.h): its decay time, damping, pre-delay, width and low cut, its level, and
// that it plays the same every time; and its plate: its decay as measured at every size, diffusion and sample rate, its
// early, dense start, that it stays stable, and that a mode, a size or a decay changed while it plays doesn't click.
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

// Plays a new reverb in blocks; input (time in samples) gives both channels, and valuesAt (the block's first sample) the
// parameters' values for each block.
template <typename Input, typename Values>
Stereo playWith (Values valuesAt, int numSamples, Input input, double sampleRate = testRate)
{
    Reverb reverb;
    reverb.prepare (sampleRate, testBlockSize);
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    Stereo played;
    played.left.reserve ((size_t) numSamples);
    played.right.reserve ((size_t) numSamples);

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, input (done + i));

        const std::vector<float>& values = valuesAt (done);
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

template <typename Input>
Stereo play (const std::vector<float>& values, int numSamples, Input input, double sampleRate = testRate)
{
    return playWith ([&values] (int) -> const std::vector<float>& { return values; }, numSamples, input, sampleRate);
}

float impulse (int t)
{
    return t == 0 ? 1.0f : 0.0f;
}

// The time it takes to fade by 60 dB, from the slope of its energy decay curve between -5 and -35 dB (Schroeder's
// backward integration, as ISO 3382 measures a room's T30).
double measureDecay (const Stereo& response, double sampleRate = testRate)
{
    std::vector<double> remaining (response.left.size() + 1, 0.0);

    for (auto i = response.left.size(); i-- > 0;)
        remaining[i] = remaining[i + 1] + (double) response.left[i] * response.left[i] + (double) response.right[i] * response.right[i];

    const auto timeAt = [&remaining, sampleRate] (double decibels)
    {
        const auto level = remaining[0] * std::pow (10.0, decibels / 10.0);

        for (size_t at = 0; at < remaining.size(); ++at)
            if (remaining[at] <= level)
                return (double) at / sampleRate;

        return (double) remaining.size() / sampleRate;
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
std::vector<float> filtered (const std::vector<float>& sound, phazite::devices::dsp::FilterShape shape, double frequency,
                             double sampleRate = testRate)
{
    const auto coefficients = phazite::devices::dsp::makeFilter (shape, frequency, sampleRate, 0.70710678);
    phazite::devices::dsp::Filter filter;
    std::vector<float> out;

    for (const auto sample : sound)
        out.push_back (filter.process (sample, coefficients));

    return out;
}
// Both sides through a low-pass twice (24 dB an octave), for a decay measured where neither the damping nor anything else
// takes the treble.
Stereo below (const Stereo& sound, double frequency, double sampleRate)
{
    const auto twice = [&] (const std::vector<float>& side)
    {
        return filtered (filtered (side, phazite::devices::dsp::FilterShape::lowPass, frequency, sampleRate),
                         phazite::devices::dsp::FilterShape::lowPass, frequency, sampleRate);
    };

    return { twice (sound.left), twice (sound.right) };
}

std::vector<float> getPlate()
{
    auto values = getDefaults();
    values[Reverb::mode] = (float) Reverb::plate;
    return values;
}

float peakOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < to && i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

double correlationOf (const Stereo& sound, size_t from)
{
    auto both = 0.0, left = 0.0, right = 0.0;

    for (auto i = from; i < sound.left.size(); ++i)
    {
        both += (double) sound.left[i] * sound.right[i];
        left += (double) sound.left[i] * sound.left[i];
        right += (double) sound.right[i] * sound.right[i];
    }

    return both / std::sqrt (left * right);
}

// When the sound first rises over a thousandth of its peak, in seconds.
double firstSoundOf (const std::vector<float>& sound, double sampleRate = testRate)
{
    const auto threshold = 0.001f * peakOf (sound, 0, sound.size());
    const auto first = std::find_if (sound.begin(), sound.end(), [threshold] (float sample) { return std::abs (sample) > threshold; });
    return (double) std::distance (sound.begin(), first) / sampleRate;
}
} // namespace

TEST_CASE ("The reverb's description holds together, and its tail lasts its decay")
{
    REQUIRE (devices::isValid (Reverb::info));
    CHECK (devices::findParameter (Reverb::info, "decay") == Reverb::decay);
    CHECK (devices::findParameter (Reverb::info, "mix") == Reverb::mix);
    CHECK (Reverb::info.parameters[Reverb::mix].defaultValue == doctest::Approx (0.25f)); // on an insert

    // As it is made: a room, its diffusion at the middle and its high cut at 12 dB an octave, as before it had them.
    CHECK (devices::findParameter (Reverb::info, "mode") == Reverb::mode);
    CHECK (devices::getChoiceName (Reverb::info.parameters[Reverb::mode], Reverb::room) == "Room");
    CHECK (devices::getChoiceName (Reverb::info.parameters[Reverb::mode], Reverb::plate) == "Plate");
    CHECK (Reverb::info.parameters[Reverb::mode].defaultValue == (float) Reverb::room);
    CHECK (Reverb::info.parameters[Reverb::diffusion].defaultValue == 0.5f);
    CHECK (devices::getChoiceName (Reverb::info.parameters[Reverb::highCutSlope],
                                   (int) Reverb::info.parameters[Reverb::highCutSlope].defaultValue)
           == "12 dB");

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

TEST_CASE ("The reverb's high cut takes the treble off its tail, from its start, and changes nothing at its top")
{
    auto values = getDefaults();

    // How much treble is in the tail's first third of a second, in decibels.
    const auto treble = [&values] (float highCut)
    {
        values[Reverb::highCut] = highCut;
        const auto tail = play (values, (int) testRate / 2, impulse);
        return 10.0
               * std::log10 (
                   energyOf (filtered (tail.left, phazite::devices::dsp::FilterShape::highPass, 8000.0), 0, tail.left.size() / 3));
    };

    CHECK (treble (2000.0f) < treble (20000.0f) - 20.0);

    // At its top, off: the same tail, sample for sample, as a reverb without one.
    values[Reverb::highCut] = Reverb::info.parameters[Reverb::highCut].maximum;
    const auto off = play (values, 4800, impulse);
    values[Reverb::highCut] = Reverb::info.parameters[Reverb::highCut].defaultValue;
    const auto byDefault = play (values, 4800, impulse);
    CHECK (off.left == byDefault.left);
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

TEST_CASE ("The plate fades by 60 dB over its decay time, at every size, diffusion and sample rate")
{
    // Below 1.5 kHz, where the damping takes nothing, as a room's time is measured: the decay asked for, short and long, on a
    // small plate and a large one, smeared little and much, at each sample rate.
    struct Case
    {
        float decay, size, diffusion;
        double sampleRate;
    };

    // clang-format off
    const Case cases[] = {
        { 2.0f, 0.5f, 0.5f, 44100.0 }, { 2.0f, 0.5f, 0.5f, 48000.0 }, { 2.0f, 0.5f, 0.5f, 96000.0 },  { 2.0f, 0.5f, 0.5f, 192000.0 },
        { 0.3f, 0.5f, 0.5f, 48000.0 }, { 0.6f, 0.0f, 1.0f, 48000.0 }, { 1.0f, 0.5f, 1.0f, 44100.0 },  { 1.0f, 1.0f, 0.0f, 48000.0 },
        { 2.0f, 0.5f, 1.0f, 48000.0 }, { 2.0f, 0.0f, 0.0f, 48000.0 }, { 2.0f, 1.0f, 0.75f, 48000.0 }, { 4.0f, 0.25f, 0.25f, 96000.0 },
        { 5.0f, 0.5f, 1.0f, 44100.0 }, { 8.0f, 1.0f, 1.0f, 48000.0 }, { 8.0f, 0.0f, 0.5f, 44100.0 },  { 3.0f, 0.75f, 0.9f, 192000.0 },
    };
    // clang-format on

    for (const auto& each : cases)
    {
        CAPTURE (each.decay);
        CAPTURE (each.size);
        CAPTURE (each.diffusion);
        CAPTURE (each.sampleRate);
        auto values = getPlate();
        values[Reverb::decay] = each.decay;
        values[Reverb::size] = each.size;
        values[Reverb::diffusion] = each.diffusion;
        values[Reverb::predelay] = 0.0f;
        values[Reverb::damping] = 20000.0f;
        values[Reverb::lowCut] = 20.0f;
        const auto response = play (values, (int) (each.sampleRate * (each.decay * 1.6 + 0.5)), impulse, each.sampleRate);
        const auto measured = measureDecay (below (response, 1500.0, each.sampleRate), each.sampleRate);
        CHECK (measured == doctest::Approx (each.decay).epsilon (0.15));

        // And what its editor draws for the bass is the decay.
        CHECK (Reverb::getDecayAt (values, 50.0, each.sampleRate) == doctest::Approx (each.decay).epsilon (0.02));
    }
}

TEST_CASE ("The plate is the paper's at the size's middle, shrinks for a decay too short for it, and smears no longer than it lasts")
{
    auto values = getPlate();
    values[Reverb::decay] = 2.0f;
    auto setting = Reverb::getPlateSetting (values);
    CHECK (setting.scale == doctest::Approx (1.0));
    CHECK (setting.diffusion == doctest::Approx (0.5));
    CHECK (setting.gain > 0.3);
    CHECK (setting.gain < 0.6); // the paper's own decay, 0.5, is about 1.9 s
    CHECK (Reverb::getPlateSize (setting.scale) == doctest::Approx (0.5));

    // Larger and smaller with the size; a longer decay is a higher gain, always under 1.
    values[Reverb::size] = 1.0f;
    CHECK (Reverb::getPlateSetting (values).scale == doctest::Approx (1.6));
    values[Reverb::size] = 0.0f;
    CHECK (Reverb::getPlateSetting (values).scale == doctest::Approx (0.4));
    auto last = 0.0;

    for (const auto decay : { 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f })
    {
        for (const auto diffusion : { 0.0f, 0.5f, 1.0f })
        {
            values[Reverb::decay] = decay;
            values[Reverb::diffusion] = diffusion;
            const auto gain = Reverb::getPlateSetting (values).gain;
            CHECK (gain > 0.0);
            CHECK (gain < 0.999);

            if (diffusion == 0.5f)
            {
                CHECK (gain > last);
                last = gain;
            }
        }
    }

    // A decay too short for a plate of the size asked for: a smaller plate, which the editor says.
    values[Reverb::size] = 1.0f;
    values[Reverb::decay] = 0.3f;
    setting = Reverb::getPlateSetting (values);
    CHECK (setting.scale < 0.6);
    CHECK (Reverb::getPlateSize (setting.scale) < 0.2);

    // And full diffusion on a short decay: only as much as fades within it.
    values[Reverb::size] = 0.5f;
    values[Reverb::decay] = 1.0f;
    values[Reverb::diffusion] = 1.0f;
    CHECK (Reverb::getPlateSetting (values).diffusion < 0.7);
    values[Reverb::decay] = 4.0f;
    CHECK (Reverb::getPlateSetting (values).diffusion == doctest::Approx (1.0));
}

TEST_CASE ("The plate starts within milliseconds, where the room waits for its lines; its sides differ, and it plays the same every time")
{
    auto plate = getPlate();
    plate[Reverb::predelay] = 0.0f;
    auto room = getDefaults();
    room[Reverb::predelay] = 0.0f;

    // The plate's first sound comes 9 ms after the hit (the paper's first tap, 266 samples of its 29.8 kHz); the room's when
    // its shortest line comes round.
    const auto plateResponse = play (plate, (int) testRate, impulse), roomResponse = play (room, (int) testRate, impulse);
    CHECK (firstSoundOf (plateResponse.left) == doctest::Approx (0.009).epsilon (0.1));
    CHECK (firstSoundOf (plateResponse.right) == doctest::Approx (0.012).epsilon (0.1));
    CHECK (firstSoundOf (roomResponse.left) > 0.02);

    // Its pre-delay waits before that.
    plate[Reverb::predelay] = 100.0f;
    CHECK (firstSoundOf (play (plate, (int) testRate, impulse).left) == doctest::Approx (0.109).epsilon (0.05));
    plate[Reverb::predelay] = 0.0f;

    // And it sounds from then on: a hundredth of its first fifth of a second's energy has come before the room's first.
    const auto early = (size_t) (0.02 * testRate), fifth = (size_t) (0.2 * testRate);
    CHECK (energyOf (plateResponse.left, 0, early) > 0.01 * energyOf (plateResponse.left, 0, fifth));
    CHECK (energyOf (roomResponse.left, 0, early) < 1.0e-6 * energyOf (roomResponse.left, 0, fifth));

    // The same sound twice, bit for bit, from a burst of noise; its sides differ, and at no width they are the same.
    phazite::devices::dsp::Random random (7);
    std::vector<float> burst (4800);

    for (auto& sample : burst)
        sample = 0.5f * random.nextBipolar();

    const auto noise = [&burst] (int t) { return t < (int) burst.size() ? burst[(size_t) t] : 0.0f; };
    const auto once = play (plate, 48000, noise), twice = play (plate, 48000, noise);
    CHECK (once.left == twice.left);
    CHECK (once.right == twice.right);
    CHECK (std::abs (correlationOf (once, 4800)) < 0.3);
    plate[Reverb::width] = 0.0f;
    const auto narrow = play (plate, 48000, noise);
    CHECK (narrow.left == narrow.right);
}

TEST_CASE ("The plate's tail is about as loud as the room's, and its damping makes the treble fade sooner")
{
    // Steady noise through each as it is made, its spectrum falling past 1 kHz as music's does: the plate's tail within 2 dB
    // of the room's, so that a mode changed keeps the level.
    phazite::devices::dsp::Random random (3);
    std::vector<float> noise ((size_t) testRate * 3);
    auto falling = 0.0f;
    const auto share = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 1000.0 / testRate));

    for (auto& sample : noise)
        sample = falling += (random.nextBipolar() - falling) * share;

    const auto input = [&noise] (int t) { return noise[(size_t) t]; };
    const auto from = noise.size() * 2 / 3;
    const auto roomTail = play (getDefaults(), (int) noise.size(), input), plateTail = play (getPlate(), (int) noise.size(), input);
    const auto ratio = 10.0 * std::log10 (energyOf (plateTail.left, from, noise.size()) / energyOf (roomTail.left, from, noise.size()));
    MESSAGE ("the plate's tail against the room's, in decibels: " << ratio);
    CHECK (std::abs (ratio) < 2.0);

    // The damping: the treble fades sooner, as its editor draws it. The plate's sound passes its damping three times a second
    // or so, where the room's passes it twenty times and more, so its first moments keep the hit's treble: measured from two
    // thirds of a second to four thirds.
    auto values = getPlate();
    const auto trebleUnderBass = [&values] (float damping)
    {
        values[Reverb::damping] = damping;
        const auto tail = play (values, (int) testRate * 3 / 2, impulse);
        const auto start = (size_t) (testRate * 2 / 3), end = (size_t) (testRate * 4 / 3);
        const auto bass = energyOf (filtered (tail.left, phazite::devices::dsp::FilterShape::lowPass, 1000.0), start, end);
        const auto treble = energyOf (filtered (tail.left, phazite::devices::dsp::FilterShape::highPass, 4000.0), start, end);
        return 10.0 * std::log10 (bass / treble);
    };

    CHECK (trebleUnderBass (2000.0f) > trebleUnderBass (20000.0f) + 10.0);
    values[Reverb::damping] = 2000.0f;
    values[Reverb::decay] = 3.0f;
    CHECK (Reverb::getDecayAt (values, 8000.0, testRate) < 0.5 * 3.0);
    CHECK (Reverb::getDecayAt (values, 50.0, testRate) == doctest::Approx (3.0).epsilon (0.02));

    // The damping means the same in both modes: an octave under its frequency, where the tail's treble starts to fade sooner,
    // the plate's fades about as fast as the room's, at any size (within a fifth, as their editor draws them); above it, both
    // fade sooner still, the room's the faster, as its sound passes its damping many more times a second.
    for (const auto size : { 0.0f, 0.5f, 1.0f })
    {
        for (const auto damping : { 2000.0f, 6000.0f, 12000.0f })
        {
            CAPTURE (size);
            CAPTURE (damping);
            auto asRoom = getDefaults();
            asRoom[Reverb::size] = size;
            asRoom[Reverb::damping] = damping;
            asRoom[Reverb::decay] = 3.0f;
            auto asPlate = asRoom;
            asPlate[Reverb::mode] = (float) Reverb::plate;
            const auto below = 0.5 * damping;
            CHECK (Reverb::getDecayAt (asPlate, below, testRate)
                   == doctest::Approx (Reverb::getDecayAt (asRoom, below, testRate)).epsilon (0.2));
            CHECK (Reverb::getDecayAt (asPlate, damping, testRate) < Reverb::getDecayAt (asPlate, below, testRate));
            CHECK (Reverb::getDecayAt (asRoom, damping, testRate) < Reverb::getDecayAt (asPlate, damping, testRate));
        }
    }

    // Measured in the octave around 4 kHz: what the editor draws there, about.
    values[Reverb::damping] = 4000.0f;
    values[Reverb::lowCut] = 20.0f;
    values[Reverb::predelay] = 0.0f;
    const auto response = play (values, (int) (testRate * 4.0), impulse);
    const auto band = [] (const std::vector<float>& side)
    {
        return filtered (filtered (side, phazite::devices::dsp::FilterShape::bandPass, 4000.0),
                         phazite::devices::dsp::FilterShape::bandPass, 4000.0);
    };
    CHECK (measureDecay ({ band (response.left), band (response.right) })
           == doctest::Approx (Reverb::getDecayAt (values, 4000.0, testRate)).epsilon (0.3));
}

TEST_CASE ("The reverb's high cut falls by 6 or 12 dB an octave, as its slope says, in both modes")
{
    for (const auto mode : { Reverb::room, Reverb::plate })
    {
        CAPTURE ((int) mode);
        auto values = getDefaults();
        values[Reverb::mode] = (float) mode;
        values[Reverb::damping] = 20000.0f;

        // The tail's level in the band around 8 kHz, two octaves above the cut, in decibels.
        const auto level = [&values] (float highCut, int slope)
        {
            values[Reverb::highCut] = highCut;
            values[Reverb::highCutSlope] = (float) slope;
            const auto tail = play (values, (int) testRate / 2, impulse);
            const auto band = filtered (filtered (tail.left, phazite::devices::dsp::FilterShape::bandPass, 8000.0),
                                        phazite::devices::dsp::FilterShape::bandPass, 8000.0);
            return 10.0 * std::log10 (energyOf (band, 0, band.size()));
        };

        // A one-pole's 12 dB at four times its corner, and the second-order filter's 24, about (the band is wide).
        const auto open = level (20000.0f, 1);
        const auto gentle = level (2000.0f, 0) - open, steep = level (2000.0f, 1) - open;
        CHECK (gentle < -9.0);
        CHECK (gentle > -15.0);
        CHECK (steep < -19.0);
        CHECK (steep > -29.0);

        // At its top, off, whatever its slope.
        CHECK (level (20000.0f, 0) == doctest::Approx (open).epsilon (0.0001));
    }
}

TEST_CASE ("The reverb's diffusion smears what comes in: none leaves the hit's echoes sharp, in both modes")
{
    for (const auto mode : { Reverb::room, Reverb::plate })
    {
        CAPTURE ((int) mode);
        auto values = getDefaults();
        values[Reverb::mode] = (float) mode;
        values[Reverb::predelay] = 0.0f;
        values[Reverb::damping] = 20000.0f;

        // How far the first tenth of a second's highest sample stands over its level: sharp echoes stand high.
        const auto crest = [&values] (float diffusion)
        {
            values[Reverb::diffusion] = diffusion;
            const auto response = play (values, 4800, impulse);
            return (double) peakOf (response.left, 0, 4800) / std::sqrt (energyOf (response.left, 0, 4800) / 4800.0);
        };

        const auto sharp = crest (0.0f), middle = crest (0.5f), full = crest (1.0f);
        MESSAGE ("crest at no, middle and full diffusion: " << sharp << ", " << middle << ", " << full);
        CHECK (sharp > 1.5 * middle);
        CHECK (sharp > 1.5 * full);
    }
}

TEST_CASE ("The plate stays stable at its longest decay and fullest diffusion, at any size and sample rate")
{
    phazite::devices::dsp::Random random (11);
    std::vector<float> noise (96000);

    for (auto& sample : noise)
        sample = random.nextBipolar();

    for (const auto sampleRate : { 44100.0, 192000.0 })
    {
        for (const auto size : { 0.0f, 0.5f, 1.0f })
        {
            CAPTURE (sampleRate);
            CAPTURE (size);
            auto values = getPlate();
            values[Reverb::decay] = 20.0f;
            values[Reverb::diffusion] = 1.0f;
            values[Reverb::damping] = 20000.0f;
            values[Reverb::lowCut] = 20.0f;
            values[Reverb::size] = size;

            // Full-scale noise, then nothing for four seconds: the tail never grows, and falls.
            const auto total = (int) (sampleRate * 4.0) + (int) noise.size();
            const auto played =
                play (values, total, [&noise] (int t) { return t < (int) noise.size() ? noise[(size_t) t] : 0.0f; }, sampleRate);
            auto finite = true;

            for (const auto sample : played.left)
                finite = finite && std::isfinite (sample);

            CHECK (finite);
            const auto during = peakOf (played.left, 0, noise.size());
            const auto after = peakOf (played.left, noise.size() + (size_t) sampleRate, played.left.size());
            const auto atEnd = peakOf (played.left, played.left.size() - (size_t) (sampleRate / 2), played.left.size());
            MESSAGE ("peak during the noise, after it and at the end: " << during << ", " << after << ", " << atEnd);
            CHECK (during < 30.0f);
            CHECK (after <= during);
            CHECK (atEnd < after);
        }
    }
}

TEST_CASE ("A mode, a size, a decay or a diffusion changed while the reverb plays doesn't click")
{
    // A low tone, steady: whatever changes, nothing sharp comes out. Above 6 kHz a 110 Hz tone's reverb holds next to nothing,
    // and a click holds much.
    const auto tone = [] (int t) { return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 110.0 * t / testRate)); };
    const auto total = (int) testRate * 3, changeAt = (int) testRate * 3 / 2;

    const auto sharpestOf = [] (const Stereo& played)
    {
        const auto high = filtered (filtered (played.left, phazite::devices::dsp::FilterShape::highPass, 6000.0),
                                    phazite::devices::dsp::FilterShape::highPass, 6000.0);
        return peakOf (high, (size_t) testRate, high.size())
               / std::max (1.0e-9f, peakOf (played.left, (size_t) testRate, played.left.size()));
    };
    const auto sharpest = [&] (const std::vector<float>& before, const std::vector<float>& after)
    {
        return sharpestOf (playWith ([&] (int at) -> const std::vector<float>& { return at < changeAt ? before : after; }, total, tone));
    };

    const auto room = getDefaults(), plate = getPlate();
    const auto changed = [] (std::vector<float> values, int parameter, float value)
    {
        values[(size_t) parameter] = value;
        return values;
    };

    // 60 dB under what plays: a step in the sound would be far over it.
    constexpr auto quiet = 0.001f;
    CHECK (sharpest (room, plate) < quiet);
    CHECK (sharpest (plate, room) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::size, 1.0f)) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::size, 0.0f)) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::decay, 12.0f)) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::decay, 0.2f)) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::diffusion, 1.0f)) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::diffusion, 0.0f)) < quiet);
    CHECK (sharpest (room, changed (room, Reverb::diffusion, 0.0f)) < quiet);
    CHECK (sharpest (plate, changed (plate, Reverb::highCutSlope, 0.0f)) < quiet);

    // The high cut's slope changed while it cuts, both ways and in both modes.
    for (const auto& values : { room, plate })
    {
        const auto cut = changed (values, Reverb::highCut, 3000.0f);
        CHECK (sharpest (cut, changed (cut, Reverb::highCutSlope, 0.0f)) < quiet);
        CHECK (sharpest (changed (cut, Reverb::highCutSlope, 0.0f), cut) < quiet);
    }

    // The room asked for again a block after the plate was, as it fades out: back from where it was, without a click.
    CHECK (sharpestOf (playWith ([&] (int at) -> const std::vector<float>&
                                 { return at >= changeAt && at < changeAt + testBlockSize ? plate : room; }, total, tone))
           < quiet);

    // The mode changed, the other one plays: the plate's tail after the room's, at about its level.
    const auto played = playWith ([&] (int at) -> const std::vector<float>& { return at < changeAt ? room : plate; }, total, tone);
    const auto alone = play (plate, total, tone);
    const auto from = (size_t) total - (size_t) testRate / 2;
    const auto difference =
        10.0 * std::log10 (energyOf (played.left, from, played.left.size()) / energyOf (alone.left, from, alone.left.size()));
    CHECK (std::abs (difference) < 1.0);
}
