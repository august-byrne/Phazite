// SPDX-License-Identifier: AGPL-3.0-or-later
// The vinyl as DSP (src/devices/vinyl/Vinyl.h, SurfaceNoise.h), measured: the sound passing in step and as late as it says,
// the wow's swing and its harmonic, the flutter's depth, the band, the two sides and the mono bass, the groove's distortion
// in the highs without aliases worth hearing, the hiss's level and the crackle's density, the noise only while the song plays
// and until two seconds after the sound stops, the crackle where the song is however it is reached, parameters that move
// without a click, its rest in silence, and all of it at every sample rate and block size, the same every time.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/dsp/Filters.h"
#include "devices/vinyl/Vinyl.h"
#include "engine/nodes/DeviceRunner.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::Vinyl;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Vinyl::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

std::vector<float> with (std::initializer_list<std::pair<std::string_view, float>> changes)
{
    auto values = getDefaults();

    for (const auto& [id, value] : changes)
    {
        const auto index = devices::findParameter (Vinyl::info, id);
        REQUIRE (index >= 0);
        values[(size_t) index] = value;
    }

    return values;
}

// Everything that colours the sound off: no wow or flutter, no distortion, the band and the sides as wide as they go, no
// noise. What is left is the sound, as late as the device says.
std::vector<float> clean (std::initializer_list<std::pair<std::string_view, float>> changes = {})
{
    auto values = with ({ { "wow", 0.0f },
                          { "flutter", 0.0f },
                          { "distortion", 0.0f },
                          { "width", 1.0f },
                          { "bassMono", 20.0f },
                          { "lowCut", 20.0f },
                          { "highCut", 20000.0f },
                          { "hiss", -96.0f },
                          { "crackle", -96.0f } });

    for (const auto& [id, value] : changes)
        values[(size_t) devices::findParameter (Vinyl::info, id)] = value;

    return values;
}

struct Stereo
{
    std::vector<float> left, right;
};

struct Transport
{
    bool playing = true;
    double tempo = 120.0;
    double beat = 0.0; // where the song is as the first block starts
};

// Plays a vinyl that is prepared, in blocks, on a sound (input gives each channel's sample at a time), with the values a
// block starts with, the song playing (or not) from a beat at a tempo.
Stereo play (Vinyl& vinyl, double rate, int numSamples, int blockSize, const std::function<std::pair<float, float> (int)>& input,
             const std::function<std::vector<float> (int)>& values, Transport transport = {})
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
        context.isPlaying = transport.playing;
        context.tempo = transport.tempo;
        context.beat = transport.beat + (transport.playing ? done * transport.tempo / 60.0 / rate : 0.0);
        vinyl.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

// The same for a mono sound and values that stay, on a new vinyl at a rate: both its channels.
Stereo play (const std::vector<float>& values, double rate, int numSamples, const std::function<float (int)>& input, int blockSize = 480,
             Transport transport = {})
{
    Vinyl vinyl;
    vinyl.prepare (rate, blockSize);
    return play (
        vinyl, rate, numSamples, blockSize,
        [&] (int t)
        {
            const auto sample = input (t);
            return std::pair { sample, sample };
        },
        [&] (int) { return values; }, transport);
}

std::function<float (int)> sine (double frequency, double rate, float amplitude)
{
    return [=] (int t) { return amplitude * (float) std::sin (twoPi * frequency * t / rate); };
}

// A tone's level and phase in a stretch of a sound, by least squares: its sine and cosine parts.
struct Tone
{
    double sinePart = 0.0, cosinePart = 0.0;

    double getLevel() const { return std::sqrt (sinePart * sinePart + cosinePart * cosinePart); }
    double getPhase() const { return std::atan2 (cosinePart, sinePart); }
};

Tone getTone (const float* sound, int from, int numSamples, double frequency, double rate)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = from; i < from + numSamples; ++i)
    {
        const auto s = std::sin (twoPi * frequency * i / rate), c = std::cos (twoPi * frequency * i / rate);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += sound[i] * s;
        yc += sound[i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    return { (ys * cc - yc * sc) / determinant, (yc * ss - ys * sc) / determinant };
}

double toDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-12));
}

// How far the band turns a frequency's phase: its two Butterworth filters, as the vinyl makes them.
double getBandPhase (const std::vector<float>& values, double frequency, double rate)
{
    namespace dsp = devices::dsp;
    const auto low = dsp::makeFilter (dsp::FilterShape::highPass, values[Vinyl::lowCut], rate, 0.70710678);
    const auto high = dsp::makeFilter (dsp::FilterShape::lowPass, values[Vinyl::highCut], rate, 0.70710678);
    return std::arg (dsp::getResponse (low, frequency, rate) * dsp::getResponse (high, frequency, rate));
}

double getRms (const std::vector<float>& sound, size_t from, size_t to)
{
    auto sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += (double) sound[i] * sound[i];

    return std::sqrt (sum / (double) std::max<size_t> (1, to - from));
}

float getPeak (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// How late a steady tone is in a sound from a sample on, from its phase: the tone's own phase, demodulated and averaged over
// four of its cycles, against what went in. In samples, every hop, up to a whole number of the tone's cycles (its phase
// tells nothing more): the first reading is put within half a cycle of near, and the rest follow on from it.
std::vector<double> getLateness (const std::vector<float>& sound, double frequency, double rate, int hop, int from, double near)
{
    const auto window = (int) std::lround (rate / frequency) * 4;
    const auto cycle = rate / frequency;
    std::vector<double> lateness;
    auto unwrapped = 0.0, last = 0.0;
    auto first = true;

    for (int start = from; start + window < (int) sound.size(); start += hop)
    {
        auto s = 0.0, c = 0.0;

        for (int i = start; i < start + window; ++i)
        {
            s += sound[(size_t) i] * std::sin (twoPi * frequency * i / rate);
            c += sound[(size_t) i] * std::cos (twoPi * frequency * i / rate);
        }

        const auto phase = std::atan2 (c, s);

        if (first)
        {
            // A later tone is behind in phase: the whole cycles that make it nearest near.
            unwrapped = phase;
            const auto late = -unwrapped / twoPi * cycle;
            unwrapped -= std::round ((near - late) / cycle) * twoPi;
            first = false;
        }
        else
        {
            unwrapped += std::remainder (phase - last, twoPi);
        }

        last = phase;
        lateness.push_back (-unwrapped / twoPi * cycle);
    }

    return lateness;
}

// The parts of a swing at a frequency in a series of readings every hop samples: its level there, by least squares.
double getSwingAt (const std::vector<double>& readings, double frequency, double rate, int hop)
{
    auto mean = 0.0;

    for (const auto reading : readings)
        mean += reading;

    mean /= (double) readings.size();
    std::vector<float> asFloats;

    for (const auto reading : readings)
        asFloats.push_back ((float) (reading - mean));

    return getTone (asFloats.data(), 0, (int) asFloats.size(), frequency, rate / hop).getLevel();
}
} // namespace

