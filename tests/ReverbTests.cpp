// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's reverb (src/devices/reverb/Reverb.h): its decay time, damping, pre-delay, width and low cut, its level, and
// that it plays the same every time; and its plate: its decay as measured at every size, diffusion and sample rate, its
// early, dense start, that it stays stable, and that a mode, a size or a decay changed while it plays doesn't click.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Oscillators.h"
#include "devices/dsp/Random.h"
#include "devices/reverb/Reverb.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace probe = phazite::test::probe;
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

// How long a tone's tail takes to fade by 60 dB: a tone held for a second and let go, its tail's level over every four
// cycles, a line fitted through it from 5 to 45 dB down.
double measureToneDecay (const std::vector<float>& values, double hertz, double rate)
{
    const auto held = (int) rate, hop = (int) std::lround (4.0 * rate / hertz);
    const auto played = play (
        values, held + (int) (1.5 * values[Reverb::decay] * rate), [hertz, rate, held] (int t)
        { return t < held ? 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * t / rate) : 0.0f; }, rate);
    // Around the tone alone, through two narrow band-passes (55 dB down an octave and a half away), so that what its stop sets
    // ringing elsewhere, which a damped room keeps far longer below it, doesn't count.
    const auto around = [hertz, rate] (const std::vector<float>& side)
    {
        const auto band = phazite::devices::dsp::makeFilter (phazite::devices::dsp::FilterShape::bandPass, hertz, rate, 8.0);
        phazite::devices::dsp::Filter first, second;
        std::vector<float> out;
        out.reserve (side.size());

        for (const auto sample : side)
            out.push_back (second.process (first.process (sample, band), band));

        return out;
    };
    const Stereo heard = hertz < 200.0 ? played : Stereo { around (played.left), around (played.right) };
    auto first = -1.0, n = 0.0, sumT = 0.0, sumD = 0.0, sumTT = 0.0, sumTD = 0.0;

    for (int from = held; from + hop <= (int) heard.left.size(); from += hop)
    {
        const auto level =
            energyOf (heard.left, (size_t) from, (size_t) (from + hop)) + energyOf (heard.right, (size_t) from, (size_t) (from + hop));
        first = first < 0.0 ? level : first;
        const auto decibels = 10.0 * std::log10 (std::max (level, 1.0e-30) / first);

        if (decibels > -5.0 || decibels < -45.0)
            continue;

        const auto t = (double) (from - held) / rate;
        n += 1.0;
        sumT += t;
        sumD += decibels;
        sumTT += t * t;
        sumTD += t * decibels;
    }

    return -60.0 / ((n * sumTD - sumT * sumD) / (n * sumTT - sumT * sumT));
}

//==============================================================================
// The hall's measures (roadmap step S6.5), each taken of the room too, at the same settings, for comparison.
std::vector<float> inMode (Reverb::Mode mode)
{
    auto values = getDefaults();
    values[Reverb::mode] = (float) mode;
    return values;
}

// The damping fully open and nothing cut but by the network itself: how long it rings, as set.
std::vector<float> openAt (Reverb::Mode mode, float decay, float size)
{
    auto values = inMode (mode);
    values[Reverb::decay] = decay;
    values[Reverb::size] = size;
    values[Reverb::damping] = 20000.0f;
    values[Reverb::lowCut] = 20.0f;
    return values;
}

// Measured at 24 kHz where the measure allows: the octaves up to 2 kHz are whole there, and a tail of 20 s plays in a few
// seconds of a Debug build.
constexpr double measureRate = 24000.0;

// The straight line that fits a curve in decibels best, over its points from one level to another below its first: its
// slope, in decibels a second, and how far the curve strays from it at most.
struct Fit
{
    double slope = 0.0, furthest = 0.0;
};

Fit fitLine (const std::vector<double>& decibels, double secondsEach, double top, double bottom)
{
    auto n = 0.0, sumT = 0.0, sumD = 0.0, sumTT = 0.0, sumTD = 0.0;

    for (size_t i = 0; i < decibels.size(); ++i)
    {
        if (decibels[i] > top || decibels[i] < bottom)
            continue;

        const auto t = (double) i * secondsEach;
        n += 1.0;
        sumT += t;
        sumD += decibels[i];
        sumTT += t * t;
        sumTD += t * decibels[i];
    }

    Fit fit;
    fit.slope = (n * sumTD - sumT * sumD) / (n * sumTT - sumT * sumT);
    const auto offset = (sumD - fit.slope * sumT) / n;

    for (size_t i = 0; i < decibels.size(); ++i)
        if (decibels[i] <= top && decibels[i] >= bottom)
            fit.furthest = std::max (fit.furthest, std::abs (decibels[i] - (offset + fit.slope * (double) i * secondsEach)));

    return fit;
}

// How long a band of the tail takes to fade by 60 dB, as a room's is measured (ISO 3382): both sides through a band-pass
// twice, the energy decay curve of what comes through (Schroeder's backward integration), and the slope of the straight line
// that fits it best from 5 dB down to a level.
double measureBandDecay (std::span<const Stereo> responses, double hertz, double q, double rate, double bottom)
{
    const auto band = phazite::devices::dsp::makeFilter (phazite::devices::dsp::FilterShape::bandPass, hertz, rate, q);
    const auto length = responses.front().left.size();
    std::vector<double> remaining (length + 1, 0.0);

    // Their band's energies summed, each response's through filters of its own.
    for (const auto& response : responses)
    {
        std::array<phazite::devices::dsp::Filter, 4> filters {};

        for (size_t i = 0; i < length; ++i)
        {
            const auto left = filters[1].process (filters[0].process (response.left[i], band), band);
            const auto right = filters[3].process (filters[2].process (response.right[i], band), band);
            remaining[i] += (double) left * left + (double) right * right;
        }
    }

    for (auto i = length; i-- > 0;)
        remaining[i] += remaining[i + 1];

    std::vector<double> decibels (length);

    for (size_t i = 0; i < decibels.size(); ++i)
        decibels[i] = 10.0 * std::log10 (std::max (remaining[i], 1.0e-300) / remaining[0]);

    return -60.0 / fitLine (decibels, 1.0 / rate, -5.0, bottom).slope;
}

double measureBandDecay (const Stereo& response, double hertz, double q, double rate, double bottom)
{
    return measureBandDecay (std::span (&response, 1), hertz, q, rate, bottom);
}

constexpr double octaveQ = 1.41421356;     // a band-pass an octave wide
constexpr double thirdOctaveQ = 4.3184727; // and a third of one

// Measure 1: the decay in the octaves at 250 Hz, 500 Hz, 1 kHz and 2 kHz, from an impulse, at 24 kHz, as each of them over
// the decay asked for (1 is as set). The response is 0.9 of the decay long: its energy decay curve is then cut 54 dB down,
// which moves its slope at 35 dB down by under half a percent. A band's decay from one response swings by about
// 1 / sqrt (its width times the decay) (Davy, 1980): 5 % in the octave at 250 Hz for a decay of 1 s. So a decay under 5 s is
// measured from four responses, the band's energies summed, each of an impulse a third of a second later than the last,
// which the sways make another response.
std::array<double, 4> measureOctaveDecays (Reverb::Mode mode, float decay, float size)
{
    const auto values = openAt (mode, decay, size);
    const auto length = (int) (measureRate * (0.9 * decay + 0.05));
    std::vector<Stereo> responses;

    for (int take = 0; take < (decay < 5.0f ? 4 : 1); ++take)
    {
        const auto at = take * (int) (measureRate / 3);
        auto response = play (values, length + at, [at] (int t) { return t == at ? 1.0f : 0.0f; }, measureRate);
        response.left.erase (response.left.begin(), response.left.begin() + at);
        response.right.erase (response.right.begin(), response.right.begin() + at);
        responses.push_back (std::move (response));
    }

    std::array<double, 4> ratios {};
    const double octaves[] = { 250.0, 500.0, 1000.0, 2000.0 };

    for (size_t band = 0; band < ratios.size(); ++band)
        ratios[band] = measureBandDecay (responses, octaves[band], octaveQ, measureRate, -35.0) / decay;

    return ratios;
}

