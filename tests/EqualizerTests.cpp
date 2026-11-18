// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's EQ (src/devices/eq/Equalizer.h): each band's shape measured as it plays against the response it reports,
// its slopes, its smoothing, and its tap for the analyzer. Then, from step S6.4, at its new parameters' defaults sample for
// sample what it played before; its bands on the middle, the side, the left or the right, each way's response, and a change of
// placement without a click; its dynamic bands, how far and how fast they move and what they hear; and all of it in blocks of
// any size.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/dsp/ControlGrid.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Math.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string_view>
#include <utility>
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

namespace probe = phazite::test::probe;
using Placement = Eq::Placement;

// The defaults with some of the values changed, by their IDs.
std::vector<float> with (std::initializer_list<std::pair<std::string_view, float>> changes, std::vector<float> values = getDefaults())
{
    for (const auto& [id, value] : changes)
    {
        const auto index = devices::findParameter (Eq::info, id);
        REQUIRE (index >= 0);
        values[(size_t) index] = value;
    }

    return values;
}

// A sound played through an EQ in place, in blocks of a size, the song playing from its start, on as many channels as the
// sound has (one: a mono track's); before each block, beforeBlock (its first sample) may change the values, and after it,
// afterBlock (the sample after its last) may read the EQ.
void playEq (Eq& eq, probe::Sound& sound, int blockSize, std::vector<float>& values, const std::function<void (int)>& beforeBlock = {},
             const std::function<void (int)>& afterBlock = {})
{
    for (int start = 0; start < sound.getNumSamples(); start += blockSize)
    {
        if (beforeBlock)
            beforeBlock (start);

        const auto length = std::min (blockSize, sound.getNumSamples() - start);
        float* channels[] = { sound.getWritePointer (0, start), sound.getWritePointer (sound.getNumChannels() - 1, start) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = sound.getNumChannels();
        block.numSamples = length;
        block.parameters = values;
        block.isPlaying = true;
        block.songSample = start;
        eq.process (block);

        if (afterBlock)
            afterBlock (start + length);
    }
}

probe::Sound playedThrough (std::vector<float> values, const probe::Sound& in, int blockSize = 480)
{
    Eq eq;
    eq.prepare (testRate, blockSize);
    auto out = in;
    playEq (eq, out, blockSize, values);
    return out;
}

// A sine on the left and the right, each with its own amplitude (a negative one upside down).
probe::Sound makeStereoSine (double hertz, int numSamples, float left, float right)
{
    probe::Sound sound (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto x = (float) std::sin (twoPi * hertz * i / testRate);
        sound.setSample (0, i, left * x);
        sound.setSample (1, i, right * x);
    }

    return sound;
}

int countDifferences (const probe::Sound& a, const probe::Sound& b, int channel)
{
    auto differences = 0;

    for (int i = 0; i < a.getNumSamples(); ++i)
        if (! juce::exactlyEqual (a.getSample (channel, i), b.getSample (channel, i)))
            ++differences;

    return differences;
}

// The largest step from one sample to the next, which a click raises, from a sample on.
double largestStep (const probe::Sound& sound, int from = 1)
{
    auto largest = 0.0;

    for (int channel = 0; channel < sound.getNumChannels(); ++channel)
        for (int i = std::max (1, from); i < sound.getNumSamples(); ++i)
            largest = std::max (largest, (double) std::abs (sound.getSample (channel, i) - sound.getSample (channel, i - 1)));

    return largest;
}

//==============================================================================
// The EQ as it was before step S6.4, word for word but for its names: six parameters a band, every band on both channels.
class EqAsBefore
{
public:
    static constexpr int fields = 6, output = Eq::numBands * fields;

    // Its values from the EQ's own: each band's first six, then the output.
    static std::vector<float> from (const std::vector<float>& values)
    {
        std::vector<float> old;

        for (int band = 0; band < Eq::numBands; ++band)
            for (int field = 0; field < fields; ++field)
                old.push_back (values[(size_t) Eq::getParameterIndex (band, (Eq::Field) field)]);

        old.push_back (values[(size_t) Eq::outputParameter]);
        return old;
    }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        smoothing = (float) (1.0 - std::exp (-stretch / (0.02 * sampleRate)));

        for (auto& band : bands)
        {
            band = Band {};
            band.type = -1;
        }

        outputGain = -1.0f;
        grid.reset();
    }

    void process (float* const* channelData, int numChannels, int numSamples, const std::vector<float>& values, std::int64_t songSample)
    {
        const auto targetOutput = devices::dsp::decibelsToGain (values[(size_t) output]);
        grid.startBlock (true, songSample);

        for (int start = 0; start < numSamples;)
        {
            if (grid.startsStep())
            {
                grid.startStep();

                for (int index = 0; index < Eq::numBands; ++index)
                    update (bands[(size_t) index], index, values);

                const auto reached = grid.getReached();
                outputFrom = outputGain < 0.0f ? targetOutput
                             : reached >= 1.0f ? outputGain
                                               : outputFrom + (outputGain - outputFrom) * reached;
                outputGain =
                    std::abs (outputFrom - targetOutput) <= 1.0e-5f ? targetOutput : outputFrom + (targetOutput - outputFrom) * smoothing;
            }

            const auto length = grid.take (numSamples - start);
            const auto done = grid.getDone(), stretchLength = grid.getStepLength();

            for (auto& band : bands)
                for (int channel = 0; channel < numChannels; ++channel)
                {
                    auto* data = channelData[channel] + start;

                    for (int section = 0; section < band.numSections; ++section)
                    {
                        auto& filter = band.filters[(size_t) channel][(size_t) section];
                        const auto& coefficients = band.sections[(size_t) section];

                        for (int i = 0; i < length; ++i)
                            data[i] = filter.process (data[i], coefficients);
                    }
                }

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* data = channelData[channel] + start;

                for (int i = 0; i < length; ++i)
                    data[i] *= outputFrom + (outputGain - outputFrom) * (float) (done + i + 1) / (float) stretchLength;
            }

            grid.advance (length);
            start += length;
        }
    }

