// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices of roadmap step 2.10, as DSP (src/devices/): the delay's timing and ping-pong, the saturator's curves and
// level, the filter's response, the multiband dynamics' flat split and both-way squeeze, the maximizer's ceiling, and the
// synth's unison. The stereo expander's have a file of their own (StereoExpanderTests.cpp), as the phaser's and the
// flanger's do (MotionEffectTests.cpp).
#include "TestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/dynamics/Maximizer.h"
#include "devices/dynamics/Multiband.h"
#include "devices/filter/FilterEffect.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"

#include <cmath>
#include <set>
#include <vector>

namespace
{
namespace devices = phazite::devices;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 480;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults (const devices::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, const devices::DeviceInfo& info, std::string_view id, float value)
{
    const auto index = devices::findParameter (info, id);
    REQUIRE (index >= 0);
    values[(size_t) index] = value;
}

// A stereo sound: its two channels.
struct Stereo
{
    std::vector<float> left, right;
};

// Plays a processor in blocks of 10 ms at 48 kHz on a sound (input gives each channel's sample at a time) and returns
// what comes out, at the tempo given.
template <typename Input>
Stereo play (devices::Processor& processor, const std::vector<float>& values, int numSamples, Input input, double tempo = 120.0)
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
        context.tempo = tempo;
        processor.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

auto sine (double frequency, float amplitude)
{
    return [frequency, amplitude] (int t)
    {
        const auto value = amplitude * (float) std::sin (twoPi * frequency * t / testRate);
        return std::pair { value, value };
    };
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

// How strong a frequency is in a sound (its amplitude), from a single bin of a Fourier transform.
float getAmplitudeAt (const std::vector<float>& sound, size_t from, size_t to, double frequency)
{
    auto re = 0.0, im = 0.0;

    for (auto i = from; i < to; ++i)
    {
        re += sound[i] * std::cos (twoPi * frequency * (double) i / testRate);
        im += sound[i] * std::sin (twoPi * frequency * (double) i / testRate);
    }

    return (float) (2.0 * std::sqrt (re * re + im * im) / (double) (to - from));
}
} // namespace

TEST_CASE ("The devices of step 2.10 describe themselves, with type and parameter IDs of their own")
{
    std::set<std::string_view> types;

    for (const auto* info : { &devices::Delay::info, &devices::Saturator::info, &devices::FilterEffect::info,
                              &devices::StereoExpander::info, &devices::Multiband::info, &devices::Maximizer::info, &devices::Synth::info })
    {
        INFO (std::string (info->typeID));
        CHECK (devices::isValid (*info));
        CHECK (types.insert (info->typeID).second);
    }

    // The synth's unison plays one voice at first.
    CHECK (devices::Synth::info.parameters[devices::Synth::unisonVoices].defaultValue == doctest::Approx (0.0f));

    // The stereo expander changes nothing until turned, and has no mix.
    CHECK (devices::findParameter (devices::StereoExpander::info, devices::mixParameterID) < 0);
}

TEST_CASE ("The delay: echoes on the beat at the song's tempo, fewer each time, and bouncing in ping-pong")
{
    auto values = getDefaults (devices::Delay::info);
    CHECK (devices::Delay::getDelaySeconds (values, 120.0) == doctest::Approx (0.375)); // a dotted eighth
    set (values, devices::Delay::info, "time", 8.0f);                                   // a quarter
    CHECK (devices::Delay::getDelaySeconds (values, 120.0) == doctest::Approx (0.5));
    CHECK (devices::Delay::getDelaySeconds (values, 90.0) == doctest::Approx (60.0 / 90.0));
    set (values, devices::Delay::info, "time", 13.0f); // a bar, at 30 BPM 8 seconds: the most is 5
    CHECK (devices::Delay::getDelaySeconds (values, 30.0) == doctest::Approx (devices::Delay::maximumSeconds));
    set (values, devices::Delay::info, "sync", 0.0f);
    set (values, devices::Delay::info, "milliseconds", 250.0f);
    CHECK (devices::Delay::getDelaySeconds (values, 120.0) == doctest::Approx (0.25));

    // An impulse with a quarter-note echo at 120 BPM: the first echo half a second later, the second smaller.
    values = getDefaults (devices::Delay::info);
    set (values, devices::Delay::info, "time", 8.0f);
    devices::Delay delay;
    delay.prepare (testRate, testBlockSize);
    const auto impulse = [] (int t) { return std::pair { t == 0 ? 1.0f : 0.0f, t == 0 ? 1.0f : 0.0f }; };
    const auto out = play (delay, values, 60000, impulse);

    CHECK (getPeak (out.left, 0, 23900) < 1.0e-4f);
    const auto first = getPeak (out.left, 23900, 24500), second = getPeak (out.left, 47900, 48500);
    CHECK (first > 0.05f);
    CHECK (second > 0.01f);
    CHECK (second < first * 0.5f);
    CHECK (delay.getTailSeconds() > 1.0);

    // Ping-pong: the first echo on the left, the next on the right.
    set (values, devices::Delay::info, "pingPong", 1.0f);
    delay.reset();
    const auto bounced = play (delay, values, 60000, impulse);
    CHECK (getPeak (bounced.left, 23900, 24500) > 0.02f);
    CHECK (getPeak (bounced.right, 23900, 24500) < 1.0e-3f);
    CHECK (getPeak (bounced.right, 47900, 48500) > 0.005f);
}

TEST_CASE ("The saturator: curves that round peaks off, harmonics when driven, and auto gain that keeps quiet sound level")
{
    using Shape = devices::Saturator::Shape;

    for (const auto shape : { Shape::soft, Shape::tape, Shape::tube, Shape::hard })
    {
        CHECK (std::abs (devices::Saturator::shapeSample (shape, 0.0f)) < 1.0e-6f);
        CHECK (devices::Saturator::shapeSample (shape, 0.5f) > devices::Saturator::shapeSample (shape, 0.25f));
        CHECK (std::abs (devices::Saturator::shapeSample (shape, 10.0f)) <= 1.0f + 1.0e-4f);
    }

    CHECK (std::abs (devices::Saturator::shapeSample (Shape::fold, 2.0f)) < 1.0e-3f); // folded back down

    // A quiet sine at the default drive, with auto gain: about as loud as it came in.
    auto values = getDefaults (devices::Saturator::info);
    devices::Saturator saturator;
    saturator.prepare (testRate, testBlockSize);
    const auto quiet = play (saturator, values, 24000, sine (440.0, 0.01f));
    CHECK (toDecibels (getRms (quiet.left, 12000, 24000)) == doctest::Approx (toDecibels (0.01f / std::sqrt (2.0f))).epsilon (0.03));

    // A loud sine driven hard: its peaks held down, and the third harmonic there.
    set (values, devices::Saturator::info, "drive", 24.0f);
    set (values, devices::Saturator::info, "autoGain", 0.0f);
    saturator.reset();
    const auto loud = play (saturator, values, 24000, sine (440.0, 0.5f));
    CHECK (getPeak (loud.left, 12000, 24000) < 1.1f);
    CHECK (getAmplitudeAt (loud.left, 12000, 24000, 1320.0) > 0.05f);
    CHECK (saturator.getLatencySamples() >= 0);
}

TEST_CASE ("The filter: open at first, and low-pass, high-pass and band-pass shapes that fall as steeply as their slope")
{
    auto values = getDefaults (devices::FilterEffect::info);
    CHECK (devices::FilterEffect::getMagnitude (values, 1000.0, testRate) == doctest::Approx (1.0).epsilon (0.02));

    set (values, devices::FilterEffect::info, "cutoff", 1000.0f);
    set (values, devices::FilterEffect::info, "resonance", 0.0f);
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 100.0, testRate)) == doctest::Approx (0.0).epsilon (0.5));
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 1000.0, testRate)) == doctest::Approx (-3.0).epsilon (0.2));
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 4000.0, testRate)) < -40.0f); // 24 dB per octave

    set (values, devices::FilterEffect::info, "filterSlope", 0.0f); // 12 dB per octave
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 4000.0, testRate)) > -30.0f);
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 4000.0, testRate)) < -18.0f);

    set (values, devices::FilterEffect::info, "filterType", 1.0f); // high-pass
    set (values, devices::FilterEffect::info, "filterSlope", 1.0f);
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 10000.0, testRate)) == doctest::Approx (0.0).epsilon (0.5));
    CHECK (toDecibels ((float) devices::FilterEffect::getMagnitude (values, 250.0, testRate)) < -40.0f);

    // Resonance rings at the cutoff.
    set (values, devices::FilterEffect::info, "filterType", 0.0f);
    set (values, devices::FilterEffect::info, "resonance", 0.8f);
    CHECK (devices::FilterEffect::getMagnitude (values, 1000.0, testRate) > 2.0);

    // Played: a low tone through, a high one gone.
    set (values, devices::FilterEffect::info, "resonance", 0.0f);
    devices::FilterEffect filter;
    filter.prepare (testRate, testBlockSize);
    const auto low = play (filter, values, 24000, sine (100.0, 0.5f));
    filter.reset();
    const auto high = play (filter, values, 24000, sine (8000.0, 0.5f));
    CHECK (getRms (low.left, 12000, 24000) > 0.3f);
    CHECK (getRms (high.left, 12000, 24000) < 0.005f);
}