TEST_CASE ("The vinyl describes itself: an effect with a wow, a band, a groove, surface noise and a mix its runner applies")
{
    CHECK (devices::isValid (Vinyl::info));
    CHECK (Vinyl::info.typeID == "phaziteVinyl");
    CHECK (Vinyl::info.kind == devices::DeviceKind::effect);
    CHECK (devices::findParameter (Vinyl::info, devices::mixParameterID) == Vinyl::mix);
    CHECK (Vinyl::info.parameters[Vinyl::mix].defaultValue == doctest::Approx (1.0f));

    // Gentle at first: a record of a few cents' wow at 33 1/3 rpm, quiet hiss and crackle, a little narrower and duller.
    const auto values = getDefaults();
    CHECK (values[Vinyl::wow] <= 5.0f);
    CHECK (values[Vinyl::wowRate] * 60.0f == doctest::Approx (100.0f / 3.0f).epsilon (0.001));
    CHECK (values[Vinyl::flutter] == doctest::Approx (0.0f));
    CHECK (values[Vinyl::hiss] <= -60.0f);
    CHECK (values[Vinyl::crackle] <= -30.0f);
    CHECK (values[Vinyl::distortion] <= 0.2f);
    CHECK (values[Vinyl::width] >= 0.85f);
    CHECK (values[Vinyl::highCut] >= 12000.0f);

    // Hiss, Crackle and Bass Mono are off at their lowest, and only there.
    for (const auto which : { Vinyl::hiss, Vinyl::crackle, Vinyl::bassMono })
    {
        CAPTURE ((int) which);
        auto off = getDefaults();
        off[(size_t) which] = Vinyl::info.parameters[(size_t) which].minimum;
        CHECK (Vinyl::isOff (off, which));
        CHECK_FALSE (Vinyl::isOff (values, which));
    }

    // The wow once a turn: the turn's swing, a quarter as much at twice the rate, flat for longer than sharp.
    CHECK (Vinyl::getWowCents (8.0f, 0.0f) == doctest::Approx (6.0f).epsilon (0.01));
    CHECK (Vinyl::getWowCents (8.0f, 0.5f) == doctest::Approx (-10.0f).epsilon (0.01));
    CHECK (Vinyl::getWowCents (0.0f, 0.3f) == doctest::Approx (0.0f));

    // The groove's curve: the sound itself at 0 %, lopsided once bent, and bending more the louder it is.
    for (const auto sample : { -1.0f, -0.1f, 0.0f, 0.3f, 1.0f })
        CHECK (Vinyl::shapeSample (0.0f, sample) == doctest::Approx (sample));

    CHECK (std::abs (Vinyl::shapeSample (1.0f, 0.01f) - 0.01f) < 1.0e-4f);
    CHECK (std::abs (Vinyl::shapeSample (1.0f, 0.5f) + Vinyl::shapeSample (1.0f, -0.5f)) > 0.01f);
    CHECK (std::abs (Vinyl::shapeSample (1.0f, 1.0f)) < 1.0f);
}

TEST_CASE ("The vinyl passes the sound in step and as late as it says, at every sample rate, with everything off")
{
    for (const auto rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        CAPTURE (rate);
        Vinyl vinyl;
        vinyl.prepare (rate, 512);
        const auto latency = vinyl.getLatencySamples();

        // The delay line's middle: as deep a wow as the slowest rate allows, and the flutter's, about 11 ms.
        MESSAGE ("latency at " << rate << " Hz: " << latency << " samples, " << 1000.0 * latency / rate << " ms");
        CHECK (latency >= (int) std::lround (0.010 * rate));
        CHECK (latency <= (int) std::lround (0.012 * rate));

        // Tones through it, everything off: as loud as the band says and in step with what went in, as late as it says, but
        // for the band's own turn of the phase.
        const auto values = clean();

        for (const auto hertz : { 100.0, 1000.0, 5000.0 })
        {
            const auto length = (int) (rate / 4);
            const auto played = play (values, rate, length, sine (hertz, rate, 0.25f));
            std::vector<float> delayed ((size_t) length, 0.0f);

            for (int i = latency; i < length; ++i)
                delayed[(size_t) i] = sine (hertz, rate, 0.25f) (i - latency);

            const auto from = length / 2, count = length / 2 - 1;
            const auto out = getTone (played.left.data(), from, count, hertz, rate),
                       in = getTone (delayed.data(), from, count, hertz, rate);
            CAPTURE (hertz);
            CHECK (toDecibels (out.getLevel() / in.getLevel())
                   == doctest::Approx (toDecibels (Vinyl::getBandGain (values, hertz, rate))).epsilon (0.02));
            // The oversampling's half-band filters turn the top's phase a little more than its lows', which the latency counts.
            CHECK (std::abs (std::remainder (out.getPhase() - in.getPhase() - getBandPhase (values, hertz, rate), twoPi))
                   < juce::degreesToRadians (hertz < 2000.0 ? 0.5 : 3.0));
        }
    }
}