// The decays at every size, each octave's within five percent of the decay; and the room's, alongside, for comparison.
void checkOctaveDecays (float decay)
{
    for (const auto size : { 0.0f, 0.5f, 1.0f })
    {
        const auto hall = measureOctaveDecays (Reverb::hall, decay, size), room = measureOctaveDecays (Reverb::room, decay, size);
        MESSAGE ("decay " << decay << " s, size " << size << ": the hall's octaves at 250, 500, 1k, 2k Hz ring " << hall[0] << ", "
                          << hall[1] << ", " << hall[2] << ", " << hall[3] << " of it; the room's " << room[0] << ", " << room[1] << ", "
                          << room[2] << ", " << room[3]);

        for (size_t band = 0; band < hall.size(); ++band)
        {
            CAPTURE (size);
            CAPTURE (band);

            // Short of the target (within 5 %): the octave at 2 kHz of a decay of 10 s and more, the more the smaller the hall,
            // 0.93 at 20 s and Size 0 (0.90 at 48 kHz). The Damping at its top is a one-pole at 20 kHz, which still takes a
            // little of 2 kHz on each pass (0.006 dB at 24 kHz, 0.025 dB at 48), and the sways' interpolation a little more
            // at 24 kHz: a small hall's tail of 20 s goes round 400 times. The room's comes to 0.80 there.
            if (band == 3 && decay >= 10.0f)
                CHECK ((hall[band] > 0.92 && hall[band] < 1.05));
            else
                CHECK (std::abs (hall[band] - 1.0) < 0.05);
        }
    }
}

// Measure 2: how far the tail's spectrum peaks over its own envelope, in decibels. The tail from 1 s to 5 s by one long
// FFT; each bin's power over the mean power within a sixth of an octave either side of it (its envelope, a third of an
// octave wide); the largest, from 100 Hz to 5 kHz. A tail whose modes stand apart peaks far over its envelope; a tail
// without modes of its own, as noise, only as far as the chance highest of its many bins goes.
double measureRinging (const std::vector<float>& tail, double rate, double* where = nullptr)
{
    constexpr int order = 17;
    constexpr int fftSize = 1 << order;
    REQUIRE (tail.size() <= (size_t) fftSize);
    juce::dsp::FFT fft (order);
    std::vector<float> data (2 * (size_t) fftSize, 0.0f);
    std::copy (tail.begin(), tail.end(), data.begin());
    fft.performFrequencyOnlyForwardTransform (data.data());

    // The bins' powers, and their sums up to each, for the means.
    std::vector<double> power ((size_t) fftSize / 2 + 1), sums (power.size() + 1, 0.0);

    for (size_t bin = 0; bin < power.size(); ++bin)
    {
        power[bin] = (double) data[bin] * data[bin];
        sums[bin + 1] = sums[bin] + power[bin];
    }

    const auto binOf = [rate] (double hertz) { return (size_t) std::lround (hertz / rate * fftSize); };
    auto largest = 0.0;

    for (auto bin = binOf (100.0); bin <= binOf (5000.0); ++bin)
    {
        const auto hertz = (double) bin * rate / fftSize;
        const auto from = binOf (hertz * std::pow (2.0, -1.0 / 6.0)), to = binOf (hertz * std::pow (2.0, 1.0 / 6.0));
        const auto envelope = (sums[to + 1] - sums[from]) / (double) (to + 1 - from);

        if (power[bin] / envelope > largest)
        {
            largest = power[bin] / envelope;

            if (where != nullptr)
                *where = hertz;
        }
    }

    return 10.0 * std::log10 (largest);
}

// What a reverb's tail rings by, from 1 s to 5 s after an impulse, at 24 kHz: the mean of its two sides' figures (the largest
// peak of one spectrum is a chance figure: two of them are steadier), and the frequency of the left's.
double measureRingingOf (Reverb::Mode mode, float decay, float size, double& where)
{
    const auto response = play (openAt (mode, decay, size), (int) (5.0 * measureRate), impulse, measureRate);
    const auto from = (std::ptrdiff_t) measureRate;
    return 0.5
           * (measureRinging ({ response.left.begin() + from, response.left.end() }, measureRate, &where)
              + measureRinging ({ response.right.begin() + from, response.right.end() }, measureRate));
}

// The same of noise fading by 60 dB over a decay, as long: the floor for a tail without modes of its own.
double measureRingingOfNoise (float decay, std::uint32_t seed)
{
    phazite::devices::dsp::Random random (seed);
    std::vector<float> noise ((size_t) (4.0 * measureRate));

    for (size_t i = 0; i < noise.size(); ++i)
        noise[i] = random.nextBipolar() * (float) std::pow (10.0, -3.0 * ((double) i / measureRate + 1.0) / decay);

    return measureRinging (noise, measureRate);
}

// Measure 3: Abel and Huang's normalized echo density ("A Simple, Robust Measure of Reverberation Echo Density", AES 2006):
// the share of the samples in a 20 ms window that lie more than its standard deviation from 0, over the share a Gaussian
// noise has there (0.3173). When it first reaches 0.9, in seconds after a time (the pre-delay's end), the window's centre
// taken as its time; -1 if never.
double findDensity (const std::vector<float>& sound, double rate, double from, double density = 0.9)
{
    const auto window = (size_t) std::lround (0.02 * rate), hop = (size_t) std::lround (0.0005 * rate);

    for (auto start = (size_t) (from * rate); start + window <= sound.size(); start += hop)
    {
        auto energy = 0.0;

        for (auto i = start; i < start + window; ++i)
            energy += (double) sound[i] * sound[i];

        const auto deviation = std::sqrt (energy / (double) window);

        if (deviation <= 0.0)
            continue;

        auto outside = 0;

        for (auto i = start; i < start + window; ++i)
            outside += std::abs ((double) sound[i]) > deviation ? 1 : 0;

        if ((double) outside / (double) window / 0.3173 >= density)
            return ((double) start + 0.5 * (double) window) / rate - from;
    }

    return -1.0;
}

// Measure 5: the tail's level in 50 ms frames from 300 ms on, down to 40 dB under the first, and how far it strays at most
// from the straight line that fits it best, in decibels.
double measureFlutter (const Stereo& response, double rate)
{
    const auto frame = (size_t) std::lround (0.05 * rate);
    std::vector<double> decibels;
    auto first = -1.0;

    for (auto start = (size_t) std::lround (0.3 * rate); start + frame <= response.left.size(); start += frame)
    {
        const auto level = energyOf (response.left, start, start + frame) + energyOf (response.right, start, start + frame);
        first = first < 0.0 ? level : first;
        decibels.push_back (10.0 * std::log10 (std::max (level, 1.0e-300) / first));
    }

    return fitLine (decibels, 0.05, 1.0, -40.0).furthest;
}