private:
    static constexpr int stretch = 32;

    struct Band
    {
        bool on = false;
        int type = (int) Eq::BandType::bell, slope = 1;
        float frequency = 1000.0f, gain = 0.0f, q = 1.0f;
        int numSections = 0;
        std::array<devices::dsp::FilterCoefficients, 4> sections {};
        std::array<std::array<devices::dsp::Filter, 4>, 2> filters {};
    };

    double sampleRate = 48000.0;
    float smoothing = 0.1f, outputGain = 1.0f, outputFrom = 1.0f;
    devices::dsp::ControlGrid grid { stretch };
    std::array<Band, Eq::numBands> bands {};

    static bool isNear (float a, float b, float tolerance) { return std::abs (a - b) <= tolerance; }

    static bool isShelfOrBell (int type)
    {
        return type == (int) Eq::BandType::lowShelf || type == (int) Eq::BandType::bell || type == (int) Eq::BandType::highShelf;
    }

    int makeSections (Band& band) const
    {
        using devices::dsp::FilterShape;
        auto& sections = band.sections;
        auto count = 0;

        switch ((Eq::BandType) band.type)
        {
            case Eq::BandType::lowCut:
            case Eq::BandType::highCut:
            {
                const auto order = 2 * (std::clamp (band.slope, 0, 3) + 1);
                count = order / 2;

                for (int section = 0; section < count; ++section)
                {
                    auto sectionQ = devices::dsp::getButterworthQ (order, section);

                    if (section == count - 1)
                        sectionQ *= (double) band.q / 0.70710678;

                    sections[(size_t) section] = devices::dsp::makeMatchedFilter (
                        band.type == (int) Eq::BandType::lowCut ? FilterShape::highPass : FilterShape::lowPass, band.frequency, sampleRate,
                        sectionQ);
                }

                break;
            }

            case Eq::BandType::lowShelf:
                sections[0] = devices::dsp::makeMatchedFilter (FilterShape::lowShelf, band.frequency, sampleRate, band.q, band.gain);
                count = 1;
                break;

            case Eq::BandType::bell:
                sections[0] = devices::dsp::makeMatchedFilter (FilterShape::bell, band.frequency, sampleRate, band.q, band.gain);
                count = 1;
                break;

            case Eq::BandType::notch:
                sections[0] = devices::dsp::makeMatchedFilter (FilterShape::notch, band.frequency, sampleRate, band.q);
                count = 1;
                break;

            case Eq::BandType::highShelf:
                sections[0] = devices::dsp::makeMatchedFilter (FilterShape::highShelf, band.frequency, sampleRate, band.q, band.gain);
                count = 1;
                break;
        }

        return count;
    }

    void update (Band& band, int index, const std::vector<float>& values)
    {
        const auto at = [&values, index] (int field) { return values[(size_t) (index * fields + field)]; };
        const auto newOn = at (0) > 0.5f;
        const auto newType = (int) std::lround (at (1)), newSlope = (int) std::lround (at (5));
        const auto targetFrequency = at (2), targetGain = at (3), targetQ = at (4);
        auto jump = false;

        if (newOn != band.on || newType != band.type || newSlope != band.slope)
        {
            band.on = newOn;
            band.type = newType;
            band.slope = newSlope;
            jump = true;

            for (auto& channel : band.filters)
                for (auto& filter : channel)
                    filter.reset();
        }

        if (! band.on)
        {
            band.numSections = 0;
            return;
        }

        const auto settled = isNear (band.frequency, targetFrequency, targetFrequency * 1.0e-4f) && isNear (band.gain, targetGain, 1.0e-3f)
                             && isNear (band.q, targetQ, targetQ * 1.0e-4f);

        if (settled && ! jump)
            return;

        if (jump || settled)
        {
            band.frequency = targetFrequency;
            band.gain = targetGain;
            band.q = targetQ;
        }
        else
        {
            band.frequency *= std::pow (targetFrequency / band.frequency, smoothing);
            band.gain += (targetGain - band.gain) * smoothing;
            band.q *= std::pow (targetQ / band.q, smoothing);
        }

        const auto wasPlayed = band.numSections > 0;

        band.numSections = isShelfOrBell (band.type) && std::abs (band.gain) < 0.001f ? 0 : makeSections (band);

        if (! wasPlayed && band.numSections > 0)
            for (auto& channel : band.filters)
                for (auto& filter : channel)
                    filter.reset();
    }
};

// Each of the EQ's presets' values, in the EQ's order (a parameter a preset doesn't name at its default).
std::vector<std::pair<juce::String, std::vector<float>>> getPresetValues()
{
    std::vector<std::pair<juce::String, std::vector<float>>> presets;
    const auto folder = phazite::test::getFactoryPresetsFolder().getChildFile ("EQ");

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, true, "*.phazitepreset"))
    {
        const auto preset = phazite::readPreset (file);
        REQUIRE (preset.isObject());
        std::vector<float> values;

        for (const auto& parameter : Eq::info.parameters)
            values.push_back (phazite::readSavedValue (
                parameter, preset["device"]["parameters"][juce::Identifier (juce::String (parameter.id.data(), parameter.id.size()))]));

        presets.emplace_back (file.getRelativePathFrom (folder), std::move (values));
    }

    std::sort (presets.begin(), presets.end());
    return presets;
}
} // namespace

TEST_CASE ("The EQ's description holds together, and a new EQ changes nothing")
{
    REQUIRE (devices::isValid (Eq::info));
    CHECK (Eq::info.parameters.size() == 74);
    CHECK (devices::findParameter (Eq::info, "band3Gain") == Eq::getParameterIndex (2, Eq::gain));
    CHECK (devices::findParameter (Eq::info, "band8Slope") == Eq::getParameterIndex (7, Eq::slope));
    CHECK (devices::findParameter (Eq::info, "output") == Eq::outputParameter);
    CHECK (devices::findParameter (Eq::info, "mix") == Eq::mixParameter); // which its runner applies (engine/nodes/DeviceRunner.h)

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
    const auto* info = phazite::findOwnDevice (Eq::typeID);
    REQUIRE (info != nullptr);
    CHECK (info->parameters.size() == 74);
    CHECK (dynamic_cast<Eq*> (phazite::createOwnDevice (Eq::typeID).get()) != nullptr);

    // A song holds one by its type, named after it, with its parameters at their defaults.
    auto song = phazite::createSong();
    const auto id = phazite::insertDeviceState (*song, phazite::getTrackIDs (*song)[0], phazite::makeOwnDeviceState (*song, Eq::typeID));
    REQUIRE (id.isValid());
    CHECK (phazite::getDeviceName (*song, id) == juce::String ("EQ"));
    CHECK (phazite::getValueSetByHand (*song, phazite::ParameterRef { id, "band1Frequency" }) == doctest::Approx (30.0f));
}