TEST_CASE ("The vinyl's runner mixes what came in back as late as the vinyl plays it, and its noise needs no sleep")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;
    phazite::engine::DeviceRunner runner (Vinyl::info, std::make_unique<Vinyl>());
    runner.prepare (rate, blockSize);
    const auto latency = runner.getLatencySamples();
    REQUIRE (latency > 0);
    CHECK_FALSE (runner.getProcessor().sleepsOnSilence());

    // At no mix: the sound as the vinyl's linear way leaves it (its oversampling turns the top octave's phase), as late as the
    // vinyl plays it: what came in, that late, to within what that turn makes of a sound below a sixteenth of the rate, with Bass
    // Mono off; none of the wow, flutter, distortion or crackle.
    const auto values = with (
        { { "mix", 0.0f }, { "wow", 25.0f }, { "flutter", 10.0f }, { "distortion", 1.0f }, { "crackle", -6.0f }, { "bassMono", 20.0f } });
    juce::Random random (5);
    std::vector<float> noise ((size_t) (40 * blockSize)), played;

    for (auto& sample : noise)
        sample = random.nextFloat() - 0.5f;

    // Below a sixteenth of the rate, where the half-band filters turn next to nothing.
    const auto lows = devices::dsp::makeFilter (devices::dsp::FilterShape::lowPass, rate / 16.0, rate, 0.70710678);

    devices::dsp::Filter first, second;

    for (auto& sample : noise)
        sample = second.process (first.process (sample, lows), lows);

    juce::AudioBuffer<float> buffer (2, blockSize);
    phazite::engine::DeviceRunner::Block block;
    block.values = values;
    block.isPlaying = true;

    for (size_t done = 0; done < noise.size(); done += (size_t) blockSize)
    {
        for (int channel = 0; channel < 2; ++channel)
            buffer.copyFrom (channel, 0, noise.data() + done, blockSize);

        block.beat = (double) done / rate * 2.0;
        runner.process (buffer, 0, blockSize, block);
        played.insert (played.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    auto largest = 0.0f;

    for (auto i = (size_t) std::max (2 * blockSize, latency); i < noise.size(); ++i)
        largest = std::max (largest, std::abs (played[i] - noise[i - (size_t) latency]));

    INFO ("what comes out differs from what came in by " << 20.0 * std::log10 (std::max (largest, 1.0e-12f) / 0.5f) << " dB under it");
    CHECK (largest < 0.5f * 0.01f); // 40 dB under it
}

TEST_CASE ("At a mix below 100 %, the vinyl meets what came in in phase: its oversampling and Bass Mono turn the copy as the sound")
{
    // The same tone on both sides, with nothing moving it (no wow or flutter, no distortion, no noise): what comes out is the
    // sound through the band, half and half (or a quarter) with what came in. The copy for the mix was only delayed, while the
    // sound went through the oversampling's half-band filters, which turn the top octave's phase, and through Bass Mono's
    // all-pass, which turns the middle's half a cycle at its frequency: half and half, the two cancelled each other there.
    // The copy now takes the sound's way through both, and what is left is the band's own doing.
    const auto levelAt = [] (double rate, double hertz, float mix, float bassMono)
    {
        const auto values = clean ({ { "bassMono", bassMono }, { "mix", mix } });
        phazite::engine::DeviceRunner runner (Vinyl::info, std::make_unique<Vinyl>());
        constexpr int blockSize = 480, numSamples = 28800;
        runner.prepare (rate, blockSize);
        juce::AudioBuffer<float> sound (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
            for (int channel = 0; channel < 2; ++channel)
                sound.setSample (channel, i, 0.25f * (float) std::sin (twoPi * hertz * i / rate));

        phazite::engine::DeviceRunner::Block block;
        block.values = values;

        for (int start = 0; start < numSamples; start += blockSize)
            runner.process (sound, start, blockSize, block);

        // Read over the second half, as a least-squares fit of the tone.
        std::complex<double> fit;

        for (int i = numSamples / 2; i < numSamples; ++i)
            fit += (double) sound.getSample (0, i) * std::polar (1.0, -twoPi * hertz * i / rate);

        return 2.0 * std::abs (fit) / (numSamples / 2) / 0.25;
    };

    for (const auto bassMono : { 100.0f, 20.0f }) // Bass Mono on and off
        for (const auto rate : { 44100.0, 48000.0 })
        {
            for (const auto hertz : { 50.0, 100.0, 200.0, 1000.0, 10000.0, 15000.0, 18000.0 })
            {
                // The band's response, as the vinyl plays it (Butterworth filters of 12 dB an octave); and what the way through the
                // record does to the level of both (its delay line read between samples dulls the very top a little).
                const auto band =
                    devices::dsp::getResponse (devices::dsp::makeFilter (devices::dsp::FilterShape::highPass, 20.0, rate, 0.70710678),
                                               hertz, rate)
                    * devices::dsp::getResponse (devices::dsp::makeFilter (devices::dsp::FilterShape::lowPass, 20000.0, rate, 0.70710678),
                                                 hertz, rate);
                const auto way = levelAt (rate, hertz, 1.0f, bassMono) / std::abs (band);
                CHECK (std::abs (20.0 * std::log10 (way)) < 0.5);

                for (const auto mix : { 0.5f, 0.25f })
                {
                    const auto expected = way * std::abs ((double) (1.0f - mix) + (double) mix * band); // the copy the sound's way
                    const auto decibels = 20.0 * std::log10 (levelAt (rate, hertz, mix, bassMono) / expected);
                    INFO ("Bass Mono at " << bassMono << " Hz, at " << rate << " Hz, mix " << mix << ", " << hertz << " Hz: " << decibels
                                          << " dB from the band's");
                    CHECK (std::abs (decibels) < 0.1);
                }
            }
        }
}

TEST_CASE ("The vinyl's Mix leaving 0 % and coming back to it, with Bass Mono on, turns the copy for the mix without a click")
{
    // Bass Mono's all-pass turns the bass half a cycle at its frequency. The copy for the mix, merely delayed at 0 % and
    // turned as the sound above it, was faded between the two over 64 samples as the Mix left 0 %, while the copy was still
    // nearly all that was heard: the bass went from its level through nothing to its level upside down in 1.5 ms, a click.
    // The copy is now turned at every mix while Bass Mono is on, so the Mix moving turns nothing. Measured against the Mix
    // held where the ramp starts and where it ends: the largest step from one sample to the next, which a click raises.
    namespace probe = phazite::test::probe;
    constexpr double sampleRate = 48000.0;
    constexpr int bar = 96000, numSamples = bar + 9600; // a bar at 120 beats a minute, and a little more

    // A 100 Hz tone, and a kick: a falling thump on each beat.
    auto tone = probe::makeSine (100.0, numSamples, sampleRate, 0.5f);
    probe::Sound kick (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = (double) (i % 24000) / sampleRate;
        const auto value =
            (float) (0.8 * std::exp (-t * 12.0) * std::sin (twoPi * (50.0 * t + 40.0 * (1.0 - std::exp (-t * 30.0)) / 30.0)));
        kick.setSample (0, i, value);
        kick.setSample (1, i, value);
    }

    const auto largestStep = [] (const probe::Sound& sound)
    {
        auto largest = 0.0;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 1; i < sound.getNumSamples(); ++i)
                largest = std::max (largest, (double) std::abs (sound.getSample (channel, i) - sound.getSample (channel, i - 1)));

        return largest;
    };

    for (const auto* what : { "the tone", "the kick" })
    {
        const auto& in = std::string_view (what) == "the tone" ? tone : kick;

        for (const auto rising : { true, false })
        {
            // The Mix moving, against it held where the ramp starts and where it ends.
            const auto playedWith = [&] (float from, float to)
            {
                probe::Device device (Vinyl::info);
                device.values = clean ({ { "bassMono", 100.0f }, { "mix", from } });
                device.prepare (sampleRate, 480);

                if (! juce::exactlyEqual (from, to))
                    device.automate (Vinyl::mix, from, to, bar);

                return device.played (in, 480);
            };

            const auto start = rising ? 0.0f : 0.3f, end = rising ? 0.3f : 0.0f;
            const auto moving = largestStep (playedWith (start, end));
            const auto held = std::max (largestStep (playedWith (start, start)), largestStep (playedWith (end, end)));
            const auto raised = 20.0 * std::log10 (moving / held);
            INFO (std::string (what) << ", the Mix " << std::string (rising ? "leaving 0 %" : "coming back to 0 %") << ": its largest step "
                                     << raised << " dB over the Mix held");
            CHECK (raised < 0.5);
        }
    }
}

TEST_CASE ("A vinyl's Mix resting a little over 0 % with Bass Mono on leaves the bass at its frequency as loud as at 0 %")
{
    // The copy for the mix was merely delayed at 0 % and turned as the Mix went through its first 5 %, so a Mix resting there
    // set the two half a cycle apart at Bass Mono's frequency, and the bass there fell by tens of decibels. Turned at every mix
    // while Bass Mono is on, the copy leaves a tone there at its level, whatever the Mix.
    namespace probe = phazite::test::probe;
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 28800;
    const auto tone = probe::makeSine (100.0, numSamples, sampleRate, 0.25f);

    const auto levelAt = [&tone] (float mix)
    {
        probe::Device device (Vinyl::info);
        device.values = clean ({ { "bassMono", 100.0f }, { "mix", mix } });
        device.prepare (sampleRate, 480);
        return probe::getToneLevel (device.played (tone, 480), 0, 100.0, sampleRate, numSamples / 2);
    };

    const auto atNone = levelAt (0.0f);

    for (const auto mix : { 0.01f, 0.025f, 0.05f })
    {
        const auto decibels = probe::inDecibels (levelAt (mix) / atNone);
        INFO ("at a Mix of " << mix * 100.0f << " %, 100 Hz is " << decibels << " dB from its level at 0 %");
        CHECK (std::abs (decibels) < 0.1);
    }
}

TEST_CASE ("Bass Mono switched on and off at a Mix below 100 % turns the vinyl's copy for the mix without a click")
{
    // As Bass Mono comes on or goes off, the sound's split comes in or goes over 20 ms, and the copy for the mix goes between
    // merely delayed and turned as far as the split has come in. Its largest step from one sample to the next, against the
    // vinyl's at either setting held: no larger, on a tone at Bass Mono's frequency and on a kick.
    namespace probe = phazite::test::probe;
    constexpr double sampleRate = 48000.0;
    constexpr int stretch = 9600, numStretches = 6;
    const auto tone = probe::makeSine (100.0, stretch * numStretches, sampleRate, 0.5f);
    probe::Sound kick (2, stretch * numStretches);

    for (int i = 0; i < kick.getNumSamples(); ++i)
    {
        const auto t = (double) (i % 24000) / sampleRate;
        const auto value =
            (float) (0.8 * std::exp (-t * 12.0) * std::sin (twoPi * (50.0 * t + 40.0 * (1.0 - std::exp (-t * 30.0)) / 30.0)));
        kick.setSample (0, i, value);
        kick.setSample (1, i, value);
    }

    const auto largestStep = [] (const probe::Sound& sound)
    {
        auto largest = 0.0;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 1; i < sound.getNumSamples(); ++i)
                largest = std::max (largest, (double) std::abs (sound.getSample (channel, i) - sound.getSample (channel, i - 1)));

        return largest;
    };

    const probe::Sound* inputs[] = { &tone, &kick };

    for (const auto* in : inputs)
    {
        const auto playedWith = [in] (int switches)
        {
            probe::Device device (Vinyl::info);
            device.values = clean ({ { "bassMono", switches == 2 ? 20.0f : 100.0f }, { "mix", 0.3f } });
            device.prepare (sampleRate, 480);
            probe::Sound out;

            for (int part = 0; part < numStretches; ++part)
            {
                if (switches == 0)
                    device.values[Vinyl::bassMono] = part % 2 == 0 ? 100.0f : 20.0f;

                const auto played = device.played (probe::getPart (*in, part * stretch, stretch), 480);
                out = part == 0 ? played : probe::join (out, played);
            }

            return out;
        };

        const auto switched = largestStep (playedWith (0)), held = std::max (largestStep (playedWith (1)), largestStep (playedWith (2)));
        const auto raised = 20.0 * std::log10 (switched / held);
        INFO (std::string (in == &tone ? "the tone" : "the kick") << ": its largest step " << raised << " dB over Bass Mono's held");
        CHECK (raised < 0.5);
    }
}

