// SPDX-License-Identifier: AGPL-3.0-or-later
// The saturator as DSP (src/devices/saturation/Saturator.h), measured: its curves and their level, Colour's even harmonics
// that keep a quiet sound's level and leave no offset, Crush's steps, one latency for every Quality, the Band (a split that
// sums flat, harmonics above the band and nothing under it, Output lifting the band alone, a copy for the mix in phase with
// it), Listen, switches and knobs that move without a click, a new shape heard soon and without a thump, Colour turned up
// without a swing, and the same samples in blocks of any size. Its aliases, at each Quality, set by hand or automated, are
// measured with the others' in AntialiasingTests.cpp.
#include "MotionTestHelpers.h"
#include "TestHelpers.h"
#include "devices/saturation/Saturator.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Saturator;
using Shape = Saturator::Shape;
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr double rate = 48000.0;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Saturator::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

std::vector<float> with (std::initializer_list<std::pair<std::string_view, float>> changes)
{
    auto values = getDefaults();

    for (const auto& [id, value] : changes)
    {
        const auto index = devices::findParameter (Saturator::info, id);
        REQUIRE (index >= 0);
        values[(size_t) index] = value;
    }

    return values;
}

struct Stereo
{
    std::vector<float> left, right;
};

using Input = std::function<std::pair<float, float> (int)>;
using Values = std::function<std::vector<float> (int)>;

// Plays a saturator that is prepared, in blocks, on a sound (input gives each channel's sample at a time), with the values a
// block starts with (values gives them at the block's first sample).
Stereo play (Saturator& saturator, int numSamples, int blockSize, const Input& input, const Values& values)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    Stereo played;
    std::int64_t place = 0;

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
        context.isPlaying = true;
        context.songSample = place;
        saturator.process (context);
        place += length;

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

// The same for a mono sound and values that stay, on a new saturator: its left channel.
std::vector<float> play (const std::vector<float>& values, int numSamples, const std::function<float (int)>& input, int blockSize = 480,
                         double sampleRate = rate)
{
    Saturator saturator;
    saturator.prepare (sampleRate, blockSize);
    return play (
               saturator, numSamples, blockSize,
               [&] (int t)
               {
                   const auto sample = input (t);
                   return std::pair { sample, sample };
               },
               [&] (int) { return values; })
        .left;
}

std::function<float (int)> sine (double frequency, float amplitude, double sampleRate = rate)
{
    return [=] (int t) { return amplitude * (float) std::sin (twoPi * frequency * t / sampleRate); };
}

// A tone's level in a stretch of a sound, by least squares, as a gain.
double getLevel (const std::vector<float>& sound, int from, int numSamples, double frequency, double sampleRate = rate)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = from; i < from + numSamples; ++i)
    {
        const auto s = std::sin (twoPi * frequency * i / sampleRate), c = std::cos (twoPi * frequency * i / sampleRate);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += sound[(size_t) i] * s;
        yc += sound[(size_t) i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    const auto a = (ys * cc - yc * sc) / determinant, b = (yc * ss - ys * sc) / determinant;
    return std::sqrt (a * a + b * b);
}

double toDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}

double getMean (const std::vector<float>& sound, int from, int numSamples)
{
    auto sum = 0.0;

    for (int i = from; i < from + numSamples; ++i)
        sum += sound[(size_t) i];

    return sum / numSamples;
}

double getRms (const std::vector<float>& sound, int from, int numSamples)
{
    auto sum = 0.0;

    for (int i = from; i < from + numSamples; ++i)
        sum += (double) sound[(size_t) i] * sound[(size_t) i];

    return std::sqrt (sum / numSamples);
}

// How rough a stretch of a sound is: its largest second difference, which a click (a step, or a kink) raises far above a
// tone's own.
float getRoughness (const std::vector<float>& sound, size_t from, size_t to)
{
    auto roughest = 0.0f;

    for (auto i = std::max<size_t> (from, 2); i < std::min (to, sound.size()); ++i)
        roughest = std::max (roughest, std::abs (sound[i] - 2.0f * sound[i - 1] + sound[i - 2]));

    return roughest;
}

// A low tone through a saturator whose values change every so often between two settings (a switch thrown, a knob turned
// fast): it is no more than twice as rough where they change as either setting is played steadily (a step of a
// thousandth would be ten times as rough).
void checkChangesWithoutClick (const char* what, const std::vector<float>& first, const std::vector<float>& second, float allowance = 2.0f)
{
    const std::string change (what);
    CAPTURE (change);
    constexpr int blockSize = 64, every = 12000, length = 6 * every; // long enough for a lopsided shape to warm up and fade in
    const auto tone = sine (100.0, 0.5f);
    const auto steady = [&] (const std::vector<float>& values) { return play (values, every * 2, tone, blockSize); };
    const auto smooth = std::max (getRoughness (steady (first), (size_t) every, (size_t) (2 * every)),
                                  getRoughness (steady (second), (size_t) every, (size_t) (2 * every)));

    Saturator saturator;
    saturator.prepare (rate, blockSize);
    const auto changing = play (
        saturator, length, blockSize,
        [&] (int t)
        {
            const auto sample = tone (t);
            return std::pair { sample, sample };
        },
        [&] (int t) { return (t / every) % 2 == 0 ? first : second; });

    const auto rough = getRoughness (changing.left, (size_t) every / 2, changing.left.size());
    CAPTURE (rough);
    CAPTURE (smooth);
    CHECK (rough <= smooth * allowance + 1.0e-5f);
}