//==============================================================================
TEST_CASE ("The EQ at its new parameters' defaults plays exactly what it played before step S6.4, for each of its presets")
{
    // Every band on Stereo and none dynamic: the old device's bands, sample for sample, on noise in both sides and on a mono
    // track, with each preset's values and others, and with each band's gain and frequency turned half way through.
    auto settings = getPresetValues();

    // Those that came before it, the eleven that place no band and make none dynamic.
    std::erase_if (settings,
                   [] (const auto& setting)
                   {
                       for (int band = 0; band < Eq::numBands; ++band)
                           if (setting.second[(size_t) Eq::getParameterIndex (band, Eq::placement)] > 0.0f
                               || std::abs (setting.second[(size_t) Eq::getParameterIndex (band, Eq::dynamic)]) > 0.0f)
                               return true;

                       return false;
                   });

    REQUIRE (settings.size() == 11);
    settings.emplace_back ("its defaults", getDefaults());
    settings.emplace_back ("the golden render's", with ({ { "band2On", 1.0f }, { "band2Gain", 12.0f } }));
    settings.emplace_back ("every band on, a notch and steep resonant cuts", with ({ { "band1On", 1.0f },
                                                                                     { "band1Slope", 3.0f },
                                                                                     { "band1Q", 3.0f },
                                                                                     { "band8On", 1.0f },
                                                                                     { "band8Frequency", 9000.0f },
                                                                                     { "band4Type", (float) Eq::BandType::notch },
                                                                                     { "band5Gain", -9.0f },
                                                                                     { "output", -3.0f } }));
    settings.emplace_back ("a Threshold moved, its Dynamic at 0", with ({ { "band3Gain", 4.0f }, { "band3Threshold", -60.0f } }));

    constexpr int numSamples = 48000, turnedAt = 24000;
    const auto noise = probe::makeNoise (numSamples, 7, 0.5f);

    for (const auto& [name, values] : settings)
    {
        CAPTURE (name);
        auto turned = values;

        for (int band = 0; band < Eq::numBands; ++band)
        {
            turned[(size_t) Eq::getParameterIndex (band, Eq::gain)] += 3.0f;
            auto& frequency = turned[(size_t) Eq::getParameterIndex (band, Eq::frequency)];
            frequency = std::min (20000.0f, frequency * 1.5f);
        }

        turned[(size_t) Eq::outputParameter] += 2.0f;
        const auto oldValues = EqAsBefore::from (values), oldTurned = EqAsBefore::from (turned);

        for (const auto numChannels : { 2, 1 })
        {
            CAPTURE (numChannels);
            probe::Sound played (numChannels, numSamples);

            for (int channel = 0; channel < numChannels; ++channel)
                played.copyFrom (channel, 0, noise, channel, 0, numSamples);

            auto before = played;
            Eq eq;
            eq.prepare (testRate, 480);
            auto current = values;
            playEq (eq, played, 480, current, [&] (int start) { current = start < turnedAt ? values : turned; });

            EqAsBefore old;
            old.prepare (testRate);

            for (int start = 0; start < numSamples; start += 480)
            {
                float* channels[] = { before.getWritePointer (0, start), before.getWritePointer (numChannels - 1, start) };
                old.process (channels, numChannels, 480, start < turnedAt ? oldValues : oldTurned, start);
            }

            for (int channel = 0; channel < numChannels; ++channel)
                CHECK (countDifferences (played, before, channel) == 0);
        }
    }
}

TEST_CASE ("An EQ band on the middle lifts a sound alike on both sides and leaves one opposite on them as it is; on the side, the reverse")
{
    constexpr int numSamples = 48000;
    const auto alike = makeStereoSine (1000.0, numSamples, 0.25f, 0.25f), opposite = makeStereoSine (1000.0, numSamples, 0.25f, -0.25f);

    // Noise opposite on the two sides, and noise alike on them.
    auto oppositeNoise = probe::makeNoise (numSamples, 3, 0.5f), alikeNoise = oppositeNoise;

    for (int i = 0; i < numSamples; ++i)
    {
        oppositeNoise.setSample (1, i, -oppositeNoise.getSample (0, i));
        alikeNoise.setSample (1, i, alikeNoise.getSample (0, i));
    }

    for (const auto placement : { Placement::mid, Placement::side })
    {
        CAPTURE ((int) placement);
        const auto values = with ({ { "band3Frequency", 1000.0f }, { "band3Gain", 6.0f }, { "band3Placement", (float) placement } });
        const auto& lifted = placement == Placement::mid ? alike : opposite;
        const auto out = playedThrough (values, lifted);

        for (int channel = 0; channel < 2; ++channel)
            CHECK (probe::inDecibels (probe::getToneLevel (out, channel, 1000.0, testRate, numSamples / 2)
                                      / probe::getToneLevel (lifted, channel, 1000.0, testRate, numSamples / 2))
                   == doctest::Approx (6.0).epsilon (0.005));

        using Sounds = std::array<const probe::Sound*, 2>;

        for (const auto* untouched : placement == Placement::mid ? Sounds { &opposite, &oppositeNoise } : Sounds { &alike, &alikeNoise })
        {
            const auto same = playedThrough (values, *untouched);
            CHECK (countDifferences (same, *untouched, 0) == 0);
            CHECK (countDifferences (same, *untouched, 1) == 0);
        }
    }
}

TEST_CASE ("An EQ band on the left leaves the right as it is, and one on the right the left")
{
    const auto noise = probe::makeNoise (48000, 5, 0.5f);

    for (const auto placement : { Placement::left, Placement::right })
    {
        CAPTURE ((int) placement);
        const auto values = with ({ { "band3Frequency", 1000.0f }, { "band3Gain", 6.0f }, { "band3Placement", (float) placement } });
        const auto out = playedThrough (values, noise);
        const auto played = placement == Placement::left ? 0 : 1;
        CHECK (countDifferences (out, noise, 1 - played) == 0);
        CHECK (countDifferences (out, noise, played) > 40000);
    }
}

TEST_CASE ("A stereo sound through an EQ band on the side keeps its mono sum, to a float's rounding")
{
    // The middle is left as it was and the side changed, so L + R is what it was. Not sample for sample: L and R are rebuilt
    // from the middle and the changed side, m + s and m - s, each rounded to a float on its own, so their sum can be a float's
    // last bit off (here, under 0.5 at full scale, at most two of its steps, 6e-8 each).
    const auto noise = probe::makeNoise (48000, 9, 0.5f);
    const auto values = with ({ { "band7Gain", 9.0f },
                                { "band7Placement", (float) Placement::side },
                                { "band2Gain", -6.0f },
                                { "band2Placement", (float) Placement::side } });
    const auto out = playedThrough (values, noise);
    auto furthest = 0.0;

    for (int i = 0; i < noise.getNumSamples(); ++i)
        furthest = std::max (
            furthest, std::abs ((double) out.getSample (0, i) + out.getSample (1, i) - noise.getSample (0, i) - noise.getSample (1, i)));

    CHECK (furthest < 2.5e-7);
    CHECK (probe::getLargestDifference (out, noise) > 0.01); // the side did change
}

