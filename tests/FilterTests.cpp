// SPDX-License-Identifier: AGPL-3.0-or-later
// The Filter as DSP (src/devices/filter/FilterEffect.h), measured: its parameters; its types' shapes, combs, vowel, phaser and
// sample and hold; Clean at 2x sample for sample what it played before step S6.6, a sample later; the Driven filter, which
// matches Clean for a quiet sound, sings in tune at the top of its Resonance at every Quality, drives louder and thicker, keeps
// its band-pass's and notch's level for a loud sound, leaves a sound under its ceiling's knee alone, stays bounded and finite
// swept fast, and changes Character and Quality without a click; its aliases and its latency at each Quality; and all of it in
// blocks of any size, the same after a sleep as in a render, and never asleep while Driven.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/MultimodeFilter.h"
#include "devices/dsp/Saturation.h"
#include "devices/filter/FilterEffect.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace dsp = devices::dsp;
namespace probe = phazite::test::probe;
using devices::FilterEffect;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 480;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : FilterEffect::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, std::string_view id, float value)
{
    const auto index = devices::findParameter (FilterEffect::info, id);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

std::vector<float> with (std::initializer_list<std::pair<std::string_view, float>> changes, std::vector<float> values = getDefaults())
{
    for (const auto& [id, value] : changes)
        set (values, id, value);

    return values;
}

// A stereo sound: its two channels.
struct Stereo
{
    std::vector<float> left, right;
};

// The values at a sample: those of the block starting there.
using Values = std::function<std::vector<float> (int)>;

// Plays a filter in blocks of a size on a sound (input gives each channel's sample at a time) and returns what comes out,
// the song playing from its start; the values are those of each block's first sample.
template <typename Input>
Stereo play (FilterEffect& filter, const Values& values, int numSamples, Input input, int blockSize = testBlockSize)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    Stereo played;
    played.left.reserve ((size_t) numSamples);
    played.right.reserve ((size_t) numSamples);

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
        context.songSample = done;
        context.isPlaying = true;
        filter.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

template <typename Input>
Stereo play (FilterEffect& filter, const std::vector<float>& values, int numSamples, Input input, int blockSize = testBlockSize)
{
    return play (filter, Values ([&values] (int) { return values; }), numSamples, input, blockSize);
}

// A filter made for a rate, playing a sound.
template <typename Input>
Stereo playAt (double rate, const std::vector<float>& values, int numSamples, Input input, int blockSize = testBlockSize)
{
    FilterEffect filter;
    filter.prepare (rate, blockSize);
    return play (filter, values, numSamples, input, blockSize);
}

auto sine (double frequency, float amplitude, double rate = testRate)
{
    return [frequency, amplitude, rate] (int t)
    {
        const auto value = amplitude * (float) std::sin (twoPi * frequency * t / rate);
        return std::pair { value, value };
    };
}

auto impulse()
{
    return [] (int t)
    {
        const auto value = t == 0 ? 1.0f : 0.0f;
        return std::pair { value, value };
    };
}

std::vector<float> makeNoise (int numSamples, std::uint32_t seed, float amplitude)
{
    juce::Random random ((juce::int64) seed);
    std::vector<float> noise ((size_t) numSamples);

    for (auto& sample : noise)
        sample = amplitude * (2.0f * random.nextFloat() - 1.0f);

    return noise;
}

float getRms (const std::vector<float>& sound, size_t from, size_t to)
{
    auto sum = 0.0;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        sum += (double) sound[i] * sound[i];

    return (float) std::sqrt (sum / (double) std::max<size_t> (1, std::min (to, sound.size()) - from));
}

float getPeak (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

float toDecibels (float gain)
{
    return 20.0f * std::log10 (std::max (gain, 1.0e-9f));
}

// A tone's level in a stretch of a sound, by least squares, as an amplitude; weighted by a Hann window where asked, so that
// other tones, louder, leak into it less.
double getToneLevel (const std::vector<float>& sound, size_t from, size_t to, double frequency, double rate = testRate,
                     bool windowed = false)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (auto i = from; i < to; ++i)
    {
        const auto w = windowed ? 0.5 - 0.5 * std::cos (twoPi * (double) (i - from) / (double) (to - from)) : 1.0;
        const auto s = std::sin (twoPi * frequency * (double) i / rate), c = std::cos (twoPi * frequency * (double) i / rate);
        ss += w * s * s;
        cc += w * c * c;
        sc += w * s * c;
        ys += w * sound[i] * s;
        yc += w * sound[i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    const auto a = (ys * cc - yc * sc) / determinant, b = (yc * ss - ys * sc) / determinant;
    return std::sqrt (a * a + b * b);
}

// The frequency of a tone, from where it crosses zero upward (placed between samples), over a stretch.
double getPitch (const std::vector<float>& sound, size_t from, size_t to, double rate)
{
    double first = -1.0, last = -1.0;
    auto crossings = 0;

    for (auto i = from + 1; i < to; ++i)
    {
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
        {
            const auto at = (double) (i - 1) + (double) -sound[i - 1] / (double) (sound[i] - sound[i - 1]);

            if (first < 0.0)
                first = at;

            last = at;
            ++crossings;
        }
    }

    return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0.0;
}

double toCents (double frequency, double reference)
{
    return 1200.0 * std::log2 (frequency / reference);
}

//==============================================================================
// The filter as it was before step S6.6, the old device's code, for the test that Clean at 2x plays what it played: its
// values in its old order (type, cutoff, resonance, slope, drive, mix).
class FilterAsBefore
{
public:
    enum
    {
        filterType,
        cutoff,
        resonance,
        filterSlope,
        drive,
        mix
    };

    static std::vector<float> from (const std::vector<float>& now)
    {
        return { now[FilterEffect::filterType],  now[FilterEffect::cutoff], now[FilterEffect::resonance],
                 now[FilterEffect::filterSlope], now[FilterEffect::drive],  now[FilterEffect::mix] };
    }

    void prepare (double newSampleRate, int maximumBlockSize)
    {
        sampleRate = newSampleRate;
        glide = (float) (1.0 - std::exp (-1.0 / (0.01 * sampleRate)));
        driveSmoothing = (float) (1.0 - std::exp (-1.0 / (0.02 * 2.0 * sampleRate)));
        oversampling =
            std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        oversampling->initProcessing ((size_t) maximumBlockSize);
        latency = (int) std::lround (oversampling->getLatencyInSamples()) + dsp::AntialiasedCurve::delay / 2;
        dryOversampling =
            std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        dryOversampling->initProcessing ((size_t) maximumBlockSize);

        for (size_t channel = 0; channel < 2; ++channel)
        {
            undriven[channel].prepare (latency);
            undrivenOut[channel].assign ((size_t) maximumBlockSize, 0.0f);
            dryUndriven[channel].prepare (latency);
            dryUndrivenOut[channel].assign ((size_t) maximumBlockSize, 0.0f);
        }

        for (auto& filter : filters)
            filter.prepare (dsp::getLongestCombDelay (20.0f, sampleRate));
    }

    int latency = 0;

    void process (float* const* channels, float* const* dry, int numSamples, std::int64_t songSample, const std::vector<float>& values)
    {
        const juce::ScopedNoDenormals noDenormals;
        const auto toOctaves = [] (float hertz) { return std::log2 (std::max (hertz, 20.0f) / 20.0f); };
        const auto toHertz = [] (float o) { return 20.0 * std::pow (2.0, (double) o); };
        const auto make = [&] (double hz)
        {
            return dsp::makeMultimode ({ .mode = dsp::toFilterMode (values[filterType]),
                                         .frequency = hz,
                                         .resonance = values[resonance],
                                         .steep = dsp::isSteep (values[filterSlope]),
                                         .q = dsp::getResonanceQ (values[resonance]) },
                                       sampleRate);
        };
        const auto targetOctaves = toOctaves (values[cutoff]);
        const auto targetDrive = dsp::decibelsToGain (values[drive]);

        if (isFirstBlock)
        {
            octaves = targetOctaves;
            driveGain = targetDrive;
            isFirstBlock = false;
        }

        driveBlock (channels, dry, numSamples, targetDrive, values[mix]);
        auto coefficients = make (toHertz (octaves));

        if (songSample != nextSongSample)
            stepLeft = 0;

        nextSongSample = songSample + numSamples;

        for (int start = 0, end = 0; start < numSamples; start = end)
        {
            if (stepLeft == 0)
            {
                const auto sinceStep = (int) (((songSample + start) % 8 + 8) % 8);
                stepLeft = 8 - sinceStep;

                if (std::abs (targetOctaves - octaves) > 1.0e-4f)
                {
                    octaves += (targetOctaves - octaves) * (1.0f - std::pow (1.0f - glide, (float) stepLeft));
                    coefficients = make (toHertz (octaves));
                }

                for (auto& filter : filters)
                    filter.set (coefficients, stepLeft);
            }

            end = std::min (numSamples, start + stepLeft);
            stepLeft -= end - start;

            for (int i = start; i < end; ++i)
                for (int channel = 0; channel < 2; ++channel)
                    channels[channel][i] = filters[(size_t) channel].process (channels[channel][i]);
        }
    }

private:
    double sampleRate = 48000.0;
    float glide = 0.001f, octaves = 10.0f, driveGain = 1.0f, driveSmoothing = 0.001f, driveBlend = 0.0f, lastMix = 1.0f;
    int stepLeft = 0;
    std::int64_t nextSongSample = -1;
    bool isFirstBlock = true, wasDriven = false, dryInStep = false;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling, dryOversampling;
    std::array<dsp::AntialiasedCurve, 2> curves, dryCurves;
    std::array<dsp::DelayLine, 2> undriven, dryUndriven;
    std::array<std::vector<float>, 2> undrivenOut, dryUndrivenOut;
    std::array<dsp::MultimodeFilter, 2> filters;

    void driveBlock (float* const* channels, float* const* dry, int numSamples, float targetDrive, float mixNow)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                undriven[(size_t) channel].push (channels[channel][i]);
                undrivenOut[(size_t) channel][(size_t) i] = undriven[(size_t) channel].read (latency);
            }
        }

        const auto driveOn = targetDrive > 1.0001f;

        if (! driveOn && driveBlend <= 0.0f)
        {
            for (int channel = 0; channel < 2; ++channel)
                std::copy (undrivenOut[(size_t) channel].begin(), undrivenOut[(size_t) channel].begin() + numSamples, channels[channel]);

            driveGain = targetDrive;
            dryBlock (dry, numSamples, false, 0.0f, 0.0f, mixNow);
            return;
        }

        if (driveBlend <= 0.0f)
        {
            oversampling->reset();

            for (auto& curve : curves)
                curve.reset();
        }

        juce::dsp::AudioBlock<float> audio (channels, 2, (size_t) numSamples);
        auto fast = oversampling->processSamplesUp (audio);

        for (size_t i = 0; i < fast.getNumSamples(); ++i)
        {
            driveGain += (targetDrive - driveGain) * driveSmoothing;
            const auto driven = driveGain > 1.0001f;

            if (driven != wasDriven)
            {
                for (auto& curve : curves)
                    curve.restartAveraging();

                wasDriven = driven;
            }

            for (size_t channel = 0; channel < 2; ++channel)
            {
                auto& sample = fast.getChannelPointer (channel)[i];
                sample = driven ? curves[channel].process (sample * driveGain, dsp::SoftCurve {}) / driveGain
                                : curves[channel].process (sample, dsp::StraightCurve {});
            }
        }

        oversampling->processSamplesDown (audio);
        const auto blendTarget = driveOn ? 1.0f : 0.0f;
        auto blend = driveBlend;

        for (int i = 0; i < numSamples; ++i)
        {
            blend += std::clamp (blendTarget - blend, -1.0f / 64.0f, 1.0f / 64.0f);

            for (int channel = 0; channel < 2; ++channel)
            {
                const auto plain = undrivenOut[(size_t) channel][(size_t) i];
                channels[channel][i] = plain + blend * (channels[channel][i] - plain);
            }
        }

        dryBlock (dry, numSamples, true, driveBlend, blendTarget, mixNow);
        driveBlend = blend;
    }

    void dryBlock (float* const* dry, int numSamples, bool throughDrive, float blendFrom, float blendTarget, float mixNow)
    {
        if (dry == nullptr)
            return;

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                dryUndriven[(size_t) channel].push (dry[channel][i]);
                dryUndrivenOut[(size_t) channel][(size_t) i] = dryUndriven[(size_t) channel].read (latency);
            }
        }

        const auto turned = throughDrive && (mixNow < 1.0f || lastMix < 1.0f);
        lastMix = mixNow;

        if (turned)
        {
            if (! dryInStep)
            {
                dryOversampling->reset();

                for (auto& straight : dryCurves)
                    straight.reset();
            }

            juce::dsp::AudioBlock<float> came (dry, 2, (size_t) numSamples);
            auto fast = dryOversampling->processSamplesUp (came);

            for (size_t channel = 0; channel < 2; ++channel)
                for (size_t i = 0; i < fast.getNumSamples(); ++i)
                    fast.getChannelPointer (channel)[i] =
                        dryCurves[channel].process (fast.getChannelPointer (channel)[i], dsp::StraightCurve {});

            dryOversampling->processSamplesDown (came);
        }

        auto blend = blendFrom;

        for (int i = 0; i < numSamples; ++i)
        {
            blend += std::clamp (blendTarget - blend, -1.0f / 64.0f, 1.0f / 64.0f);

            for (int channel = 0; channel < 2; ++channel)
            {
                const auto plain = dryUndrivenOut[(size_t) channel][(size_t) i];
                dry[channel][i] = turned ? plain + blend * (dry[channel][i] - plain) : plain;
            }
        }

        dryInStep = turned;
    }
};
} // namespace