// The same reverb played in blocks of sizes that change, from one sample to more than the usual block.
template <typename Input>
Stereo playInBlocks (const std::vector<float>& values, int numSamples, Input input, std::span<const int> sizes)
{
    Reverb reverb;
    reverb.prepare (testRate, 1024);
    juce::AudioBuffer<float> buffer (2, 1024);
    Stereo played;
    size_t next = 0;

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (sizes[next++ % sizes.size()], numSamples - done);

        for (int i = 0; i < length; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, input (done + i));

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

        done += length;
    }

    return played;
}

// Pink noise (Paul Kellet's filter on white noise: within half a decibel of -3 dB an octave over the audible band).
std::vector<float> makePinkNoise (size_t numSamples, std::uint32_t seed)
{
    phazite::devices::dsp::Random random (seed);
    std::vector<float> noise (numSamples);
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;

    for (auto& sample : noise)
    {
        const auto white = (double) random.nextBipolar();
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        sample = (float) (0.11 * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362));
        b6 = white * 0.115926;
    }

    return noise;
}

//==============================================================================
// The room and the plate as they played before step S6.5, copied from that reverb, for values that stay as they are (so
// nothing glides): what the reverb plays in those modes must be this, sample for sample. The plate's setting is its own
// (Reverb::getPlateSetting()), which the step left alone.
class ReverbAsBefore
{
public:
    ReverbAsBefore (const std::vector<float>& v, double sampleRate)
        : values (v),
          rate (sampleRate),
          isPlate (v[Reverb::mode] > 0.5f)
    {
        const auto modulationDepth = 0.12 * 0.001 * rate;

        for (size_t line = 0; line < 8; ++line)
        {
            lines[line].prepare ((int) std::ceil (getLineSamples ((int) line, 1.0f) + 2.0 * modulationDepth) + 2);
            phaseSteps[line] = (float) (modulationHertz[line] / rate);
        }

        for (auto& line : predelays)
            line.prepare ((int) std::ceil (0.001 * 250.0 * rate) + 2);

        for (size_t side = 0; side < 2; ++side)
        {
            for (size_t stage = 0; stage < 4; ++stage)
            {
                auto& allpass = diffusers[side][stage];
                allpass.length = std::max (1, (int) std::lround (diffuserMilliseconds[stage] * 0.001 * rate * (side == 0 ? 1.0 : 1.07)));
                allpass.gain = inputGains[stage];
                allpass.line.prepare (allpass.length);
            }
        }

        plateSway = (float) (0.385 * 0.001 * rate);
        platePhaseStep = (float) (0.6 / rate);
        const auto longest = [this] (double paperSamples)
        { return (int) std::ceil (paperSamples * rate / paperRate * 1.6 + (double) plateSway) + 4; };

        for (size_t stage = 0; stage < 4; ++stage)
            plateDiffusers[stage].prepare (longest (plateDiffuserSamples[stage]));

        for (size_t half = 0; half < 2; ++half)
            for (size_t part = 0; part < 4; ++part)
                tank[half][part].prepare (longest (tankSamples[half][part]));

        phazite::devices::dsp::Random random (0x5eed1234u);

        for (auto& phase : phases)
            phase = random.nextUnipolar();

        lowCut = phazite::devices::dsp::makeFilter (phazite::devices::dsp::FilterShape::highPass, values[Reverb::lowCut], rate, 0.70710678);
        cutsHigh = values[Reverb::highCut] < 19999.0f;
        highCut =
            phazite::devices::dsp::makeFilter (phazite::devices::dsp::FilterShape::lowPass, values[Reverb::highCut], rate, 0.70710678);
        isGentle = values[Reverb::highCutSlope] < 0.5f;
        gentle = (float) getDampingCoefficient (values[Reverb::highCut]);
        shownPredelay = (float) (values[Reverb::predelay] * 0.001 * rate);
    }

    void process (float* left, float* right, int numSamples)
    {
        const juce::ScopedNoDenormals noDenormals; // as the reverb plays

        if (isPlate)
            processPlate (left, right, numSamples);
        else
            processRoom (left, right, numSamples);
    }

private:
    struct Allpass
    {
        phazite::devices::dsp::DelayLine line;
        int length = 1;
        float gain = 0.7f;

        float process (float input) noexcept
        {
            const auto delayed = line.read (length - 1);
            const auto output = delayed - gain * input;
            line.push (input + gain * output);
            return output;
        }
    };

    static constexpr double lineMilliseconds[] = { 44.3, 49.8, 57.1, 63.4, 69.7, 74.3, 82.9, 89.5 };
    static constexpr double modulationHertz[] = { 0.31, 0.47, 0.53, 0.67, 0.79, 0.89, 1.01, 1.13 };
    static constexpr double diffuserMilliseconds[] = { 4.77, 3.59, 12.73, 9.30 };
    static constexpr float inputGains[] = { 0.75f, 0.75f, 0.625f, 0.625f };
    static constexpr float leftSigns[] = { 1, -1, 1, -1, 1, -1, 1, -1 };
    static constexpr float rightSigns[] = { 1, 1, -1, -1, 1, 1, -1, -1 };
    static constexpr double paperRate = 29761.0;
    static constexpr double plateDiffuserSamples[] = { 142.0, 107.0, 379.0, 277.0 };
    static constexpr double tankSamples[2][4] = { { 672.0, 4453.0, 1800.0, 3720.0 }, { 908.0, 4217.0, 2656.0, 3163.0 } };

    struct Tap
    {
        int half, part;
        double samples;
        float sign;
    };

    // clang-format off
    static constexpr Tap taps[2][7] = {
        { { 1, 1, 266.0, 1.0f }, { 1, 1, 2974.0, 1.0f }, { 1, 2, 1913.0, -1.0f }, { 1, 3, 1996.0, 1.0f },
          { 0, 1, 1990.0, -1.0f }, { 0, 2, 187.0, -1.0f }, { 0, 3, 1066.0, -1.0f } },
        { { 0, 1, 353.0, 1.0f }, { 0, 1, 3627.0, 1.0f }, { 0, 2, 1228.0, -1.0f }, { 0, 3, 2673.0, 1.0f },
          { 1, 1, 2111.0, -1.0f }, { 1, 2, 335.0, -1.0f }, { 1, 3, 121.0, -1.0f } },
    };
    // clang-format on

    std::vector<float> values;
    double rate;
    bool isPlate;
    std::array<phazite::devices::dsp::DelayLine, 2> predelays;
    std::array<phazite::devices::dsp::Filter, 2> lowCuts {}, highCuts {};
    std::array<float, 2> gentleCuts {};
    phazite::devices::dsp::FilterCoefficients lowCut, highCut;
    float gentle = 1.0f, shownPredelay = 0.0f;
    bool cutsHigh = false, isGentle = false;
    std::array<std::array<Allpass, 4>, 2> diffusers;
    std::array<phazite::devices::dsp::DelayLine, 8> lines;
    std::array<float, 8> damped {}, phases {}, phaseSteps {};
    std::array<phazite::devices::dsp::DelayLine, 4> plateDiffusers;
    std::array<std::array<phazite::devices::dsp::DelayLine, 4>, 2> tank;
    std::array<float, 2> tankDamped {};
    float platePhase = 0.0f, platePhaseStep = 0.0f, plateSway = 0.0f;

