// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DspProbe.h"
#include "devices/dsp/Random.h"
#include "model/devices/internal/Devices.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace phazite::test::probe
{
namespace
{
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr double songTempo = 120.0, barLength = 4.0; // 4/4

    // A stretch's length where none is given: to the sound's end.
    int lengthOf (const Sound& sound, int from, int numSamples)
    {
        return numSamples < 0 ? sound.getNumSamples() - from : std::min (numSamples, sound.getNumSamples() - from);
    }
} // namespace

//==============================================================================
Sound makeSilence (int numSamples, int numChannels)
{
    Sound sound (numChannels, numSamples);
    sound.clear();
    return sound;
}

Sound makeSine (double hertz, int numSamples, double sampleRate, float amplitude, int numChannels)
{
    Sound sound (numChannels, numSamples);

    for (int channel = 0; channel < numChannels; ++channel)
        for (int i = 0; i < numSamples; ++i)
            sound.setSample (channel, i, amplitude * (float) std::sin (twoPi * hertz * i / sampleRate));

    return sound;
}

Sound makeImpulse (int numSamples, int at, float amplitude, int numChannels)
{
    auto sound = makeSilence (numSamples, numChannels);

    for (int channel = 0; channel < numChannels; ++channel)
        sound.setSample (channel, at, amplitude);

    return sound;
}

Sound makeNoise (int numSamples, std::uint32_t seed, float amplitude, int numChannels)
{
    Sound sound (numChannels, numSamples);
    devices::dsp::Random random (seed);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < numChannels; ++channel)
            sound.setSample (channel, i, amplitude * random.nextBipolar());

    return sound;
}

Sound makeNoiseBelow (double hertz, int numSamples, double sampleRate, std::uint32_t seed, float amplitude, int numChannels)
{
    // White noise through a low-pass filter of 255 taps (a sinc under a Hann window), then as loud as asked.
    constexpr int reach = 127;
    const auto white = makeNoise (numSamples + 2 * reach, seed, 1.0f, numChannels);
    std::vector<double> taps ((size_t) (2 * reach + 1));

    for (int k = -reach; k <= reach; ++k)
    {
        const auto x = twoPi * hertz / sampleRate * k;
        const auto window = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * k / (reach + 1));
        taps[(size_t) (k + reach)] = window * (k == 0 ? 1.0 : std::sin (x) / x);
    }

    Sound sound (numChannels, numSamples);

    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* in = white.getReadPointer (channel);
        auto* out = sound.getWritePointer (channel);

        for (int i = 0; i < numSamples; ++i)
        {
            auto sum = 0.0;

            for (int k = 0; k <= 2 * reach; ++k)
                sum += taps[(size_t) k] * (double) in[i + k];

            out[i] = (float) sum;
        }
    }

    const auto peak = getPeak (sound);

    if (peak > 0.0)
        sound.applyGain (amplitude / (float) peak);

    return sound;
}

Sound join (const Sound& first, const Sound& second)
{
    Sound sound (first.getNumChannels(), first.getNumSamples() + second.getNumSamples());
    sound.clear();

    for (int channel = 0; channel < first.getNumChannels(); ++channel)
    {
        sound.copyFrom (channel, 0, first, channel, 0, first.getNumSamples());

        if (channel < second.getNumChannels())
            sound.copyFrom (channel, first.getNumSamples(), second, channel, 0, second.getNumSamples());
    }

    return sound;
}

Sound getPart (const Sound& sound, int from, int numSamples)
{
    Sound part (sound.getNumChannels(), numSamples);

    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        part.copyFrom (channel, 0, sound, channel, from, numSamples);

    return part;
}

//==============================================================================
int findNotFinite (const Sound& sound)
{
    for (int i = 0; i < sound.getNumSamples(); ++i)
        for (int channel = 0; channel < sound.getNumChannels(); ++channel)
            if (! std::isfinite (sound.getSample (channel, i)))
                return i;

    return -1;
}