//==============================================================================
TEST_CASE ("The filter describes itself: its parameters in the order its sound takes them, Character and Quality new")
{
    CHECK (devices::isValid (FilterEffect::info));
    const char* ids[] = { "filterType", "cutoff", "resonance", "filterSlope", "character", "drive", "quality", "mix" };
    REQUIRE (FilterEffect::info.parameters.size() == std::size (ids));

    for (size_t i = 0; i < std::size (ids); ++i)
        CHECK (FilterEffect::info.parameters[i].id == std::string_view (ids[i]));

    const auto& characters = FilterEffect::info.parameters[FilterEffect::character].choices;
    REQUIRE (characters.size() == 2);
    CHECK (characters[0] == std::string_view ("Clean"));
    CHECK (characters[1] == std::string_view ("Driven"));
    CHECK (FilterEffect::info.parameters[FilterEffect::character].defaultValue == doctest::Approx (0.0f));

    const auto& qualities = FilterEffect::info.parameters[FilterEffect::quality].choices;
    REQUIRE (qualities.size() == 3);
    CHECK (qualities[0] == std::string_view ("2x"));
    CHECK (qualities[2] == std::string_view ("8x"));
    CHECK (FilterEffect::info.parameters[FilterEffect::quality].defaultValue == doctest::Approx (0.0f));
    CHECK (FilterEffect::info.parameters[FilterEffect::drive].maximum == doctest::Approx (24.0f));

    // Driven only where a type has it: the low-pass, high-pass, band-pass and notch.
    for (int type = 0; type < (int) std::size (dsp::filterModeNames); ++type)
    {
        CAPTURE (type);
        CHECK (FilterEffect::isDriven (with ({ { "filterType", (float) type }, { "character", 1.0f } })) == (type <= 3));
        CHECK_FALSE (FilterEffect::isDriven (with ({ { "filterType", (float) type } })));
    }
}

TEST_CASE ("The filter: open at first, and low-pass, high-pass and band-pass shapes that fall as steeply as their slope")
{
    auto values = getDefaults();
    CHECK (FilterEffect::getMagnitude (values, 1000.0, testRate) == doctest::Approx (1.0).epsilon (0.02));

    set (values, "cutoff", 1000.0f);
    set (values, "resonance", 0.0f);
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 100.0, testRate)) == doctest::Approx (0.0).epsilon (0.5));
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 1000.0, testRate)) == doctest::Approx (-3.0).epsilon (0.2));
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 4000.0, testRate)) < -40.0f); // 24 dB per octave

    set (values, "filterSlope", 0.0f); // 12 dB per octave
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 4000.0, testRate)) > -30.0f);
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 4000.0, testRate)) < -18.0f);

    set (values, "filterType", 1.0f); // high-pass
    set (values, "filterSlope", 1.0f);
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 10000.0, testRate)) == doctest::Approx (0.0).epsilon (0.5));
    CHECK (toDecibels ((float) FilterEffect::getMagnitude (values, 250.0, testRate)) < -40.0f);

    // Resonance rings at the cutoff.
    set (values, "filterType", 0.0f);
    set (values, "resonance", 0.8f);
    CHECK (FilterEffect::getMagnitude (values, 1000.0, testRate) > 2.0);

    // Played: a low tone through, a high one gone.
    set (values, "resonance", 0.0f);
    const auto low = playAt (testRate, values, 24000, sine (100.0, 0.5f));
    const auto high = playAt (testRate, values, 24000, sine (8000.0, 0.5f));
    CHECK (getRms (low.left, 12000, 24000) > 0.3f);
    CHECK (getRms (high.left, 12000, 24000) < 0.005f);
}