// A sound through a Butterworth low-pass of an even order (sections of two), in double precision: what is under a frequency.
std::vector<double> getLowPassed (const std::vector<float>& sound, double frequency, int sections, double sampleRate = rate)
{
    std::vector<double> passed (sound.begin(), sound.end());

    for (int k = 1; k <= sections; ++k)
    {
        // Each section's Q, from the poles of the whole (RBJ's biquad).
        const auto q = 1.0 / (2.0 * std::cos ((2 * k - 1) * juce::MathConstants<double>::pi / (4.0 * sections)));
        const auto w = twoPi * frequency / sampleRate, alpha = std::sin (w) / (2.0 * q), cosine = std::cos (w);
        const auto a0 = 1.0 + alpha;
        const auto b0 = (1.0 - cosine) / 2.0 / a0, b1 = (1.0 - cosine) / a0, b2 = b0;
        const auto a1 = -2.0 * cosine / a0, a2 = (1.0 - alpha) / a0;
        auto x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        for (auto& sample : passed)
        {
            const auto y = b0 * sample + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = sample;
            y2 = y1;
            y1 = y;
            sample = y;
        }
    }

    return passed;
}

double getPeak (const std::vector<double>& sound, size_t from, size_t to)
{
    auto peak = 0.0;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// The number of levels a sound stays on for a while: the steps a slow ramp is rounded to.
int countPlateaus (const std::vector<float>& sound, size_t from, size_t to, float tolerance)
{
    std::vector<float> levels;
    auto runStart = from;

    for (auto i = from + 1; i <= to; ++i)
    {
        if (i < to && std::abs (sound[i] - sound[runStart]) <= tolerance)
            continue;

        if (i - runStart >= 16)
        {
            const auto level = sound[(runStart + i) / 2];

            if (levels.empty() || std::abs (levels.back() - level) > tolerance)
                levels.push_back (level);
        }

        runStart = i;
    }

    return (int) levels.size();
}
} // namespace

TEST_CASE ("The saturator describes itself: its parameters in the order the sound meets them, and a mix its runner applies")
{
    CHECK (devices::isValid (Saturator::info));
    CHECK (Saturator::info.typeID == std::string_view ("phaziteSaturator"));

    const std::vector<std::string_view> order { "drive", "shape",   "colour",   "tone",    "output", "autoGain",
                                                "band",  "bandLow", "bandHigh", "quality", "mix" };
    REQUIRE (Saturator::info.parameters.size() == order.size());

    for (size_t i = 0; i < order.size(); ++i)
        CHECK (Saturator::info.parameters[i].id == order[i]);

    CHECK (devices::findParameter (Saturator::info, devices::mixParameterID) == Saturator::mix);
    CHECK (Saturator::info.parameters[Saturator::shape].choices.size() == 6);
    CHECK (Saturator::info.parameters[Saturator::quality].choices.size() == 3);
    CHECK (Saturator::info.parameters[Saturator::band].defaultValue == doctest::Approx (0.0f));
    CHECK (Saturator::info.parameters[Saturator::colour].defaultValue == doctest::Approx (0.0f));
    CHECK (Saturator::info.parameters[Saturator::bandLow].defaultValue == doctest::Approx (3000.0f));
    CHECK (Saturator::info.parameters[Saturator::bandHigh].defaultValue == doctest::Approx (20000.0f));

    // The curves round the peaks off, as steep at the middle at any colour, and the fold folds back down.
    for (const auto shape : { Shape::soft, Shape::tape, Shape::tube, Shape::hard })
    {
        for (const auto colour : { 0.0f, 0.5f, 1.0f })
        {
            CAPTURE ((int) shape);
            CAPTURE (colour);
            CHECK (std::abs (Saturator::playCurve (shape, 0.0f, 0.0f, colour)) < 1.0e-6f);
            CHECK (Saturator::playCurve (shape, 0.5f, 0.0f, colour) > Saturator::playCurve (shape, 0.25f, 0.0f, colour));
            CHECK ((Saturator::playCurve (shape, 0.001f, 0.0f, colour) - Saturator::playCurve (shape, -0.001f, 0.0f, colour)) / 0.002f
                   == doctest::Approx (1.0f).epsilon (0.01));
        }

        CHECK (std::abs (Saturator::playCurve (shape, 10.0f, 0.0f, 0.0f)) <= 1.0f + 1.0e-4f);
    }

    CHECK (std::abs (Saturator::playCurve (Shape::fold, 2.0f, 0.0f, 0.0f)) < 1.0e-3f); // folded back down

    // Colour makes a curve lopsided: it gives way sooner on one side than the other.
    CHECK (std::abs (Saturator::playCurve (Shape::soft, 1.0f, 6.0f, 0.0f))
           == doctest::Approx (std::abs (Saturator::playCurve (Shape::soft, -1.0f, 6.0f, 0.0f))));
    CHECK (std::abs (Saturator::playCurve (Shape::soft, 1.0f, 6.0f, 1.0f))
           < 0.9f * std::abs (Saturator::playCurve (Shape::soft, -1.0f, 6.0f, 1.0f)));

    // Crush rounds to steps of 1/128 at 0 dB, twice as large for every 6 dB of drive.
    CHECK (Saturator::playCurve (Shape::crush, 0.3f, 0.0f, 0.0f) == doctest::Approx (std::round (0.3f * 128.0f) / 128.0f));
    CHECK (Saturator::playCurve (Shape::crush, 0.003f, 0.0f, 0.0f) == 0.0f);
    CHECK (Saturator::playCurve (Shape::crush, 0.2f, 36.0f, 0.5f) == 0.0f); // steps of 0.49 at +36 dB, whatever the colour
    CHECK (Saturator::playCurve (Shape::crush, 0.3f, 36.0f, 0.0f) == doctest::Approx (devices::dsp::decibelsToGain (36.0f) / 128.0f));

    // Crush's curve, played by its antiderivative: its integral meets itself at every half step.
    for (const auto x : { -2.5, -1.5, -0.5, 0.5, 1.5, 2.5 })
        CHECK (devices::dsp::CrushCurve::integral (x - 1.0e-9)
               == doctest::Approx (devices::dsp::CrushCurve::integral (x + 1.0e-9)).epsilon (1.0e-6));

    // The band: To kept an octave above From, and an edge at the end of its range has no crossover.
    const auto shown = Saturator::getBand (with ({ { "bandLow", 4000.0f }, { "bandHigh", 5000.0f } }));
    CHECK (shown.to == doctest::Approx (8000.0f));
    CHECK (shown.hasLow);
    CHECK (shown.hasHigh);
    CHECK_FALSE (Saturator::getBand (with ({ { "bandLow", 20.0f } })).hasLow);
    CHECK_FALSE (Saturator::getBand (getDefaults()).hasHigh);
}