double getPeak (const Sound& sound, int from, int numSamples)
{
    const auto length = lengthOf (sound, from, numSamples);
    auto peak = 0.0;

    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        for (int i = from; i < from + length; ++i)
            peak = std::max (peak, (double) std::abs (sound.getSample (channel, i)));

    return peak;
}

double getRms (const Sound& sound, int channel, int from, int numSamples)
{
    const auto length = lengthOf (sound, from, numSamples);
    auto sum = 0.0;

    for (int i = from; i < from + length; ++i)
        sum += (double) sound.getSample (channel, i) * sound.getSample (channel, i);

    return std::sqrt (sum / std::max (1, length));
}

double getMean (const Sound& sound, int channel, int from, int numSamples)
{
    const auto length = lengthOf (sound, from, numSamples);
    auto sum = 0.0;

    for (int i = from; i < from + length; ++i)
        sum += (double) sound.getSample (channel, i);

    return sum / std::max (1, length);
}

double getOffsetShare (const Sound& sound, int channel, int from, int numSamples)
{
    const auto length = lengthOf (sound, from, numSamples);
    auto sum = 0.0, squares = 0.0, weights = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto weight = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / length);
        const auto sample = (double) sound.getSample (channel, from + i);
        sum += weight * sample;
        squares += weight * sample * sample;
        weights += weight;
    }

    return squares > 0.0 ? (sum / weights) / std::sqrt (squares / weights) : 0.0;
}

double getLargestDifference (const Sound& a, const Sound& b, int from, int numSamples)
{
    const auto length = std::min (lengthOf (a, from, numSamples), lengthOf (b, from, numSamples));
    auto largest = 0.0;

    for (int channel = 0; channel < std::min (a.getNumChannels(), b.getNumChannels()); ++channel)
        for (int i = from; i < from + length; ++i)
            largest = std::max (largest, std::abs ((double) a.getSample (channel, i) - (double) b.getSample (channel, i)));

    return largest;
}

double inDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}

double getToneLevel (const Sound& sound, int channel, double hertz, double sampleRate, int from, int numSamples)
{
    const auto length = lengthOf (sound, from, numSamples);
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto s = std::sin (twoPi * hertz * i / sampleRate), c = std::cos (twoPi * hertz * i / sampleRate);
        const auto y = (double) sound.getSample (channel, from + i);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += y * s;
        yc += y * c;
    }

    const auto determinant = ss * cc - sc * sc;

    if (! (std::abs (determinant) > 0.0))
        return 0.0;

    const auto a = (ys * cc - yc * sc) / determinant, b = (yc * ss - ys * sc) / determinant;
    return std::sqrt (a * a + b * b);
}

std::vector<double> getResponse (const std::function<Sound (const Sound&)>& play, std::span<const double> hertz, double sampleRate,
                                 float amplitude, int settle, int measure)
{
    std::vector<double> response;

    for (const auto frequency : hertz)
    {
        const auto out = play (makeSine (frequency, settle + measure, sampleRate, amplitude));
        response.push_back (inDecibels (getToneLevel (out, 0, frequency, sampleRate, settle, measure) / (double) amplitude));
    }

    return response;
}

double onBin (double hertz, double sampleRate)
{
    auto bin = (int) std::lround (hertz * aliasWindow / sampleRate);

    if (bin % 2 == 0)
        ++bin;

    return bin * sampleRate / aliasWindow;
}

double getAliasesUnder (const Sound& sound, int channel, int from, double hertz, double sampleRate)
{
    REQUIRE (from + aliasWindow <= sound.getNumSamples());
    static const juce::dsp::FFT fft (juce::roundToInt (std::log2 ((double) aliasWindow)));
    std::vector<float> data ((size_t) aliasWindow * 2, 0.0f);
    std::copy_n (sound.getReadPointer (channel, from), aliasWindow, data.begin());
    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    const auto bin = (int) std::lround (hertz * aliasWindow / sampleRate);
    const auto tone = (double) data[(size_t) bin];
    auto loudest = 1.0e-12;

    for (int k = 1; k < aliasWindow / 2 && k * sampleRate / aliasWindow < 20000.0; ++k)
        if (k % bin != 0)
            loudest = std::max (loudest, (double) data[(size_t) k]);

    return 20.0 * std::log10 (std::max (tone, 1.0e-12) / loudest);
}