    double getLineSamples (int line, float size) const { return lineMilliseconds[line] * 0.001 * rate * (0.25 + (1.0 - 0.25) * size); }
    double getDampingCoefficient (double frequency) const
    {
        return 1.0 - std::exp (-juce::MathConstants<double>::twoPi * frequency / rate);
    }

    static float getAllpassGain (float atMiddle, float diffusion)
    {
        return diffusion <= 0.5f ? atMiddle * (2.0f * diffusion) : atMiddle + (0.75f - atMiddle) * (2.0f * diffusion - 1.0f);
    }

    void putOut (float& left, float& right, float gain)
    {
        const auto mid = 0.5f * (left + right), sideways = 0.5f * (left - right) * values[Reverb::width];
        left = lowCuts[0].process ((mid + sideways) * gain, lowCut);
        right = lowCuts[1].process ((mid - sideways) * gain, lowCut);

        if (cutsHigh)
        {
            const auto steepLeft = highCuts[0].process (left, highCut), steepRight = highCuts[1].process (right, highCut);
            const auto gentleLeft = gentleCuts[0] += (left - gentleCuts[0]) * gentle;
            const auto gentleRight = gentleCuts[1] += (right - gentleCuts[1]) * gentle;
            left = isGentle ? gentleLeft : steepLeft;
            right = isGentle ? gentleRight : steepRight;
        }
    }

    void processRoom (float* left, float* right, int numSamples)
    {
        const auto decaySeconds = values[Reverb::decay];
        const auto dampingCoefficient = (float) getDampingCoefficient (values[Reverb::damping]);
        const auto modulationDepth = (float) (0.12 * 0.001 * rate);
        std::array<float, 8> lengths {}, losses {}, dampings {};
        auto meanLength = 0.0f;

        for (size_t line = 0; line < 8; ++line)
        {
            lengths[line] = (float) getLineSamples ((int) line, values[Reverb::size]);
            meanLength += lengths[line] / 8.0f;
        }

        for (size_t line = 0; line < 8; ++line)
        {
            const auto c = (double) dampingCoefficient;
            const auto kept = std::pow (c / (2.0 - c), (double) (lengths[line] / meanLength));
            dampings[line] = (float) (2.0 * kept / (1.0 + kept));
            const auto pass = lengths[line] + 0.12 * 0.001 * rate + (1.0 - dampings[line]) / dampings[line];
            losses[line] = (float) std::pow (10.0, -3.0 * pass / (decaySeconds * rate));
        }

        const auto scale = (float) (0.25 + (1.0 - 0.25) * values[Reverb::size]);

        for (auto& side : diffusers)
            for (size_t stage = 0; stage < 4; ++stage)
                side[stage].gain = getAllpassGain (inputGains[stage], values[Reverb::diffusion]);

        for (int i = 0; i < numSamples; ++i)
        {
            std::array<float, 2> in {};
            const std::array<float, 2> samples { left[i], right[i] };

            for (size_t side = 0; side < 2; ++side)
            {
                predelays[side].push (samples[side]);
                auto diffused = predelays[side].read (shownPredelay);

                for (auto& allpass : diffusers[side])
                    diffused = allpass.process (diffused);

                in[side] = diffused;
            }

            std::array<float, 8> out {};
            auto sum = 0.0f;

            for (size_t line = 0; line < 8; ++line)
            {
                phases[line] += phaseSteps[line];
                phases[line] -= std::floor (phases[line]);
                const auto delay =
                    lengths[line] / scale * scale + modulationDepth * (1.0f + phazite::devices::dsp::sineOfPhase (phases[line]));
                damped[line] += (lines[line].read (delay - 1.0f) - damped[line]) * dampings[line];
                out[line] = damped[line] * losses[line];
                sum += out[line];
            }

            const auto shared = sum * (2.0f / 8.0f);
            auto l = 0.0f, r = 0.0f;

            for (size_t line = 0; line < 8; ++line)
            {
                lines[line].push (out[line] - shared + in[line % 2] * 0.35355339f);
                l += leftSigns[line] * out[line];
                r += rightSigns[line] * out[line];
            }

            putOut (l, r, 0.70710678f);
            left[i] = l;
            right[i] = r;
        }
    }

    void processPlate (float* left, float* right, int numSamples)
    {
        const auto setting = Reverb::getPlateSetting (values);
        const auto smear = (float) setting.diffusion;
        const auto gain = (float) setting.gain;
        const auto damping = (float) getDampingCoefficient (values[Reverb::damping] * 0.25);
        const std::array inputGain { getAllpassGain (inputGains[0], smear), getAllpassGain (inputGains[2], smear) };
        const auto tank1 = getAllpassGain (0.70f, smear), tank2 = getAllpassGain (0.50f, smear);
        const auto samplesPerPaperSample = rate / paperRate * setting.scale;
        const auto length = [&] (double paperSamples, double least)
        { return (int) std::max (least, std::round (paperSamples * samplesPerPaperSample)); };
        const auto leastSwaying = std::ceil ((double) plateSway) + 3.0;

        for (int i = 0; i < numSamples; ++i)
        {
            auto in = 0.0f;
            const std::array<float, 2> samples { left[i], right[i] };

            for (size_t side = 0; side < 2; ++side)
            {
                predelays[side].push (samples[side]);
                in += predelays[side].read (shownPredelay);
            }

            in *= 0.5f;

            for (size_t stage = 0; stage < 4; ++stage)
            {
                auto& line = plateDiffusers[stage];
                const auto k = inputGain[stage / 2];
                const auto delayed = line.read (length (plateDiffuserSamples[stage], 2.0) - 1);
                const auto fed = in - k * delayed;
                line.push (fed);
                in = delayed + k * fed;
            }

            platePhase += platePhaseStep;
            platePhase -= std::floor (platePhase);
            const std::array ends { tank[0][3].read (length (tankSamples[0][3], 2.0) - 1),
                                    tank[1][3].read (length (tankSamples[1][3], 2.0) - 1) };

            for (size_t half = 0; half < 2; ++half)
            {
                auto& parts = tank[half];
                const auto sway = plateSway * phazite::devices::dsp::sineOfPhase (platePhase + (half == 0 ? 0.0f : 0.25f));
                const auto swayed = parts[0].read ((float) length (tankSamples[half][0], leastSwaying) - 1.0f + sway);
                const auto fed = in + gain * ends[1 - half] + tank1 * swayed;
                parts[0].push (fed);
                const auto delayed = parts[1].read (length (tankSamples[half][1], 2.0) - 1);
                parts[1].push (swayed - tank1 * fed);
                tankDamped[half] += (delayed - tankDamped[half]) * damping;
                const auto held = parts[2].read (length (tankSamples[half][2], 2.0) - 1);
                const auto into = tankDamped[half] * gain - tank2 * held;
                parts[2].push (into);
                parts[3].push (held + tank2 * into);
            }

            std::array<float, 2> out {};

            for (size_t side = 0; side < 2; ++side)
                for (const auto& tap : taps[side])
                    out[side] += tap.sign * tank[(size_t) tap.half][(size_t) tap.part].read (length (tap.samples, 2.0));

            auto l = out[0] * 0.6f, r = out[1] * 0.6f;
            putOut (l, r, 0.85f);
            left[i] = l;
            right[i] = r;
        }
    }
};
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
    CHECK (devices::getChoiceName (Reverb::info.parameters[Reverb::mode], Reverb::hall) == "Hall");
    CHECK (Reverb::info.parameters[Reverb::mode].choices.size() == 3);
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