TEST_CASE ("The vinyl's copy for its mix rests at 100 % and comes back without a click as the Mix leaves it")
{
    // At 100 % its runner never reads the copy, so the vinyl doesn't make it: an all-pass, a second pair of half-band filters
    // and a read of a delay line a sample, each side. As the Mix leaves 100 % the copy is made again from what came in lately,
    // so that its first samples are as if it had never stopped: what comes out is what comes out of a vinyl whose Mix was a
    // hair under 100 %, its copy made all along.
    namespace probe = phazite::test::probe;
    constexpr double sampleRate = 48000.0;
    const auto in = probe::makeNoise (48000, 9, 0.5f);

    const auto playedFrom = [&in] (float start)
    {
        probe::Device device (Vinyl::info);
        device.values = clean ({ { "bassMono", 100.0f }, { "mix", start } });
        device.prepare (sampleRate, 480);
        auto out = device.played (probe::getPart (in, 0, 24000), 480);
        device.values[Vinyl::mix] = 0.5f; // by hand: the runner glides there over the next block
        return probe::join (out, device.played (probe::getPart (in, 24000, 24000), 480));
    };

    const auto rested = playedFrom (1.0f), madeAllAlong = playedFrom (0.99999f);
    const auto under = probe::inDecibels (probe::getLargestDifference (rested, madeAllAlong) / probe::getPeak (madeAllAlong));
    INFO ("the two differ by " << under << " dB under the sound");
    CHECK (under < -80.0);
}