TEST_CASE ("On a mono track, the EQ's Stereo, Mid and Left bands play on its channel, and its Side and Right bands do nothing")
{
    constexpr int numSamples = 48000;
    const auto sine = probe::makeSine (1000.0, numSamples, testRate, 0.25f, 1);

    for (const auto placement : { Placement::stereo, Placement::mid, Placement::side, Placement::left, Placement::right })
    {
        CAPTURE ((int) placement);
        const auto values = with ({ { "band3Frequency", 1000.0f }, { "band3Gain", 6.0f }, { "band3Placement", (float) placement } });
        const auto out = playedThrough (values, sine);

        if (placement == Placement::side || placement == Placement::right)
            CHECK (countDifferences (out, sine, 0) == 0);
        else
            CHECK (probe::inDecibels (probe::getToneLevel (out, 0, 1000.0, testRate, numSamples / 2)
                                      / probe::getToneLevel (sine, 0, 1000.0, testRate, numSamples / 2))
                   == doctest::Approx (6.0).epsilon (0.005));
    }
}

TEST_CASE ("A change of an EQ band's placement while a sine plays makes no click")
{
    // The band on its new placement starts from rest; it fades in over 10 ms as the band as it played fades out. A sine on
    // both sides, louder on the left, so that each placement does something of its own to it; a bell at +9 dB on it. Each
    // change against the band held at either placement: the largest step from one sample to the next, which a click raises.
    // The change comes as the sine peaks (1001 Hz, a quarter of a cycle on from a whole number of them), where a band that
    // jumped to its new placement would jump furthest. And a second change 3 ms after the first, in the middle of its fade (a
    // placement automated through three in a row), which waits for the fade's end: cut short, the fade jumped by what was left
    // of it.
    constexpr int numSamples = 24000, changeAt = 12000, secondAt = 12144, blockSize = 48;
    const auto sine = makeStereoSine (1001.0, numSamples, 0.3f, 0.1f);
    const auto playedWith = [&sine] (Placement first, Placement then, Placement last)
    {
        auto values = with ({ { "band3Frequency", 1000.0f }, { "band3Gain", 9.0f }, { "band3Placement", (float) first } });
        Eq eq;
        eq.prepare (testRate, blockSize);
        auto out = sine;
        playEq (eq, out, blockSize, values,
                [&values, then, last] (int start)
                {
                    if (start == changeAt || start == secondAt)
                        values[(size_t) Eq::getParameterIndex (2, Eq::placement)] = (float) (start == changeAt ? then : last);
                });
        return out;
    };

    const Placement all[] = { Placement::stereo, Placement::mid, Placement::side, Placement::left, Placement::right };
    std::array<double, std::size (all)> held {};

    for (const auto placement : all)
        held[(size_t) placement] = largestStep (playedWith (placement, placement, placement), changeAt - 4800);

    for (const auto first : all)
    {
        for (const auto then : all)
        {
            if (then == first)
                continue;

            for (const auto last : all)
            {
                CAPTURE ((int) first);
                CAPTURE ((int) then);
                CAPTURE ((int) last);
                const auto moving = largestStep (playedWith (first, then, last), changeAt - 4800);
                CHECK (moving <= std::max ({ held[(size_t) first], held[(size_t) then], held[(size_t) last] }) * 1.02);
            }
        }
    }
}

TEST_CASE ("The EQ's response for each way through it is what a sine through it measures")
{
    // The middle and the side: a band on Stereo, one on Mid and one on Side, measured on a sine alike on both sides and on one
    // opposite on them. The left and the right: one on Stereo, one on Left and one on Right, measured on a sine on that side
    // alone.
    struct Setting
    {
        Placement way;
        std::vector<float> values;
        float left, right; // the sine's amplitude on each side
    };

    const auto middleAndSide = with ({ { "band2Frequency", 200.0f },
                                       { "band2Gain", 3.0f },
                                       { "band3Frequency", 1000.0f },
                                       { "band3Gain", 6.0f },
                                       { "band3Placement", (float) Placement::mid },
                                       { "band6Type", (float) Eq::BandType::highShelf },
                                       { "band6Frequency", 4000.0f },
                                       { "band6Gain", -4.0f },
                                       { "band6Placement", (float) Placement::side } });
    const auto leftAndRight = with ({ { "band2Frequency", 200.0f },
                                      { "band2Gain", 3.0f },
                                      { "band3Frequency", 1000.0f },
                                      { "band3Gain", 6.0f },
                                      { "band3Placement", (float) Placement::left },
                                      { "band5Frequency", 3000.0f },
                                      { "band5Gain", -6.0f },
                                      { "band5Q", 2.0f },
                                      { "band5Placement", (float) Placement::right } });

    const Setting settings[] = { { Placement::mid, middleAndSide, 0.25f, 0.25f },
                                 { Placement::side, middleAndSide, 0.25f, -0.25f },
                                 { Placement::left, leftAndRight, 0.25f, 0.0f },
                                 { Placement::right, leftAndRight, 0.0f, 0.25f },
                                 { Placement::stereo, with ({ { "band3Gain", 6.0f }, { "band6Gain", -3.0f } }), 0.25f, 0.25f } };

    for (const auto& setting : settings)
    {
        CAPTURE ((int) setting.way);
        const auto channel = setting.way == Placement::right ? 1 : 0;

        for (const auto hertz : { 100.0, 300.0, 1000.0, 3000.0, 8000.0 })
        {
            CAPTURE (hertz);
            const auto in = makeStereoSine (hertz, 24000, setting.left, setting.right);
            const auto out = playedThrough (setting.values, in);
            const auto measured = probe::inDecibels (probe::getToneLevel (out, channel, hertz, testRate, 12000)
                                                     / probe::getToneLevel (in, channel, hertz, testRate, 12000));
            CHECK (std::abs (measured - 20.0 * std::log10 (Eq::getMagnitude (setting.values, hertz, testRate, setting.way))) < 0.1);
        }
    }

    // Stereo's: the bands on Stereo alone, which act on all of it.
    auto stereoAlone = middleAndSide;
    stereoAlone[(size_t) Eq::getParameterIndex (2, Eq::on)] = 0.0f;
    stereoAlone[(size_t) Eq::getParameterIndex (5, Eq::on)] = 0.0f;

    for (const auto hertz : { 100.0, 1000.0, 5000.0 })
        CHECK (Eq::getMagnitude (middleAndSide, hertz, testRate) == doctest::Approx (Eq::getMagnitude (stereoAlone, hertz, testRate)));
}

//==============================================================================
namespace
{
// A dynamic bell: band 4 at 1 kHz, Q 2, its gain moving by -8 dB from a Threshold of -30 dB.
std::vector<float> dynamicBell (float gain = 0.0f, float dynamic = -8.0f)
{
    return with ({ { "band4Frequency", 1000.0f },
                   { "band4Q", 2.0f },
                   { "band4Gain", gain },
                   { "band4Dynamic", dynamic },
                   { "band4Threshold", -30.0f } });
}

// A sine through an EQ for a second: how much louder (below 0, quieter) it comes out over its last half, and the gain the band
// published as it ended.
std::pair<double, float> measureDynamic (const std::vector<float>& values, int band, double hertz, float amplitude)
{
    const auto in = makeStereoSine (hertz, 48000, amplitude, amplitude);
    Eq eq;
    eq.prepare (testRate, 480);
    auto out = in;
    auto current = values;
    playEq (eq, out, 480, current);
    const auto decibels =
        probe::inDecibels (probe::getToneLevel (out, 0, hertz, testRate, 24000) / probe::getToneLevel (in, 0, hertz, testRate, 24000));
    return { decibels, eq.getGainMoved (band) };
}
} // namespace