TEST_CASE ("The saturator: harmonics when driven, and auto gain that keeps quiet sound level")
{
    // A quiet sine at the default drive, with auto gain: about as loud as it came in.
    const auto quiet = play (getDefaults(), 24000, sine (440.0, 0.01f));
    CHECK (toDecibels (getRms (quiet, 12000, 12000)) == doctest::Approx (toDecibels (0.01 / std::sqrt (2.0))).epsilon (0.03));

    // A loud sine driven hard: its peaks held down, and the third harmonic there.
    const auto loud = play (with ({ { "drive", 24.0f }, { "autoGain", 0.0f } }), 24000, sine (440.0, 0.5f));
    auto peak = 0.0f;

    for (size_t i = 12000; i < loud.size(); ++i)
        peak = std::max (peak, std::abs (loud[i]));

    CHECK (peak < 1.1f);
    CHECK (getLevel (loud, 12000, 12000, 1320.0) > 0.05);

    // Its drive goes below 0 dB too, for a gentle bend on a loud sound: a loud sine 18 dB down into the soft curve, without
    // auto gain, keeps almost all of its shape (its third harmonic 40 dB under it) and comes out 18 dB quieter.
    const auto gentle = play (with ({ { "drive", -18.0f }, { "autoGain", 0.0f } }), 24000, sine (440.0, 0.5f));
    CHECK (getLevel (gentle, 12000, 12000, 1320.0) < 0.01 * getLevel (gentle, 12000, 12000, 440.0));
    CHECK (toDecibels (getRms (gentle, 12000, 12000)) == doctest::Approx (toDecibels (0.5 / std::sqrt (2.0)) - 18.0).epsilon (0.03));
}

TEST_CASE ("The saturator's Colour adds a second harmonic that grows with it, keeps a quiet sound's level and leaves no offset")
{
    constexpr int length = 48000, from = 24000, count = 19200; // a whole number of the tone's cycles
    constexpr double hertz = 500.0;

    for (const auto shape : { Shape::soft, Shape::tape, Shape::hard, Shape::fold })
    {
        CAPTURE ((int) shape);
        const auto secondAt = [&] (float colour)
        {
            const auto played =
                play (with ({ { "shape", (float) shape }, { "drive", 12.0f }, { "colour", colour } }), length, sine (hertz, 0.5f));
            return toDecibels (getLevel (played, from, count, 2.0 * hertz) / 0.5);
        };

        // Even-sided at 0 %: no second harmonic; then more of it as Colour turns.
        const auto none = secondAt (0.0f), some = secondAt (0.3f), more = secondAt (1.0f);
        CHECK (none < -90.0);
        CHECK (some > -50.0);
        CHECK (more > some + 3.0);
    }

    // A quiet sine keeps its level within 0.1 dB at any colour, on every shape but Crush (the level it has at 0 %: the fold's
    // middle is steeper than a straight line, by pi / 2); and a loud one, made lopsided, leaves no offset: the DC blocker
    // takes it.
    for (const auto shape : { Shape::soft, Shape::tape, Shape::tube, Shape::hard, Shape::fold })
    {
        const auto even =
            getLevel (play (with ({ { "shape", (float) shape }, { "drive", 6.0f } }), length, sine (hertz, 0.001f)), from, count, hertz);

        for (const auto colour : { 0.0f, 0.5f, 1.0f })
        {
            CAPTURE ((int) shape);
            CAPTURE (colour);
            const auto quiet =
                play (with ({ { "shape", (float) shape }, { "drive", 6.0f }, { "colour", colour } }), length, sine (hertz, 0.001f));
            CHECK (std::abs (toDecibels (getLevel (quiet, from, count, hertz) / even)) < 0.1);

            if (shape != Shape::fold)
                CHECK (std::abs (toDecibels (even / 0.001)) < 0.1);

            const auto loud =
                play (with ({ { "shape", (float) shape }, { "drive", 18.0f }, { "colour", colour } }), length, sine (hertz, 0.5f));
            CHECK (std::abs (getMean (loud, from, count)) < 1.0e-3);
        }
    }
}