TEST_CASE ("Finding 3: the vinyl's copy for its mix goes on a second at 100 %: a Mix moved through it and back never makes it again")
{
    // As the Mix left 100 % the copy was made again at once from what came in lately, its latency's worth and 256 samples
    // more, in the block it left in: automation moving the Mix through 100 % and back made it again each time. It now goes
    // on for a second after the Mix reaches 100 % and rests only then; made again after a longer rest, it comes back as if it
    // had never stopped, at 192 kHz as at 48.
    namespace probe = phazite::test::probe;

    for (const auto sampleRate : { 48000.0, 192000.0 })
    {
        CAPTURE (sampleRate);
        const auto seconds = [sampleRate] (double s) { return (int) (s * sampleRate); };
        const auto in = probe::makeNoise (seconds (2.6), 9, 0.5f);

        // The Mix at 50 %, then at 100 % for half a second and back, then for a second and a half and back; or, made all
        // along, a hair under 100 % where it is at 100 %.
        const auto playedWith = [&] (float full, std::vector<int>* rebuilds)
        {
            probe::Device device (Vinyl::info);
            device.values = clean ({ { "bassMono", 100.0f }, { "mix", 0.5f } });
            device.prepare (sampleRate, 480);
            auto& vinyl = dynamic_cast<Vinyl&> (device.getRunner().getProcessor());
            probe::Sound out;
            auto at = 0;

            for (const auto& [mix, length] : { std::pair { 0.5f, 0.2 }, { full, 0.5 }, { 0.5f, 0.2 }, { full, 1.5 }, { 0.5f, 0.2 } })
            {
                device.values[Vinyl::mix] = mix;
                const auto played = device.played (probe::getPart (in, at, seconds (length)), 480);
                out = at == 0 ? played : probe::join (out, played);
                at += seconds (length);

                if (rebuilds != nullptr)
                    rebuilds->push_back (vinyl.getDryRebuilds());
            }

            return out;
        };

        std::vector<int> rebuilds;
        const auto rested = playedWith (1.0f, &rebuilds), madeAllAlong = playedWith (0.99999f, nullptr);
        CHECK (rebuilds == std::vector<int> { 1, 1, 1, 1, 2 }); // at its start; and after resting, not after half a second
        const auto under = probe::inDecibels (probe::getLargestDifference (rested, madeAllAlong) / probe::getPeak (madeAllAlong));
        INFO ("the two differ by " << under << " dB under the sound");
        CHECK (under < -80.0);
    }
}

TEST_CASE ("The vinyl's wow swings the pitch once a turn, with a quarter as much again at twice the rate, as deep as it says")
{
    constexpr double rate = 48000.0, hertz = 1000.0;
    constexpr int hop = 96; // a reading every 2 ms

    for (const auto& [cents, turns] : { std::pair { 8.8f, 1.82f }, { 25.0f, 0.25f }, { 4.0f, 0.5555556f }, { 15.0f, 4.0f } })
    {
        CAPTURE (cents);
        CAPTURE (turns);
        const auto seconds = std::max (6.0, 3.0 / turns);
        Vinyl vinyl;
        vinyl.prepare (rate, 480);
        const auto latency = vinyl.getLatencySamples();
        const auto played =
            play (clean ({ { "wow", cents }, { "wowRate", turns } }), rate, (int) ((seconds + 0.1) * rate), sine (hertz, rate, 0.25f));
        const auto lateness = getLateness (played.left, hertz, rate, hop, (int) (0.1 * rate), latency);

        // How late the sound is swings by the wow's depth over the turn's rate: d = A / (2 pi f) for a pitch swing of A.
        const auto swing = std::pow (2.0, cents / 1200.0) - 1.0;
        const auto first = getSwingAt (lateness, turns, rate, hop) / rate;
        const auto second = getSwingAt (lateness, 2.0 * turns, rate, hop) / rate;
        CHECK (first * twoPi * turns == doctest::Approx (swing).epsilon (0.03));
        CHECK (second * twoPi * 2.0 * turns == doctest::Approx (0.25 * swing).epsilon (0.06));

        // And it swings around the latency, the delay line's middle: within a sample of it, over whole turns.
        const auto readingsATurn = rate / hop / turns;
        const auto whole = (size_t) (std::floor ((double) lateness.size() / readingsATurn) * readingsATurn);
        REQUIRE (whole > 0);
        auto mean = 0.0;

        for (size_t i = 0; i < whole; ++i)
            mean += lateness[i];

        mean /= (double) whole;
        CHECK (std::abs (std::remainder (mean - latency, rate / hertz)) < 1.0); // the tone's phase tells it to a cycle
    }
}

TEST_CASE ("The vinyl's flutter wobbles the pitch as deep as it says, quickly and unsteadily, and the same every time")
{
    constexpr double rate = 48000.0, hertz = 1000.0;
    constexpr int hop = 48; // a reading every millisecond
    const auto values = clean ({ { "flutter", 6.0f } });
    const auto played = play (values, rate, (int) (20.0 * rate), sine (hertz, rate, 0.25f));
    const auto lateness = getLateness (played.left, hertz, rate, hop, (int) (0.1 * rate), 0.0);

    // Its pitch, from how fast the lateness moves: its RMS is the depth over the square root of two, as a sine's of that depth.
    std::vector<double> cents;

    for (size_t i = 1; i < lateness.size(); ++i)
        cents.push_back (1200.0 * std::log2 (1.0 - (lateness[i] - lateness[i - 1]) / hop));

    auto sum = 0.0, squares = 0.0;

    for (const auto each : cents)
    {
        sum += each;
        squares += each * each;
    }

    const auto mean = sum / (double) cents.size();
    const auto rms = std::sqrt (squares / (double) cents.size() - mean * mean);
    MESSAGE ("flutter of 6 cents: " << rms << " cents RMS");
    CHECK (rms == doctest::Approx (6.0 / std::sqrt (2.0)).epsilon (0.25));

    // Mostly below 15 Hz, little at 40 Hz.
    CHECK (getSwingAt (lateness, 4.0, rate, hop) > 10.0 * getSwingAt (lateness, 40.0, rate, hop));

    // The same twice.
    const auto again = play (values, rate, (int) (2.0 * rate), sine (hertz, rate, 0.25f));
    auto same = true;

    for (size_t i = 0; i < again.left.size(); ++i)
        same = same && juce::exactlyEqual (again.left[i], played.left[i]);

    CHECK (same);
}