TEST_CASE ("A dynamic EQ band moves its gain by as much of its Dynamic as the sound in its band is over its Threshold")
{
    // From none at the Threshold to all of it 12 dB over: a sine at the bell's frequency, as loud as each of those, and louder.
    struct Level
    {
        float over;  // the sine's peak over the Threshold, in decibels
        float moved; // how far the gain moves, as a part of the Dynamic
    };

    for (const auto& [over, moved] : { Level { 20.0f, 1.0f }, Level { 12.0f, 1.0f }, Level { 6.0f, 0.5f }, Level { 0.0f, 0.0f } })
    {
        CAPTURE (over);
        const auto [decibels, played] = measureDynamic (dynamicBell(), 3, 1000.0, juce::Decibels::decibelsToGain (-30.0f + over));
        CHECK (std::abs (decibels - (-8.0 * moved)) < 0.3);
        CHECK (std::abs (played - (-8.0f * moved)) < 0.3f);
    }

    // A lift that lets go when the band is already loud: +4 dB with a Dynamic of -4 dB.
    CHECK (std::abs (measureDynamic (dynamicBell (4.0f, -4.0f), 3, 1000.0, 0.0316f).first - 4.0) < 0.3); // at the Threshold
    CHECK (std::abs (measureDynamic (dynamicBell (4.0f, -4.0f), 3, 1000.0, 0.5f).first) < 0.3);

    // A low shelf turned down 6 dB at 120 Hz when the lows get loud, as its response says with its gain moved all the way.
    const auto boom = with ({ { "band2Frequency", 120.0f }, { "band2Dynamic", -6.0f }, { "band2Threshold", -30.0f } });
    auto settled = boom;
    settled[(size_t) Eq::getParameterIndex (1, Eq::gain)] = -6.0f;
    const auto [shelved, shelfPlayed] = measureDynamic (boom, 1, 60.0, 0.5f);
    CHECK (std::abs (shelved - reportedDecibels (settled, 60.0)) < 0.3);
    CHECK (shelfPlayed == doctest::Approx (-6.0f).epsilon (0.05));

    // A cut or a notch ignores its Dynamic.
    for (const auto type : { Eq::BandType::notch, Eq::BandType::highCut })
    {
        auto values = dynamicBell();
        values[(size_t) Eq::getParameterIndex (3, Eq::type)] = (float) type;
        auto still = values;
        still[(size_t) Eq::getParameterIndex (3, Eq::dynamic)] = 0.0f;
        const auto loud = probe::makeNoise (24000, 2, 0.5f);
        CHECK (probe::getLargestDifference (playedThrough (values, loud), playedThrough (still, loud)) == 0.0);
    }
}

TEST_CASE ("A dynamic EQ bell hears its own band: a sine an octave and a half from it barely moves it")
{
    // As loud as the sine at its frequency that moves it all the way (12 dB over its Threshold): an octave and a half above and
    // below, it moves it by less than a tenth of its Dynamic.
    for (const auto hertz : { 1000.0 * std::pow (2.0, 1.5), 1000.0 / std::pow (2.0, 1.5) })
    {
        CAPTURE (hertz);
        CHECK (std::abs (measureDynamic (dynamicBell(), 3, hertz, juce::Decibels::decibelsToGain (-18.0f)).second) < 0.8f);
    }
}

TEST_CASE ("A dynamic EQ band's gain goes in about 3 ms, holds 25 ms and comes back in about 60 ms more")
{
    // A sine 12 dB over the Threshold for half a second, then silence: the gain the band publishes after each block of 32
    // samples, and how long it takes the level the band hears to come 63 % of its way up (4 dB under the sine's peak, where the
    // gain has gone two thirds of its way) and to fall to 37 % of it (8.7 dB under it). Up, 3 ms, and the band-pass the band hears
    // through (about 0.6 ms at Q 2 and 1 kHz) and the step of 32 samples its gain is worked out on (0.7 ms) on top; down, the
    // 24 to 25 ms the loudest is held, then 60 ms.
    constexpr int start = 12000, stop = 36000, numSamples = 60000;
    constexpr float upGain = -8.0f * (12.0f - 3.99f) / 12.0f, downGain = -8.0f * (12.0f - 8.69f) / 12.0f;
    auto in = makeStereoSine (1000.0, numSamples, 0.125f, 0.125f);

    for (int i = 0; i < numSamples; ++i)
        if (i < start || i >= stop)
            for (int channel = 0; channel < 2; ++channel)
                in.setSample (channel, i, 0.0f);

    Eq eq;
    eq.prepare (testRate, 32);
    auto values = dynamicBell();
    int wentAt = -1, cameBackAt = -1;

    playEq (eq, in, 32, values, {},
            [&eq, &wentAt, &cameBackAt] (int end)
            {
                const auto played = eq.getGainMoved (3);

                if (wentAt < 0 && end > start && played <= upGain)
                    wentAt = end;

                if (cameBackAt < 0 && end > stop && played >= downGain)
                    cameBackAt = end;
            });

    const auto went = (wentAt - start) / testRate * 1000.0, cameBack = (cameBackAt - stop) / testRate * 1000.0;
    MESSAGE ("it went in " << went << " ms and came back in " << cameBack << " ms");
    CHECK (went > 3.0);
    CHECK (went < 5.5);
    CHECK (cameBack > 80.0);
    CHECK (cameBack < 90.0);
}

TEST_CASE ("A dynamic EQ band lets go soon after a burst: back within 1 dB of its Gain about 150 ms after it ends")
{
    // A de-esser's band (7 kHz, Q 2, Dynamic -8 dB from -32 dB) and 100 ms of a 7 kHz sine 20 dB over its Threshold, as an "s"
    // is: how long after its end the gain the band publishes (after each block of 32 samples) is back within 1 dB of its Gain.
    // Held 24 to 25 ms, then down 18.5 dB at 60 ms (from 20 dB over to 1.5 dB over, where 8 dB's share is 1 dB): about 153 ms.
    constexpr int start = 4800, stop = 9600, numSamples = 48000;
    auto in = makeStereoSine (7000.0, numSamples, 0.251f, 0.251f);

    for (int i = 0; i < numSamples; ++i)
        if (i < start || i >= stop)
            for (int channel = 0; channel < 2; ++channel)
                in.setSample (channel, i, 0.0f);

    Eq eq;
    eq.prepare (testRate, 32);
    auto values = with ({ { "band6Frequency", 7000.0f }, { "band6Q", 2.0f }, { "band6Dynamic", -8.0f }, { "band6Threshold", -32.0f } });
    int backAt = -1;
    auto deepest = 0.0f;

    playEq (eq, in, 32, values, {},
            [&eq, &backAt, &deepest] (int end)
            {
                const auto moved = eq.getGainMoved (5);
                deepest = std::min (deepest, moved);

                if (backAt < 0 && end > stop && moved >= -1.0f)
                    backAt = end;
            });

    const auto back = (backAt - stop) / testRate * 1000.0;
    MESSAGE ("back within 1 dB " << back << " ms after the burst");
    CHECK (deepest == doctest::Approx (-8.0f).epsilon (0.01));
    CHECK (back > 140.0);
    CHECK (back < 160.0);
}