TEST_CASE ("The filter's combs: Comb + on the cutoff's multiples, Comb - on its odd ones, ringing on after the sound")
{
    // The types every Phazite filter offers (dsp/MultimodeFilter.h), in its order.
    const auto& types = devices::FilterEffect::info.parameters[devices::FilterEffect::filterType].choices;
    REQUIRE (types.size() == 9);
    CHECK (types[3] == std::string_view ("Notch"));
    CHECK (types[(size_t) devices::dsp::FilterMode::combPlus] == std::string_view ("Comb +"));
    CHECK (types[(size_t) devices::dsp::FilterMode::combMinus] == std::string_view ("Comb -"));

    auto values = getDefaults (devices::FilterEffect::info);
    set (values, devices::FilterEffect::info, "filterType", (float) devices::dsp::FilterMode::combPlus);
    set (values, devices::FilterEffect::info, "cutoff", 1000.0f);
    set (values, devices::FilterEffect::info, "resonance", 0.5f);
    CHECK (devices::FilterEffect::getMagnitude (values, 2000.0, testRate) > 1.5);
    CHECK (devices::FilterEffect::getMagnitude (values, 1500.0, testRate) < 0.01);

    // It has no slope.
    set (values, devices::FilterEffect::info, "filterSlope", 0.0f);
    CHECK (devices::FilterEffect::getMagnitude (values, 2500.0, testRate) < 0.01);
    CHECK (devices::FilterEffect::getMagnitude (values, 3000.0, testRate) > 1.5);

    // Played: a tone on a peak comes through, louder; one between them goes.
    devices::FilterEffect filter;
    filter.prepare (testRate, testBlockSize);
    const auto onPeak = play (filter, values, 24000, sine (2000.0, 0.25f));
    filter.reset();
    const auto between = play (filter, values, 24000, sine (1500.0, 0.25f));
    CHECK (getRms (onPeak.left, 12000, 24000) > 0.25f);
    CHECK (getRms (between.left, 12000, 24000) < 0.005f);

    // Comb -: the odd multiples only, at the same pitch.
    set (values, devices::FilterEffect::info, "filterType", (float) devices::dsp::FilterMode::combMinus);
    CHECK (devices::FilterEffect::getMagnitude (values, 3000.0, testRate) > 1.5);
    CHECK (devices::FilterEffect::getMagnitude (values, 2000.0, testRate) < 0.01);

    // At the most resonance it rings on once a sound stops, and says how long: about a third of a second at 1 kHz.
    set (values, devices::FilterEffect::info, "filterType", (float) devices::dsp::FilterMode::combPlus);
    set (values, devices::FilterEffect::info, "resonance", 1.0f);
    filter.reset();
    const auto burst = [] (int t)
    {
        const auto value = t < 480 ? 0.5f * (float) std::sin (twoPi * 1000.0 * t / testRate) : 0.0f;
        return std::pair { value, value };
    };
    const auto rung = play (filter, values, 48000, burst);
    CHECK (filter.getTailSeconds() == doctest::Approx (0.343).epsilon (0.02));
    CHECK (getRms (rung.left, 4800, 5280) > 0.01f);     // 0.1 s after
    CHECK (getRms (rung.left, 24000, 24480) < 1.0e-4f); // 0.5 s after

    // A low-pass again has no tail.
    set (values, devices::FilterEffect::info, "filterType", 0.0f);
    play (filter, values, 480, burst);
    CHECK (filter.getTailSeconds() == doctest::Approx (0.0));
}