TEST_CASE ("The vinyl's band rolls the ends off, its two sides narrow, and the lows below Bass Mono go to the middle")
{
    constexpr double rate = 48000.0;
    const auto length = (int) (rate / 2);
    const auto from = length / 2, count = length / 2 - 1;

    // The band, as Low Cut and High Cut set it: as getBandGain() says.
    const auto band = clean ({ { "lowCut", 200.0f }, { "highCut", 4000.0f } });

    for (const auto hertz : { 50.0, 200.0, 1000.0, 4000.0, 12000.0 })
    {
        CAPTURE (hertz);
        const auto played = play (band, rate, length, sine (hertz, rate, 0.25f));
        CHECK (toDecibels (getTone (played.left.data(), from, count, hertz, rate).getLevel() / 0.25)
               == doctest::Approx (toDecibels (Vinyl::getBandGain (band, hertz, rate))).epsilon (0.03));
    }

    CHECK (toDecibels (Vinyl::getBandGain (band, 200.0, rate)) == doctest::Approx (-3.0).epsilon (0.02));
    CHECK (toDecibels (Vinyl::getBandGain (band, 4000.0, rate)) == doctest::Approx (-3.0).epsilon (0.03));
    CHECK (toDecibels (Vinyl::getBandGain (band, 50.0, rate)) < -23.0);

    // The sides: the left and right out of step, at 1 kHz (above the bass) and at 50 Hz (in it).
    const auto playSide = [&] (const std::vector<float>& values, double hertz)
    {
        Vinyl vinyl;
        vinyl.prepare (rate, 480);
        const auto played = play (
            vinyl, rate, length, 480,
            [&] (int t)
            {
                const auto sample = sine (hertz, rate, 0.25f) (t);
                return std::pair { sample, -sample };
            },
            [&] (int) { return values; });
        return toDecibels (getTone (played.left.data(), from, count, hertz, rate).getLevel() / 0.25);
    };

    CHECK (playSide (clean ({ { "width", 0.5f } }), 1000.0) == doctest::Approx (-6.02).epsilon (0.01));
    CHECK (playSide (clean ({ { "width", 0.0f } }), 1000.0) < -80.0);
    CHECK (playSide (clean ({ { "bassMono", 200.0f } }), 1000.0) == doctest::Approx (0.0).epsilon (0.05));
    CHECK (playSide (clean ({ { "bassMono", 200.0f } }), 50.0) < -40.0);

    // Off at its lowest: the bass's sides stay, as the band leaves them; and the middle stays as it was, bass included.
    const auto atFifty = toDecibels (Vinyl::getBandGain (clean(), 50.0, rate));
    CHECK (playSide (clean(), 50.0) == doctest::Approx (atFifty).epsilon (0.05));
    const auto middle = play (clean ({ { "bassMono", 200.0f } }), rate, length, sine (50.0, rate, 0.25f));
    CHECK (toDecibels (getTone (middle.left.data(), from, count, 50.0, rate).getLevel() / 0.25)
           == doctest::Approx (atFifty).epsilon (0.05));
}

TEST_CASE ("The vinyl's groove distorts the loud highs more than the lows, adds nothing at 0 %, and folds back no aliases worth hearing")
{
    constexpr double rate = 48000.0;
    constexpr int length = 48000, from = 24000, count = 19200;

    const auto harmonics = [&] (float distortionValue, double hertz, float amplitude)
    {
        const auto played = play (clean ({ { "distortion", distortionValue } }), rate, length, sine (hertz, rate, amplitude));
        const auto fundamental = getTone (played.left.data(), from, count, hertz, rate).getLevel();
        return std::pair { toDecibels (getTone (played.left.data(), from, count, 2.0 * hertz, rate).getLevel() / fundamental),
                           toDecibels (getTone (played.left.data(), from, count, 3.0 * hertz, rate).getLevel() / fundamental) };
    };

    // None at 0 %.
    const auto none = harmonics (0.0f, 3000.0, 0.5f);
    CHECK (none.first < -100.0);
    CHECK (none.second < -100.0);

    // At 50 %, a second harmonic first: more of it for a loud tone in the highs than for one as loud lower down, and more the
    // louder it is.
    const auto high = harmonics (0.5f, 6000.0, 0.25f), low = harmonics (0.5f, 300.0, 0.25f), quietHigh = harmonics (0.5f, 6000.0, 0.05f);
    MESSAGE ("50 %, -12 dBFS: 6 kHz H2 " << high.first << " H3 " << high.second << "; 300 Hz H2 " << low.first << " H3 " << low.second
                                         << "; 6 kHz at -26 dBFS H2 " << quietHigh.first);
    CHECK (high.first > -40.0);
    CHECK (high.first > low.first + 10.0);
    CHECK (high.first > quietHigh.first + 10.0);

    // Pushed hard, a third too.
    const auto hard = harmonics (1.0f, 4000.0, 0.5f);
    CHECK (hard.second > -40.0);

    // A loud tone near the top, driven hardest: what folds back under the tone (anything that isn't one of its harmonics) stays
    // far under it.
    constexpr int order = 15, size = 1 << order;
    const auto hertz = 9001.0 / 48000.0 * rate;
    const auto played = play (clean ({ { "distortion", 1.0f } }), rate, size + 9600, sine (hertz, rate, 0.5f));
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::blackmanHarris);
    std::vector<float> data ((size_t) size * 2, 0.0f);
    std::copy (played.left.begin() + 9600, played.left.begin() + 9600 + size, data.begin());
    window.multiplyWithWindowingTable (data.data(), (size_t) size);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);
    const auto bin = (int) std::lround (hertz * size / rate);
    auto alias = 1.0e-12;

    for (int k = 8; k < size / 2 && k * rate / size < 20000.0; ++k)
    {
        const auto nearest = (int) std::lround ((double) k / bin) * bin;

        if (std::abs (k - nearest) > 8 && k * rate / size > 100.0)
            alias = std::max (alias, (double) data[(size_t) k]);
    }

    MESSAGE ("aliases " << toDecibels (data[(size_t) bin] / alias) << " dB under a 9 kHz tone at -6 dBFS, driven hardest");
    CHECK (toDecibels (data[(size_t) bin] / alias) > 60.0);
}