TEST_CASE ("A dynamic EQ band's gain stands still on a steady bass: a 30 Hz sine moves it by less than 0.1 dB")
{
    // A low shelf at 120 Hz, Dynamic -6 dB from -24 dB, and a 30 Hz sine 6 dB over it, where the gain moves with the level by half
    // the Dynamic: between the sine's peaks, 17 ms apart, the loudest held over 25 ms doesn't fall, so the gain doesn't ripple at
    // twice the bass's frequency. Each block of 32 samples' published gain, over its second second.
    const auto in = makeStereoSine (30.0, 96000, 0.126f, 0.126f);
    Eq eq;
    eq.prepare (testRate, 32);
    auto values = with ({ { "band2Frequency", 120.0f }, { "band2Dynamic", -6.0f }, { "band2Threshold", -24.0f } });
    auto out = in;
    auto lowest = 0.0f, highest = -24.0f;

    playEq (eq, out, 32, values, {},
            [&eq, &lowest, &highest] (int end)
            {
                if (end > 48000)
                {
                    lowest = std::min (lowest, eq.getGainMoved (1));
                    highest = std::max (highest, eq.getGainMoved (1));
                }
            });

    MESSAGE ("the gain moved between " << lowest << " and " << highest << " dB");
    CHECK (lowest == doctest::Approx (-3.0f).epsilon (0.05));
    CHECK (highest - lowest < 0.1f);
}

TEST_CASE ("The EQ as a de-esser: a burst at 7 kHz over quiet noise dips while it lasts, and the noise before and after is untouched")
{
    // The De-Ess preset: a bell at 7 kHz, Q 2, Dynamic -8 dB, Threshold -32 dB; white noise far under it, and half a second of a
    // 7 kHz sine 20 dB over it.
    constexpr int burstFrom = 24000, burstTo = 48000, numSamples = 72000;
    std::vector<float> values;

    for (const auto& [name, preset] : getPresetValues())
        if (name == juce::File::addTrailingSeparator ("Dynamic") + "De-Ess.phazitepreset")
            values = preset;

    REQUIRE (values.size() == Eq::info.parameters.size());
    CHECK (values[(size_t) Eq::getParameterIndex (5, Eq::frequency)] == doctest::Approx (7000.0f));
    CHECK (values[(size_t) Eq::getParameterIndex (5, Eq::dynamic)] == doctest::Approx (-8.0f));
    auto in = probe::makeNoise (numSamples, 17, 0.01f);
    const auto burst = makeStereoSine (7000.0, numSamples, 0.25f, 0.25f);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = burstFrom; i < burstTo; ++i)
            in.addSample (channel, i, burst.getSample (channel, i));

    Eq eq;
    eq.prepare (testRate, 480);
    auto out = in;
    auto current = values;
    float lowest = 0.0f;
    playEq (eq, out, 480, current, {},
            [&eq, &lowest] (int end)
            {
                if (end > burstFrom + 4800 && end <= burstTo)
                    lowest = std::min (lowest, eq.getGainMoved (5));
            });

    // Before it, the noise as it came; in it, the burst 8 dB down; and from 300 ms after it, the noise as it came again.
    CHECK (probe::getLargestDifference (out, in, 0, burstFrom) == 0.0);
    CHECK (lowest == doctest::Approx (-8.0f).epsilon (0.04));
    CHECK (probe::inDecibels (probe::getToneLevel (out, 0, 7000.0, testRate, burstFrom + 9600, 9600)
                              / probe::getToneLevel (in, 0, 7000.0, testRate, burstFrom + 9600, 9600))
           == doctest::Approx (-8.0).epsilon (0.04));
    CHECK (probe::getLargestDifference (out, in, burstTo + 14400) == 0.0);
}

TEST_CASE ("The EQ's mid-side presets touch only what they are for, and its dynamic presets leave a quiet sound as it is")
{
    std::map<juce::String, std::vector<float>> presets;

    for (const auto& [name, values] : getPresetValues())
        presets[name.replaceCharacter ('\\', '/')] = values;

    const auto get = [&presets] (const char* name) -> const std::vector<float>&
    {
        REQUIRE (presets.count (name) == 1);
        return presets[name];
    };

    // Wider Top and Mono Bass Cleanup leave a sound alike on both sides sample for sample, and change one opposite on them;
    // Centre Presence the reverse, its middle 2 dB up at 3 kHz.
    auto alike = probe::makeNoise (24000, 31, 0.5f), opposite = alike;

    for (int i = 0; i < alike.getNumSamples(); ++i)
    {
        alike.setSample (1, i, alike.getSample (0, i));
        opposite.setSample (1, i, -opposite.getSample (0, i));
    }

    for (const auto* name : { "Mid-Side/Wider Top.phazitepreset", "Mid-Side/Mono Bass Cleanup.phazitepreset" })
    {
        CAPTURE (name);
        CHECK (probe::getLargestDifference (playedThrough (get (name), alike), alike) == 0.0);
        CHECK (probe::getLargestDifference (playedThrough (get (name), opposite), opposite) > 0.01);
    }

    const auto& presence = get ("Mid-Side/Centre Presence.phazitepreset");
    CHECK (probe::getLargestDifference (playedThrough (presence, opposite), opposite) == 0.0);
    CHECK (20.0 * std::log10 (Eq::getMagnitude (presence, 3000.0, testRate, Placement::mid)) == doctest::Approx (2.0).epsilon (0.01));

    // Mono Bass Cleanup takes the sides' bass away: 60 Hz on the side alone, 20 dB down or more.
    const auto bass = makeStereoSine (60.0, 48000, 0.25f, -0.25f);
    const auto cleaned = playedThrough (get ("Mid-Side/Mono Bass Cleanup.phazitepreset"), bass);
    CHECK (
        probe::inDecibels (probe::getToneLevel (cleaned, 0, 60.0, testRate, 24000) / probe::getToneLevel (bass, 0, 60.0, testRate, 24000))
        < -20.0);

    // Under their Thresholds, the dynamic ones play the sound as it came.
    const auto quiet = probe::makeNoise (48000, 37, 0.003f);

    for (const auto* name :
         { "Dynamic/De-Ess.phazitepreset", "Dynamic/Tame Harshness.phazitepreset", "Dynamic/Boom Control.phazitepreset" })
    {
        CAPTURE (name);
        CHECK (probe::getLargestDifference (playedThrough (get (name), quiet), quiet) == 0.0);
    }
}

