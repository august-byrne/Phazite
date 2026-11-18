// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices of roadmap step 2.10, as DSP (src/devices/): the delay's timing and ping-pong, the filter's response and the
// synth's unison. Stereo's have a file of their own (StereoTests.cpp), as the phaser's and the flanger's do
// (MotionEffectTests.cpp), the multiband's (MultibandTests.cpp) and the saturator's (SaturatorTests.cpp).
#include "MotionTestHelpers.h"
#include "TestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/dsp/Filters.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Multiband.h"
#include "devices/filter/FilterEffect.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/Stereo.h"
#include "devices/synth/Synth.h"
#include "model/devices/internal/Devices.h"

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

    for (const auto* info : { &devices::Delay::info, &devices::Saturator::info, &devices::FilterEffect::info, &devices::Stereo::info,
                              &devices::Multiband::info, &devices::Synth::info })
    {
        INFO (std::string (info->typeID));
        CHECK (devices::isValid (*info));
        CHECK (types.insert (info->typeID).second);
    }

    // The synth's unison is off at first, one voice playing, and three once it is switched on (the owner, 2026-11-06: not
    // every part need be on at first).
    CHECK (devices::Synth::info.parameters[devices::Synth::unisonOn].defaultValue == doctest::Approx (0.0f));
    CHECK (devices::Synth::info.parameters[devices::Synth::unisonVoices].defaultValue == doctest::Approx (2.0f));

    // Stereo changes nothing until turned, and has no mix; nor has the multiband, whose LMH Mix blends its bands
    // with the dry sound before its master band, so that the master still limits what comes out.
    CHECK (devices::findParameter (devices::Stereo::info, devices::mixParameterID) < 0);
    CHECK (devices::findParameter (devices::Multiband::info, devices::mixParameterID) < 0);

    // The delay's parameters are named as its editor names them: the note value, and the time in milliseconds.
    CHECK (devices::Delay::info.parameters[(size_t) devices::findParameter (devices::Delay::info, "time")].name == "Note");
    CHECK (devices::Delay::info.parameters[(size_t) devices::findParameter (devices::Delay::info, "milliseconds")].name == "Time");
}