TEST_CASE ("The filter's vowel, phaser and sample and hold: after the combs, each with its own shape and no slope")
{
    const auto& types = devices::FilterEffect::info.parameters[devices::FilterEffect::filterType].choices;
    REQUIRE (types.size() == 9);
    CHECK (types[(size_t) devices::dsp::FilterMode::vowel] == std::string_view ("Vowel"));
    CHECK (types[(size_t) devices::dsp::FilterMode::phaser] == std::string_view ("Phaser"));
    CHECK (types[(size_t) devices::dsp::FilterMode::sampleAndHold] == std::string_view ("Sample & Hold"));

    // The vowel: at a cutoff of 1080 Hz, an A, its first formant at 650 Hz well above what lies past it.
    auto values = getDefaults (devices::FilterEffect::info);
    set (values, devices::FilterEffect::info, "filterType", (float) devices::dsp::FilterMode::vowel);
    set (values, devices::FilterEffect::info, "cutoff", 1080.0f);
    set (values, devices::FilterEffect::info, "resonance", 0.5f);
    const auto vowel = devices::FilterEffect::getMagnitude (values, 650.0, testRate);
    CHECK (vowel > 2.5 * devices::FilterEffect::getMagnitude (values, 850.0, testRate));

    // No slope for it.
    set (values, devices::FilterEffect::info, "filterSlope", 0.0f);
    CHECK (devices::FilterEffect::getMagnitude (values, 650.0, testRate) == doctest::Approx (vowel));

    // The phaser: a notch at the cutoff.
    set (values, devices::FilterEffect::info, "filterType", (float) devices::dsp::FilterMode::phaser);
    set (values, devices::FilterEffect::info, "cutoff", 1000.0f);
    set (values, devices::FilterEffect::info, "resonance", 0.0f);
    CHECK (devices::FilterEffect::getMagnitude (values, 1000.0, testRate) < 0.01);

    devices::FilterEffect filter;
    filter.prepare (testRate, testBlockSize);
    const auto notched = play (filter, values, 24000, sine (1000.0, 0.25f));
    CHECK (getRms (notched.left, 12000, 24000) < 0.005f);

    // Sample and hold at a cutoff of 1 kHz: 2.7 kHz folds back to 700 Hz.
    set (values, devices::FilterEffect::info, "filterType", (float) devices::dsp::FilterMode::sampleAndHold);
    filter.reset();
    const auto folded = play (filter, values, 24000, sine (2700.0, 0.25f));
    CHECK (getAmplitudeAt (folded.left, 12000, 24000, 700.0) > 0.1f);
    CHECK (filter.getTailSeconds() == doctest::Approx (0.0));
}