TEST_CASE ("A dynamic EQ band at 0 dB comes in without a click, whether the sound swells or starts at once")
{
    // A 7 kHz sine through a de-esser's band, swelling over half a second or starting at full level, against the same band
    // played from the start (its Gain at -0.002 dB, its Dynamic the same, so that its moving gain is never 0): the two differ by
    // no more than that -0.002 dB explains, 2.3e-4 of the sine (5.8e-5 at 0.25), and rounding: under 1e-4, where a click would
    // be a step of a hundredth. A dynamic band is played at 0 dB too, its filter under way: one that was played only once its
    // gain left 0, starting from rest then, differed by 0.05 on the sine starting at once, whose first step takes the gain more
    // than a decibel from 0.
    const auto values =
        with ({ { "band6Frequency", 7000.0f }, { "band6Q", 2.0f }, { "band6Dynamic", -8.0f }, { "band6Threshold", -32.0f } });
    auto playedAll = values;
    playedAll[(size_t) Eq::getParameterIndex (5, Eq::gain)] = -0.002f;

    for (const auto swells : { true, false })
    {
        CAPTURE (swells);
        auto in = makeStereoSine (7000.0, 48000, 0.25f, 0.25f);

        if (swells)
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < 24000; ++i)
                    in.setSample (channel, i, in.getSample (channel, i) * (float) i / 24000.0f);

        const auto out = playedThrough (values, in), fromTheStart = playedThrough (playedAll, in);
        const auto difference = probe::getLargestDifference (out, fromTheStart);
        MESSAGE ("the band coming in differs from the band played from the start by " << difference);
        CHECK (difference < 1.0e-4);
        CHECK (probe::getToneLevel (out, 0, 7000.0, testRate, 36000) < 0.5 * probe::getToneLevel (in, 0, 7000.0, testRate, 36000));
    }
}

TEST_CASE ("A dynamic EQ band hears what it plays on: a band on the middle hears the middle alone, one on the left the left")
{
    struct Heard
    {
        Placement placement;
        float left, right; // the sine's amplitude on each side
        bool moves;
    };

    const Heard cases[] = { { Placement::mid, 0.25f, -0.25f, false }, { Placement::mid, 0.25f, 0.25f, true },
                            { Placement::side, 0.25f, 0.25f, false }, { Placement::side, 0.25f, -0.25f, true },
                            { Placement::left, 0.0f, 0.25f, false },  { Placement::left, 0.25f, 0.0f, true },
                            { Placement::right, 0.25f, 0.0f, false }, { Placement::stereo, 0.0f, 0.25f, true } };

    for (const auto& heard : cases)
    {
        CAPTURE ((int) heard.placement);
        CAPTURE (heard.left);
        CAPTURE (heard.right);
        auto values = dynamicBell();
        values[(size_t) Eq::getParameterIndex (3, Eq::placement)] = (float) heard.placement;
        const auto in = makeStereoSine (1000.0, 24000, heard.left, heard.right);
        Eq eq;
        eq.prepare (testRate, 480);
        auto out = in;
        playEq (eq, out, 480, values);

        if (heard.moves)
        {
            CHECK (eq.getGainMoved (3) == doctest::Approx (-8.0f).epsilon (0.04));
        }
        else
        {
            CHECK (juce::exactlyEqual (eq.getGainMoved (3), 0.0f));
            CHECK (probe::getLargestDifference (out, in) == 0.0);
        }
    }
}

TEST_CASE ("The EQ plays the same in blocks of any size, with dynamic bands on each placement and placements changing")
{
    // Noise with bursts of tones in it, louder on one side or the other; a dynamic band on each placement; and, half way, one
    // band moved to another placement and one Dynamic turned, at the same sample whatever the blocks (the block it falls in cut
    // there, as a runner cuts a block where automation changes). Blocks of 24 and 48 samples split the EQ's steps of 32, and
    // those of 100 and 1000 don't fall on the change.
    constexpr int numSamples = 49152, changeAt = 24576;
    auto in = probe::makeNoise (numSamples, 23, 0.05f);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = (double) i / testRate;
        const auto burst = (i / 6000) % 2 == 1 ? 0.3 : 0.02;
        in.addSample (0, i, (float) (burst * std::sin (twoPi * 1000.0 * t)));
        in.addSample (1, i, (float) (burst * 0.5 * std::sin (twoPi * 6000.0 * t) + 0.1 * std::sin (twoPi * 80.0 * t)));
    }

    const auto values = with ({ { "band2Dynamic", -6.0f },
                                { "band2Threshold", -30.0f },
                                { "band3Frequency", 1000.0f },
                                { "band3Gain", 2.0f },
                                { "band3Dynamic", -9.0f },
                                { "band3Threshold", -36.0f },
                                { "band3Placement", (float) Placement::mid },
                                { "band4Frequency", 1000.0f },
                                { "band4Dynamic", -5.0f },
                                { "band4Placement", (float) Placement::left },
                                { "band5Frequency", 6000.0f },
                                { "band5Dynamic", 6.0f },
                                { "band5Threshold", -40.0f },
                                { "band5Placement", (float) Placement::right },
                                { "band7Gain", -2.0f },
                                { "band7Dynamic", 4.0f },
                                { "band7Placement", (float) Placement::side } });
    auto changed = values;
    changed[(size_t) Eq::getParameterIndex (2, Eq::placement)] = (float) Placement::side;
    changed[(size_t) Eq::getParameterIndex (3, Eq::dynamic)] = -12.0f;

    const auto playedIn = [&] (int blockSize)
    {
        Eq eq;
        eq.prepare (testRate, blockSize);
        auto out = in;
        auto blocks = 0;

        for (int start = 0; start < numSamples; ++blocks)
        {
            auto length = std::min (blockSize, numSamples - start);

            if (start < changeAt && start + length > changeAt)
                length = changeAt - start;

            float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = length;
            block.parameters = start < changeAt ? values : changed;
            block.isPlaying = true;
            block.songSample = start;
            eq.process (block);
            start += length;
        }

        CHECK (blocks >= numSamples / blockSize);
        return out;
    };

    const auto in32 = playedIn (32);
    CHECK (probe::getLargestDifference (in32, in) > 0.01);

    for (const auto blockSize : { 24, 48, 96, 100, 512, 1000, 4096 })
    {
        CAPTURE (blockSize);
        const auto other = playedIn (blockSize);
        CHECK (countDifferences (other, in32, 0) == 0);
        CHECK (countDifferences (other, in32, 1) == 0);
    }
}