TEST_CASE ("The filter's combs: Comb + on the cutoff's multiples, Comb - on its odd ones, ringing on after the sound")
{
    // The types every Phazite filter offers (dsp/MultimodeFilter.h), in its order.
    const auto& types = FilterEffect::info.parameters[FilterEffect::filterType].choices;
    REQUIRE (types.size() == 9);
    CHECK (types[3] == std::string_view ("Notch"));
    CHECK (types[(size_t) dsp::FilterMode::combPlus] == std::string_view ("Comb +"));
    CHECK (types[(size_t) dsp::FilterMode::combMinus] == std::string_view ("Comb -"));

    auto values = with ({ { "filterType", (float) dsp::FilterMode::combPlus }, { "cutoff", 1000.0f }, { "resonance", 0.5f } });
    CHECK (FilterEffect::getMagnitude (values, 2000.0, testRate) > 1.5);
    CHECK (FilterEffect::getMagnitude (values, 1500.0, testRate) < 0.01);

    // It has no slope, and no Driven character.
    set (values, "filterSlope", 0.0f);
    set (values, "character", 1.0f);
    CHECK (FilterEffect::getMagnitude (values, 2500.0, testRate) < 0.01);
    CHECK (FilterEffect::getMagnitude (values, 3000.0, testRate) > 1.5);

    // Played: a tone on a peak comes through, louder; one between them goes.
    const auto onPeak = playAt (testRate, values, 24000, sine (2000.0, 0.25f));
    const auto between = playAt (testRate, values, 24000, sine (1500.0, 0.25f));
    CHECK (getRms (onPeak.left, 12000, 24000) > 0.25f);
    CHECK (getRms (between.left, 12000, 24000) < 0.005f);

    // Comb -: the odd multiples only, at the same pitch.
    set (values, "filterType", (float) dsp::FilterMode::combMinus);
    CHECK (FilterEffect::getMagnitude (values, 3000.0, testRate) > 1.5);
    CHECK (FilterEffect::getMagnitude (values, 2000.0, testRate) < 0.01);

    // At the most resonance it rings on once a sound stops, and says how long: about a third of a second at 1 kHz.
    set (values, "filterType", (float) dsp::FilterMode::combPlus);
    set (values, "resonance", 1.0f);
    FilterEffect filter;
    filter.prepare (testRate, testBlockSize);
    const auto burst = [] (int t)
    {
        const auto value = t < 480 ? 0.5f * (float) std::sin (twoPi * 1000.0 * t / testRate) : 0.0f;
        return std::pair { value, value };
    };
    const auto rung = play (filter, values, 48000, burst);
    CHECK (filter.getTailSeconds() == doctest::Approx (0.343).epsilon (0.02));
    CHECK (getRms (rung.left, 4800, 5280) > 0.01f);     // 0.1 s after
    CHECK (getRms (rung.left, 24000, 24480) < 1.0e-4f); // 0.5 s after

    // A low-pass again has no tail, Clean.
    set (values, "filterType", 0.0f);
    set (values, "character", 0.0f);
    play (filter, values, 480, burst);
    CHECK (filter.getTailSeconds() == doctest::Approx (0.0));
}

TEST_CASE ("The filter's vowel, phaser and sample and hold: after the combs, each with its own shape and no slope")
{
    const auto& types = FilterEffect::info.parameters[FilterEffect::filterType].choices;
    REQUIRE (types.size() == 9);
    CHECK (types[(size_t) dsp::FilterMode::vowel] == std::string_view ("Vowel"));
    CHECK (types[(size_t) dsp::FilterMode::phaser] == std::string_view ("Phaser"));
    CHECK (types[(size_t) dsp::FilterMode::sampleAndHold] == std::string_view ("Sample & Hold"));

    // The vowel: at a cutoff of 1080 Hz, an A, its first formant at 650 Hz well above what lies past it.
    auto values = with ({ { "filterType", (float) dsp::FilterMode::vowel }, { "cutoff", 1080.0f }, { "resonance", 0.5f } });
    const auto vowel = FilterEffect::getMagnitude (values, 650.0, testRate);
    CHECK (vowel > 2.5 * FilterEffect::getMagnitude (values, 850.0, testRate));

    // No slope for it.
    set (values, "filterSlope", 0.0f);
    CHECK (FilterEffect::getMagnitude (values, 650.0, testRate) == doctest::Approx (vowel));

    // The phaser: a notch at the cutoff.
    set (values, "filterType", (float) dsp::FilterMode::phaser);
    set (values, "cutoff", 1000.0f);
    set (values, "resonance", 0.0f);
    CHECK (FilterEffect::getMagnitude (values, 1000.0, testRate) < 0.01);

    FilterEffect filter;
    filter.prepare (testRate, testBlockSize);
    const auto notched = play (filter, values, 24000, sine (1000.0, 0.25f));
    CHECK (getRms (notched.left, 12000, 24000) < 0.005f);

    // Sample and hold at a cutoff of 1 kHz: 2.7 kHz folds back to 700 Hz.
    set (values, "filterType", (float) dsp::FilterMode::sampleAndHold);
    filter.reset();
    const auto folded = play (filter, values, 24000, sine (2700.0, 0.25f));
    CHECK (getToneLevel (folded.left, 12000, 24000, 700.0) > 0.1);
    CHECK (filter.getTailSeconds() == doctest::Approx (0.0));
}

TEST_CASE ("The filter, Clean at 2x, plays exactly what it played before step S6.6, a sample later")
{
    // Every type, both slopes, undriven and driven, its mix at 100 % and below it (its copy for the mix too): the old device's
    // code, sample for sample, one sample later, as every Quality now takes 8x's latency.
    REQUIRE (FilterEffect::getLatency() == 7);
    const auto noise = makeNoise (19200, 3, 0.5f), other = makeNoise (19200, 4, 0.5f);

    for (int type = 0; type < (int) std::size (dsp::filterModeNames); ++type)
    {
        for (const auto slope : { 0.0f, 1.0f })
        {
            for (const auto& [driveDecibels, mixAmount] : { std::pair (0.0f, 1.0f), std::pair (12.0f, 1.0f), std::pair (24.0f, 0.5f) })
            {
                CAPTURE (type);
                CAPTURE (slope);
                CAPTURE (driveDecibels);
                const auto values = with ({ { "filterType", (float) type },
                                            { "cutoff", 900.0f },
                                            { "resonance", 0.6f },
                                            { "filterSlope", slope },
                                            { "drive", driveDecibels },
                                            { "mix", mixAmount } });
                FilterEffect filter;
                filter.prepare (testRate, testBlockSize);
                FilterAsBefore before;
                before.prepare (testRate, testBlockSize);
                REQUIRE (filter.getLatencySamples() == before.latency + 1);
                const auto old = FilterAsBefore::from (values);

                auto differences = 0, dryDifferences = 0;
                std::vector<float> was, wasDry, now, nowDry;
                juce::AudioBuffer<float> a (2, testBlockSize), aDry (2, testBlockSize), b (2, testBlockSize), bDry (2, testBlockSize);

                for (int done = 0; done < (int) noise.size(); done += testBlockSize)
                {
                    for (int i = 0; i < testBlockSize; ++i)
                    {
                        for (auto* buffer : { &a, &aDry, &b, &bDry })
                        {
                            buffer->setSample (0, i, noise[(size_t) (done + i)]);
                            buffer->setSample (1, i, other[(size_t) (done + i)]);
                        }
                    }

                    before.process (a.getArrayOfWritePointers(), aDry.getArrayOfWritePointers(), testBlockSize, done, old);
                    devices::ProcessContext context;
                    context.channels = b.getArrayOfWritePointers();
                    context.dry = bDry.getArrayOfWritePointers();
                    context.numChannels = 2;
                    context.numSamples = testBlockSize;
                    context.parameters = values;
                    context.songSample = done;
                    filter.process (context);

                    for (int i = 0; i < testBlockSize; ++i)
                    {
                        was.push_back (a.getSample (0, i) + a.getSample (1, i) * 3.0f);
                        wasDry.push_back (aDry.getSample (0, i) + aDry.getSample (1, i) * 3.0f);
                        now.push_back (b.getSample (0, i) + b.getSample (1, i) * 3.0f);
                        nowDry.push_back (bDry.getSample (0, i) + bDry.getSample (1, i) * 3.0f);
                    }
                }

                for (size_t i = 0; i + 1 < was.size(); ++i)
                {
                    differences += juce::exactlyEqual (was[i], now[i + 1]) ? 0 : 1;
                    dryDifferences += juce::exactlyEqual (wasDry[i], nowDry[i + 1]) ? 0 : 1;
                }

                CHECK (getPeak (now, 0, now.size()) > 0.01f);
                CHECK (dryDifferences == 0);

                // Sample & Hold holds on the device's own samples, counted from its start, so that a sample more latency holds
                // other samples of the sound: the same sound, at another phase of its holds, as loud.
                if (type == (int) dsp::FilterMode::sampleAndHold)
                    CHECK (std::abs (toDecibels (getRms (now, 4800, now.size())) - toDecibels (getRms (was, 4800, was.size()))) < 1.0f);
                else
                    CHECK (differences == 0);
            }
        }
    }
}