TEST_CASE ("The delay: echoes on the beat at the song's tempo, fewer each time, and bouncing in ping-pong")
{
    auto values = getDefaults (devices::Delay::info);
    CHECK (devices::Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (0.375)); // a dotted eighth
    set (values, devices::Delay::info, "time", 8.0f);                                        // a quarter
    CHECK (devices::Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (0.5));
    CHECK (devices::Delay::getDelaySeconds (values, 90.0, 4.0) == doctest::Approx (60.0 / 90.0));
    CHECK (devices::Delay::getDelaySeconds (values, 120.0, 3.0) == doctest::Approx (0.5)); // a note value in any time signature
    set (values, devices::Delay::info, "time",
         13.0f); // a bar, the song's: three quarter notes in 6/8; at 30 BPM in 4/4, 8 s, but at most 5
    CHECK (devices::Delay::getDelaySeconds (values, 120.0, 3.0) == doctest::Approx (1.5));
    CHECK (devices::Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (2.0));
    CHECK (devices::Delay::getDelaySeconds (values, 30.0, 4.0) == doctest::Approx (devices::Delay::maximumSeconds));
    set (values, devices::Delay::info, "sync", 0.0f);
    set (values, devices::Delay::info, "milliseconds", 250.0f);
    CHECK (devices::Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (0.25));

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

TEST_CASE ("The delay's echo comes exactly as late as its time says, however short")
{
    // It read its line before it pushed the new sample, and took nothing off for it: each echo a sample late, 2 % long at
    // 1 ms, which detuned a comb made with it. The echo is what came in through its band, exactly as late (whole
    // samples at these rates).
    for (const auto rate : { 48000.0, 96000.0 })
    {
        for (const auto milliseconds : { 1.0f, 2.5f, 10.0f })
        {
            auto values = getDefaults (devices::Delay::info);
            set (values, devices::Delay::info, "sync", 0.0f);
            set (values, devices::Delay::info, "milliseconds", milliseconds);
            set (values, devices::Delay::info, "feedback", 0.0f);
            set (values, devices::Delay::info, "lowCut", 20.0f);
            set (values, devices::Delay::info, "highCut", 20000.0f);
            devices::Delay delay;
            delay.prepare (rate, testBlockSize);
            juce::Random random (7);
            std::vector<float> noise (4800);

            for (auto& sample : noise)
                sample = random.nextFloat() - 0.5f;

            const auto out =
                play (delay, values, (int) noise.size(), [&noise] (int t) { return std::pair { noise[(size_t) t], noise[(size_t) t] }; });

            // What came in, as late as set, through the band.
            const auto late = (int) std::lround (milliseconds * 0.001 * rate);
            const auto lowCut = devices::dsp::makeFilter (devices::dsp::FilterShape::highPass, 20.0, rate, 0.70710678);
            const auto highCut = devices::dsp::makeFilter (devices::dsp::FilterShape::lowPass, 20000.0, rate, 0.70710678);
            devices::dsp::Filter low, high;
            auto largest = 0.0f;

            for (size_t i = 0; i < noise.size(); ++i)
            {
                const auto in = i >= (size_t) late ? noise[i - (size_t) late] : 0.0f;
                largest = std::max (largest, std::abs (out.left[i] - high.process (low.process (in, lowCut), highCut)));
            }

            INFO (milliseconds << " ms at " << rate << " Hz: the echo differs by " << toDecibels (largest / 0.5f) << " dB");
            CHECK (largest < 1.0e-5f);
        }
    }
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

    set (values, devices::Synth::info, "unisonOn", 1.0f);
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

TEST_CASE ("An effect handed what isn't sound plays on as if it had been silence: not a number, infinite, or beyond any level")
{
    // A plug-in gone wrong or a stray value in a float file can hand an effect a sample that isn't sound. Its filters and delay
    // lines would keep it, and pass it on, until they are reset (a NaN in a filter stays there), and the multiband read its
    // curve's table far outside itself and crashed. Each effect takes silence in its place: a block of it on the left, then
    // sound again, plays exactly as that block of silence does. The same in the sidechain of those that hear one.
    constexpr int numBlocks = 60, badBlock = 12;
    const auto soundAt = [] (int t)
    { return 0.25f * (float) (std::sin (twoPi * 220.0 * t / testRate) + 0.5 * std::sin (twoPi * 3100.0 * t / testRate)); };
    const auto keyAt = [] (int t) { return 0.5f * (float) std::sin (twoPi * 110.0 * t / testRate); };

    for (const auto* info : phazite::getOwnDevices())
    {
        if (info->kind != devices::DeviceKind::effect)
            continue;

        for (const auto inKey : { false, true })
        {
            if (inKey && ! info->hasSidechain)
                continue;

            // With a sidechain, set to hear it.
            auto values = getDefaults (*info);

            if (info->typeID == std::string_view (devices::Compressor::typeID))
                set (values, *info, "mode", (float) devices::Compressor::Mode::duck);
            else if (info->typeID == std::string_view (devices::Multiband::typeID))
                set (values, *info, "midKey", 1.0f);
            else if (info->hasSidechain)
                set (values, *info, "carrier", 3.0f); // the vocoder's: the track its sidechain hears

            // What it plays when the bad block's left samples (of its sound, or of its key) are this.
            const auto playWith = [&] (float inBadBlock)
            {
                auto processor = phazite::createOwnDevice (info->typeID);
                REQUIRE (processor != nullptr);
                processor->prepare (testRate, testBlockSize);
                juce::AudioBuffer<float> buffer (2, testBlockSize), key (2, testBlockSize);
                Stereo played;

                for (int block = 0; block < numBlocks; ++block)
                {
                    for (int i = 0; i < testBlockSize; ++i)
                    {
                        const auto t = block * testBlockSize + i;
                        const auto bad = block == badBlock;
                        buffer.setSample (0, i, bad && ! inKey ? inBadBlock : soundAt (t));
                        buffer.setSample (1, i, soundAt (t));
                        key.setSample (0, i, bad && inKey ? inBadBlock : keyAt (t));
                        key.setSample (1, i, keyAt (t));
                    }

                    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
                    const float* keys[] = { key.getReadPointer (0), key.getReadPointer (1) };
                    devices::ProcessContext context;
                    context.channels = channels;
                    context.numChannels = 2;
                    context.numSamples = testBlockSize;
                    context.parameters = values;

                    if (info->hasSidechain)
                    {
                        context.sidechain = keys;
                        context.numSidechainChannels = 2;
                    }

                    processor->process (context);
                    played.left.insert (played.left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + testBlockSize);
                    played.right.insert (played.right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + testBlockSize);
                }

                return played;
            };

            const auto expected = playWith (0.0f);
            CHECK (getPeak (expected.left, (size_t) ((badBlock + 2) * testBlockSize), expected.left.size()) > 1.0e-3f);

            for (const auto bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity(), 1.0e30f })
            {
                INFO (std::string (info->typeID) << std::string (inKey ? ", in its key: " : ": ") << bad);
                const auto played = playWith (bad);
                auto unsound = 0, different = 0;

                for (size_t i = 0; i < played.left.size(); ++i)
                {
                    unsound += std::isfinite (played.left[i]) && std::isfinite (played.right[i]) ? 0 : 1;
                    different +=
                        juce::exactlyEqual (played.left[i], expected.left[i]) && juce::exactlyEqual (played.right[i], expected.right[i])
                            ? 0
                            : 1;
                }

                CHECK (unsound == 0);
                CHECK (different == 0);
            }
        }
    }

    // A curve's gains, asked for a level that isn't one: a gain all the same, from inside its table.
    devices::curves::Gains gains;
    gains.update (devices::Compressor::getCurve (getDefaults (devices::Compressor::info)));

    for (const auto level : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(), -1.0f, 0.0f, 1.0e30f })
    {
        CAPTURE (level);
        CHECK (std::isfinite (gains.getDecibels (level)));
    }
}