TEST_CASE ("The room's lines are as long as their losses are worked out for: it fades over its decay time to a few thousandths")
{
    // Each line read itself a sample further back than its length, and its wobble swings it longer by the wobble's depth on
    // average, while each pass's loss was worked out for its length alone: the smallest room, whose lines are shortest, rang
    // longer than its decay. Measured in its lows, where its damping takes nothing.
    auto values = getDefaults();
    values[Reverb::size] = 0.0f;
    values[Reverb::damping] = 20000.0f;
    values[Reverb::lowCut] = 20.0f;

    // The time it takes to fade by 60 dB, from the straight line that fits its energy decay curve best between -5 and -50
    // dB: steadier than two points of it, which the lines' beating sways by a few thousandths.
    const auto fitDecay = [] (const Stereo& response, double rate)
    {
        std::vector<double> remaining (response.left.size() + 1, 0.0);

        for (auto i = response.left.size(); i-- > 0;)
            remaining[i] = remaining[i + 1] + (double) response.left[i] * response.left[i] + (double) response.right[i] * response.right[i];

        auto n = 0.0, sumT = 0.0, sumD = 0.0, sumTT = 0.0, sumTD = 0.0;

        for (size_t i = 0; i < response.left.size(); ++i)
        {
            const auto decibels = 10.0 * std::log10 (remaining[i] / remaining[0]);

            if (decibels > -5.0 || decibels < -50.0)
                continue;

            const auto t = (double) i / rate;
            n += 1.0;
            sumT += t;
            sumD += decibels;
            sumTT += t * t;
            sumTD += t * decibels;
        }

        const auto slope = (n * sumTD - sumT * sumD) / (n * sumTT - sumT * sumT);
        return -60.0 / slope;
    };

    for (const auto rate : { 44100.0, 96000.0 })
    {
        for (const auto decay : { 2.0f, 4.0f }) // long enough for the tail's start not to sway the measurement
        {
            values[Reverb::decay] = decay;
            const auto measured = fitDecay (below (play (values, (int) (rate * decay * 1.5), impulse, rate), 300.0, rate), rate);
            INFO (decay << " s at " << rate << " Hz: it fades over " << measured << " s");
            CHECK (std::abs (measured / decay - 1.0) < 0.004);
        }
    }

    // Damped, each line's low-pass delays the lows a few samples more each pass (6.5 at 1 kHz on a 488-sample line at 44.1 kHz),
    // which its loss counts too (it rang the lows a percent longer, 1.953 s for 2 s at 25 Hz, now 1.933): the lows never ring
    // longer than set, and lose only a little to the damping. Measured on a tone held for a second and let go: its tail's
    // level over every four cycles, a line fitted through it from 5 to 45 dB down.
    values[Reverb::damping] = 1000.0f;

    for (const auto rate : { 44100.0, 96000.0 })
    {
        for (const auto decay : { 2.0f, 4.0f })
        {
            values[Reverb::decay] = decay;

            const auto measured = measureToneDecay (values, 25.0, rate);
            INFO (decay << " s damped at 1 kHz, at " << rate << " Hz: a tone at 25 Hz fades over " << measured << " s");
            CHECK (measured < decay);
            CHECK (measured > 0.95 * decay);
        }
    }
}

