// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's EQ (src/devices/eq/Equalizer.h): each band's shape measured as it plays against the response it reports,
// its slopes, its smoothing, and its tap for the analyzer.
#include "TestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "model/devices/DevicePlugin.h"

#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using Eq = devices::Equalizer;
constexpr double testRate = 48000.0;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Eq::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, int band, Eq::Field field, float value)
{
    values[(size_t) Eq::getParameterIndex (band, field)] = value;
}

double reportedDecibels (const std::vector<float>& values, double frequency)
{
    return 20.0 * std::log10 (Eq::getMagnitude (values, frequency, testRate));
}

// Plays the EQ, with these values, in blocks of 512 samples: input (time in samples, channel) gives the sound.
template <typename Input, typename Output>
void play (Eq& eq, const std::vector<float>& values, int numSamples, Input input, Output output)
{
    juce::AudioBuffer<float> buffer (2, 512);

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (512, numSamples - done);

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < length; ++i)
                buffer.setSample (channel, i, input (done + i, channel));

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        eq.process (block);

        for (int i = 0; i < length; ++i)
            output (done + i, buffer.getSample (0, i), buffer.getSample (1, i));

        done += length;
    }
}

// A sine through a new EQ: its level once settled (after half a second), relative to the sine's, in decibels.
double measuredDecibels (const std::vector<float>& values, double frequency)
{
    Eq eq;
    eq.prepare (testRate, 512);
    const auto sine = [frequency] (int t, int) { return (float) (0.5 * std::sin (twoPi * frequency * t / testRate)); };
    double input = 0.0, output = 0.0;
    auto channelsDiffer = 0.0f;

    play (eq, values, (int) testRate, sine,
          [&] (int t, float left, float right)
          {
              channelsDiffer = std::max (channelsDiffer, std::abs (left - right));

              if (t >= (int) testRate / 2)
              {
                  const auto x = (double) sine (t, 0);
                  input += x * x;
                  output += (double) left * left;
              }
          });

    CHECK (channelsDiffer < 1.0e-6f); // both channels alike
    return 10.0 * std::log10 (output / input);
}
} // namespace

TEST_CASE ("The EQ's description holds together, and a new EQ changes nothing")
{
    REQUIRE (devices::isValid (Eq::info));
    CHECK (Eq::info.parameters.size() == 49);
    CHECK (devices::findParameter (Eq::info, "band3Gain") == Eq::getParameterIndex (2, Eq::gain));
    CHECK (devices::findParameter (Eq::info, "band8Slope") == Eq::getParameterIndex (7, Eq::slope));
    CHECK (devices::findParameter (Eq::info, "output") == Eq::outputParameter);

    const auto values = getDefaults();

    for (const auto frequency : { 40.0, 1000.0, 12000.0 })
    {
        CAPTURE (frequency);
        CHECK (std::abs (reportedDecibels (values, frequency)) < 0.001);
        CHECK (std::abs (measuredDecibels (values, frequency)) < 0.01);
    }
}

TEST_CASE ("Each of the EQ's band shapes plays as its response says")
{
    struct Shape
    {
        const char* name;
        int band;
        Eq::BandType type;
        float frequency, gain, q;
        int slope;
    };

    const Shape shapes[] = {
        { "bell", 2, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1 },
        { "deep bell", 3, Eq::BandType::bell, 3000.0f, -15.0f, 4.0f, 1 },
        { "low shelf", 1, Eq::BandType::lowShelf, 200.0f, 6.0f, 0.71f, 1 },
        { "high shelf", 6, Eq::BandType::highShelf, 5000.0f, -6.0f, 0.71f, 1 },
        { "notch", 4, Eq::BandType::notch, 1000.0f, 0.0f, 2.0f, 1 },
        { "low cut, 12 dB", 0, Eq::BandType::lowCut, 100.0f, 0.0f, 0.71f, 0 },
        { "low cut, 24 dB", 0, Eq::BandType::lowCut, 100.0f, 0.0f, 0.71f, 1 },
        { "high cut, 48 dB", 7, Eq::BandType::highCut, 4000.0f, 0.0f, 0.71f, 3 },
        { "resonant low cut", 0, Eq::BandType::lowCut, 300.0f, 0.0f, 4.0f, 1 },
    };

    for (const auto& shape : shapes)
    {
        CAPTURE (shape.name);
        auto values = getDefaults();
        set (values, shape.band, Eq::on, 1.0f);
        set (values, shape.band, Eq::type, (float) shape.type);
        set (values, shape.band, Eq::frequency, shape.frequency);
        set (values, shape.band, Eq::gain, shape.gain);
        set (values, shape.band, Eq::q, shape.q);
        set (values, shape.band, Eq::slope, (float) shape.slope);

        for (const auto frequency : { 50.0, 100.0, 200.0, 1000.0, 3000.0, 8000.0 })
        {
            CAPTURE (frequency);
            const auto reported = reportedDecibels (values, frequency);

            // Deep in a cut or a notch the level is too low to measure well; it only has to be that low.
            if (reported < -40.0)
                CHECK (measuredDecibels (values, frequency) < -38.0);
            else
                CHECK (measuredDecibels (values, frequency) == doctest::Approx (reported).epsilon (0.005));
        }
    }
}