TEST_CASE ("The multiband dynamics: a flat split, quiet sound brought up and loud sound turned down")
{
    auto values = getDefaults (devices::Multiband::info);

    // Its curve: loud down, quiet up, silence left alone.
    set (values, devices::Multiband::info, "depth", 1.0f);
    CHECK (devices::Multiband::getGainChange (values, -6.0f) < -10.0f);
    CHECK (devices::Multiband::getGainChange (values, -55.0f) > 5.0f);
    CHECK (devices::Multiband::getGainChange (values, -120.0f) == doctest::Approx (0.0f));
    CHECK (devices::Multiband::getGainChange (values, -35.0f) == doctest::Approx (0.0f));

    // With no depth, every frequency comes out as loud as it went in.
    set (values, devices::Multiband::info, "depth", 0.0f);
    devices::Multiband multiband;
    multiband.prepare (testRate, testBlockSize);

    for (const auto frequency : { 50.0, 90.0, 1000.0, 2500.0, 8000.0 })
    {
        INFO (frequency);
        multiband.reset();
        const auto out = play (multiband, values, 48000, sine (frequency, 0.25f));
        CHECK (toDecibels (getRms (out.left, 24000, 48000)) == doctest::Approx (toDecibels (0.25f / std::sqrt (2.0f))).epsilon (0.02));
    }

    // At full depth: a quiet tone louder, a loud one quieter.
    set (values, devices::Multiband::info, "depth", 1.0f);
    multiband.reset();
    const auto quiet = play (multiband, values, 96000, sine (1000.0, 0.001f));
    multiband.reset();
    const auto loud = play (multiband, values, 96000, sine (1000.0, 0.8f));
    CHECK (getRms (quiet.left, 48000, 96000) > 2.0f * 0.001f / std::sqrt (2.0f));
    CHECK (getRms (loud.left, 48000, 96000) < 0.5f * 0.8f / std::sqrt (2.0f));
}