//==============================================================================
TEST_CASE ("The filter Driven, undriven, with a quiet sound and its Resonance under 80 %, matches Clean within 0.5 dB")
{
    // Tones 50 dB under full scale together (the loop's curve straight for them), at 48 kHz: each type and slope, at 200 Hz and
    // 2 kHz, and a Resonance of none, half and 79 %, through both, each tone's level measured alike: within 0.5 dB up to 5 kHz
    // wherever Clean is within 24 dB of full. Above 5 kHz the two part: the Driven filter, worked out at twice the rate, keeps
    // the analog shape there, where Clean's bilinear transform at the rate squeezes it toward the Nyquist frequency: within 1.5
    // dB (measured 1.2 dB at most: a 24 dB band-pass at 2 kHz, at 8 kHz, Driven nearer the analog filter), and above 10 kHz,
    // where they part most, compared only where Clean passes within 3 dB (a band-pass at 2 kHz 6.8 dB lower than Driven's at
    // 19.5 kHz, where both are some 20 dB down).
    const double tones[] = { 31.0,   63.0,   125.0,  177.0,  250.0,  355.0,  500.0,   710.0,   1000.0,
                             1410.0, 2000.0, 2830.0, 4000.0, 5660.0, 8000.0, 11300.0, 16000.0, 19500.0 };
    constexpr float level = 0.003f;
    constexpr int settle = 24000, measure = 24000;
    const auto sum = [&] (int t)
    {
        auto value = 0.0;

        for (const auto hz : tones)
            value += std::sin (twoPi * hz * t / testRate + hz);

        return std::pair { level * (float) value, level * (float) value };
    };

    for (int type = 0; type < 4; ++type)
    {
        for (const auto slope : { 0.0f, 1.0f })
        {
            for (const auto hertz : { 200.0f, 2000.0f })
            {
                for (const auto amount : { 0.0f, 0.5f, 0.79f })
                {
                    CAPTURE (type);
                    CAPTURE (slope);
                    CAPTURE (hertz);
                    CAPTURE (amount);
                    const auto clean =
                        with ({ { "filterType", (float) type }, { "filterSlope", slope }, { "cutoff", hertz }, { "resonance", amount } });
                    auto driven = clean;
                    set (driven, "character", 1.0f);
                    const auto played = playAt (testRate, driven, settle + measure, sum);
                    const auto asClean = playAt (testRate, clean, settle + measure, sum);
                    const auto levelOf = [] (const Stereo& sound, double hz)
                    {
                        return toDecibels (
                            (float) (getToneLevel (sound.left, (size_t) settle, (size_t) (settle + measure), hz, testRate, true) / level));
                    };

                    for (const auto hz : tones)
                    {
                        const auto expected = levelOf (asClean, hz);

                        if (expected < -24.0f || (hz > 10000.0 && expected < -3.0f))
                            continue;

                        CAPTURE (hz);
                        CAPTURE (expected);
                        CHECK (std::abs (levelOf (played, hz) - expected) < (hz <= 5000.0 ? 0.5f : 1.5f));
                    }
                }
            }
        }
    }
}

TEST_CASE ("The filter Driven at the top of its Resonance sings at its cutoff with nothing coming in, in tune at every Quality")
{
    // One impulse, then silence: a sine at the cutoff, within 5 cents, at 50 Hz, 440 Hz and 5 kHz, at each Quality and at 44.1 and
    // 96 kHz, as loud as -15 to -9 dBFS.
    for (const auto rate : { 44100.0, 96000.0 })
    {
        for (int quality = 0; quality < 3; ++quality)
        {
            for (const auto hertz : { 50.0f, 440.0f, 5000.0f })
            {
                CAPTURE (rate);
                CAPTURE (quality);
                CAPTURE (hertz);
                const auto values = with ({ { "filterSlope", 0.0f },
                                            { "cutoff", hertz },
                                            { "resonance", 1.0f },
                                            { "character", 1.0f },
                                            { "quality", (float) quality } });
                const auto settle = (int) ((hertz < 100.0f ? 3.0 : 1.0) * rate), measure = (int) (0.5 * rate);
                const auto sung = playAt (rate, values, settle + measure, impulse());
                const auto pitch = getPitch (sung.left, (size_t) settle, (size_t) (settle + measure), rate);
                const auto peak = toDecibels (getPeak (sung.left, (size_t) settle, (size_t) (settle + measure)));
                MESSAGE (rate << " Hz, Quality " << quality << ", " << hertz << " Hz: " << toCents (pitch, hertz) << " cents, " << peak
                              << " dBFS");
                CHECK (std::abs (toCents (pitch, hertz)) < 5.0);
                CHECK (peak > -15.0f);
                CHECK (peak < -9.0f);
            }
        }
    }

    // Its level falls with the Drive, whose half given back turns the loop's output down: about -18 dBFS at 12 dB, -24 at 24.
    for (const auto& [driveDecibels, expected] : { std::pair { 12.0f, -18.0f }, std::pair { 24.0f, -24.0f } })
    {
        CAPTURE (driveDecibels);
        const auto values = with (
            { { "filterSlope", 0.0f }, { "cutoff", 440.0f }, { "resonance", 1.0f }, { "character", 1.0f }, { "drive", driveDecibels } });
        const auto sung = playAt (testRate, values, 72000, impulse());
        const auto peak = toDecibels (getPeak (sung.left, 48000, 72000));
        MESSAGE ("Singing at a Drive of " << driveDecibels << " dB: " << peak << " dBFS");
        CHECK (std::abs (peak - expected) < 3.0f);
    }
}

TEST_CASE ("The filter Driven sings steadily for 10 s, at 12 and 24 dB, and at 95 % rings long and dies")
{
    constexpr double rate = 44100.0;

    for (const auto slope : { 0.0f, 1.0f })
    {
        CAPTURE (slope);
        const auto values = with ({ { "filterSlope", slope }, { "cutoff", 440.0f }, { "resonance", 1.0f }, { "character", 1.0f } });
        const auto sung = playAt (rate, values, (int) (11.0 * rate), impulse());
        const auto first = getPeak (sung.left, (size_t) rate, (size_t) (2.0 * rate));

        for (int second = 1; second < 11; ++second)
        {
            CAPTURE (second);
            const auto peak = getPeak (sung.left, (size_t) (second * rate), (size_t) ((second + 1) * rate));
            CHECK (std::abs (toDecibels (peak) - toDecibels (first)) < 0.1f);
            CHECK (toDecibels (peak) > -15.0f);
            CHECK (toDecibels (peak) < -9.0f);
        }
    }

    // From silence alone: the kick a loop at rest takes starts it, within half a second at 440 Hz; Clean, nothing.
    const auto fromSilence = with ({ { "filterSlope", 0.0f }, { "cutoff", 440.0f }, { "resonance", 1.0f }, { "character", 1.0f } });
    const auto silence = [] (int) { return std::pair { 0.0f, 0.0f }; };
    CHECK (toDecibels (getPeak (playAt (rate, fromSilence, (int) rate, silence).left, (size_t) (0.5 * rate), (size_t) rate)) > -15.0f);
    auto cleanAtTop = fromSilence;
    set (cleanAtTop, "character", 0.0f);
    CHECK (getPeak (playAt (rate, cleanAtTop, (int) rate, silence).left, 0, (size_t) rate) < 1.0e-9f);

    // At 95 %, an impulse rings for longer than Clean's at its most resonance, and is gone in 2 s.
    const auto ringing = with ({ { "filterSlope", 0.0f }, { "cutoff", 440.0f }, { "resonance", 0.95f }, { "character", 1.0f } });
    auto clean = ringing;
    set (clean, "character", 0.0f);
    set (clean, "resonance", 1.0f);
    const auto driven = playAt (rate, ringing, (int) (2.0 * rate), impulse());
    const auto cleanRing = playAt (rate, clean, (int) (2.0 * rate), impulse());
    const auto at50 = (size_t) (0.05 * rate), at60 = (size_t) (0.06 * rate);
    MESSAGE ("50 ms after: Driven at 95 % " << toDecibels (getPeak (driven.left, at50, at60)) << " dBFS, Clean at 100 % "
                                            << toDecibels (getPeak (cleanRing.left, at50, at60)) << " dBFS");
    CHECK (getPeak (driven.left, at50, at60) > 2.0f * getPeak (cleanRing.left, at50, at60));
    CHECK (getPeak (driven.left, (size_t) (1.9 * rate), (size_t) (2.0 * rate)) < 1.0e-5f);
}