TEST_CASE ("The saturator's Crush rounds the sound to as many steps as its drive says, and keeps its level")
{
    // A slow ramp from -1 to 1 rounded to steps: 8 bits at 0 dB (256 steps across it, 257 levels counting both ends), a bit
    // fewer for every 6 dB of drive, so 2 bits at +36 dB and 12 at -24 dB.
    for (const auto& [decibels, levels] : { std::pair { 0.0f, 257 }, { 36.0f, 5 }, { 12.0f, 65 }, { -24.0f, 4057 } })
    {
        const auto ramp = levels * 200; // 200 samples on each step
        const auto played = play (with ({ { "shape", (float) Shape::crush }, { "drive", decibels } }), ramp + 1000,
                                  [ramp] (int t) { return std::clamp (-1.0f + 2.0f * (float) t / (float) ramp, -1.0f, 1.0f); });
        const auto step = devices::dsp::decibelsToGain (decibels) / 128.0f;
        const auto found = countPlateaus (played, 100, played.size(), 0.1f * step);
        CAPTURE (decibels);
        CHECK (std::abs (found - levels) <= levels / 50 + 2);
    }

    // A loud sine keeps its level, whatever the drive, Auto Gain on or off: Crush takes nothing off.
    for (const auto decibels : { 0.0f, 18.0f })
    {
        for (const auto autoGain : { 0.0f, 1.0f })
        {
            const auto played = play (with ({ { "shape", (float) Shape::crush }, { "drive", decibels }, { "autoGain", autoGain } }), 48000,
                                      sine (500.0, 0.5f));
            CAPTURE (decibels);
            CAPTURE (autoGain);
            CHECK (std::abs (toDecibels (getLevel (played, 24000, 19200, 500.0) / 0.5)) < 0.3);
        }
    }

    // Output is its gain.
    const auto louder =
        play (with ({ { "shape", (float) Shape::crush }, { "drive", 6.0f }, { "output", 6.0f } }), 48000, sine (500.0, 0.25f));
    CHECK (toDecibels (getLevel (louder, 24000, 19200, 500.0) / 0.25) == doctest::Approx (6.0).epsilon (0.05));
}

TEST_CASE ("The saturator's latency is the same at every Quality, 7 samples, and what the sound takes; automation moves the Quality "
           "anywhere")
{
    // An impulse through it, undriven, at each Quality and rate: the centre of what comes out is 7 samples late, which it
    // reports, whatever its values (no setting changes it).
    for (const auto quality : { 0.0f, 1.0f, 2.0f })
    {
        for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
        {
            const auto values = with ({ { "drive", -24.0f }, { "quality", quality } });
            Saturator saturator;
            saturator.prepare (sampleRate, 512);
            const auto latency = saturator.getLatencySamples();
            CHECK (latency == 7);
            CHECK (latency == Saturator::getLatency());
            CHECK (saturator.getLatencySamplesFor (values) == -1);

            const auto played = play (
                saturator, 4096, 512, [] (int t) { return std::pair { t == 100 ? 0.01f : 0.0f, t == 100 ? 0.01f : 0.0f }; },
                [&] (int) { return values; });
            auto weight = 0.0, moment = 0.0;

            for (size_t i = 0; i < played.left.size(); ++i)
            {
                weight += played.left[i];
                moment += (double) i * played.left[i];
            }

            CAPTURE (quality);
            CAPTURE (sampleRate);
            CHECK (moment / weight - 100.0 == doctest::Approx (7.0).epsilon (0.1 / 7.0));
        }
    }

    // Automation that moves the Quality from any to any plays it as late.
    for (const auto first : { 0.0f, 1.0f, 2.0f })
    {
        for (const auto then : { 0.0f, 1.0f, 2.0f })
        {
            const auto before = with ({ { "drive", -24.0f }, { "quality", first } });
            const auto after = with ({ { "drive", -24.0f }, { "quality", then } });
            Saturator saturator;
            saturator.prepare (rate, 256);
            const auto played = play (
                saturator, 8192, 256, [] (int t) { return std::pair { t == 5000 ? 0.01f : 0.0f, 0.0f }; },
                [&] (int t) { return t < 1024 ? before : after; });
            auto weight = 0.0, moment = 0.0;

            for (size_t i = 4096; i < played.left.size(); ++i)
            {
                weight += played.left[i];
                moment += (double) i * played.left[i];
            }

            CAPTURE (first);
            CAPTURE (then);
            CHECK (moment / weight - 5000.0 == doctest::Approx (7.0).epsilon (0.1 / 7.0));
        }
    }
}