TEST_CASE ("The maximizer: louder, but never over its ceiling, and the limiter alone with nothing turned")
{
    auto values = getDefaults (devices::Maximizer::info);
    devices::Maximizer maximizer;
    maximizer.prepare (testRate, testBlockSize);
    const auto latency = (size_t) maximizer.getLatencySamples();

    // Without gain or squeeze, a quiet tone comes out as it went in, after the limiter's look-ahead.
    set (values, devices::Maximizer::info, "lowSqueeze", 0.0f);
    set (values, devices::Maximizer::info, "midSqueeze", 0.0f);
    set (values, devices::Maximizer::info, "highSqueeze", 0.0f);
    const auto plain = play (maximizer, values, 48000, sine (440.0, 0.1f));
    CHECK (toDecibels (getRms (plain.left, 24000, 48000)) == doctest::Approx (toDecibels (0.1f / std::sqrt (2.0f))).epsilon (0.02));

    // Pushed 18 dB with the default squeeze: much louder, and never over the ceiling, -1 dB.
    values = getDefaults (devices::Maximizer::info);
    set (values, devices::Maximizer::info, "gain", 18.0f);
    maximizer.reset();
    const auto pushed = play (maximizer, values, 96000, sine (220.0, 0.25f));
    CHECK (getRms (pushed.left, 48000, 96000) > 1.5f * 0.25f / std::sqrt (2.0f));
    CHECK (getPeak (pushed.left, latency, 96000) <= juce::Decibels::decibelsToGain (-1.0f) * 1.0001f);
    CHECK (getPeak (pushed.right, latency, 96000) <= juce::Decibels::decibelsToGain (-1.0f) * 1.0001f);

    // A band's squeeze turns it down once it is loud.
    CHECK (devices::Maximizer::getReduction (1.0f, 0.0f) > 6.0f);
    CHECK (devices::Maximizer::getReduction (0.0f, 0.0f) == doctest::Approx (0.0f));
    CHECK (devices::Maximizer::getReduction (1.0f, -30.0f) == doctest::Approx (0.0f));
}

TEST_CASE ("The synth's unison: one voice the same on both sides; seven spread across them")
{
    auto values = getDefaults (devices::Synth::info);
    devices::Synth synth;
    synth.prepare (testRate, testBlockSize);

    // Plays a held note for half a second and returns what came out.
    const auto playNote = [&synth] (const std::vector<float>& parameters)
    {
        synth.reset();
        juce::AudioBuffer<float> buffer (2, testBlockSize);
        Stereo played;

        for (int block = 0; block < 50; ++block)
        {
            buffer.clear();
            float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
            const devices::NoteEvent on { devices::NoteEvent::Type::noteOn, 0, 57, 1.0f };
            devices::ProcessContext context;
            context.channels = channels;
            context.numChannels = 2;
            context.numSamples = testBlockSize;
            context.parameters = parameters;

            if (block == 0)
                context.events = std::span<const devices::NoteEvent> (&on, 1);

            synth.process (context);

            for (int i = 0; i < testBlockSize; ++i)
            {
                played.left.push_back (buffer.getSample (0, i));
                played.right.push_back (buffer.getSample (1, i));
            }
        }

        return played;
    };

    const auto one = playNote (values);

    for (size_t i = 4800; i < 24000; i += 101)
        CHECK (one.left[i] == doctest::Approx (one.right[i]));

    set (values, devices::Synth::info, "unisonVoices", 6.0f); // seven
    const auto seven = playNote (values);
    auto difference = 0.0f;

    for (size_t i = 4800; i < 24000; ++i)
        difference = std::max (difference, std::abs (seven.left[i] - seven.right[i]));

    CHECK (difference > 0.01f);

    // About as loud: within 6 dB.
    const auto oneLevel = toDecibels (getRms (one.left, 4800, 24000)), sevenLevel = toDecibels (getRms (seven.left, 4800, 24000));
    CHECK (std::abs (oneLevel - sevenLevel) < 6.0f);
}