TEST_CASE ("The filter's Drive, Driven: half its gain given back, louder up to the curve's ceiling, and odd harmonics rising")
{
    // A quiet tone, open: 6 dB louder at a Drive of 12 dB, 12 dB louder at 24.
    const auto open = with ({ { "character", 1.0f } });
    const auto playedAt = [&] (float driveDecibels, float amplitude, double hz)
    {
        auto values = open;
        set (values, "drive", driveDecibels);
        return playAt (testRate, values, 24000, sine (hz, amplitude));
    };

    const auto quiet = [&] (float driveDecibels)
    { return toDecibels ((float) getToneLevel (playedAt (driveDecibels, 0.01f, 1000.0).left, 12000, 24000, 1000.0)); };
    const auto none = quiet (0.0f);
    CHECK (std::abs (none - toDecibels (0.01f)) < 0.1f);
    CHECK (std::abs (quiet (12.0f) - none - 6.0f) < 0.5f);
    CHECK (std::abs (quiet (24.0f) - none - 12.0f) < 0.5f);

    // A sine at full scale, driven 24 dB, comes out no louder than the curve's ceiling, full scale (the half-band filters
    // overshoot it a little), and louder than undriven.
    for (const auto driveDecibels : { 0.0f, 12.0f, 24.0f })
        MESSAGE ("A full-scale sine at a Drive of " << driveDecibels << " dB peaks at "
                                                    << toDecibels (getPeak (playedAt (driveDecibels, 1.0f, 1000.0).left, 4800, 24000))
                                                    << " dBFS");

    const auto loud = playedAt (24.0f, 1.0f, 1000.0);
    CHECK (getPeak (loud.left, 4800, 24000) < 1.05f);
    CHECK (getRms (loud.left, 4800, 24000) > getRms (playedAt (0.0f, 1.0f, 1000.0).left, 4800, 24000));

    // A tone at -12 dBFS, open (the loop straight for it, far below its cutoff): at a Drive of 0 and 12 dB it comes out at -12
    // and -6 dBFS, at most at the ceiling's knee, and keeps its shape, every harmonic more than 100 dB under full scale. At 18
    // and 24 dB the ceiling rounds it: its third harmonic rises with the Drive, louder than its fifth; its second stays out of
    // hearing, the curve being odd.
    auto lastThird = -200.0f;

    for (const auto driveDecibels : { 0.0f, 12.0f, 18.0f, 24.0f })
    {
        CAPTURE (driveDecibels);
        const auto played = playedAt (driveDecibels, 0.25f, 200.0);
        const auto harmonic = [&] (int n) { return toDecibels ((float) getToneLevel (played.left, 4800, 24000, 200.0 * n)); };
        const auto third = harmonic (3);
        MESSAGE ("Drive " << driveDecibels << " dB: second " << harmonic (2) << ", third " << third << ", fifth " << harmonic (5)
                          << " dBFS");

        if (driveDecibels < 15.0f)
        {
            for (int n = 2; n <= 9; ++n)
                CHECK (harmonic (n) < -100.0f);

            continue;
        }

        CHECK (third > lastThird);
        CHECK (third > harmonic (5));
        CHECK (harmonic (2) < third - 40.0f);
        lastThird = third;
    }
}

TEST_CASE ("The filter Driven's band-pass passes a tone at its cutoff however loud, and louder driven; its notch alike")
{
    // A tone at the cutoff (1 kHz) through the Driven band-pass, 12 and 24 dB, at Resonance 50, 85 and 95 %, Drive 0, against
    // Clean's gain (the same at any level). At 50 and 85 %, at -40 dBFS, within 1 dB of Clean. At 95 % the Driven damping has
    // left Clean's on its way to singing, so its response to a quiet sound peaks 6.9 dB over Clean's (Clean's damping over
    // the Driven one), and at -40 dBFS the band-pass state (0.3) is already where the curve bends: measured 5.4 dB over Clean,
    // held between Clean's and the quiet response. Louder, the loop's damping saturates, and the band-pass, which puts out
    // the damping term, follows what comes in: at -12 dBFS no more than 3 dB under Clean. Driven 18 dB, the -12 dBFS tone
    // comes out louder than at Drive 0.
    constexpr double hertz = 1000.0;
    constexpr int settle = 24000, measure = 24000;
    const auto gainOf = [&] (const std::vector<float>& values, float decibels, double tone)
    {
        const auto amplitude = dsp::decibelsToGain (decibels);
        const auto played = playAt (testRate, values, settle + measure, sine (tone, amplitude));
        return toDecibels ((float) (getToneLevel (played.left, (size_t) settle, (size_t) (settle + measure), tone) / amplitude));
    };

    for (const auto slope : { 0.0f, 1.0f })
    {
        for (const auto amount : { 0.5f, 0.85f, 0.95f })
        {
            CAPTURE (slope);
            CAPTURE (amount);
            const auto clean =
                with ({ { "filterType", 2.0f }, { "filterSlope", slope }, { "cutoff", (float) hertz }, { "resonance", amount } });
            auto driven = clean;
            set (driven, "character", 1.0f);
            const auto cleanGain = gainOf (clean, -40.0f, hertz);
            const auto quietGain = toDecibels ((float) FilterEffect::getMagnitude (driven, hertz, testRate));
            const auto quiet = gainOf (driven, -40.0f, hertz), loud = gainOf (driven, -12.0f, hertz),
                       louder = gainOf (driven, -6.0f, hertz);
            MESSAGE ("Band-pass, slope " << slope << ", Resonance " << amount << ": Clean " << cleanGain << " dB; Driven, quiet response "
                                         << quietGain << " dB, at -40 dBFS " << quiet << " dB, at -12 " << loud << " dB, at -6 " << louder
                                         << " dB");
            if (amount < 0.9f)
            {
                CHECK (std::abs (quietGain - cleanGain) < 0.01f);
                CHECK (std::abs (quiet - cleanGain) < 1.0f);
            }
            else
            {
                CHECK (quiet > cleanGain);
                CHECK (quiet < quietGain);
            }

            CHECK (loud > cleanGain - 3.0f);

            auto pushed = driven;
            set (pushed, "drive", 18.0f);
            const auto pushedGain = gainOf (pushed, -12.0f, hertz);
            MESSAGE ("  at Drive 18 dB, at -12 dBFS: " << pushedGain << " dB");
            CHECK (pushedGain > loud);
        }
    }

    // The notch, made of the same terms (what comes in less the damping), at 90 %: a tone an octave above its cutoff passes
    // at the same gain at -40 and at -12 dBFS.
    for (const auto slope : { 0.0f, 1.0f })
    {
        CAPTURE (slope);
        const auto notch = with ({ { "filterType", 3.0f },
                                   { "filterSlope", slope },
                                   { "cutoff", (float) hertz },
                                   { "resonance", 0.9f },
                                   { "character", 1.0f } });
        const auto quiet = gainOf (notch, -40.0f, 2.0 * hertz), loud = gainOf (notch, -12.0f, 2.0 * hertz);
        MESSAGE ("Notch, slope " << slope << ", an octave above: " << quiet << " dB at -40 dBFS, " << loud << " dB at -12 dBFS");
        CHECK (std::abs (loud - quiet) < 0.5f);
    }
}

TEST_CASE ("The Driven filter's ceiling: straight up to half of full scale, then rounding off to it, played by its integral")
{
    // The curve: its value the slope of its integral everywhere, the integral whole at the knee, the slope 1 on both sides of
    // the knee, nothing past 1, and odd.
    using Knee = dsp::KneeCurve;

    for (auto x = -4.0; x <= 4.0; x += 0.0625)
    {
        CAPTURE (x);
        const auto h = 1.0e-5;
        CHECK ((Knee::integral (x + h) - Knee::integral (x - h)) / (2.0 * h)
               == doctest::Approx ((double) Knee::value ((float) x)).epsilon (1.0e-5));
        CHECK (std::abs (Knee::value ((float) x)) <= 1.0f);
        CHECK (Knee::value ((float) -x) == doctest::Approx (-Knee::value ((float) x)));
    }

    for (const auto knee : { -0.5, 0.5 })
        CHECK (Knee::integral (knee - 1.0e-9) == doctest::Approx (Knee::integral (knee + 1.0e-9)).epsilon (1.0e-7));

    CHECK ((Knee::value (0.5f) - Knee::value (0.499f)) / 0.001f == doctest::Approx (1.0f).epsilon (1.0e-2));
    CHECK ((Knee::value (0.501f) - Knee::value (0.5f)) / 0.001f == doctest::Approx (1.0f).epsilon (1.0e-2));
    CHECK (juce::exactlyEqual (Knee::value (0.3f), 0.3f));
    CHECK (Knee::value (100.0f) <= 1.0f);
    CHECK (Knee::integral (100.0) > 0.0);

    // Driven at Drive 0 and Resonance 0 leaves a sine in its pass band (1 kHz, the low-pass open) as Clean does: at -12 and
    // -7 dBFS, within 0.01 dB of Clean, and every harmonic more than 100 dB under full scale.
    for (const auto decibels : { -12.0f, -7.0f })
    {
        CAPTURE (decibels);
        const auto clean = with ({ { "resonance", 0.0f } });
        auto driven = clean;
        set (driven, "character", 1.0f);
        const auto amplitude = dsp::decibelsToGain (decibels);
        const auto played = playAt (testRate, driven, 48000, sine (1000.0, amplitude));
        const auto asClean = playAt (testRate, clean, 48000, sine (1000.0, amplitude));
        const auto levelOf = [] (const Stereo& sound, double tone)
        { return toDecibels ((float) getToneLevel (sound.left, 4800, 48000, tone)); };
        const auto difference = levelOf (played, 1000.0) - levelOf (asClean, 1000.0);
        auto loudestHarmonic = -300.0f;

        for (int n = 2; n <= 9; ++n)
            loudestHarmonic = std::max (loudestHarmonic, levelOf (played, 1000.0 * n));

        MESSAGE ("A sine at " << decibels << " dBFS, Driven at no Drive: " << difference << " dB from Clean, its loudest harmonic "
                              << loudestHarmonic << " dBFS");
        CHECK (std::abs (difference) < 0.01f);
        CHECK (loudestHarmonic < -100.0f);
    }

    // A sine at full scale driven 24 dB: the ceiling keeps it to full scale at its rate, nearly a square there, and the
    // half-band filters back to the sound's rate overshoot that: measured +0.36 dBFS as it starts and +0.15 dBFS steady
    // (+0.11 dBFS with the soft clip as the ceiling before its knee), held under the Drive's test's bound.
    const auto loud = playAt (testRate, with ({ { "character", 1.0f }, { "drive", 24.0f } }), 24000, sine (1000.0, 1.0f));
    MESSAGE ("A full-scale sine driven 24 dB peaks at " << toDecibels (getPeak (loud.left, 0, loud.left.size())) << " dBFS");
    CHECK (getPeak (loud.left, 0, loud.left.size()) < 1.05f);
}