Delay findDelay (const Sound& in, const Sound& out, int channel, int longest)
{
    // What went in, but its last `longest + 1` samples, against as long a stretch of what came out, each delay later: every
    // delay is judged over as many samples. One delay before none and one after the longest are worked out too, for the
    // parabola's sake.
    const auto length = std::min (in.getNumSamples(), out.getNumSamples()) - longest - 1;
    REQUIRE (length > 0);
    const auto* x = in.getReadPointer (channel);
    const auto* y = out.getReadPointer (channel);

    const auto correlationAt = [&] (int delay)
    {
        auto sum = 0.0;

        for (int i = std::max (0, -delay); i < length; ++i)
            sum += (double) x[i] * (double) y[i + delay];

        return sum;
    };

    std::vector<double> correlation ((size_t) longest + 3);

    for (int delay = -1; delay <= longest + 1; ++delay)
        correlation[(size_t) (delay + 1)] = std::abs (correlationAt (delay));

    const auto peak = (int) (std::max_element (correlation.begin() + 1, correlation.end() - 1) - correlation.begin()) - 1;
    const auto before = correlation[(size_t) peak], at = correlation[(size_t) peak + 1], after = correlation[(size_t) peak + 2];
    const auto bend = before - 2.0 * at + after;

    auto energyIn = 0.0, energyOut = 0.0;

    for (int i = 0; i < length; ++i)
    {
        energyIn += (double) x[i] * (double) x[i];
        energyOut += (double) y[i + peak] * (double) y[i + peak];
    }

    Delay delay;
    delay.samples = peak + (bend < 0.0 ? std::clamp (0.5 * (before - after) / bend, -0.5, 0.5) : 0.0);
    delay.likeness = energyIn > 0.0 && energyOut > 0.0 ? at / std::sqrt (energyIn * energyOut) : 0.0;
    return delay;
}