TEST_CASE ("At a mix below 100 %, what an effect leaves straight keeps its level to 20 kHz: its filters turn the dry sound as its own")
{
    // The saturator, the filter's drive and the compressor's play at twice the rate, through half-band filters that turn the
    // phase of the top octave. Mixed with what came in merely delayed, that cancelled the highs (the saturator's at 50 %: 10 dB
    // down at 18 kHz). Each passes what came in through the same filters, so that the two meet in phase: a quiet tone, which
    // their curves leave straight, comes out as loud as it went in, at any mix.
    struct Case
    {
        const devices::DeviceInfo* info;
        std::vector<std::pair<std::string_view, float>> settings;
    };

    const Case cases[] = {
        { &devices::Saturator::info, { { "drive", 0.0f } } },
        { &devices::Saturator::info, { { "drive", 12.0f } } },
        // A high-pass at 20 Hz, which leaves these tones as they are, so that only its drive's filters turn them.
        { &devices::FilterEffect::info, { { "filterType", 1.0f }, { "cutoff", 20.0f }, { "resonance", 0.0f }, { "drive", 12.0f } } },
        // No compression, driven: a straight curve from its first point to its last.
        { &devices::Compressor::info,
          { { "points", 2.0f }, { "point2X", 1.0f }, { "point2Y", 1.0f }, { "point2Tension", 0.0f }, { "drive", 0.5f } } },
    };

    for (const auto rate : { 44100.0, 48000.0 })
    {
        for (const auto& device : cases)
        {
            for (const auto mix : { 1.0f, 0.5f, 0.25f })
            {
                for (const auto hertz : { 1000.0, 10000.0, 15000.0, 18000.0, 20000.0 })
                {
                    auto values = getDefaults (*device.info);

                    for (const auto& [id, value] : device.settings)
                        set (values, *device.info, id, value);

                    set (values, *device.info, devices::mixParameterID, mix);
                    constexpr int numSamples = 19200;
                    const auto played =
                        phazite::test::playThroughRunner (*device.info, phazite::createOwnDevice (device.info->typeID), values,
                                                          phazite::test::makeTone (hertz, numSamples, rate, 0.001f), rate, 480);
                    const auto level = phazite::test::readTone (played, hertz, rate, numSamples / 2, numSamples).left;

                    INFO (std::string (device.info->typeID) << " at " << rate << " Hz, mix " << mix << ", " << hertz
                                                            << " Hz: " << phazite::test::inDecibels (level / 0.001) << " dB");
                    CHECK (std::abs (phazite::test::inDecibels (level / 0.001)) < 0.5);
                }
            }
        }
    }
}