TEST_CASE ("The saturator's copy for the mix comes as late as its sound while automation moves its Quality, and an impulse in the "
           "middle of a Quality's crossfade comes out once, 7 samples late")
{
    // At a mix of 50 % (so that it delays the copy of what came in), the Quality automated from one to another a little after
    // the start: an impulse in the middle of the crossfade, and one after it, in the sound and in the copy alike. Each comes
    // out 7 samples late, as what a saturator set to either Quality all along plays of it, or a blend of the two: both ways
    // are 7 samples late, so one impulse, not two a sample apart.
    constexpr int blockSize = 64, change = 1024, length = 8192;
    constexpr int inFade = change + 256 + 240; // past the new Quality's warm-up of 5 ms (256 samples at least), half way along
                                               // its fade of 10 ms
    constexpr int after = 6000;

    struct Played
    {
        std::vector<float> wet, dry;
    };

    // An impulse through a saturator whose values are the first until the change, then the second.
    const auto playImpulse = [] (const std::vector<float>& first, const std::vector<float>& second, int impulseAt)
    {
        Saturator saturator;
        saturator.prepare (rate, blockSize);
        juce::AudioBuffer<float> buffer (2, blockSize), dryBuffer (2, blockSize);
        Played played;

        for (int done = 0; done < length; done += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto sample = done + i == impulseAt ? 0.01f : 0.0f;

                for (int c = 0; c < 2; ++c)
                {
                    buffer.setSample (c, i, sample);
                    dryBuffer.setSample (c, i, sample);
                }
            }

            float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
            float* dryChannels[] = { dryBuffer.getWritePointer (0), dryBuffer.getWritePointer (1) };
            devices::ProcessContext context;
            context.channels = channels;
            context.dry = dryChannels;
            context.numChannels = 2;
            context.numSamples = blockSize;
            context.parameters = done < change ? first : second;
            context.isPlaying = true;
            context.songSample = done;
            saturator.process (context);
            played.wet.insert (played.wet.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
            played.dry.insert (played.dry.end(), dryBuffer.getReadPointer (0), dryBuffer.getReadPointer (0) + blockSize);
        }

        return played;
    };

    for (const auto& [first, then] : { std::pair { 0.0f, 2.0f }, { 2.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } })
    {
        for (const auto impulseAt : { inFade, after })
        {
            CAPTURE (first);
            CAPTURE (then);
            CAPTURE (impulseAt);
            const auto before = with ({ { "drive", -24.0f }, { "quality", first }, { "mix", 0.5f } });
            const auto later = with ({ { "drive", -24.0f }, { "quality", then }, { "mix", 0.5f } });
            const auto automated = playImpulse (before, later, impulseAt);
            const auto steadyFirst = playImpulse (before, before, impulseAt), steadyThen = playImpulse (later, later, impulseAt);

            for (const auto copy : { false, true })
            {
                CAPTURE (copy);
                const auto& sound = copy ? automated.dry : automated.wet;
                const auto& a = copy ? steadyFirst.dry : steadyFirst.wet;
                const auto& b = copy ? steadyThen.dry : steadyThen.wet;

                // Its centre 7 samples late.
                auto weight = 0.0, moment = 0.0;

                for (auto i = (size_t) impulseAt; i < sound.size(); ++i)
                {
                    weight += sound[i];
                    moment += (double) i * sound[i];
                }

                CHECK (moment / weight - impulseAt == doctest::Approx (7.0).epsilon (0.1 / 7.0));

                // What the two Qualities set by hand play of it, blended: the least-squares blend of the two leaves under
                // -25 dB of it (-33 to -52 dB measured; the blend moves a little along the impulse's few dozen samples; an impulse a sample later in
                // one of them would leave a few decibels).
                auto aa = 0.0, ab = 0.0, bb = 0.0, ya = 0.0, yb = 0.0, yy = 0.0;

                for (auto i = (size_t) impulseAt; i < sound.size(); ++i)
                {
                    aa += (double) a[i] * a[i];
                    ab += (double) a[i] * b[i];
                    bb += (double) b[i] * b[i];
                    ya += (double) sound[i] * a[i];
                    yb += (double) sound[i] * b[i];
                    yy += (double) sound[i] * sound[i];
                }

                const auto determinant = aa * bb - ab * ab;
                const auto [p, q] = std::abs (determinant) > 1.0e-30
                                        ? std::pair { (ya * bb - yb * ab) / determinant, (yb * aa - ya * ab) / determinant }
                                        : std::pair { ya / aa, 0.0 };
                auto left = 0.0;

                for (auto i = (size_t) impulseAt; i < sound.size(); ++i)
                {
                    const auto residual = sound[i] - p * a[i] - q * b[i];
                    left += residual * residual;
                }

                CAPTURE (p);
                CAPTURE (q);
                const auto residual = toDecibels (std::sqrt (left / yy));
                CHECK (residual < -25.0);
            }
        }
    }
}

TEST_CASE ("The saturator plays the same samples in blocks of any size while its shape changes every 53 ms, even or lopsided")
{
    // Three changes of shape 2560 samples apart (where blocks of 32 and of 512 both start), the next coming while the last's
    // DC blocker may still be joining: each change starts on the sample where the last one's fade ends, whatever the blocks.
    constexpr int every = 2560, length = 5 * every;
    juce::Random random (5);
    std::vector<float> noise ((size_t) length);

    for (auto& sample : noise)
        sample = 0.5f * (random.nextFloat() - 0.5f) + 0.3f * (float) std::sin (twoPi * 60.0 * (double) (&sample - noise.data()) / rate);

    struct Run
    {
        std::array<Shape, 4> shapes;
        float colour;
    };

    for (const auto& [shapes, colour] : { Run { { Shape::soft, Shape::hard, Shape::soft, Shape::tape }, 0.0f },
                                          Run { { Shape::tube, Shape::soft, Shape::tape, Shape::fold }, 0.5f },
                                          Run { { Shape::soft, Shape::tube, Shape::hard, Shape::soft }, 0.0f } })
    {
        CAPTURE (colour);
        const auto playIn = [&] (int blockSize)
        {
            Saturator saturator;
            saturator.prepare (rate, blockSize);
            return play (
                       saturator, length, blockSize, [&] (int t) { return std::pair { noise[(size_t) t], -noise[(size_t) t] }; },
                       [&] (int t)
                       {
                           const auto index = (size_t) std::min (3, t / every);
                           return with ({ { "shape", (float) shapes[index] }, { "colour", colour }, { "drive", 12.0f } });
                       })
                .left;
        };

        const auto small = playIn (32), large = playIn (512);
        auto worst = 0.0f;

        for (size_t i = 0; i < small.size(); ++i)
            worst = std::max (worst, std::abs (small[i] - large[i]));

        CHECK (worst < 1.0e-5f);
    }
}