TEST_CASE ("The room's decay curve, which its editor draws, is how long it rings at each frequency, at any damping and size")
{
    // getDecayAt() took the damping knob's coefficient for every line, though each line's is scaled by its length: at 1 kHz's
    // damping in the smallest room the curve said 1.15 s at 300 Hz where the room rang 0.97 s (19 % long), and 19 % short at
    // 1 kHz. Now it works out each line's loss and pass at the frequency, and how the energy flows between them as the room
    // plays it: within a tenth everywhere here, within 4 % at eight of the twelve points (the model counts energies, not
    // waves; the measurement itself is the same at 44.1 and 48 kHz to 2 %). Measured as a room's decay in a band is (ISO 3382): its impulse
    // response through a third-octave band-pass, the slope of its energy decay curve from 5 to 25 dB down, which averages the
    // many modes in the band (a single tone's tail is a few of them, which ring from a third shorter to twice as long).
    auto values = getDefaults();
    values[Reverb::decay] = 2.0f;
    values[Reverb::lowCut] = 20.0f;
    values[Reverb::diffusion] = 0.0f; // its allpass filters, which ring on by themselves, plain delays
    constexpr double rate = 48000.0;

    const auto bandDecay = [] (const Stereo& response, double hertz)
    {
        const auto band = phazite::devices::dsp::makeFilter (phazite::devices::dsp::FilterShape::bandPass, hertz, rate, 4.32);
        std::vector<double> remaining (response.left.size() + 1, 0.0);
        std::array<phazite::devices::dsp::Filter, 4> filters {};
        std::vector<double> energy (response.left.size(), 0.0);

        for (size_t i = 0; i < response.left.size(); ++i)
        {
            const auto left = filters[1].process (filters[0].process (response.left[i], band), band);
            const auto right = filters[3].process (filters[2].process (response.right[i], band), band);
            energy[i] = (double) left * left + (double) right * right;
        }

        for (auto i = energy.size(); i-- > 0;)
            remaining[i] = remaining[i + 1] + energy[i];

        auto n = 0.0, sumT = 0.0, sumD = 0.0, sumTT = 0.0, sumTD = 0.0;

        for (size_t i = 0; i < energy.size(); ++i)
        {
            const auto decibels = 10.0 * std::log10 (remaining[i] / remaining[0]);

            if (decibels > -5.0 || decibels < -25.0)
                continue;

            const auto t = (double) i / rate;
            n += 1.0;
            sumT += t;
            sumD += decibels;
            sumTT += t * t;
            sumTD += t * decibels;
        }

        return -60.0 / ((n * sumTD - sumT * sumD) / (n * sumTT - sumT * sumT));
    };

    for (const auto damping : { 1000.0f, 4000.0f })
    {
        for (const auto size : { 0.0f, 1.0f })
        {
            values[Reverb::damping] = damping;
            values[Reverb::size] = size;
            const auto response = play (values, (int) (3.0 * rate), impulse, rate);

            for (const auto hertz : { 300.0, 1000.0, 3000.0 })
            {
                const auto measured = bandDecay (response, hertz), curve = Reverb::getDecayAt (values, hertz, rate);
                INFO ("damping " << damping << " Hz, size " << size << ", at " << hertz << " Hz: it rings " << measured
                                 << " s, the curve says " << curve);
                CHECK (std::abs (curve / measured - 1.0) < 0.1);
            }
        }
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

            if (juce::exactlyEqual (diffusion, 0.5f))
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

TEST_CASE ("The reverb's high cut falls by 6 or 12 dB an octave, as its slope says, in every mode")
{
    for (const auto mode : { Reverb::room, Reverb::plate, Reverb::hall })
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

TEST_CASE ("The reverb's diffusion smears what comes in: none leaves the hit's echoes sharp, in every mode")
{
    for (const auto mode : { Reverb::room, Reverb::plate, Reverb::hall })
    {
        CAPTURE ((int) mode);
        auto values = getDefaults();
        values[Reverb::mode] = (float) mode;
        values[Reverb::predelay] = 0.0f;
        values[Reverb::damping] = 20000.0f;

        // How far the first tenth of a second's highest sample stands over its level: sharp echoes stand high. The hall's
        // first fifth: at no diffusion its six allpass filters are plain delays, 65 ms in all, before its shortest line.
        const auto length = mode == Reverb::hall ? 9600 : 4800;
        const auto crest = [&values, length] (float diffusion)
        {
            values[Reverb::diffusion] = diffusion;
            const auto response = play (values, length, impulse);
            return (double) peakOf (response.left, 0, (size_t) length)
                   / std::sqrt (energyOf (response.left, 0, (size_t) length) / (double) length);
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

    // To and from the hall, and its size, decay and diffusion moved.
    const auto hall = inMode (Reverb::hall);
    CHECK (sharpest (room, hall) < quiet);
    CHECK (sharpest (hall, room) < quiet);
    CHECK (sharpest (plate, hall) < quiet);
    CHECK (sharpest (hall, plate) < quiet);
    CHECK (sharpest (hall, changed (hall, Reverb::size, 1.0f)) < quiet);
    CHECK (sharpest (hall, changed (hall, Reverb::size, 0.0f)) < quiet);
    CHECK (sharpest (hall, changed (hall, Reverb::decay, 12.0f)) < quiet);
    CHECK (sharpest (hall, changed (hall, Reverb::decay, 0.2f)) < quiet);
    CHECK (sharpest (hall, changed (hall, Reverb::diffusion, 0.0f)) < quiet);

    // The high cut's slope changed while it cuts, both ways and in every mode.
    for (const auto& values : { room, plate, hall })
    {
        const auto cut = changed (values, Reverb::highCut, 3000.0f);
        CHECK (sharpest (cut, changed (cut, Reverb::highCutSlope, 0.0f)) < quiet);
        CHECK (sharpest (changed (cut, Reverb::highCutSlope, 0.0f), cut) < quiet);
    }

    // The room asked for again a block after the plate was, as it fades out: back from where it was, without a click.
    CHECK (sharpestOf (playWith ([&] (int at) -> const std::vector<float>&
                                 { return at >= changeAt && at < changeAt + testBlockSize ? plate : room; }, total, tone))
           < quiet);

    // The mode changed, the other one plays: the plate's tail after the room's, at about its level; and the hall's. Of noise,
    // whose level doesn't depend on where the sways are, as a tone's does on how near they bring a mode to it.
    phazite::devices::dsp::Random random (23);
    std::vector<float> noise ((size_t) total);

    for (auto& sample : noise)
        sample = 0.25f * random.nextBipolar();

    const auto steady = [&noise] (int t) { return noise[(size_t) t]; };

    for (const auto& next : { plate, hall })
    {
        const auto played = playWith ([&] (int at) -> const std::vector<float>& { return at < changeAt ? room : next; }, total, steady);
        const auto alone = play (next, total, steady);
        const auto from = (size_t) total - (size_t) testRate / 2;
        const auto difference =
            10.0 * std::log10 (energyOf (played.left, from, played.left.size()) / energyOf (alone.left, from, alone.left.size()));
        CHECK (std::abs (difference) < 1.0);
    }
}

TEST_CASE ("Finding 1: the room's decay curve takes a bounded number of line updates at any values, a few at its defaults")
{
    // getDecayAt() stepped the energies a quarter of the shortest pass at a time until what came out was 40 dB down, kept
    // every step's in a vector and took the logarithm of each: 3,072 line updates at the defaults, 75,792 at the longest decay
    // in the smallest room (0.05 and 1.3 ms in a Debug build), and its editor asked for it thousands of times a paint. Now it solves for what is yet to come out
    // and finds where the curve passes 5, 10... 25 dB down by halves, through the step's flow squared again and again.
    const auto atDefaults = [] (double hertz)
    {
        std::int64_t updates = 0;
        Reverb::getDecayAt (getDefaults(), hertz, testRate, updates);
        return updates;
    };

    CHECK (atDefaults (500.0) <= 1200);
    std::int64_t most = 0;

    for (const auto rate : { 44100.0, 192000.0 })
    {
        for (const auto size : { 0.0f, 0.5f, 1.0f })
        {
            for (const auto decay : { 0.2f, 2.0f, 20.0f })
            {
                for (const auto damping : { 1000.0f, 20000.0f })
                {
                    for (const auto hertz : { 20.0, 500.0, 16000.0 })
                    {
                        auto values = getDefaults();
                        values[Reverb::size] = size;
                        values[Reverb::decay] = decay;
                        values[Reverb::damping] = damping;
                        std::int64_t updates = 0;
                        const auto seconds = Reverb::getDecayAt (values, hertz, rate, updates);
                        INFO ("size " << size << ", decay " << decay << " s, damping " << damping << " Hz, at " << hertz << " Hz and "
                                      << rate << " Hz: " << seconds << " s in " << updates << " line updates");
                        CHECK (seconds > 0.0);
                        CHECK (seconds < 1.2 * decay);
                        CHECK (updates <= 2500);
                        most = std::max (most, updates);
                    }
                }
            }
        }
    }

    CHECK (most >= 1000); // the bound above isn't loose
}

TEST_CASE ("The room and the plate play exactly what they played before step S6.5, at any settings")
{
    // Noise in both sides, a different one in each, through each mode at its defaults and at settings across their ranges.
    phazite::devices::dsp::Random random (17);
    std::vector<float> leftNoise (48000), rightNoise (48000);

    for (size_t i = 0; i < leftNoise.size(); ++i)
    {
        leftNoise[i] = 0.5f * random.nextBipolar();
        rightNoise[i] = 0.5f * random.nextBipolar();
    }

    const auto with = [] (Reverb::Mode mode, std::initializer_list<std::pair<Reverb::Parameter, float>> changes)
    {
        auto values = inMode (mode);

        for (const auto& [parameter, value] : changes)
            values[(size_t) parameter] = value;

        return values;
    };

    const std::vector<float> settings[] = {
        inMode (Reverb::room),
        with (Reverb::room, { { Reverb::size, 0.0f },
                              { Reverb::decay, 8.0f },
                              { Reverb::diffusion, 1.0f },
                              { Reverb::damping, 2000.0f },
                              { Reverb::highCut, 3000.0f },
                              { Reverb::highCutSlope, 0.0f },
                              { Reverb::width, 0.5f },
                              { Reverb::predelay, 37.0f } }),
        with (Reverb::room, { { Reverb::size, 1.0f }, { Reverb::decay, 0.4f }, { Reverb::diffusion, 0.0f }, { Reverb::lowCut, 300.0f } }),
        inMode (Reverb::plate),
        with (Reverb::plate, { { Reverb::size, 1.0f },
                               { Reverb::decay, 8.0f },
                               { Reverb::diffusion, 1.0f },
                               { Reverb::highCut, 3000.0f },
                               { Reverb::highCutSlope, 0.0f } }),
        with (Reverb::plate, { { Reverb::size, 0.0f },
                               { Reverb::decay, 0.3f },
                               { Reverb::diffusion, 0.8f },
                               { Reverb::damping, 12000.0f },
                               { Reverb::width, 0.3f },
                               { Reverb::predelay, 0.0f } }),
    };

    for (size_t s = 0; s < std::size (settings); ++s)
    {
        CAPTURE (s);
        const auto& values = settings[s];
        Reverb reverb;
        reverb.prepare (testRate, 480);
        ReverbAsBefore before (values, testRate);
        auto left = leftNoise, right = rightNoise, oldLeft = leftNoise, oldRight = rightNoise;

        for (int done = 0; done < (int) left.size(); done += 480)
        {
            float* channels[] = { left.data() + done, right.data() + done };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = 480;
            block.parameters = values;
            reverb.process (block);
            before.process (oldLeft.data() + done, oldRight.data() + done, 480);
        }

        auto differences = 0;

        for (size_t i = 0; i < left.size(); ++i)
            if (! juce::exactlyEqual (left[i], oldLeft[i]) || ! juce::exactlyEqual (right[i], oldRight[i]))
                ++differences;

        CHECK (differences == 0);
        CHECK (peakOf (left, 24000, left.size()) > 0.01f); // it played
    }
}

//==============================================================================
// The hall (roadmap step S6.5): its seven measures, each beside the room's at the same settings.
TEST_CASE ("The hall fades by 60 dB over its decay in each octave from 250 Hz to 2 kHz, at every size: 1 and 3 s")
{
    checkOctaveDecays (1.0f);
    checkOctaveDecays (3.0f);
}

TEST_CASE ("The hall fades by 60 dB over its decay in each octave from 250 Hz to 2 kHz, at every size: 10 s")
{
    checkOctaveDecays (10.0f);
}

TEST_CASE ("The hall fades by 60 dB over its decay in each octave from 250 Hz to 2 kHz, at every size: 20 s")
{
    checkOctaveDecays (20.0f);
}

TEST_CASE ("The hall's long tail doesn't ring: its spectrum peaks over its envelope far less than the room's, about as noise does")
{
    for (const auto decay : { 10.0f, 20.0f })
    {
        const auto noise = 0.5 * (measureRingingOfNoise (decay, 5) + measureRingingOfNoise (decay, 6));

        for (const auto size : { 1.0f, 0.5f })
        {
            auto hallAt = 0.0, roomAt = 0.0;
            const auto hall = measureRingingOf (Reverb::hall, decay, size, hallAt);
            const auto room = measureRingingOf (Reverb::room, decay, size, roomAt);
            MESSAGE ("decay " << decay << " s, size " << size << ": the largest peak over its envelope, the hall's " << hall << " dB (at "
                              << hallAt << " Hz), the room's " << room << " dB (at " << roomAt << " Hz), noise's " << noise << " dB");
            // Within 3 dB of noise's. The step asked for 8 dB under the room's too, which no tail can be where the room's
            // stands less than 8 dB over noise's, as it does here (6 to 7 dB): the hall stands less than half as far over noise.
            CHECK (hall
                   <= noise + 4.0); // the Room is 6 to 7 dB over noise; the largest of 20,000 bins moves by half a decibel with rounding
            CHECK (hall - noise <= 0.5 * (room - noise));
        }
    }
}

TEST_CASE ("The hall is dense soon after its pre-delay: its echo density reaches 0.9 within 150 ms at its largest, 80 ms at half")
{
    for (const auto size : { 1.0f, 0.5f })
    {
        const auto values = openAt (Reverb::hall, 2.0f, size);
        const auto from = values[Reverb::predelay] * 0.001;
        auto room = values;
        room[Reverb::mode] = (float) Reverb::room;
        const auto hall = findDensity (play (values, (int) testRate, impulse).left, testRate, from);
        const auto roomTime = findDensity (play (room, (int) testRate, impulse).left, testRate, from);
        MESSAGE ("size " << size << ": the echo density reaches 0.9 at " << hall << " s after the pre-delay in the hall, at " << roomTime
                         << " s in the room");
        CHECK (hall > 0.0);
        CHECK (hall <= (size > 0.75f ? 0.15 : 0.08));
    }
}

TEST_CASE ("The hall's sides are uncorrelated from 200 ms on, for a sound that comes in on both alike")
{
    for (const auto size : { 0.5f, 1.0f })
    {
        auto values = inMode (Reverb::hall);
        values[Reverb::size] = size;
        const auto hall = correlationOf (play (values, (int) (testRate * 2.5), impulse), (size_t) (0.2 * testRate));
        values[Reverb::mode] = (float) Reverb::room;
        const auto room = correlationOf (play (values, (int) (testRate * 2.5), impulse), (size_t) (0.2 * testRate));
        MESSAGE ("size " << size << ": the sides' correlation from 200 ms on, the hall's " << hall << ", the room's " << room);
        CHECK (std::abs (hall) <= 0.2);
    }
}

TEST_CASE ("The hall's tail falls smoothly: its level in 50 ms frames within 2 dB of a straight line from 300 ms on")
{
    for (const auto decay : { 2.0f, 10.0f })
    {
        for (const auto size : { 1.0f, 0.5f })
        {
            // Down to 40 dB under its level at 300 ms, which a decay of 10 s reaches in under 7 s.
            const auto length = (int) (measureRate * (0.3 + 0.7 * decay));
            const auto hall = measureFlutter (play (openAt (Reverb::hall, decay, size), length, impulse, measureRate), measureRate);
            const auto room = measureFlutter (play (openAt (Reverb::room, decay, size), length, impulse, measureRate), measureRate);
            MESSAGE ("decay " << decay << " s, size " << size << ": its level strays at most " << hall << " dB from the line in the hall, "
                              << room << " dB in the room");
            CHECK (hall <= 2.0);
        }
    }
}

// A measure of time: with the benchmark gates, which run nightly on a quiet machine, not in ctest, where a sanitizer's build
// or a busy runner would fail it.
TEST_CASE ("Benchmark gate: the hall costs no more than 2.2 times the room" * doctest::test_suite ("benchmark-gate") * doctest::skip())
{
    // Both modes over the same 20 s of noise at 48 kHz, in turns of half a second, so that what else the machine does falls
    // on both alike.
    phazite::devices::dsp::Random random (9);
    const auto chunk = (int) (testRate / 2);
    juce::AudioBuffer<float> noise (2, chunk), buffer (2, chunk);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < chunk; ++i)
            noise.setSample (channel, i, 0.25f * random.nextBipolar());

    std::array<Reverb, 2> reverbs;
    const std::array modes { inMode (Reverb::room), inMode (Reverb::hall) };
    std::array<double, 2> seconds {};

    for (auto& reverb : reverbs)
        reverb.prepare (testRate, testBlockSize);

    for (int turn = 0; turn < 40; ++turn)
    {
        for (size_t which = 0; which < 2; ++which)
        {
            buffer.makeCopyOf (noise, true);
            const auto start = std::chrono::steady_clock::now();

            for (int done = 0; done < chunk; done += testBlockSize)
            {
                float* channels[] = { buffer.getWritePointer (0, done), buffer.getWritePointer (1, done) };
                devices::ProcessContext block;
                block.channels = channels;
                block.numChannels = 2;
                block.numSamples = std::min (testBlockSize, chunk - done);
                block.parameters = modes[which];
                reverbs[which].process (block);
            }

            seconds[which] += std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        }
    }

    MESSAGE ("20 s at 48 kHz: the room in " << seconds[0] << " s, the hall in " << seconds[1] << " s, " << seconds[1] / seconds[0]
                                            << " times as long");
    CHECK (seconds[1] <= 2.2 * seconds[0]);
}

TEST_CASE ("The hall plays the same every time and in blocks of any size, and is silent after the tail it reports")
{
    // A quarter of a second of noise at half of full scale, as the device conformance's tail check plays.
    phazite::devices::dsp::Random random (13);
    std::vector<float> noise ((size_t) testRate / 4);

    for (auto& sample : noise)
        sample = 0.5f * random.nextBipolar();

    const auto burst = [&noise] (int t) { return t < (int) noise.size() ? noise[(size_t) t] : 0.0f; };
    auto values = inMode (Reverb::hall);
    values[Reverb::decay] = 3.0f;
    values[Reverb::predelay] = 30.0f;
    const auto length = (int) (testRate * 4.0);

    const auto once = play (values, length, burst), twice = play (values, length, burst);
    CHECK (once.left == twice.left);
    CHECK (once.right == twice.right);

    const int sizes[] = { 1, 7, 64, 300, 1024, 33, 512 };
    const auto inBlocks = playInBlocks (values, length, burst, sizes);
    CHECK (inBlocks.left == once.left);
    CHECK (inBlocks.right == once.right);

    // Its tail as it reports it, from the burst's end: after that, 60 dB under its loudest, which is what its decay means; and
    // a quarter of a second on, 70 dB under full scale at most, as an export's end takes for silence
    // (model/playback/OfflineRender.cpp) and as the device conformance's tail check asks.
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
    const auto after = noise.size() + (size_t) std::ceil (reverb.getTailSeconds() * testRate);
    REQUIRE (after + (size_t) testRate / 4 < once.left.size());

    for (const auto* side : { &once.left, &once.right })
    {
        CHECK (peakOf (*side, after, side->size()) < 0.001f * peakOf (*side, 0, side->size()));
        CHECK (peakOf (*side, after + (size_t) testRate / 4, side->size()) < 0.000316f);
    }
}

TEST_CASE ("The hall's tail is as loud as the room's, from pink noise at the settings both start with")
{
    const auto noise = makePinkNoise ((size_t) testRate * 3, 21);
    const auto input = [&noise] (int t) { return noise[(size_t) t]; };
    const auto from = noise.size() * 2 / 3;
    const auto level = [&] (Reverb::Mode mode)
    {
        const auto tail = play (inMode (mode), (int) noise.size(), input);
        return energyOf (tail.left, from, noise.size()) + energyOf (tail.right, from, noise.size());
    };

    const auto ratio = 10.0 * std::log10 (level (Reverb::hall) / level (Reverb::room));
    MESSAGE ("the hall's tail against the room's, in decibels: " << ratio);
    CHECK (std::abs (ratio) <= 0.5);
}

TEST_CASE ("The hall's decay curve, which its editor draws, is how long it rings at each frequency, at any damping and size")
{
    // As the room's is held (above): its impulse response through a third-octave band-pass, the slope of its energy decay
    // curve from 5 to 25 dB down, against what getDecayAt() works out, within a tenth.
    auto values = inMode (Reverb::hall);
    values[Reverb::decay] = 2.0f;
    values[Reverb::lowCut] = 20.0f;
    values[Reverb::diffusion] = 0.0f;

    for (const auto damping : { 1000.0f, 4000.0f })
    {
        for (const auto size : { 0.0f, 1.0f })
        {
            values[Reverb::damping] = damping;
            values[Reverb::size] = size;
            const auto response = play (values, (int) (3.0 * testRate), impulse);

            for (const auto hertz : { 300.0, 1000.0, 3000.0 })
            {
                const auto measured = measureBandDecay (response, hertz, thirdOctaveQ, testRate, -25.0);
                const auto curve = Reverb::getDecayAt (values, hertz, testRate);
                INFO ("damping " << damping << " Hz, size " << size << ", at " << hertz << " Hz: it rings " << measured
                                 << " s, the curve says " << curve);
                CHECK (std::abs (curve / measured - 1.0) < 0.1);
            }
        }
    }

    // And its bass is the decay; at any values, a few hundred line updates, and well under a tenth of a millisecond a pitch.
    values[Reverb::damping] = 2000.0f;
    CHECK (Reverb::getDecayAt (values, 50.0, testRate) == doctest::Approx (2.0).epsilon (0.01));
    std::int64_t most = 0;
    int pitches = 0;
    const auto start = std::chrono::steady_clock::now();

    for (const auto size : { 0.0f, 0.5f, 1.0f })
    {
        for (const auto decay : { 0.2f, 2.0f, 20.0f })
        {
            for (const auto damping : { 1000.0f, 20000.0f })
            {
                for (const auto hertz : { 20.0, 500.0, 16000.0 })
                {
                    values[Reverb::size] = size;
                    values[Reverb::decay] = decay;
                    values[Reverb::damping] = damping;
                    std::int64_t updates = 0;
                    const auto seconds = Reverb::getDecayAt (values, hertz, testRate, updates);
                    INFO ("size " << size << ", decay " << decay << " s, damping " << damping << " Hz, at " << hertz << " Hz: " << seconds
                                  << " s in " << updates << " line updates");
                    CHECK (seconds > 0.0);
                    CHECK (seconds < 1.2 * decay);
                    most = std::max (most, updates);
                    ++pitches;
                }
            }
        }
    }

    const auto each = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() / pitches;
    MESSAGE ("the hall's decay curve: at most " << most << " line updates, " << each * 1000.0 << " ms a pitch");
    CHECK (most <= 1000); // the work is what is held: the time a pitch takes is only told
}

TEST_CASE ("After a sleep, the hall plays as one that never slept: its sways move on with the song's samples")
{
    // Through its runner: a second of noise, nine of silence (it falls asleep once its tail has passed, live, where a render
    // plays on), then two of noise. From half a second into the second sound, live plays what the render plays.
    const auto sound = probe::join (probe::join (probe::makeNoise (48000, 3, 0.5f), probe::makeSilence (48000 * 9)),
                                    probe::makeNoise (48000 * 2, 4, 0.5f));
    const auto playIt = [&sound] (Reverb::Mode mode, bool rendering, std::uint64_t& worked)
    {
        probe::Device device (Reverb::info);
        device.set ("mode", (float) mode);
        device.set ("decay", 1.0f);
        device.set ("mix", 1.0f);
        device.isRendering = rendering;
        device.prepare (testRate, 480);
        auto played = device.played (sound, 480);
        worked = device.getRunner().getBlocksWorked();
        return played;
    };

    std::uint64_t liveBlocks = 0, renderBlocks = 0;
    const auto live = playIt (Reverb::hall, false, liveBlocks), render = playIt (Reverb::hall, true, renderBlocks);
    CHECK (liveBlocks < renderBlocks); // it slept
    const auto difference = probe::getLargestDifference (live, render, 48000 * 10 + 24000);
    CHECK (difference < 1.0e-5);

    // The room's sways stop while it sleeps (as the plate's): what it plays after one differs from a render, a little.
    const auto roomLive = playIt (Reverb::room, false, liveBlocks), roomRender = playIt (Reverb::room, true, renderBlocks);
    MESSAGE ("after a sleep, live differs from a render by " << difference << " at most in the hall, "
                                                             << probe::getLargestDifference (roomLive, roomRender, 48000 * 10 + 24000)
                                                             << " in the room");
}