TEST_CASE ("The EQ's cuts are as steep as their slopes, and a cut's Q makes a bump")
{
    auto values = getDefaults();
    set (values, 0, Eq::on, 1.0f);
    set (values, 0, Eq::frequency, 400.0f);

    // Down 3 dB at the cutoff, and about 6 dB per octave per order an octave below.
    for (int slope = 0; slope < 4; ++slope)
    {
        CAPTURE (slope);
        set (values, 0, Eq::slope, (float) slope);
        const auto order = 2.0 * (slope + 1);
        CHECK (reportedDecibels (values, 400.0) == doctest::Approx (-3.01).epsilon (0.01));
        CHECK (reportedDecibels (values, 200.0) == doctest::Approx (-10.0 * std::log10 (1.0 + std::pow (2.0, 2.0 * order))).epsilon (0.03));
    }

    set (values, 0, Eq::q, 4.0f);
    CHECK (reportedDecibels (values, 400.0) > 6.0);
}

TEST_CASE ("The EQ's output gain scales everything")
{
    auto values = getDefaults();
    values[(size_t) Eq::outputParameter] = -6.0f;

    for (const auto frequency : { 60.0, 2000.0 })
    {
        CHECK (reportedDecibels (values, frequency) == doctest::Approx (-6.0).epsilon (0.001));
        CHECK (measuredDecibels (values, frequency) == doctest::Approx (-6.0).epsilon (0.002));
    }
}

TEST_CASE ("A change to an EQ band moves smoothly instead of jumping")
{
    Eq eq;
    eq.prepare (testRate, 512);
    auto values = getDefaults();
    set (values, 2, Eq::frequency, 1000.0f);

    // A steady 1 kHz sine with the bell flat, then the bell at +12 dB at once.
    const auto sine = [] (int t, int) { return (float) std::sin (twoPi * 1000.0 * t / testRate); };
    play (eq, values, 4800, sine, [] (int, float, float) {});

    set (values, 2, Eq::gain, 12.0f);
    auto peakAt = [&] (int from, int to)
    {
        float peak = 0.0f;
        play (
            eq, values, to - from, [&] (int t, int channel) { return sine (from + t, channel); },
            [&] (int, float left, float) { peak = std::max (peak, std::abs (left)); });
        return peak;
    };

    const auto target = juce::Decibels::decibelsToGain (12.0f);
    CHECK (peakAt (4800, 4896) < 0.6f * target); // the first 2 ms: well on the way, not there
    CHECK (peakAt (4896, 4800 + 9600) > 0.9f * target);
    CHECK (peakAt (14400, 24000) == doctest::Approx (target).epsilon (0.01)); // settled after 200 ms
}

TEST_CASE ("The EQ taps what it plays while a display listens")
{
    Eq eq;
    eq.prepare (testRate, 512);
    const auto values = getDefaults();
    auto& tap = eq.getOutputTap();
    std::vector<float> taken (1024, 9.0f);

    // Nothing while nobody listens.
    play (eq, values, 256, [] (int t, int channel) { return channel == 0 ? 0.5f : (float) t / 256.0f; }, [] (int, float, float) {});
    CHECK (tap.pull (taken.data(), 1024) == 0);

    tap.setListening (true);
    std::vector<float> played;
    play (
        eq, values, 300, [] (int t, int channel) { return channel == 0 ? 0.5f : (float) t / 300.0f; },
        [&played] (int, float left, float right) { played.push_back (0.5f * (left + right)); });

    REQUIRE (tap.pull (taken.data(), 1024) == 300);

    for (size_t i = 0; i < 300; i += 37)
        CHECK (taken[i] == doctest::Approx (played[i]));

    CHECK (tap.pull (taken.data(), 1024) == 0); // each sample once
}

TEST_CASE ("The EQ is one of the engine's devices")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Eq::typeID, {});
    auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
    REQUIRE (device != nullptr);
    CHECK (device->getName() == juce::String ("EQ"));
    CHECK (device->getAutomatableParameters().size() == 49);
    CHECK (device->findParameter ("band1Frequency")->getCurrentValue() == doctest::Approx (30.0f));
    CHECK (dynamic_cast<Eq*> (&device->getProcessor()) != nullptr);
}