TEST_CASE ("The saturator's switches and knobs, moved fast, make no click")
{
    // Each shape to each other.
    for (int from = 0; from < 6; ++from)
    {
        for (int to = 0; to < 6; ++to)
        {
            if (from == to)
                continue;

            CAPTURE (from);
            CAPTURE (to);
            checkChangesWithoutClick ("shape", with ({ { "shape", (float) from } }), with ({ { "shape", (float) to } }));
        }
    }

    // Auto Gain, the Quality, the Band, and each knob from one end to the other.
    checkChangesWithoutClick ("auto gain", with ({ { "drive", 18.0f } }), with ({ { "drive", 18.0f }, { "autoGain", 0.0f } }));
    checkChangesWithoutClick ("quality 8x to 2x", with ({ { "quality", 2.0f } }), with ({ { "quality", 0.0f } }));
    checkChangesWithoutClick ("quality 8x to 4x", with ({ { "quality", 2.0f } }), with ({ { "quality", 1.0f } }));
    checkChangesWithoutClick ("band", with ({ { "band", 0.0f } }), with ({ { "band", 1.0f }, { "bandLow", 300.0f } }));
    checkChangesWithoutClick ("drive", with ({ { "drive", -24.0f } }), with ({ { "drive", 36.0f } }));
    checkChangesWithoutClick ("colour", with ({ { "colour", 0.0f } }), with ({ { "colour", 1.0f } }));
    checkChangesWithoutClick ("tone", with ({ { "tone", -1.0f } }), with ({ { "tone", 1.0f } }));
    checkChangesWithoutClick ("output", with ({ { "output", -24.0f } }), with ({ { "output", 12.0f } }));
    checkChangesWithoutClick ("from", with ({ { "band", 1.0f }, { "bandLow", 20.0f } }),
                              with ({ { "band", 1.0f }, { "bandLow", 1000.0f } }));
    checkChangesWithoutClick ("to", with ({ { "band", 1.0f }, { "bandLow", 20.0f }, { "bandHigh", 300.0f } }),
                              with ({ { "band", 1.0f }, { "bandLow", 20.0f }, { "bandHigh", 20000.0f } }));
}

TEST_CASE ("The saturator's new shape is heard within 40 ms, lopsided or not, without a thump under it")
{
    // A 100 Hz sine at -6 dBFS, the shape changed at a second in: within 40 ms of the change the sound is the new shape's (as a
    // saturator playing it all along plays it), and what is under 30 Hz around the change stays within 3 dB of what it is in
    // steady playing of either shape (the 100 Hz tone's own leak through the measuring filter). A lopsided shape started its
    // DC blocker from rest under the sound, and was kept unheard for 150 ms while it settled; started at 200 Hz and glided
    // down to 10 Hz in 30 ms, it thumped at -25 dBFS (what it held of the tone couldn't follow the glide); faded in over 10 ms,
    // two shapes that play the tone differently thumped at -36 dBFS.
    constexpr int blockSize = 64, change = 48000, length = change + 24000, soon = change + 1920; // 40 ms after the change
    const auto tone = sine (100.0, 0.5f);

    struct Change
    {
        Shape from, to;
        float colour, quality = 0.0f, nextQuality = 0.0f;
    };

    // Shapes, and (a lopsided one playing) a Quality, whose new way takes the DC blockers of the one playing.
    for (const auto& [from, to, colour, quality, nextQuality] :
         { Change { Shape::soft, Shape::tube, 0.0f }, Change { Shape::soft, Shape::hard, 0.5f }, Change { Shape::tube, Shape::tape, 0.5f },
           Change { Shape::tube, Shape::soft, 0.0f }, Change { Shape::tube, Shape::tube, 0.5f, 0.0f, 2.0f } })
    {
        CAPTURE ((int) from);
        CAPTURE ((int) to);
        CAPTURE (colour);
        CAPTURE (nextQuality);
        const auto first = with ({ { "shape", (float) from }, { "colour", colour }, { "quality", quality } });
        const auto second = with ({ { "shape", (float) to }, { "colour", colour }, { "quality", nextQuality } });
        const auto steadyFirst = play (first, length, tone, blockSize), steadySecond = play (second, length, tone, blockSize);

        Saturator saturator;
        saturator.prepare (rate, blockSize);
        const auto changing = play (
                                  saturator, length, blockSize,
                                  [&] (int t)
                                  {
                                      const auto sample = tone (t);
                                      return std::pair { sample, sample };
                                  },
                                  [&] (int t) { return t < change ? first : second; })
                                  .left;

        // Heard: from 40 ms on, what is left of the old shape (the share of what tells the two apart that is still there) is
        // under 2 %. (What the new shape's DC blocker still takes off a little higher than its 10 Hz, as it glides down, turns
        // the lowest of that difference a little, across it, not along it.)
        std::vector<float> left (changing.size()), apart (changing.size());
        auto along = 0.0, whole = 0.0;

        for (size_t i = 0; i < changing.size(); ++i)
        {
            left[i] = changing[i] - steadySecond[i];
            apart[i] = steadyFirst[i] - steadySecond[i];

            if (i >= (size_t) soon && i < (size_t) soon + 960)
            {
                along += (double) left[i] * apart[i];
                whole += (double) apart[i] * apart[i];
            }
        }

        if (from != to) // two Qualities play a low tone alike
        {
            const auto oldShare = along / whole;
            CAPTURE (oldShare);
            CHECK (std::abs (oldShare) < 0.02);
        }

        // No thump: under 30 Hz, around the change, as in steady playing.
        const auto under = [&] (const std::vector<float>& sound)
        { return getPeak (getLowPassed (sound, 30.0, 2), (size_t) change, (size_t) change + 12000); };
        const auto steady = std::max (under (steadyFirst), under (steadySecond)), around = under (changing);
        CAPTURE (toDecibels (steady));
        CAPTURE (toDecibels (around));
        CHECK (around < steady * juce::Decibels::decibelsToGain (3.0));
    }
}