TEST_CASE ("The Driven loop itself stays bounded swept from 20 Hz to 20 kHz and back in 10 ms, singing and driven hard")
{
    // The loop alone (dsp::MultimodeFilter, without the ceiling after it, which bounds what the device puts out whatever the
    // loop does): full-scale noise driven 24 dB at Resonance 100 %, its cutoff swept as the device's test sweeps it, new
    // coefficients each 8 of the sound's samples (each sample of the noise held for the rate's factor), at twice, four and
    // eight times 48 kHz, 12 and 24 dB, each of the four types. What it puts out and its two states stay under bounds measured
    // here and held with 6 dB of margin: at most 38.6 out (a 24 dB high-pass at 8x, the noise's peak 15.8) and 23.3 in a
    // state (a 24 dB low-pass at 2x), measured 2026-11-18.
    constexpr float loudestOut = 2.0f * 38.6f, loudestState = 2.0f * 23.3f;
    const auto noise = makeNoise (48000, 7, 1.0f);
    const auto drive = dsp::decibelsToGain (24.0f);

    for (const auto factor : { 2, 4, 8 })
    {
        for (const auto steep : { false, true })
        {
            for (const auto mode :
                 { dsp::FilterMode::lowPass, dsp::FilterMode::highPass, dsp::FilterMode::bandPass, dsp::FilterMode::notch })
            {
                CAPTURE (factor);
                CAPTURE (steep);
                CAPTURE ((int) mode);
                const auto rate = testRate * factor;
                dsp::MultimodeFilter filter;
                filter.resetDriven();
                auto out = 0.0f, states = 0.0f;
                auto finite = true;

                for (size_t i = 0; i < noise.size(); ++i)
                {
                    if (i % 8 == 0)
                    {
                        const auto along = (double) (i % 480) / 480.0;
                        const auto octaves = 10.0 * (along < 0.5 ? 2.0 * along : 2.0 - 2.0 * along);
                        filter.setDriven (dsp::makeDriven (
                            { .mode = mode, .frequency = 20.0 * std::pow (2.0, octaves), .resonance = 1.0f, .steep = steep }, rate));
                    }

                    for (int k = 0; k < factor; ++k)
                    {
                        const auto y = filter.processDriven (noise[i] * drive);
                        finite = finite && std::isfinite (y);
                        out = std::max (out, std::abs (y));
                        states = std::max ({ states, std::abs (filter.getDrivenState1()), std::abs (filter.getDrivenState2()) });
                    }
                }

                MESSAGE ("At " << factor << " times, " << (steep ? "24" : "12") << " dB, mode " << (int) mode << ": out " << out
                               << ", states " << states);
                CHECK (finite);
                CHECK (out < loudestOut);
                CHECK (states < loudestState);
            }
        }
    }
}

TEST_CASE ("The filter Driven stays finite and under +6 dBFS swept from 20 Hz to 20 kHz and back in 10 ms, singing and driven hard")
{
    // Noise at full scale, Resonance 100 %, Drive 24 dB, the cutoff set from 20 Hz to 20 kHz and back each 10 ms in blocks of 32
    // samples, at each Quality and slope.
    const auto noise = makeNoise (48000, 7, 1.0f);

    for (int quality = 0; quality < 3; ++quality)
    {
        for (const auto slope : { 0.0f, 1.0f })
        {
            CAPTURE (quality);
            CAPTURE (slope);
            const Values values = [&] (int t)
            {
                const auto along = (double) (t % 480) / 480.0;
                const auto octaves = 10.0 * (along < 0.5 ? 2.0 * along : 2.0 - 2.0 * along);
                return with ({ { "filterSlope", slope },
                               { "cutoff", (float) (20.0 * std::pow (2.0, octaves)) },
                               { "resonance", 1.0f },
                               { "character", 1.0f },
                               { "drive", 24.0f },
                               { "quality", (float) quality } });
            };
            FilterEffect filter;
            filter.prepare (testRate, 32);
            const auto played =
                play (filter, values, (int) noise.size(), [&] (int t) { return std::pair { noise[(size_t) t], -noise[(size_t) t] }; }, 32);
            auto finite = true;

            for (const auto sample : played.left)
                finite = finite && std::isfinite (sample);

            CHECK (finite);
            CHECK (getPeak (played.left, 0, played.left.size()) < 2.0f);
            CHECK (getPeak (played.right, 0, played.right.size()) < 2.0f);
        }
    }
}

namespace
{
// The largest step from one sample to the next in a stretch.
float getLargestStep (const std::vector<float>& sound, size_t from, size_t to)
{
    auto largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < to; ++i)
        largest = std::max (largest, std::abs (sound[i] - sound[i - 1]));

    return largest;
}

// Each window's RMS, in decibels, over a stretch.
std::vector<float> getLevels (const std::vector<float>& sound, size_t from, size_t to, size_t window)
{
    std::vector<float> levels;

    for (auto at = from; at + window <= to; at += window)
        levels.push_back (toDecibels (getRms (sound, at, at + window)));

    return levels;
}
} // namespace