//==============================================================================
std::vector<float> getDefaultValues (const devices::DeviceInfo& info)
{
    std::vector<float> values;
    values.reserve (info.parameters.size());

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void playAlone (devices::Processor& processor, std::span<const float> values, Sound& sound, int blockSize,
                std::span<const devices::NoteEvent> notes)
{
    REQUIRE (blockSize > 0);
    REQUIRE (sound.getNumChannels() >= 2);
    std::vector<devices::NoteEvent> blockNotes;
    size_t nextNote = 0;

    for (int start = 0; start < sound.getNumSamples(); start += blockSize)
    {
        const auto length = std::min (blockSize, sound.getNumSamples() - start);
        blockNotes.clear();

        for (; nextNote < notes.size() && notes[nextNote].sample < start + length; ++nextNote)
        {
            blockNotes.push_back (notes[nextNote]);
            blockNotes.back().sample = std::max (0, notes[nextNote].sample - start);
        }

        float* channels[] = { sound.getWritePointer (0, start), sound.getWritePointer (1, start) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        block.events = blockNotes;
        processor.process (block);
    }
}

//==============================================================================
namespace
{
    std::unique_ptr<devices::Processor> checked (std::unique_ptr<devices::Processor> processor)
    {
        REQUIRE (processor != nullptr);
        return processor;
    }
} // namespace

Device::Device (const devices::DeviceInfo& deviceInfo)
    : Device (deviceInfo, createOwnDevice (deviceInfo.typeID))
{
}

Device::Device (const devices::DeviceInfo& deviceInfo, std::unique_ptr<devices::Processor> processor)
    : info (deviceInfo),
      values (getDefaultValues (deviceInfo)),
      runner (deviceInfo, checked (std::move (processor)))
{
}

bool Device::has (std::string_view parameterID) const
{
    return devices::findParameter (info, parameterID) >= 0;
}

void Device::set (std::string_view parameterID, float value)
{
    const auto index = devices::findParameter (info, parameterID);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

void Device::automate (int parameter, float from, float to, std::int64_t overSamples)
{
    // In the song's beats, at the rate it is prepared at.
    const auto beats = (double) overSamples * songTempo / (60.0 * sampleRate);
    curves.push_back (std::make_unique<engine::Curve> (std::vector<engine::CurvePoint> { { 0.0, from, 0.0f }, { beats, to, 0.0f } }));
    automation.push_back ({ parameter, curves.back().get() });
}

void Device::clearAutomation()
{
    automation.clear();
    curves.clear();
}

void Device::prepare (double rate, int maximumBlockSize)
{
    // Its values first, as the model gives them to a device it makes (model/playback/internal/SongPlayer.h): one's latency
    // depends on them.
    runner.getProcessor().takeValuesBeforePrepare (values);
    runner.prepare (rate, maximumBlockSize);
    sampleRate = rate;
    longestBlock = maximumBlockSize;
    songSample = 0;
    valuesPlayed.clear();
}

void Device::reset()
{
    runner.reset();
    songSample = 0;
    valuesPlayed.clear();
}

engine::DeviceRunner::Block Device::makeBlock (bool hasSidechain)
{
    const auto beatsPerSample = songTempo / (60.0 * sampleRate);
    engine::DeviceRunner::Block block;
    block.values = values;
    // Whether a value was set since the last block, as the device's node tells its runner (engine/nodes/DeviceNode.h).
    block.valuesChanged = values != valuesPlayed;
    valuesPlayed = values;
    block.isPlaying = true;
    block.isRendering = isRendering;
    block.hasSidechain = hasSidechain;
    block.tempo = songTempo;
    block.beat = (double) songSample * beatsPerSample;
    block.bars = block.beat / barLength;
    block.songSample = songSample;
    block.barLength = barLength;
    block.barStart = std::floor (block.bars) * barLength;
    block.automation = automation;
    // Its automation is as much earlier as it plays late, as the engine has it (engine/core/Engine.cpp).
    block.automationBeat = automation.empty() ? block.beat : (double) (songSample - runner.getLatencySamples()) * beatsPerSample;
    block.sample = songSample;
    block.beatsPerSample = beatsPerSample;
    block.barsPerSample = beatsPerSample / barLength;
    return block;
}

void Device::play (Sound& sound, int blockSize, std::span<const devices::NoteEvent> notes)
{
    REQUIRE (blockSize > 0);
    REQUIRE (blockSize <= longestBlock); // prepare() it for blocks as long
    const auto hasSidechain = info.hasSidechain && sound.getNumChannels() >= 4;
    size_t nextNote = 0;

    for (int start = 0; start < sound.getNumSamples(); start += blockSize)
    {
        const auto length = std::min (blockSize, sound.getNumSamples() - start);
        blockNotes.clear();

        for (; nextNote < notes.size() && notes[nextNote].sample < start + length; ++nextNote)
        {
            blockNotes.push_back (notes[nextNote]);
            blockNotes.back().sample = std::max (0, notes[nextNote].sample - start);
        }

        auto block = makeBlock (hasSidechain);
        block.events = blockNotes;
        runner.process (sound, start, length, block);
        songSample += length;
    }
}

Sound Device::played (const Sound& sound, int blockSize, std::span<const devices::NoteEvent> notes)
{
    auto copy = sound;
    play (copy, blockSize, notes);
    return copy;
}

void Device::playNothing()
{
    // No samples of a buffer that has some, as the engine's are.
    auto buffer = makeSilence (16);
    runner.process (buffer, 0, 0, makeBlock (false));
}

devices::NoteEvent noteOn (int sample, int note, float velocity)
{
    return { devices::NoteEvent::Type::noteOn, sample, note, velocity };
}

devices::NoteEvent noteOff (int sample, int note)
{
    return { devices::NoteEvent::Type::noteOff, sample, note, 0.0f };
}
} // namespace phazite::test::probe