TEST_CASE ("The saturator's Colour, turned up fast from 0, leaves what is under 20 Hz below -26 dBFS and gone within 300 ms")
{
    // A 40 Hz sine at -3 dBFS, Colour from 0 to 100 % in 50 ms: the DC blocker, which the sound goes round while the curve is
    // even, has played all along, so it doesn't start from rest under the sound when the curve becomes lopsided (which made
    // a swing at about 10 Hz, about a third of the bass's level). What is left under 20 Hz, against a saturator at 100 %
    // all along, is the offset itself coming in as Colour turns (about a fifth of full scale here), which a 10 Hz high-pass
    // takes off over its own time: one lobe of -29 dBFS, gone in a quarter of a second.
    constexpr int blockSize = 64, start = 48000, sweep = 2400, length = start + 48000;
    const auto tone = sine (40.0, juce::Decibels::decibelsToGain (-3.0f));
    Saturator saturator;
    saturator.prepare (rate, blockSize);
    const auto played = play (
                            saturator, length, blockSize,
                            [&] (int t)
                            {
                                const auto sample = tone (t);
                                return std::pair { sample, sample };
                            },
                            [&] (int t) { return with ({ { "colour", std::clamp ((float) (t - start) / (float) sweep, 0.0f, 1.0f) } }); })
                            .left;
    const auto steady = play (with ({ { "colour", 1.0f } }), length, tone, blockSize);
    std::vector<float> difference (played.size());

    for (size_t i = 0; i < played.size(); ++i)
        difference[i] = played[i] - steady[i];

    const auto under = getLowPassed (difference, 20.0, 4);
    const auto swing = toDecibels (getPeak (under, (size_t) start, (size_t) length));
    CAPTURE (swing);
    CHECK (swing < -26.0);
    CHECK (toDecibels (getPeak (under, (size_t) start + 14400, (size_t) length)) < -70.0); // gone 300 ms on: no ring
}