TEST_CASE ("The vinyl's hiss is as loud as it says, and its crackle comes as often as its density says, the largest at its level")
{
    constexpr double rate = 48000.0;
    const auto silence = [] (int) { return 0.0f; };

    // Hiss alone, the band wide: its RMS, flat to 2 kHz and falling above, a little alike on both sides.
    const auto hissed = play (clean ({ { "hiss", -40.0f } }), rate, (int) (4.0 * rate), silence);
    const auto rms = getRms (hissed.left, (size_t) rate, hissed.left.size());
    CHECK (toDecibels (rms) == doctest::Approx (-40.0).epsilon (0.02));
    auto product = 0.0, leftSquares = 0.0, rightSquares = 0.0;

    for (auto i = (size_t) rate; i < hissed.left.size(); ++i)
    {
        product += (double) hissed.left[i] * hissed.right[i];
        leftSquares += (double) hissed.left[i] * hissed.left[i];
        rightSquares += (double) hissed.right[i] * hissed.right[i];
    }

    const auto alike = product / std::sqrt (leftSquares * rightSquares);
    CHECK (alike > 0.3);
    CHECK (alike < 0.7);

    // Crackle alone: as many clicks a second as the density says, over 20 s, none louder than its level and the largest near it.
    for (const auto density : { 2.0f, 20.0f, 200.0f })
    {
        CAPTURE (density);
        Vinyl vinyl;
        vinyl.prepare (rate, 480);
        vinyl.getSurfaceNoise().takeClicks();
        const auto values = clean ({ { "crackle", -12.0f }, { "density", density } });
        const auto crackled =
            play (vinyl, rate, (int) (20.0 * rate), 480, [] (int) { return std::pair { 0.0f, 0.0f }; }, [&] (int) { return values; });
        const auto clicks = vinyl.getSurfaceNoise().takeClicks();
        MESSAGE (density << " a second: " << clicks / 20.0 << " clicks a second");
        CHECK (clicks / 20.0 == doctest::Approx (density).epsilon (density < 10.0f ? 0.3 : 0.1));

        const auto peak = getPeak (crackled.left, 0, crackled.left.size());
        CHECK (toDecibels (peak) < -12.0 + 3.0); // a click leans to a side by up to 30 %
        CHECK (toDecibels (peak) > -12.0 - 6.0);
    }
}

TEST_CASE ("The vinyl's noise plays only while the song does, from a silent start on, and stops two seconds after the sound does")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 480;
    const auto values = clean ({ { "hiss", -40.0f }, { "crackle", -20.0f }, { "density", 50.0f } });
    const auto energy = [] (const std::vector<float>& sound, double fromSeconds, double toSeconds)
    { return getRms (sound, (size_t) (fromSeconds * rate), (size_t) (toSeconds * rate)); };

    // Stopped: nothing, whatever comes in.
    {
        const auto played = play (values, rate, (int) (2.0 * rate), [] (int) { return 0.0f; }, blockSize, { .playing = false });
        CHECK (getPeak (played.left, 0, played.left.size()) == 0.0f);
    }

    // Playing a silent song: the noise from its start, as a record's lead-in plays.
    {
        const auto played = play (values, rate, (int) (4.0 * rate), [] (int) { return 0.0f; }, blockSize);
        CHECK (toDecibels (energy (played.left, 0.1, 4.0)) > -45.0);
    }

    // A tone for a second, then silence: the noise plays on for two seconds after it, then fades out within a moment.
    {
        const auto tone = sine (440.0, rate, 0.25f);
        const auto played = play (values, rate, (int) (6.0 * rate), [&] (int t) { return t < (int) rate ? tone (t) : 0.0f; }, blockSize);
        CHECK (toDecibels (energy (played.left, 1.2, 2.9)) > -45.0);
        CHECK (getPeak (played.left, (size_t) (3.2 * rate), played.left.size()) == 0.0f);
    }

    // Stopping fades it out within 20 ms, without a click.
    {
        Vinyl vinyl;
        vinyl.prepare (rate, blockSize);
        const auto played =
            play (vinyl, rate, (int) (2.0 * rate), blockSize, [] (int) { return std::pair { 0.0f, 0.0f }; }, [&] (int) { return values; });
        CHECK (toDecibels (energy (played.left, 1.0, 2.0)) > -45.0);
        const auto after = play (
            vinyl, rate, (int) (1.0 * rate), blockSize, [] (int) { return std::pair { 0.0f, 0.0f }; }, [&] (int) { return values; },
            { .playing = false });
        CHECK (toDecibels (getRms (after.left, (size_t) (0.05 * rate), (size_t) (0.15 * rate))) < -70.0); // the band rings out
        CHECK (getPeak (after.left, (size_t) (0.2 * rate), after.left.size()) == 0.0f);                   // and resting
    }
}