TEST_CASE ("An EQ with a dynamic band sleeps only once its level has fallen to none, so that it plays live as in a render")
{
    // Its runner lets a device sleep once what comes in and what it plays have been silent a quarter of a second and as long as
    // the tail it reports, and wakes it without a reset; a render never lets it sleep. De-Ess's band, a burst 20 dB over its
    // Threshold, 3 s of silence, and the burst again: live, it sleeps through most of the silence (fewer blocks worked), and the
    // second burst comes out as from the EQ that never slept. Its tail: held 26 ms, then 60 ms for each 8.7 dB from 12 dB over
    // full scale to 24 dB under its Threshold.
    constexpr int block = 480, burst = 9600, silence = 144000;
    const auto values =
        with ({ { "band6Frequency", 7000.0f }, { "band6Q", 2.0f }, { "band6Dynamic", -8.0f }, { "band6Threshold", -32.0f } });
    const auto tone = makeStereoSine (7000.0, burst, 0.251f, 0.251f);
    const auto in = probe::join (probe::join (probe::join (tone, probe::makeSilence (silence)), tone), probe::makeSilence (block * 10));
    const auto secondAt = burst + silence;

    const auto playedLive = [&in] (const std::vector<float>& settings, bool live, std::uint64_t& blocksWorked, double& tail)
    {
        probe::Device device (Eq::info);
        device.values = settings;
        device.isRendering = ! live;
        device.prepare (testRate, block);
        auto out = in;
        device.play (out, block);
        blocksWorked = device.getRunner().getBlocksWorked();
        tail = device.getTailSeconds();
        return out;
    };

    std::uint64_t liveBlocks = 0, renderBlocks = 0;
    auto tail = 0.0, renderTail = 0.0;
    const auto live = playedLive (values, true, liveBlocks, tail);
    const auto render = playedLive (values, false, renderBlocks, renderTail);
    const auto expectedTail = 0.026 + 0.06 * std::log (10.0) / 20.0 * (12.0 + 32.0 + 24.0);
    MESSAGE ("live, " << liveBlocks << " blocks worked of " << renderBlocks << "; its tail " << tail << " s");
    CHECK (tail == doctest::Approx (expectedTail).epsilon (0.001));
    CHECK (liveBlocks + (std::uint64_t) (silence / block / 2) < renderBlocks); // asleep for more than half the silence
    CHECK (probe::getLargestDifference (live, render, secondAt) < 1.0e-5);
    CHECK (probe::getToneLevel (live, 0, 7000.0, testRate, secondAt + burst / 2, burst / 2)
           < 0.5 * probe::getToneLevel (in, 0, 7000.0, testRate, secondAt + burst / 2, burst / 2)); // turned down again

    // An EQ without a dynamic band reports no tail, and sleeps a quarter of a second into the silence, as it did.
    auto plain = values;
    plain[(size_t) Eq::getParameterIndex (5, Eq::dynamic)] = 0.0f;
    plain[(size_t) Eq::getParameterIndex (5, Eq::gain)] = -6.0f;
    std::uint64_t plainBlocks = 0;
    auto plainTail = 1.0;
    playedLive (plain, true, plainBlocks, plainTail);
    CHECK (juce::exactlyEqual (plainTail, 0.0));
    CHECK (plainBlocks <= (std::uint64_t) (2 * burst / block + 2 * (int) (0.25 * testRate) / block + 4));
}

TEST_CASE ("Boom Control leaves a bass at a usual level alone and turns its boomy notes down")
{
    // A bass line at -18 dBFS RMS (each note half a second, a fundamental with its second and third harmonics at half and a
    // quarter), and one boomy note 8 dB louder: the loudest the band hears of each (a 12 dB per octave low-pass at 120 Hz), and
    // how far Boom Control moves its gain in each, in its second half.
    std::vector<float> values;

    for (const auto& [name, preset] : getPresetValues())
        if (name == juce::File::addTrailingSeparator ("Dynamic") + "Boom Control.phazitepreset")
            values = preset;

    REQUIRE (values.size() == Eq::info.parameters.size());
    constexpr int noteLength = 24000;
    const double notes[] = { 41.2, 55.0, 73.4, 49.0, 55.0, 61.7 };
    constexpr int boomy = 4;
    probe::Sound bass (2, noteLength * (int) std::size (notes));
    auto power = 0.0;

    for (int note = 0; note < (int) std::size (notes); ++note)
    {
        for (int i = 0; i < noteLength; ++i)
        {
            const auto phase = twoPi * notes[(size_t) note] * i / testRate;
            const auto value = std::sin (phase) + 0.5 * std::sin (2.0 * phase) + 0.25 * std::sin (3.0 * phase);
            bass.setSample (0, note * noteLength + i, (float) value);

            if (note != boomy)
                power += value * value;
        }
    }

    const auto rms = std::sqrt (power / (noteLength * ((int) std::size (notes) - 1)));
    const auto scale = (float) (juce::Decibels::decibelsToGain (-18.0) / rms);

    for (int i = 0; i < bass.getNumSamples(); ++i)
    {
        const auto louder = i / noteLength == boomy ? juce::Decibels::decibelsToGain (8.0f) : 1.0f;
        bass.setSample (0, i, bass.getSample (0, i) * scale * louder);
        bass.setSample (1, i, bass.getSample (0, i));
    }

    // What the band hears of each note.
    auto ear = devices::dsp::makeFilter (devices::dsp::FilterShape::lowPass, 120.0, testRate, 0.70710678);
    devices::dsp::Filter filter;
    std::vector<double> heard (std::size (notes), 0.0);

    for (int i = 0; i < bass.getNumSamples(); ++i)
    {
        const auto x = std::abs (filter.process (bass.getSample (0, i), ear));
        auto& loudest = heard[(size_t) (i / noteLength)];
        loudest = std::max (loudest, (double) x);
    }

    // And how far it turns each note down, once in it.
    Eq eq;
    eq.prepare (testRate, 480);
    auto out = bass;
    std::vector<float> moved (std::size (notes), 0.0f);
    playEq (eq, out, 480, values, {},
            [&eq, &moved] (int end)
            {
                // In each note's second half, where the last note's boom has gone.
                if ((end - 1) % noteLength >= noteLength / 2)
                {
                    auto& most = moved[(size_t) ((end - 1) / noteLength)];
                    most = std::min (most, eq.getGainMoved (1));
                }
            });

    for (size_t note = 0; note < std::size (notes); ++note)
    {
        CAPTURE (note);
        MESSAGE ("note " << note << " (" << notes[note] << " Hz): heard at " << 20.0 * std::log10 (heard[note]) << " dBFS, turned "
                         << moved[note] << " dB");

        if ((int) note == boomy)
            CHECK (moved[note] < -2.0f);
        else
            CHECK (moved[note] > -0.1f);
    }
}