TEST_CASE ("The filter's Character switched while it sings fades out without a click, and its Quality switched while driven keeps "
           "the level")
{
    // Singing at 440 Hz from an impulse, switched to Clean after a second: silence plays into Clean, so the sound fades to it,
    // over 10 ms after the new way has settled, never louder than the song and with no step larger than the song's own.
    constexpr int switchAt = 48000;
    const auto singing = with ({ { "filterSlope", 0.0f }, { "cutoff", 440.0f }, { "resonance", 1.0f }, { "character", 1.0f } });
    auto clean = singing;
    set (clean, "character", 0.0f);
    FilterEffect filter;
    filter.prepare (testRate, testBlockSize);
    const auto played = play (filter, Values ([&] (int t) { return t < switchAt ? singing : clean; }), 2 * switchAt, impulse());
    const auto peak = getPeak (played.left, switchAt - 4800, switchAt);
    const auto step = getLargestStep (played.left, switchAt - 4800, switchAt);
    CHECK (toDecibels (peak) > -15.0f);
    CHECK (getPeak (played.left, switchAt, played.left.size()) <= peak * 1.01f);
    CHECK (getLargestStep (played.left, switchAt, played.left.size()) <= step * 1.01f);
    CHECK (getPeak (played.left, switchAt + 9600, played.left.size()) < 1.0e-6f); // Clean, in silence

    // Back to Driven while silence plays: it starts singing again on its own, from the kick a loop at rest takes, without a
    // click: it swells.
    const auto back = play (filter, singing, 2 * switchAt, [] (int) { return std::pair { 0.0f, 0.0f }; });
    CHECK (getPeak (back.left, 0, 4800) < 1.0e-3f);
    CHECK (toDecibels (getPeak (back.left, back.left.size() - 4800, back.left.size())) > -15.0f);
    CHECK (getLargestStep (back.left, 0, back.left.size()) <= step * 1.01f);

    // A saw at 200 Hz through it driven, resonant, its Quality switched from 2x to 8x and back, Clean and Driven: the level in
    // 5 ms windows (a cycle each) stays within 0.5 dB of the level before, and no sample steps further than the sound does
    // before it.
    const auto saw = [] (int t)
    {
        const auto value = 0.25f * (2.0f * (float) std::fmod (t * 200.0 / testRate, 1.0) - 1.0f);
        return std::pair { value, value };
    };

    for (const auto character : { 0.0f, 1.0f })
    {
        CAPTURE (character);
        const auto at = [&] (float quality)
        {
            return with (
                { { "cutoff", 1000.0f }, { "resonance", 0.5f }, { "character", character }, { "drive", 12.0f }, { "quality", quality } });
        };
        const auto twice = at (0.0f), eightTimes = at (2.0f);
        FilterEffect switched;
        switched.prepare (testRate, testBlockSize);
        const auto sound =
            play (switched, Values ([&] (int t) { return t >= switchAt && t < 2 * switchAt ? eightTimes : twice; }), 3 * switchAt, saw);
        const auto before = getRms (sound.left, switchAt - 4800, switchAt);
        const auto beforeStep = getLargestStep (sound.left, switchAt - 4800, switchAt);

        for (const auto from : { switchAt, 2 * switchAt })
        {
            CAPTURE (from);

            for (const auto level : getLevels (sound.left, (size_t) from, (size_t) (from + 4800), 240))
                CHECK (std::abs (level - toDecibels (before)) < 0.5f);

            CHECK (getLargestStep (sound.left, (size_t) from, (size_t) (from + 4800)) < beforeStep * 1.2f);
        }
    }

    // While it sings, its Quality switched: it takes the loop where it is, and sings on as loud.
    FilterEffect sung;
    sung.prepare (testRate, testBlockSize);
    auto eight = singing;
    set (eight, "quality", 2.0f);
    const auto onAndOn = play (sung, Values ([&] (int t) { return t < switchAt ? singing : eight; }), 2 * switchAt, impulse());
    const auto sungBefore = getRms (onAndOn.left, switchAt - 4800, switchAt);

    for (const auto level : getLevels (onAndOn.left, (size_t) switchAt, (size_t) (switchAt + 9600), 480))
        CHECK (std::abs (level - toDecibels (sungBefore)) < 1.0f);
}

TEST_CASE ("The filter keeps its aliases down, the more the higher its Quality, and comes as late as it says at each")
{
    // How far under the tone its loudest alias stays at each Quality, at the worst of 2.5, 5 and 12 kHz tones at half of full
    // scale, at 44.1 and 48 kHz: Clean driven 24 dB, and Driven driven 24 dB at a Resonance of 50 % (docs/design/devices/filter.md
    // gives these figures). Each Quality holds to its figure, less 3 dB, and does better than the one below it.
    struct Figures
    {
        float character;
        double figures[3];
    };

    const Figures characters[] = { { 0.0f, { 34.3, 60.1, 87.4 } },   // measured 2026-11-18: 34.30, 60.09, 87.42
                                   { 1.0f, { 45.9, 63.0, 99.5 } } }; // 45.94, 63.03, 99.50, with its ceiling's knee

    for (const auto& each : characters)
    {
        auto previous = 0.0;

        for (int quality = 0; quality < 3; ++quality)
        {
            auto worst = 1000.0;

            for (const auto rate : { 44100.0, 48000.0 })
            {
                for (const auto hertz : { 2500.0, 5000.0, 12000.0 })
                {
                    const auto values = with ({ { "character", each.character },
                                                { "drive", 24.0f },
                                                { "resonance", each.character > 0.5f ? 0.5f : 0.1f },
                                                { "quality", (float) quality } });
                    const auto frequency = probe::onBin (hertz, rate);
                    const auto played = playAt (rate, values, probe::aliasWindow + 4800, sine (frequency, 0.5f, rate));
                    probe::Sound sound (1, probe::aliasWindow);
                    std::copy (played.left.begin() + 4800, played.left.end(), sound.getWritePointer (0));
                    const auto under = probe::getAliasesUnder (sound, 0, 0, frequency, rate);
                    MESSAGE ("Character " << each.character << ", Quality " << quality << ", " << rate << " Hz, " << hertz
                                          << " Hz: " << under << " dB");
                    worst = std::min (worst, under);
                }
            }

            MESSAGE ("Character " << each.character << ", Quality " << quality << ": aliases at least " << worst << " dB under");
            CHECK (worst > each.figures[quality] - 3.0);

            if (quality > 0)
                CHECK (worst > previous);

            previous = worst;
        }
    }

    // An impulse through it, undriven, comes out as late as it says at every Quality: Clean, and Driven, as a high-pass at its
    // lowest without resonance, which passes an impulse whole (a low-pass, even open, delays its peak by its own phase).
    for (int quality = 0; quality < 3; ++quality)
    {
        for (const auto character : { 0.0f, 1.0f })
        {
            CAPTURE (quality);
            CAPTURE (character);
            const auto values = with ({ { "filterType", 1.0f },
                                        { "filterSlope", 0.0f },
                                        { "cutoff", 20.0f },
                                        { "character", character },
                                        { "resonance", 0.0f },
                                        { "quality", (float) quality } });
            FilterEffect filter;
            filter.prepare (testRate, testBlockSize);
            CHECK (filter.getLatencySamples() == FilterEffect::getLatency());

            // Clean, undriven, is a plain delay: the impulse comes out whole, that late. Driven goes through its half-band
            // filters, whose delay is a part of a sample off a whole one: measured by noise below 3 kHz, as the conformance
            // suite measures it.
            if (character < 0.5f)
            {
                const auto played = play (filter, values, 480, impulse());
                const auto loudest = std::max_element (played.left.begin(), played.left.end(),
                                                       [] (float a, float b) { return std::abs (a) < std::abs (b); });
                CHECK (loudest - played.left.begin() == filter.getLatencySamples());
            }
            else
            {
                const auto in = probe::makeNoiseBelow (testRate / 16.0, 8192, testRate, 6, 0.01f);
                const auto out = play (filter, values, in.getNumSamples(),
                                       [&] (int t) { return std::pair { in.getSample (0, t), in.getSample (1, t) }; });
                probe::Sound played (2, in.getNumSamples());
                std::copy (out.left.begin(), out.left.end(), played.getWritePointer (0));
                std::copy (out.right.begin(), out.right.end(), played.getWritePointer (1));
                const auto found = probe::findDelay (in, played, 0, 32);
                MESSAGE ("Driven at Quality " << quality << ": " << found.samples << " samples late");
                CHECK (std::abs (found.samples - filter.getLatencySamples()) < 0.5);
            }
        }
    }
}

TEST_CASE ("The filter Driven plays the same in blocks of 32 samples as of 512, its cutoff, Character and Quality moving")
{
    const auto noise = makeNoise (48000 * 2, 21, 0.5f), other = makeNoise (48000 * 2, 22, 0.5f);
    const Values values = [] (int t)
    {
        const auto step = t / 512;
        return with ({ { "cutoff", 200.0f * std::pow (2.0f, (float) (step % 40) / 8.0f) },
                       { "resonance", 0.97f },
                       { "character", step < 120 || step > 150 ? 1.0f : 0.0f },
                       { "drive", 12.0f },
                       { "quality", (float) ((step / 30) % 3) },
                       { "mix", 0.7f } });
    };

    const auto playIn = [&] (int size)
    {
        FilterEffect filter;
        filter.prepare (testRate, 512);
        return play (filter, values, (int) noise.size(), [&] (int t) { return std::pair { noise[(size_t) t], other[(size_t) t] }; }, size);
    };

    const auto big = playIn (512), small = playIn (32);
    auto worst = 0.0f;

    for (size_t i = 0; i < big.left.size(); ++i)
        worst = std::max ({ worst, std::abs (big.left[i] - small.left[i]), std::abs (big.right[i] - small.right[i]) });

    CHECK (getPeak (big.left, 0, big.left.size()) > 0.1f);
    CHECK (worst < 1.0e-5f);
}