TEST_CASE ("The vinyl's crackle is where the song is: the same clicks at the same beats however playback reached them")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 480;
    const auto values = clean ({ { "crackle", -12.0f }, { "density", 40.0f } });
    const auto silence = [] (int) { return std::pair { 0.0f, 0.0f }; };

    // From the song's start, for 8 beats at 120 BPM.
    Vinyl fromStart;
    fromStart.prepare (rate, blockSize);
    const auto whole = play (fromStart, rate, (int) (4.0 * rate), blockSize, silence, [&] (int) { return values; });

    // From beat 4, after playing somewhere else first: its clicks from beat 4 on are the first's, sample for sample, once
    // the latency has passed.
    Vinyl elsewhere;
    elsewhere.prepare (rate, blockSize);
    play (elsewhere, rate, (int) (1.0 * rate), blockSize, silence, [&] (int) { return values; }, { .beat = 100.0 });
    const auto later = play (elsewhere, rate, (int) (2.0 * rate), blockSize, silence, [&] (int) { return values; }, { .beat = 4.0 });
    const auto latency = (size_t) fromStart.getLatencySamples();
    auto worst = 0.0f;

    for (auto i = latency + 24000; i < later.left.size(); ++i)
        worst = std::max (worst, std::abs (later.left[i] - whole.left[i + (size_t) (2.0 * rate)]));

    CHECK (getPeak (later.left, latency, later.left.size()) > 0.01f);
    CHECK (worst < 1.0e-6f);

    // In stretches that are each given the block's own beat, as its runner hands them over while a parameter glides: the same.
    Vinyl inStretches;
    inStretches.prepare (rate, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> stretched;

    for (int done = 0; done < (int) (2.0 * rate); done += blockSize)
    {
        buffer.clear();

        for (int start = 0; start < blockSize; start += 32)
        {
            float* channels[] = { buffer.getWritePointer (0, start), buffer.getWritePointer (1, start) };
            devices::ProcessContext context;
            context.channels = channels;
            context.numChannels = 2;
            context.numSamples = 32;
            context.parameters = values;
            context.isPlaying = true;
            context.beat = done * 2.0 / rate; // the block's, for every stretch
            inStretches.process (context);
        }

        stretched.insert (stretched.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    worst = 0.0f;

    for (size_t i = 0; i < stretched.size(); ++i)
        worst = std::max (worst, std::abs (stretched[i] - whole.left[i]));

    CHECK (worst < 1.0e-6f);
}

TEST_CASE ("The vinyl's parameters move without a click, and it plays the same in blocks of any size")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 64, length = 48000;

    // A low tone while the wow, its rate, the flutter, the distortion, the width, the band and the bass mono jump every block or
    // two: no sample steps further from the last than the tone itself could, with all of it at its most.
    {
        Vinyl vinyl;
        vinyl.prepare (rate, blockSize);
        const auto tone = sine (200.0, rate, 0.5f);
        const auto played = play (
            vinyl, rate, length, blockSize,
            [&] (int t)
            {
                const auto sample = tone (t);
                return std::pair { sample, -0.5f * sample };
            },
            [] (int t)
            {
                const auto step = (t / blockSize) % 4;
                return clean ({ { "wow", step < 2 ? 0.0f : 25.0f },
                                { "wowRate", step % 2 == 0 ? 0.25f : 4.0f },
                                { "flutter", step == 1 ? 10.0f : 0.0f },
                                { "distortion", step == 3 ? 1.0f : 0.0f },
                                { "width", step == 2 ? 0.0f : 1.0f },
                                { "lowCut", step == 1 ? 150.0f : 20.0f },
                                { "highCut", step == 2 ? 1000.0f : 20000.0f },
                                { "bassMono", step == 3 ? 500.0f : 20.0f } });
            });

        auto largestStep = 0.0f;

        for (int i = 4800; i < length; ++i)
            largestStep = std::max (largestStep, std::abs (played.left[(size_t) i] - played.left[(size_t) i - 1]));

        // A 200 Hz tone at 0.5 moves 2 pi 200 / 48000 x 0.5 a sample at most: 0.013; its distortion and filters a little more.
        CHECK (largestStep < 0.03f);
    }

    // Noise in, everything on: the same in blocks of any size, also longer than it was prepared for.
    const auto values =
        with ({ { "wow", 12.0f }, { "flutter", 5.0f }, { "distortion", 0.6f }, { "crackle", -18.0f }, { "density", 30.0f } });
    juce::Random random (3);
    std::vector<float> noise ((size_t) length);

    for (auto& sample : noise)
        sample = 0.5f * (random.nextFloat() - 0.5f);

    const auto playIn = [&] (int size, int preparedFor)
    {
        Vinyl vinyl;
        vinyl.prepare (rate, preparedFor);
        return play (
                   vinyl, rate, length, size, [&] (int t) { return std::pair { noise[(size_t) t], -noise[(size_t) t] }; },
                   [&] (int) { return values; })
            .left;
    };

    const auto reference = playIn (512, 512);

    for (const auto& [size, preparedFor] : { std::pair { 1, 512 }, { 17, 512 }, { 64, 64 }, { 4096, 512 }, { 4096, 4096 } })
    {
        CAPTURE (size);
        CAPTURE (preparedFor);
        const auto played = playIn (size, preparedFor);
        auto worst = 0.0f;

        for (size_t i = 0; i < reference.size(); ++i)
            worst = std::max (worst, std::abs (played[i] - reference[i]));

        CHECK (worst < 1.0e-5f);
    }

    // And nothing but numbers, the loudest sound driven as hard as it goes.
    const auto loud =
        play (with ({ { "distortion", 1.0f }, { "wow", 25.0f }, { "flutter", 10.0f }, { "crackle", -6.0f }, { "hiss", -30.0f } }), rate,
              length, [&] (int t) { return 8.0f * noise[(size_t) t]; });
    auto finite = true;

    for (const auto sample : loud.left)
        finite = finite && std::isfinite (sample);

    CHECK (finite);
}

TEST_CASE ("The vinyl rests in silence while the song is stopped, and plays what comes after as it would have")
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;
    const auto values = clean ({ { "hiss", -60.0f }, { "crackle", -24.0f } }); // no wow, which would bring the tone earlier
    const auto tone = sine (440.0, rate, 0.25f);

    // Two seconds of silence, stopped, then a tone: the tone comes out as a fresh vinyl plays it, after its latency.
    Vinyl rested;
    rested.prepare (rate, blockSize);
    const auto quiet = play (
        rested, rate, (int) (2.0 * rate), blockSize, [] (int) { return std::pair { 0.0f, 0.0f }; }, [&] (int) { return values; },
        { .playing = false });
    CHECK (getPeak (quiet.left, 0, quiet.left.size()) == 0.0f);

    const auto after = play (
        rested, rate, (int) rate, blockSize,
        [&] (int t)
        {
            const auto sample = tone (t);
            return std::pair { sample, sample };
        },
        [&] (int) { return values; }, { .playing = false });

    Vinyl fresh;
    fresh.prepare (rate, blockSize);
    const auto first = play (
        fresh, rate, (int) rate, blockSize,
        [&] (int t)
        {
            const auto sample = tone (t);
            return std::pair { sample, sample };
        },
        [&] (int) { return values; }, { .playing = false });

    const auto from = (int) (0.5 * rate), count = (int) (0.4 * rate);
    CHECK (getTone (after.left.data(), from, count, 440.0, rate).getLevel()
           == doctest::Approx (getTone (first.left.data(), from, count, 440.0, rate).getLevel()).epsilon (0.01));
    CHECK (getPeak (after.left, 0, (size_t) rested.getLatencySamples() - 8) < 1.0e-6f);
    CHECK (getPeak (after.left, (size_t) rested.getLatencySamples() + 64, after.left.size()) > 0.1f);

    // A mono sound: its one channel.
    Vinyl mono;
    mono.prepare (rate, blockSize);
    juce::AudioBuffer<float> buffer (1, blockSize);
    auto peak = 0.0f;

    for (int done = 0; done < (int) rate; done += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
            buffer.setSample (0, i, tone (done + i));

        float* channels[] = { buffer.getWritePointer (0) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 1;
        context.numSamples = blockSize;
        context.parameters = values;
        mono.process (context);
        peak = std::max (peak, buffer.getMagnitude (0, 0, blockSize));
    }

    CHECK (peak > 0.2f);
    CHECK (peak < 0.3f);
}