TEST_CASE ("The saturator's Band, with the curve left straight, changes nothing but the sound's phase: flat within 0.1 dB at every "
           "Quality")
{
    // Quiet tones from 20 Hz to 20 kHz, with the band on and off: as loud either way, whatever its edges and its Quality.
    struct Edges
    {
        float from, to;
    };

    for (const auto quality : { 0.0f, 1.0f, 2.0f })
        for (const auto sampleRate : { 44100.0, 48000.0 })
        {
            for (const auto& edges :
                 { Edges { 3000.0f, 20000.0f }, Edges { 300.0f, 3000.0f }, Edges { 20.0f, 1000.0f }, Edges { 80.0f, 200.0f } })
            {
                for (const auto hertz :
                     { 20.0, 40.0, 100.0, 200.0, 300.0, 600.0, 1000.0, 2000.0, 3000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
                {
                    const auto length = (int) (sampleRate * 0.25) + (int) (4.0 * sampleRate / hertz);
                    const auto count = (int) (std::floor (0.2 * hertz) * sampleRate / hertz); // whole cycles
                    const auto off = play (with ({ { "drive", -24.0f }, { "quality", quality } }), length, sine (hertz, 0.01f, sampleRate),
                                           480, sampleRate);
                    const auto on = play (with ({ { "drive", -24.0f },
                                                  { "quality", quality },
                                                  { "band", 1.0f },
                                                  { "bandLow", edges.from },
                                                  { "bandHigh", edges.to } }),
                                          length, sine (hertz, 0.01f, sampleRate), 480, sampleRate);
                    CAPTURE (quality);
                    CAPTURE (sampleRate);
                    CAPTURE (edges.from);
                    CAPTURE (edges.to);
                    CAPTURE (hertz);
                    const auto from = length - count;
                    CHECK (std::abs (
                               toDecibels (getLevel (on, from, count, hertz, sampleRate) / getLevel (off, from, count, hertz, sampleRate)))
                           < 0.1);
                }
            }
        }
}

TEST_CASE ("The saturator's Band, driven hard, adds harmonics above it and nothing under From; Output lifts the band alone")
{
    // A tone in a band from 3 kHz, driven hard into a lopsided curve: its harmonics come, and nothing under the band but
    // what the tone leaves there through the crossover's slope, 80 dB under the band.
    {
        constexpr int length = 3 * 65536, fftOrder = 16, fftSize = 1 << fftOrder;
        const auto hertz = 6000.0; // a whole number of bins at 48 kHz, whose aliases land on its multiples
        const auto played =
            play (with ({ { "band", 1.0f }, { "bandLow", 3000.0f }, { "drive", 24.0f }, { "colour", 1.0f }, { "quality", 2.0f } }), length,
                  sine (hertz, 0.25f));
        juce::dsp::FFT fft (fftOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris);
        std::vector<float> data ((size_t) fftSize * 2, 0.0f);
        std::copy (played.end() - fftSize, played.end(), data.begin());
        window.multiplyWithWindowingTable (data.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);
        const auto binOf = [&] (double frequency) { return (size_t) std::lround (frequency * fftSize / rate); };
        const auto tone = (double) data[binOf (hertz)];
        auto under = 1.0e-12;

        for (auto k = binOf (20.0); k < binOf (2000.0); ++k)
            under = std::max (under, (double) data[k]);

        CHECK (toDecibels ((double) data[binOf (2.0 * hertz)] / tone) > -40.0); // the second harmonic is there
        CHECK (toDecibels (under / tone) < -80.0);
    }

    // Output lifts the band alone: a tone in it by 6 dB, tones far outside it not at all.
    const auto lifted =
        with ({ { "band", 1.0f }, { "bandLow", 1000.0f }, { "bandHigh", 4000.0f }, { "drive", -24.0f }, { "output", 6.0f } });
    const auto plain = with ({ { "band", 1.0f }, { "bandLow", 1000.0f }, { "bandHigh", 4000.0f }, { "drive", -24.0f } });

    // In the band's middle, nearly all of the tone is the band's (its crossovers' slopes overlap there a little); far
    // outside it, none.
    for (const auto hertz : { 100.0, 2000.0, 18000.0 })
    {
        const auto up = play (lifted, 24000, sine (hertz, 0.01f)), level = play (plain, 24000, sine (hertz, 0.01f));
        const auto lift = toDecibels (getLevel (up, 12000, 9600, hertz) / getLevel (level, 12000, 9600, hertz));
        CAPTURE (hertz);
        CAPTURE (lift);

        if (hertz < 1000.0 || hertz > 4000.0)
            CHECK (std::abs (lift) < 0.1);
        else
            CHECK ((lift > 4.5 && lift < 6.05));
    }
}

TEST_CASE ("The saturator's copy for the mix meets its sound in phase with the Band on: at a mix of 50 % they null")
{
    // The curve left straight, so that the sound is what came in through the band's split: mixed half and half with the copy
    // of what came in, which goes the same way, it is the same as the sound alone, to 60 dB under it.
    for (const auto quality : { 0.0f, 2.0f })
    {
        juce::Random random (9);
        std::vector<float> noise (48000);

        for (auto& sample : noise)
            sample = 0.5f * (random.nextFloat() - 0.5f);

        const auto playAt = [&] (float mixed)
        {
            auto values = with ({ { "band", 1.0f },
                                  { "bandLow", 500.0f },
                                  { "bandHigh", 5000.0f },
                                  { "drive", -24.0f },
                                  { "quality", quality },
                                  { "mix", mixed } });
            auto saturator = std::make_unique<Saturator>();
            return phazite::test::playThroughRunner (Saturator::info, std::move (saturator), values, noise, rate, 480);
        };

        const auto whole = playAt (1.0f), half = playAt (0.5f);
        auto worst = 0.0, loudest = 0.0;

        for (int i = 4800; i < whole.getNumSamples(); ++i)
        {
            worst = std::max (worst, (double) std::abs (whole.getSample (0, i) - half.getSample (0, i)));
            loudest = std::max (loudest, (double) std::abs (whole.getSample (0, i)));
        }

        CAPTURE (quality);
        CHECK (toDecibels (worst / loudest) < -60.0);
    }
}

TEST_CASE ("The saturator's Listen plays the band alone, as it goes out, whatever the mix, and stops when it is no longer asked")
{
    constexpr int blockSize = 64;
    Saturator saturator;
    const auto values = with ({ { "band", 1.0f }, { "bandLow", 2000.0f }, { "drive", -24.0f }, { "output", 6.0f } });
    saturator.prepare (rate, blockSize);
    const auto both = [] (int t)
    { return (float) (0.25 * std::sin (twoPi * 200.0 * t / rate) + 0.01 * std::sin (twoPi * 8000.0 * t / rate)); };
    const auto run = [&]
    {
        return play (
                   saturator, 9600, blockSize,
                   [&] (int t)
                   {
                       const auto sample = both (t);
                       return std::pair { sample, sample };
                   },
                   [&] (int) { return values; })
            .left;
    };

    const auto whole = run();
    CHECK_FALSE (saturator.overridesMix());
    CHECK (getLevel (whole, 4800, 4800, 200.0) == doctest::Approx (0.25).epsilon (0.01));

    phazite::test::HeldClock clock; // which moves only as the test says, however long the blocks take to play
    saturator.listenToBand();
    CHECK (saturator.isPlayingBand());
    const auto band = run();
    CHECK (saturator.overridesMix());
    CHECK (getLevel (band, 4800, 4800, 200.0) < 0.25 * 1.0e-3);                                 // the low tone is gone
    CHECK (getLevel (band, 4800, 4800, 8000.0) == doctest::Approx (0.01 * 2.0).epsilon (0.03)); // the band, lifted 6 dB

    clock.advance (499.0);
    CHECK (saturator.isPlayingBand());
    clock.advance (2.0);
    CHECK_FALSE (saturator.isPlayingBand());
    const auto again = run();
    CHECK_FALSE (saturator.overridesMix());
    CHECK (getLevel (again, 4800, 4800, 200.0) == doctest::Approx (0.25).epsilon (0.01));
}

TEST_CASE ("The saturator plays the same samples in blocks of any size, with its Band on and its settings moving")
{
    constexpr int length = 24576, change = 12288; // the values change where every block size below starts a block
    juce::Random random (3);
    std::vector<float> noise ((size_t) length);

    for (auto& sample : noise)
        sample = 0.5f * (random.nextFloat() - 0.5f);

    const auto first = with ({ { "band", 1.0f },
                               { "bandLow", 800.0f },
                               { "bandHigh", 6000.0f },
                               { "drive", 18.0f },
                               { "colour", 0.6f },
                               { "shape", (float) Shape::tube },
                               { "tone", 0.4f },
                               { "quality", 1.0f } });
    const auto second = with ({ { "band", 1.0f },
                                { "bandLow", 20.0f },
                                { "bandHigh", 2000.0f },
                                { "drive", 30.0f },
                                { "colour", 0.0f },
                                { "shape", (float) Shape::crush },
                                { "tone", -0.4f },
                                { "quality", 0.0f } });

    const auto playIn = [&] (int blockSize, int preparedFor)
    {
        Saturator saturator;
        saturator.prepare (rate, preparedFor);
        return play (
                   saturator, length, blockSize, [&] (int t) { return std::pair { noise[(size_t) t], -noise[(size_t) t] }; },
                   [&] (int t) { return t < change ? first : second; })
            .left;
    };

    const auto reference = playIn (32, 32);

    for (const auto& [blockSize, preparedFor] : { std::pair { 512, 512 }, { 64, 512 }, { 4096, 512 } })
    {
        CAPTURE (blockSize);
        const auto played = playIn (blockSize, preparedFor);
        auto worst = 0.0f;

        for (size_t i = 0; i < reference.size(); ++i)
            worst = std::max (worst, std::abs (played[i] - reference[i]));

        CHECK (worst < 1.0e-5f);
    }
}