TEST_CASE ("After a sleep, the filter Clean plays as one that never slept; Driven never sleeps, and hears its Resonance rise")
{
    // Through its runner: a second of noise, nine of silence (live, a Clean filter falls asleep once its ring has passed, where
    // a render plays on), then three of noise. Clean at 95 %, from half a second into the second sound, plays live what the
    // render plays. Driven, at 95 % and 100 %, never sleeps (it could start to sing by itself): every sample the render's.
    const auto sound = probe::join (probe::join (probe::makeNoise (48000, 3, 0.5f), probe::makeSilence (48000 * 9)),
                                    probe::makeNoise (48000 * 3, 4, 0.5f));
    const auto playIt = [] (const probe::Sound& in, float character, float amount, bool rendering, std::uint64_t& worked)
    {
        probe::Device device (FilterEffect::info);
        device.set ("character", character);
        device.set ("cutoff", 440.0f);
        device.set ("resonance", amount);
        device.isRendering = rendering;
        device.prepare (48000.0, 480);
        auto played = device.played (in, 480);
        worked = device.getRunner().getBlocksWorked();
        return played;
    };

    for (const auto& [character, amount] : { std::pair { 0.0f, 0.95f }, std::pair { 1.0f, 0.95f }, std::pair { 1.0f, 1.0f } })
    {
        CAPTURE (character);
        CAPTURE (amount);
        std::uint64_t liveBlocks = 0, renderBlocks = 0;
        const auto live = playIt (sound, character, amount, false, liveBlocks);
        const auto render = playIt (sound, character, amount, true, renderBlocks);

        if (character < 0.5f)
        {
            CHECK (liveBlocks < renderBlocks); // it slept
            CHECK (probe::getLargestDifference (live, render, 48000 * 10 + 24000) < 1.0e-5);
        }
        else
        {
            CHECK (liveBlocks == renderBlocks);
            CHECK (probe::getLargestDifference (live, render) < 1.0e-7);
        }

        if (amount >= 1.0f)
            CHECK (probe::getPeak (live, 48000 * 9, 48000) > 0.1); // singing on its own through the silence
    }

    // Driven, silence all along, its Resonance automated from 10 % at the song's start to 100 % four seconds in: two seconds
    // in (the Resonance at 55 %) a filter that slept would long have been asleep, and wouldn't hear the lane take it to where
    // it sings. Live plays what the render plays, and both sing.
    {
        const auto silence = probe::makeSilence (48000 * 6);
        const auto playRising = [&] (bool rendering)
        {
            probe::Device device (FilterEffect::info);
            device.set ("character", 1.0f);
            device.set ("cutoff", 440.0f);
            device.set ("filterSlope", 0.0f);
            device.isRendering = rendering;
            device.prepare (48000.0, 480);
            device.automate (FilterEffect::resonance, 0.1f, 1.0f, 48000 * 4);
            return device.played (silence, 480);
        };

        const auto live = playRising (false), render = playRising (true);
        CHECK (probe::getLargestDifference (live, render) < 1.0e-5);
        CHECK (toDecibels ((float) probe::getPeak (live, 48000 * 5, 48000)) > -15.0f);
    }

    // Clean, its Resonance at 100 %, silence all along, its Character's lane switching it to Driven two seconds in (inside a
    // block: the lane lines up with the sound, which comes as late as the filter's latency): live, it has long been asleep
    // there, and wakes for the block its values change in (devices::Processor::wakesWhenValuesChange(); known bug 203 until
    // 2026-11-18: it slept on in silence). Live plays what the render plays, and both sing.
    {
        const auto silence = probe::makeSilence (48000 * 6);
        const auto playSwitched = [&] (bool rendering, std::uint64_t& worked)
        {
            probe::Device device (FilterEffect::info);
            device.set ("character", 0.0f);
            device.set ("cutoff", 440.0f);
            device.set ("resonance", 1.0f);
            device.isRendering = rendering;
            device.prepare (48000.0, 480);
            device.automate (FilterEffect::character, 0.0f, 1.0f, 48000 * 4); // Driven from halfway, two seconds in
            auto played = device.played (silence, 480);
            worked = device.getRunner().getBlocksWorked();
            return played;
        };

        std::uint64_t liveBlocks = 0, renderBlocks = 0;
        const auto live = playSwitched (false, liveBlocks), render = playSwitched (true, renderBlocks);
        CHECK (liveBlocks < renderBlocks); // it slept before the switch
        CHECK (probe::getLargestDifference (live, render) < 1.0e-5);
        CHECK (toDecibels ((float) probe::getPeak (render, 48000 * 5, 48000)) > -15.0f);
    }

    // Clean and silent, a lane sweeping its Cutoff all along: nothing a Clean filter is given makes it sound, so it sleeps
    // through the lane (devices::Processor::wakesFor()), where waking for any change would keep every modulated filter on a
    // silent track at work.
    {
        probe::Device device (FilterEffect::info);
        device.set ("character", 0.0f);
        device.set ("resonance", 1.0f);
        device.isRendering = false;
        device.prepare (48000.0, 480);
        device.automate (FilterEffect::cutoff, 200.0f, 4000.0f, 48000 * 6);
        const auto played = device.played (probe::makeSilence (48000 * 6), 480);
        CHECK (probe::getPeak (played, 0, 48000 * 6) == 0.0);
        CHECK (device.getRunner().getBlocksWorked() < 100); // of 600: asleep after the first quarter second
    }

    // Self-Oscillating Sine (its preset) at 50 Hz, the song stopped, from silence: its ring grows from the kick for seconds
    // under the runner's floor for silence, and a filter that slept would sleep before it was heard. It reaches its level.
    {
        probe::Device device (FilterEffect::info);
        device.set ("filterType", 0.0f);
        device.set ("filterSlope", 0.0f);
        device.set ("cutoff", 50.0f);
        device.set ("resonance", 1.0f);
        device.set ("character", 1.0f);
        device.isRendering = false;
        device.prepare (48000.0, 480);
        auto stopped = probe::makeSilence (48000 * 10);
        device.playStopped (stopped, 480);
        CHECK (toDecibels ((float) probe::getPeak (stopped, 48000 * 9, 48000)) > -15.0f);
    }

    // Driven for a second of noise, then switched back to Clean, with silence after: it sleeps again, as Clean does.
    {
        probe::Device device (FilterEffect::info);
        device.set ("character", 1.0f);
        device.set ("cutoff", 440.0f);
        device.isRendering = false;
        device.prepare (48000.0, 480);
        auto noise = probe::makeNoise (48000, 5, 0.5f);
        device.play (noise, 480);
        const auto workedDriven = device.getRunner().getBlocksWorked();
        CHECK (workedDriven == 100);
        device.set ("character", 0.0f);
        auto silence = probe::makeSilence (48000 * 3);
        device.play (silence, 480);
        CHECK (device.getRunner().getBlocksWorked() < workedDriven + 300); // it slept
    }
}

TEST_CASE ("The Driven loop's two Newton steps solve it within a ten-thousandth of what solving it fully gives")
{
    // The loop solved fully (in double precision, by bisection, which can't miss), sample by sample, from the same states,
    // against the filter's guess and two steps: noise driven 24 dB into a low-pass and a high-pass, at 50 and 90 % of
    // its Resonance (ringing longer, the two drift apart by a float's rounding as much as by the solve: 1e-4 of full at 97 %;
    // where it sings, in phase, as any two singing filters would), the cutoff moving from 50 Hz to 15 kHz,
    // at twice 48 kHz.
    constexpr double rate = 96000.0;
    const auto noise = makeNoise (48000, 9, 16.0f);

    for (const auto mode : { dsp::FilterMode::lowPass, dsp::FilterMode::highPass })
        for (const auto amount : { 0.5f, 0.9f })
        {
            CAPTURE ((int) mode);
            CAPTURE (amount);
            dsp::MultimodeFilter filter;
            filter.resetDriven();
            double s1 = 0.0, s2 = 0.0;
            auto worst = 0.0, loudest = 0.0;

            for (size_t i = 0; i < noise.size(); ++i)
            {
                const auto hertz = 50.0 * std::pow (300.0, (double) (i % 9600) / 9600.0);
                const auto c = dsp::makeDriven ({ .mode = mode, .frequency = hertz, .resonance = amount, .steep = false }, rate);
                filter.setDriven (c);
                const auto x = (double) noise[i];
                const auto y = (double) filter.processDriven (noise[i]);

                // a v + g (kLarge v - bend softClip (v)) = g (x - s2) + s1, rising in v: bisected.
                const auto b = (double) c.g * (x - s2) + s1;
                const auto f = [&] (double v)
                {
                    return (double) c.a * v
                           + (double) c.g * ((double) c.kLarge * v - (double) c.bend * (double) devices::dsp::softClip ((float) v)) - b;
                };
                auto low = -1.0e3, high = 1.0e3;

                for (int step = 0; step < 200; ++step)
                {
                    const auto middle = 0.5 * (low + high);
                    (f (middle) > 0.0 ? high : low) = middle;
                }

                const auto v = 0.5 * (low + high);
                const auto lp = (double) c.g * v + s2;
                const auto damping = (double) c.kLarge * v - (double) c.bend * (double) devices::dsp::softClip ((float) v);
                const auto expected = (double) c.mx * x + (double) c.mv * v + (double) c.mlp * lp + (double) c.md * damping;
                s1 = 2.0 * v - s1;
                s2 = 2.0 * lp - s2;
                worst = std::max (worst, std::abs (y - expected));
                loudest = std::max (loudest, std::abs (expected));
            }

            MESSAGE ("Mode " << (int) mode << ", Resonance " << amount << ": off by " << worst << " at most, of " << loudest);
            CHECK (worst < 1.0e-4 * loudest);
        }
}
