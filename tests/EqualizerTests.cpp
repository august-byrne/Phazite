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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
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
    CHECK (Eq::info.parameters.size() == 75);
    CHECK (devices::findParameter (Eq::info, "phase") == Eq::phaseParameter);
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
    CHECK (info->parameters.size() == 75);
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

    // Those that came before it, the eleven that place no band, make none dynamic and play in Minimum.
    std::erase_if (settings,
                   [] (const auto& setting)
                   {
                       if (setting.second[(size_t) Eq::phaseParameter] > 0.0f)
                           return true;

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

//==============================================================================
// Linear phase (step S6.4, part B): its bands as kernels symmetric about their middle tap, L samples late.
namespace
{
constexpr int linearLatency = 4096; // at 48 kHz

std::vector<float> inLinear (std::vector<float> values)
{
    values[(size_t) Eq::phaseParameter] = (float) Eq::Phase::linear;
    return values;
}

// An EQ with these values set by hand, prepared as its model prepares one (its Phase set by hand decides its latency).
std::unique_ptr<Eq> makePrepared (const std::vector<float>& values, int blockSize = 512, double rate = testRate)
{
    auto eq = std::make_unique<Eq>();
    eq->takeValuesBeforePrepare (values);
    eq->prepare (rate, blockSize);
    return eq;
}

// A band set by its fields.
struct BandSetting
{
    int band;
    Eq::BandType type;
    float frequency, gain = 0.0f, q = 1.0f;
    int slope = 1;
    Placement placement = Placement::stereo;
};

std::vector<float> withBands (std::initializer_list<BandSetting> settings, std::vector<float> values = getDefaults())
{
    for (const auto& setting : settings)
    {
        set (values, setting.band, Eq::on, 1.0f);
        set (values, setting.band, Eq::type, (float) setting.type);
        set (values, setting.band, Eq::frequency, setting.frequency);
        set (values, setting.band, Eq::gain, setting.gain);
        set (values, setting.band, Eq::q, setting.q);
        set (values, setting.band, Eq::slope, (float) setting.slope);
        set (values, setting.band, Eq::placement, (float) setting.placement);
    }

    return values;
}

// Each way through an EQ in Linear, by its impulse responses: [side in][side out], numSamples of each, from an impulse at the
// first sample after it was prepared.
using Responses = std::array<std::array<std::vector<double>, 2>, 2>;

Responses getImpulseResponses (const std::vector<float>& linearValues, int numSamples = 2 * linearLatency + 64)
{
    Responses responses;

    for (int in = 0; in < 2; ++in)
    {
        auto eq = makePrepared (linearValues);
        probe::Sound sound (2, numSamples);
        sound.clear();
        sound.setSample (in, 0, 1.0f);
        auto values = linearValues;
        playEq (*eq, sound, 512, values);

        for (int out = 0; out < 2; ++out)
            for (int i = 0; i < numSamples; ++i)
                responses[(size_t) in][(size_t) out].push_back (sound.getSample (out, i));
    }

    return responses;
}

// A response's gain at a frequency, in decibels: what a sine there comes out at once it has gone through it.
double responseDecibels (const std::vector<double>& response, double frequency, double rate = testRate)
{
    double re = 0.0, im = 0.0;

    for (size_t n = 0; n < response.size(); ++n)
    {
        const auto angle = twoPi * frequency * (double) n / rate;
        re += response[n] * std::cos (angle);
        im -= response[n] * std::sin (angle);
    }

    return 10.0 * std::log10 (std::max (1.0e-30, re * re + im * im));
}

// Frequencies from 20 Hz to 20 kHz, so many to an octave.
std::vector<double> getFrequencies (int perOctave = 12, double from = 20.0, double to = 20000.0)
{
    std::vector<double> frequencies;

    for (auto f = from; f <= to * 1.0001; f *= std::pow (2.0, 1.0 / perOctave))
        frequencies.push_back (f);

    return frequencies;
}

// The largest difference, in decibels, between a response and what the EQ reports, at frequencies where it reports more than
// a floor; and what the response is at a frequency.
double getLargestError (const std::vector<double>& response, const std::vector<float>& values, double floorDecibels = -60.0,
                        Placement way = Placement::stereo)
{
    auto largest = 0.0;

    for (const auto f : getFrequencies (24))
    {
        const auto reported = 20.0 * std::log10 (Eq::getMagnitude (values, f, testRate, way));

        if (reported > floorDecibels)
            largest = std::max (largest, std::abs (responseDecibels (response, f) - reported));
    }

    return largest;
}

// How far a response is from the same reversed about the latency's sample, at its worst, as a part of its largest tap.
double getAsymmetry (const std::vector<double>& response)
{
    auto largest = 0.0, apart = 0.0;

    for (const auto x : response)
        largest = std::max (largest, std::abs (x));

    for (int n = 1; n <= linearLatency; ++n)
        apart = std::max (apart, std::abs (response[(size_t) (linearLatency - n)] - response[(size_t) (linearLatency + n)]));

    return largest > 0.0 ? apart / largest : 0.0;
}
} // namespace

TEST_CASE ("The EQ's Phase is Minimum by default, and Linear reports its latency and plays every frequency as late as every other")
{
    CHECK (juce::exactlyEqual (Eq::info.parameters[(size_t) Eq::phaseParameter].defaultValue, 0.0f));
    CHECK (Eq::info.parameters[(size_t) Eq::phaseParameter].choices.size() == 2);
    CHECK (devices::LinearPhase::getLatencyFor (44100.0) == 4096);
    CHECK (devices::LinearPhase::getLatencyFor (48000.0) == 4096);
    CHECK (devices::LinearPhase::getLatencyFor (96000.0) == 8192);
    CHECK (devices::LinearPhase::getLatencyFor (192000.0) == 16384);

    // The latency follows the Phase set by hand.
    Eq eq;
    eq.prepare (testRate, 512);
    CHECK (eq.getLatencySamplesFor (getDefaults()) == 0);
    CHECK (eq.getLatencySamplesFor (inLinear (getDefaults())) == linearLatency);
    CHECK (eq.getLatencySamples() == 0);
    CHECK (makePrepared (inLinear (getDefaults()))->getLatencySamples() == linearLatency);

    // Six settings that between them use every type, slope and placement: each response through it is symmetric about L.
    const std::vector<float> settings[] = {
        withBands ({ { 0, Eq::BandType::lowCut, 80.0f, 0.0f, 0.71f, 0 },
                     { 3, Eq::BandType::bell, 1000.0f, 6.0f, 2.0f },
                     { 6, Eq::BandType::highShelf, 8000.0f, -4.0f, 0.71f } }),
        withBands ({ { 0, Eq::BandType::lowCut, 30.0f, 0.0f, 0.71f, 3 },
                     { 1, Eq::BandType::lowShelf, 150.0f, 3.0f, 0.71f },
                     { 4, Eq::BandType::notch, 3000.0f, 0.0f, 2.0f },
                     { 7, Eq::BandType::highCut, 12000.0f, 0.0f, 0.71f, 1 } }),
        withBands ({ { 0, Eq::BandType::lowCut, 200.0f, 0.0f, 0.71f, 1, Placement::side },
                     { 2, Eq::BandType::bell, 500.0f, -6.0f, 1.0f, 1, Placement::mid },
                     { 7, Eq::BandType::highCut, 15000.0f, 0.0f, 0.71f, 2 } }),
        withBands ({ { 1, Eq::BandType::lowShelf, 300.0f, -5.0f, 0.71f, 1, Placement::right },
                     { 3, Eq::BandType::bell, 2000.0f, 8.0f, 1.5f, 1, Placement::left } }),
        withBands ({ { 4, Eq::BandType::notch, 60.0f, 0.0f, 4.0f, 1, Placement::mid },
                     { 6, Eq::BandType::highShelf, 6000.0f, 4.0f, 0.71f, 1, Placement::side } }),
        withBands ({ { 0, Eq::BandType::lowCut, 100.0f, 0.0f, 2.0f, 2 },
                     { 2, Eq::BandType::bell, 400.0f, 5.0f, 3.0f, 1, Placement::mid },
                     { 4, Eq::BandType::notch, 7000.0f, 0.0f, 3.0f, 1, Placement::right },
                     { 7, Eq::BandType::highCut, 5000.0f, 0.0f, 0.71f, 0, Placement::left } }),
    };

    for (size_t setting = 0; setting < std::size (settings); ++setting)
    {
        CAPTURE (setting);
        const auto responses = getImpulseResponses (inLinear (settings[setting]));

        for (int in = 0; in < 2; ++in)
            for (int out = 0; out < 2; ++out)
            {
                CAPTURE (in);
                CAPTURE (out);
                const auto& response = responses[(size_t) in][(size_t) out];
                const auto asymmetry = getAsymmetry (response);
                MESSAGE ("setting " << setting << ", " << in << " to " << out << ": largest tap "
                                    << *std::max_element (response.begin(), response.end()) << ", asymmetry " << asymmetry);
                CHECK (asymmetry < 1.0e-6);
            }
    }

    // With every band off, an impulse comes out as one sample of 1 at L and nothing else above -120 dB.
    auto off = getDefaults();

    for (int band = 0; band < Eq::numBands; ++band)
        set (off, band, Eq::on, 0.0f);

    const auto through = getImpulseResponses (inLinear (off));
    auto largestElse = 0.0;

    for (int out = 0; out < 2; ++out)
        for (size_t i = 0; i < through[0][(size_t) out].size(); ++i)
            if (out != 0 || i != (size_t) linearLatency)
                largestElse = std::max (largestElse, std::abs (through[0][(size_t) out][i]));

    MESSAGE ("every band off: " << through[0][0][(size_t) linearLatency] << " at L, the largest elsewhere " << largestElse);
    CHECK (std::abs (through[0][0][(size_t) linearLatency] - 1.0) < 1.0e-6);
    CHECK (largestElse < 1.0e-6);
}

TEST_CASE ("The EQ in Linear plays the curve it reports: within 0.1 dB for bells and shelves from 200 Hz with a Q up to 4")
{
    struct Shape
    {
        Eq::BandType type;
        float frequency, gain, q;
    };

    std::vector<Shape> shapes;

    for (const auto gain : { -12.0f, 12.0f })
    {
        for (const auto frequency : { 200.0f, 1000.0f, 5000.0f, 15000.0f })
            for (const auto q : { 0.5f, 1.0f, 4.0f })
                shapes.push_back ({ Eq::BandType::bell, frequency, gain, q });

        for (const auto frequency : { 200.0f, 2000.0f })
            for (const auto q : { 0.71f, 2.0f })
            {
                shapes.push_back ({ Eq::BandType::lowShelf, frequency, gain, q });
                shapes.push_back ({ Eq::BandType::highShelf, frequency * 4.0f, gain, q });
            }
    }

    auto worst = 0.0;

    for (const auto& shape : shapes)
    {
        CAPTURE ((int) shape.type);
        CAPTURE (shape.frequency);
        CAPTURE (shape.gain);
        CAPTURE (shape.q);
        const auto values = inLinear (withBands ({ { 3, shape.type, shape.frequency, shape.gain, shape.q } }));
        const auto error = getLargestError (getImpulseResponses (values)[0][0], values);
        worst = std::max (worst, error);
        CHECK (error < 0.1);
    }

    MESSAGE ("the largest error of any of them: " << worst << " dB");

    // A sine through it comes out as its impulse response says it does.
    const auto values = inLinear (withBands ({ { 3, Eq::BandType::bell, 1000.0f, 12.0f, 4.0f } }));
    const auto response = getImpulseResponses (values)[0][0];
    auto eq = makePrepared (values);
    auto sine = makeStereoSine (1100.0, 3 * linearLatency + 9600, 0.5f, 0.5f);
    const auto in = sine;
    auto current = values;
    playEq (*eq, sine, 512, current);
    const auto measured = probe::inDecibels (probe::getToneLevel (sine, 0, 1100.0, testRate, 3 * linearLatency, 9600)
                                             / probe::getToneLevel (in, 0, 1100.0, testRate, 3 * linearLatency, 9600));
    CHECK (measured == doctest::Approx (responseDecibels (response, 1100.0)).epsilon (0.001));
}

TEST_CASE ("The EQ in Linear at the low end: how far its curve is from the reported one, the table on its design page")
{
    // Its kernel is 2 (L - P) taps long, 160 ms at 48 kHz: what rings longer than that at the low end is cut short, which
    // smears its curve there. Each figure measured (2026-11-18, windows-debug) and held with a decibel of margin;
    // docs/design/devices/eq.md has the table.
    struct Row
    {
        Eq::BandType type;
        float frequency, gain, q;
        int slope;
        double error;      // the largest, in decibels, where the curve is above -60 dB
        double at10 = 0.0; // a low cut's level at 10 Hz, as measured
    };

    // clang-format off
    const Row rows[] = {
        { Eq::BandType::bell, 30.0f, 12.0f, 1.0f, 1, 0.11 },   { Eq::BandType::bell, 30.0f, 12.0f, 4.0f, 1, 3.64 },
        { Eq::BandType::bell, 30.0f, 12.0f, 10.0f, 1, 5.36 },  { Eq::BandType::bell, 60.0f, 12.0f, 1.0f, 1, 0.002 },
        { Eq::BandType::bell, 60.0f, 12.0f, 4.0f, 1, 1.04 },   { Eq::BandType::bell, 60.0f, 12.0f, 10.0f, 1, 3.04 },
        { Eq::BandType::bell, 100.0f, 12.0f, 1.0f, 1, 0.0 },   { Eq::BandType::bell, 100.0f, 12.0f, 4.0f, 1, 0.20 },
        { Eq::BandType::bell, 100.0f, 12.0f, 10.0f, 1, 2.08 }, { Eq::BandType::bell, 30.0f, -12.0f, 1.0f, 1, 0.016 },
        { Eq::BandType::bell, 30.0f, -12.0f, 4.0f, 1, 1.21 },  { Eq::BandType::bell, 30.0f, -12.0f, 10.0f, 1, 3.72 },
        { Eq::BandType::bell, 60.0f, -12.0f, 1.0f, 1, 0.0 },   { Eq::BandType::bell, 60.0f, -12.0f, 4.0f, 1, 0.27 },
        { Eq::BandType::bell, 60.0f, -12.0f, 10.0f, 1, 2.64 }, { Eq::BandType::bell, 100.0f, -12.0f, 1.0f, 1, 0.0 },
        { Eq::BandType::bell, 100.0f, -12.0f, 4.0f, 1, 0.027 }, { Eq::BandType::bell, 100.0f, -12.0f, 10.0f, 1, 0.44 },
        { Eq::BandType::lowCut, 30.0f, 0.0f, 0.71f, 0, 0.0, -19.13 }, { Eq::BandType::lowCut, 80.0f, 0.0f, 0.71f, 0, 0.0, -36.12 },
        { Eq::BandType::lowCut, 30.0f, 0.0f, 0.71f, 3, 4.30, -61.4 }, { Eq::BandType::lowCut, 80.0f, 0.0f, 0.71f, 3, 0.06, -109.6 },
    };
    // clang-format on

    for (const auto& row : rows)
    {
        CAPTURE ((int) row.type);
        CAPTURE (row.frequency);
        CAPTURE (row.gain);
        CAPTURE (row.q);
        CAPTURE (row.slope);
        const auto values = inLinear (withBands ({ { 3, row.type, row.frequency, row.gain, row.q, row.slope } }));
        const auto response = getImpulseResponses (values)[0][0];
        const auto error = getLargestError (response, values);
        const auto at10 = responseDecibels (response, 10.0);
        MESSAGE ("error " << error << " dB; at 10 Hz " << at10 << " dB, reported "
                          << 20.0 * std::log10 (Eq::getMagnitude (values, 10.0, testRate)) << " dB");
        CHECK (error < row.error + 1.0);

        if (row.type == Eq::BandType::lowCut)
            CHECK (at10 < row.at10 + 1.0);
    }

    // A notch's depth at its frequency (Q 1): -39.3 dB at 100 Hz, -49.6 dB at 1 kHz.
    for (const auto& [frequency, depth] : { std::pair { 100.0f, -39.3 }, std::pair { 1000.0f, -49.6 } })
    {
        CAPTURE (frequency);
        const auto values = inLinear (withBands ({ { 4, Eq::BandType::notch, frequency, 0.0f, 1.0f } }));
        const auto measured = responseDecibels (getImpulseResponses (values)[0][0], frequency);
        MESSAGE ("a notch at " << frequency << " Hz: " << measured << " dB deep");
        CHECK (measured < depth + 1.0);
    }

    // Its gain at 0 Hz, a constant in and what comes out once the kernel is full of it: what the cut dropped of each kernel's
    // sum is spread over its taps, so the cuts take it out and the shelves keep it.
    struct AtZero
    {
        Eq::BandType type;
        float q;
        int slope;
    };

    for (const auto& row : { AtZero { Eq::BandType::lowCut, 0.71f, 3 }, AtZero { Eq::BandType::lowCut, 0.71f, 0 },
                             AtZero { Eq::BandType::lowShelf, 0.71f, 1 }, AtZero { Eq::BandType::lowShelf, 4.0f, 1 } })
    {
        CAPTURE ((int) row.type);
        CAPTURE (row.q);
        CAPTURE (row.slope);
        const auto isCut = row.type == Eq::BandType::lowCut;
        const auto values = inLinear (withBands ({ { 3, row.type, 30.0f, isCut ? 0.0f : 12.0f, row.q, row.slope } }));
        auto constant = probe::Sound (2, 3 * linearLatency);

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < constant.getNumSamples(); ++i)
                constant.setSample (channel, i, 0.5f);

        auto current = values;
        playEq (*makePrepared (values), constant, 512, current);
        auto out = 0.0;

        for (int i = 2 * linearLatency + 256; i < 2 * linearLatency + 512; ++i)
            out += constant.getSample (0, i) / 0.5 / 256.0;

        const auto measured = 20.0 * std::log10 (std::max (1.0e-30, std::abs (out)));
        const auto curve = 20.0 * std::log10 (std::max (1.0e-30, (double) Eq::getMagnitude (values, 0.0, testRate)));
        MESSAGE (std::string (isCut ? "a low cut" : "a low shelf")
                 << " at 30 Hz: " << measured << " dB at 0 Hz, its curve " << curve << " dB");

        if (isCut)
            CHECK (measured < -90.0);
        else
            CHECK (std::abs (measured - curve) < 0.01);
    }
}

TEST_CASE ("The EQ in Linear at a Mix of 50 % is an EQ of half the depth, which in Minimum it is not")
{
    // A bell of +12 dB: through the runner, which mixes what came in back in, as late as the EQ's latency.
    const auto played = [] (bool isLinear)
    {
        auto values = withBands ({ { 3, Eq::BandType::bell, 1000.0f, 12.0f, 1.0f } });
        values[(size_t) Eq::mixParameter] = 0.5f;

        if (isLinear)
            values = inLinear (values);

        probe::Device device (Eq::info);
        device.setAll (values);
        device.prepare (testRate, 512);
        const auto out = device.played (probe::makeImpulse (2 * linearLatency + 512), 512);
        std::vector<double> response;

        for (int i = 0; i < out.getNumSamples(); ++i)
            response.push_back (out.getSample (0, i));

        return std::pair { response, withBands ({ { 3, Eq::BandType::bell, 1000.0f, 12.0f, 1.0f } }) };
    };

    for (const auto isLinear : { true, false })
    {
        CAPTURE (isLinear);
        const auto [response, values] = played (isLinear);
        auto largestApart = 0.0, deepest = 0.0, deepestAt = 0.0;

        for (const auto f : getFrequencies (24))
        {
            const auto expected = 20.0 * std::log10 (0.5 * (1.0 + Eq::getMagnitude (values, f, testRate)));
            const auto apart = responseDecibels (response, f) - expected;
            largestApart = std::max (largestApart, std::abs (apart));

            if (apart < deepest)
            {
                deepest = apart;
                deepestAt = f;
            }
        }

        MESSAGE ((isLinear ? "Linear" : "Minimum") << ": at most " << largestApart << " dB from (1 + gain) / 2, its deepest dip " << deepest
                                                   << " dB at " << deepestAt << " Hz");

        if (isLinear)
            CHECK (largestApart < 0.1);
        else
            CHECK (deepest < -0.3); // the phase the bell turns cancels some of what came in beside it
    }
}

// How the window and the grid were chosen (devices/eq/LinearPhase.h, Design): each row's largest error where the curve is above
// -60 dB, a low cut's level at 10 Hz, and the end tap's size against the middle tap's, for the rectangle, raised-cosine tapers
// of the ends' last 2, 3 and 5 %, and Kaisers of several betas, on grids of 2 L and 4 L. Run with --no-skip to see the table
// again, as for a longer kernel.
TEST_CASE ("The EQ's linear-phase window and grid, measured" * doctest::skip())
{
    const auto kernelError =
        [] (const std::vector<float>& values, devices::LinearPhase::Design design, double floorDecibels, double& at10, double& endTap)
    {
        devices::LinearPhase linear;
        linear.prepare (testRate, values, 2, design);
        const auto taps = linear.getPlayedTaps (0);
        std::vector<double> response (taps.begin(), taps.end());
        auto largest = 0.0;

        for (const auto f : getFrequencies (24))
        {
            const auto reported = 20.0 * std::log10 (Eq::getMagnitude (values, f, testRate));

            if (reported > floorDecibels)
                largest = std::max (largest, std::abs (responseDecibels (response, f) - reported));
        }

        at10 = responseDecibels (response, 10.0);
        endTap = 20.0 * std::log10 (std::max (1.0e-30, std::abs (response.front()) / std::abs (response[response.size() / 2])));
        return largest;
    };

    std::vector<std::pair<std::string, std::vector<float>>> rows;

    for (const auto q : { 1.0f, 4.0f })
        rows.push_back ({ "bell, 200 Hz, +12 dB, Q " + std::to_string (q), withBands ({ { 3, Eq::BandType::bell, 200.0f, 12.0f, q } }) });

    rows.push_back ({ "low shelf, 200 Hz, +12 dB, Q 2", withBands ({ { 3, Eq::BandType::lowShelf, 200.0f, 12.0f, 2.0f } }) });

    // Every bell of the low end's table.
    for (const auto gain : { 12.0f, -12.0f })
        for (const auto frequency : { 30.0f, 60.0f, 100.0f })
            for (const auto q : { 1.0f, 4.0f, 10.0f })
                rows.push_back ({ "bell, " + std::to_string ((int) frequency) + " Hz, " + std::to_string ((int) gain) + " dB, Q "
                                      + std::to_string ((int) q),
                                  withBands ({ { 3, Eq::BandType::bell, frequency, gain, q } }) });

    for (const auto slope : { 0, 3 })
        for (const auto frequency : { 30.0f, 80.0f })
            rows.push_back ({ "low cut, " + std::to_string (frequency) + " Hz, " + std::to_string (12 * (slope + 1)) + " dB",
                              withBands ({ { 0, Eq::BandType::lowCut, frequency, 0.0f, 0.71f, slope } }) });

    struct Window
    {
        double beta, taper;
    };

    for (const auto grid : { 2, 4 })
        for (const auto& window : { Window { 0.0, 0.0 }, Window { 0.0, 0.02 }, Window { 0.0, 0.03 }, Window { 0.0, 0.05 },
                                    Window { 2.0, 0.0 }, Window { 4.0, 0.0 }, Window { 8.0, 0.0 } })
        {
            std::string line = "grid " + std::to_string (grid) + " L, beta " + std::to_string (window.beta) + ", taper "
                               + std::to_string (window.taper * 100.0) + " %:";

            for (const auto& [name, values] : rows)
            {
                double at10 = 0.0, endTap = 0.0;
                const auto error = kernelError (inLinear (values), { grid, window.beta, window.taper }, -60.0, at10, endTap);
                line += " [" + name + "] " + std::to_string (error)
                        + (name.rfind ("low cut", 0) == 0 ? ", at 10 Hz " + std::to_string (at10) : "") + ", end tap "
                        + std::to_string (endTap) + " dB";
            }

            MESSAGE (line);
        }
}

namespace
{
// Pink noise, the same every time: white noise through Paul Kellet's filter, at about the amplitude given.
probe::Sound makePinkNoise (int numSamples, std::uint32_t seed, float amplitude)
{
    auto white = probe::makeNoise (numSamples, seed, 1.0f);
    probe::Sound pink (2, numSamples);

    for (int channel = 0; channel < 2; ++channel)
    {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

        for (int i = 0; i < numSamples; ++i)
        {
            const double w = white.getSample (channel, i);
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            pink.setSample (channel, i, (float) (amplitude * 0.11 * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362)));
            b6 = w * 0.115926;
        }
    }

    return pink;
}

// A bell's frequency along a sweep from 200 Hz to 2 kHz over a second, as automation gives it at each block of 32 samples.
float sweptFrequency (int sample)
{
    const auto t = std::clamp ((double) (sample / 32 * 32) / testRate, 0.0, 1.0);
    return (float) (200.0 * std::pow (10.0, t));
}
} // namespace

TEST_CASE ("The EQ in Linear follows a bell swept by automation without a click, its response the automation's within a step")
{
    constexpr int numSamples = 2 * (int) testRate, half = linearLatency / 2;
    const auto values = inLinear (withBands ({ { 3, Eq::BandType::bell, 200.0f, 9.0f, 2.0f } }));
    const auto frequencyIndex = (size_t) Eq::getParameterIndex (3, Eq::frequency);

    const auto playedSwept = [&] (const probe::Sound& in, bool swept, float heldAt)
    {
        auto eq = makePrepared (values, 32);
        auto out = in;
        auto current = values;
        current[frequencyIndex] = swept ? 200.0f : heldAt;
        playEq (*eq, out, 32, current,
                [&] (int start)
                {
                    if (swept)
                        current[frequencyIndex] = sweptFrequency (start);
                });
        return out;
    };

    // Under pink noise: the largest step from a sample to the next no more than the bell held anywhere along the way gives.
    const auto pink = makePinkNoise (numSamples, 31, 0.25f);
    auto held = 0.0;

    for (const auto f : { 200.0f, 400.0f, 632.0f, 1000.0f, 2000.0f })
        held = std::max (held, largestStep (playedSwept (pink, false, f), 2 * linearLatency));

    const auto moving = largestStep (playedSwept (pink, true, 0.0f), 2 * linearLatency);
    MESSAGE ("pink noise through the swept bell: the largest step " << moving << ", held anywhere along the way " << held);
    CHECK (moving <= held * 1.02);

    // A sine at 632 Hz: what comes out at each moment is the bell at the automation's frequency of L samples before, within
    // a step of the grid (L / 2) either way: between the least and the most the bell gives the sine over that stretch.
    const auto sine = makeStereoSine (632.0, numSamples, 0.25f, 0.25f);
    const auto out = playedSwept (sine, true, 0.0f);
    constexpr int window = 256;
    auto worst = 0.0;

    for (int at = linearLatency + 2 * half; at + window < numSamples; at += window)
    {
        const auto measured = probe::inDecibels (probe::getToneLevel (out, 0, 632.0, testRate, at, window) / 0.25);
        auto least = 1.0e9, most = -1.0e9;

        for (int t = at - linearLatency - half - window; t <= at + window - linearLatency + half; t += 32)
        {
            auto bell = values;
            bell[frequencyIndex] = sweptFrequency (std::max (0, t));
            const auto gain = 20.0 * std::log10 (Eq::getMagnitude (bell, 632.0, testRate));
            least = std::min (least, gain);
            most = std::max (most, gain);
        }

        const auto outside = std::max ({ 0.0, least - measured, measured - most });
        worst = std::max (worst, outside);
    }

    MESSAGE ("the sine's level outside what the automation gives within a step: at most " << worst << " dB");
    CHECK (worst < 0.05);

    // A step of the bell's gain at a known sample is half way in the sound L samples later, within half a step of the grid.
    for (const auto stepAt : { 10 * half, 10 * half + half / 2, 10 * half + 32, 10 * half + half - 32 })
    {
        CAPTURE (stepAt);
        auto flat = values;
        flat[(size_t) Eq::getParameterIndex (3, Eq::frequency)] = 1000.0f;
        flat[(size_t) Eq::getParameterIndex (3, Eq::gain)] = 0.0f;
        auto eq = makePrepared (flat, 32);
        auto tone = makeStereoSine (1000.0, 16 * half + 4 * linearLatency, 0.25f, 0.25f);
        auto current = flat;
        playEq (*eq, tone, 32, current,
                [&current, stepAt] (int start)
                {
                    if (start == stepAt)
                        current[(size_t) Eq::getParameterIndex (3, Eq::gain)] = 12.0f;
                });

        const auto middle = 0.25 * 0.5 * (1.0 + juce::Decibels::decibelsToGain (12.0));
        auto halfWay = -1.0;

        for (int at = stepAt; at + 64 < tone.getNumSamples(); at += 16)
            if (probe::getToneLevel (tone, 0, 1000.0, testRate, at, 64) >= middle)
            {
                halfWay = at + 32.0;
                break;
            }

        MESSAGE ("a step at " << stepAt << " is half way in at " << halfWay << ", " << halfWay - stepAt - linearLatency
                              << " samples from L later");
        CHECK (std::abs (halfWay - stepAt - linearLatency) <= half / 2 + 32);
    }
}

TEST_CASE ("The EQ in Linear plays the same in blocks of 7, 32 and 512 samples, from a jump, and after a sleep as a render")
{
    // Noise, bands on the middle and the side and on one side, one dynamic, and a band's frequency and gain changed at a sample
    // whatever the blocks (the block it falls in cut there).
    constexpr int numSamples = 48000, changeAt = 20000;
    const auto in = probe::makeNoise (numSamples, 41, 0.3f);
    const auto values = inLinear (withBands ({ { 0, Eq::BandType::lowCut, 60.0f, 0.0f, 0.71f, 2 },
                                               { 2, Eq::BandType::bell, 400.0f, 4.0f, 1.0f, 1, Placement::mid },
                                               { 4, Eq::BandType::highShelf, 5000.0f, -3.0f, 0.71f, 1, Placement::side },
                                               { 5, Eq::BandType::bell, 7000.0f, 0.0f, 2.0f, 1, Placement::left } }));
    auto withDynamic = values;
    withDynamic[(size_t) Eq::getParameterIndex (5, Eq::dynamic)] = -8.0f;
    withDynamic[(size_t) Eq::getParameterIndex (5, Eq::threshold)] = -40.0f;
    auto changed = withDynamic;
    changed[(size_t) Eq::getParameterIndex (2, Eq::frequency)] = 900.0f;
    changed[(size_t) Eq::getParameterIndex (2, Eq::gain)] = -6.0f;

    const auto playedIn = [&] (int blockSize)
    {
        auto eq = makePrepared (withDynamic, blockSize);
        auto out = in;

        for (int start = 0; start < numSamples;)
        {
            auto length = std::min (blockSize, numSamples - start);

            if (start < changeAt && start + length > changeAt)
                length = changeAt - start;

            float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = length;
            block.parameters = start < changeAt ? withDynamic : changed;
            block.isPlaying = true;
            block.songSample = start;
            eq->process (block);
            start += length;
        }

        return out;
    };

    const auto in32 = playedIn (32);
    CHECK (probe::getLargestDifference (in32, in) > 0.01);

    for (const auto blockSize : { 7, 512 })
    {
        CAPTURE (blockSize);
        const auto apart = probe::getLargestDifference (playedIn (blockSize), in32);
        MESSAGE ("blocks of " << blockSize << " against 32: " << apart << " apart at most");
        CHECK (apart <= 0.0); // to the bit: the partitions, the grid and the glides fall where they fall whatever the blocks
    }

    // From the song's start, and after a jump to the same place (the EQ having played something else before it), with its
    // parameters standing still: the same, once its history holds the same sound (2 L after the jump).
    constexpr int jumpTo = 12000;
    const auto render = [&]
    {
        auto eq = makePrepared (values, 480);
        auto out = in;
        auto current = values;
        playEq (*eq, out, 480, current);
        return out;
    }();

    {
        auto eq = makePrepared (values, 480);
        auto before = probe::makeNoise (30000, 5, 0.3f);
        auto current = values;
        playEq (*eq, before, 480, current);
        auto out = in;

        for (int start = jumpTo; start < numSamples; start += 480)
        {
            const auto length = std::min (480, numSamples - start);
            float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = length;
            block.parameters = values;
            block.isPlaying = true;
            block.songSample = start;
            eq->process (block);
        }

        const auto apart = probe::getLargestDifference (out, render, jumpTo + 2 * linearLatency);
        MESSAGE ("after a jump: " << apart << " from the render at most, from 2 L after it");
        CHECK (apart < 1.0e-5);
    }

    // After a sleep on silence, live, as in a render: a tone, 3 s of silence, the tone again.
    {
        const auto tone = makeStereoSine (300.0, 9600, 0.3f, 0.3f);
        const auto sound = probe::join (probe::join (tone, probe::makeSilence (144000)), tone);
        const auto playedLive = [&] (bool live, std::uint64_t& blocks)
        {
            probe::Device device (Eq::info);
            device.setAll (values);
            device.isRendering = ! live;
            device.prepare (testRate, 480);
            auto out = sound;
            device.play (out, 480);
            blocks = device.getRunner().getBlocksWorked();
            return out;
        };

        std::uint64_t liveBlocks = 0, renderBlocks = 0;
        const auto live = playedLive (true, liveBlocks), rendered = playedLive (false, renderBlocks);
        const auto apart = probe::getLargestDifference (live, rendered);
        MESSAGE ("after a sleep (" << liveBlocks << " blocks worked of " << renderBlocks << "): " << apart << " from the render at most");
        CHECK (liveBlocks + 100 < renderBlocks);
        CHECK (apart < 1.0e-5);
    }
}

TEST_CASE ("The EQ in Linear with a band automated across a jump or a sleep: how far from a render, and for how long")
{
    // A render's kernels there come from values the jump never played, so they can't be the same: measured, for the page. Where
    // the song jumps, a design under way for the values before it is dropped and one for the values there starts at once.
    constexpr int numSamples = 64000, jumpFrom = 12000 + 16, firstJumpTo = 23 * linearLatency / 2;
    const auto in = probe::makeNoise (numSamples, 43, 0.3f);
    const auto values = inLinear (withBands ({ { 2, Eq::BandType::bell, 400.0f, 9.0f, 1.0f } }));
    const auto frequencyIndex = (size_t) Eq::getParameterIndex (2, Eq::frequency);
    const auto automated = [&] (std::int64_t sample) { return (float) (400.0 * std::pow (4.0, (double) sample / 96000.0)); };

    const auto playFrom = [&] (Eq& eq, probe::Sound& out, int from, int to)
    {
        for (int start = from; start < to; start += 32)
        {
            auto current = values;
            current[frequencyIndex] = automated (start);
            float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = std::min (32, to - start);
            block.parameters = current;
            block.isPlaying = true;
            block.songSample = start;
            eq.process (block);
        }
    };

    auto render = in;
    {
        auto eq = makePrepared (values, 32);
        playFrom (*eq, render, 0, numSamples);
    }

    const auto measureApart = [] (const probe::Sound& a, const probe::Sound& b, int from, const std::string& what)
    {
        auto largest = 0.0;
        auto lastApart = from;

        for (int i = from; i < a.getNumSamples(); ++i)
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto apart = std::abs ((double) a.getSample (channel, i) - b.getSample (channel, i));
                largest = std::max (largest, apart);

                if (apart > 1.0e-5)
                    lastApart = i;
            }

        MESSAGE ("a band automated across " << what << ": up to " << largest << " (" << probe::inDecibels (largest)
                                            << " dBFS) from a render, for " << lastApart - from << " samples after it");
        return std::pair { lastApart - from, largest };
    };

    // A jump to every 16th place of a step of the grid of L / 2. Before the jump, at the song's places before jumpFrom, the
    // sound the render had before jumpTo: so that after the jump the EQ's history is the render's, and only its kernels, from
    // the values before the jump, differ. The new values' kernels glide in from the first line after their design, done
    // L / 2 after the jump: wholly in under 1.5 L later at the worst.
    auto worstLength = 0, worstAt = 0;
    auto worstApart = 0.0;

    for (int place = 0; place < 16; ++place)
    {
        const auto jumpTo = firstJumpTo + place * linearLatency / 32;
        CAPTURE (jumpTo);
        auto jumped = in;
        auto eq = makePrepared (values, 32);
        probe::Sound before (2, numSamples);
        before.clear();

        for (int channel = 0; channel < 2; ++channel)
            before.copyFrom (channel, 0, in, channel, jumpTo - jumpFrom, jumpFrom);

        playFrom (*eq, before, 0, jumpFrom);
        playFrom (*eq, jumped, jumpTo, numSamples);
        const auto [length, apart] = measureApart (jumped, render, jumpTo, "a jump to " + std::to_string (jumpTo));
        CHECK (length < 3 * linearLatency / 2);

        if (length > worstLength)
        {
            worstLength = length;
            worstAt = jumpTo;
        }

        worstApart = std::max (worstApart, apart);
    }

    MESSAGE ("across a jump, at the worst: apart for " << worstLength << " samples (the jump to " << worstAt << "), up to "
                                                       << probe::inDecibels (worstApart) << " dBFS");

    // Live, asleep through silence while the band's automation goes on, against the render, which never slept: a tone, 3 s of
    // silence, and noise.
    const auto sound = probe::join (probe::join (makeStereoSine (300.0, 9600, 0.3f, 0.3f), probe::makeSilence (144000)),
                                    probe::makeNoise (48000, 7, 0.3f));
    const auto playedLive = [&] (bool live)
    {
        probe::Device device (Eq::info);
        device.setAll (values);
        device.isRendering = ! live;
        device.prepare (testRate, 480);
        device.automate ((int) frequencyIndex, 400.0f, 1600.0f, sound.getNumSamples());
        auto out = sound;
        device.play (out, 480);
        return out;
    };

    CHECK (measureApart (playedLive (true), playedLive (false), 9600 + 144000, "a sleep").first <= 3 * linearLatency / 2);
}

TEST_CASE ("The EQ in Linear follows its knobs after a jump to a place alike on its grid of 32 samples but not on its own")
{
    // A jump at every place in one of the EQ's steps of 32 samples, back by a whole number of them but not of L / 2 (a 16-bar
    // loop from bar 2 at 44.1 kHz and 120 BPM is one): the EQ's step goes on, the linear phase's is cut short there, and its
    // next line must still come. Then a bell's Gain turned up by 12 dB: 2 L later, a sine through it is 12 dB louder.
    constexpr int jumpAt = 3 * linearLatency, turnAfter = 64, measureAt = turnAfter + 2 * linearLatency;
    constexpr int numSamples = jumpAt + 32 + measureAt + 1024;
    const auto flat = inLinear (withBands ({ { 3, Eq::BandType::bell, 1000.0f, 0.0f, 1.0f } }));
    const auto sine = makeStereoSine (1000.0, numSamples, 0.25f, 0.25f);
    const auto gainIndex = (size_t) Eq::getParameterIndex (3, Eq::gain);
    auto worst = 0.0;

    for (const auto back : { 32 * 37, 32 * 101 })
        for (int offset = 0; offset < 32; ++offset)
        {
            CAPTURE (back);
            CAPTURE (offset);
            REQUIRE (back % (linearLatency / 2) != 0);
            auto eq = makePrepared (flat, 32);
            auto out = sine;
            auto values = flat;
            const auto jumpSample = jumpAt + offset;

            for (int start = 0; start < numSamples;)
            {
                auto length = std::min (32, numSamples - start);

                if (start < jumpSample && start + length > jumpSample)
                    length = jumpSample - start;

                if (start >= jumpSample + turnAfter)
                    values[gainIndex] = 12.0f;

                float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
                devices::ProcessContext block;
                block.channels = channels;
                block.numChannels = 2;
                block.numSamples = length;
                block.parameters = values;
                block.isPlaying = true;
                block.songSample = start < jumpSample ? start : start - back;
                eq->process (block);
                start += length;
            }

            const auto measured = probe::inDecibels (probe::getToneLevel (out, 0, 1000.0, testRate, jumpSample + measureAt, 512) / 0.25);
            worst = std::max (worst, std::abs (measured - 12.0));
            CHECK (std::abs (measured - 12.0) < 0.1);
        }

    MESSAGE ("2 L after the turn, at most " << worst << " dB from the 12 dB turned up, over 64 jumps");
}

TEST_CASE ("The EQ in Linear follows its knobs in a loop shorter than its grid's step")
{
    // A loop of 1,000 samples going round, where its grid's step is L / 2 = 2,048. Without a line of the grid inside the loop
    // (from 100 to 1,100) no line ever comes, and a design done must come in at a wrap; with one inside it (from 1,900 to
    // 2,900, the line at 2,048) and a lane that holds the bell's Gain at 0 dB before the line and at 12 dB from it, each
    // wrap finds other values than the design the line started, and one design in two must still end. Either way a sine
    // through the bell is louder 6 L after the knob is turned (12 dB in the first case, the lane's 12 dB in the second).
    constexpr int numSamples = 8 * linearLatency, turnAt = linearLatency, loopLength = 1000;
    const auto flat = inLinear (withBands ({ { 3, Eq::BandType::bell, 1000.0f, 0.0f, 1.0f } }));
    const auto sine = makeStereoSine (1000.0, numSamples, 0.25f, 0.25f);
    const auto gainIndex = (size_t) Eq::getParameterIndex (3, Eq::gain);

    for (const auto loopStart : { 100, 1900 })
    {
        CAPTURE (loopStart);
        const auto hasLine = loopStart == 1900;
        auto eq = makePrepared (flat, 32);
        auto out = sine;
        auto values = flat;
        auto place = loopStart;

        for (int start = 0; start < numSamples;)
        {
            // Blocks end at the loop's end, as the engine's do where the song jumps, and here at the grid's line too, so that
            // the lane's step is there at the line.
            const auto lineAt = linearLatency / 2;
            const auto length = std::min ({ 32, numSamples - start, loopStart + loopLength - place, place < lineAt ? lineAt - place : 32 });

            if (start >= turnAt)
                values[gainIndex] = hasLine && place < linearLatency / 2 ? 0.0f : 12.0f;

            float* channels[] = { out.getWritePointer (0, start), out.getWritePointer (1, start) };
            devices::ProcessContext block;
            block.channels = channels;
            block.numChannels = 2;
            block.numSamples = length;
            block.parameters = values;
            block.isPlaying = true;
            block.songSample = place;
            eq->process (block);
            start += length;
            place += length;

            if (place >= loopStart + loopLength)
                place = loopStart;
        }

        const auto measured = probe::inDecibels (probe::getToneLevel (out, 0, 1000.0, testRate, turnAt + 6 * linearLatency, 512) / 0.25);
        CAPTURE (measured);

        if (hasLine)
            CHECK (measured > 6.0); // it has left 0 dB for the lane's 12 dB, or is on its way between them
        else
            CHECK (std::abs (measured - 12.0) < 0.1);
    }
}

TEST_CASE ("The EQ in Linear plays each way through it as its response says, and a band on one leaves the other to the bit")
{
    // A bell on each placement in turn: a sine alike on both sides (the middle), opposite on them (the side), on the left
    // alone and on the right alone, against the response for that way, from 3 L on.
    struct Way
    {
        Placement way;
        float left, right;
    };

    constexpr int numSamples = 3 * linearLatency + 9600;
    const Way ways[] = { { Placement::mid, 0.25f, 0.25f },
                         { Placement::side, 0.25f, -0.25f },
                         { Placement::left, 0.25f, 0.0f },
                         { Placement::right, 0.0f, 0.25f } };

    for (const auto placement : { Placement::mid, Placement::side, Placement::left, Placement::right })
    {
        CAPTURE ((int) placement);
        const auto values = inLinear (withBands ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1, placement } }));
        const auto isMidSide = placement == Placement::mid || placement == Placement::side;

        for (const auto& way : ways)
        {
            // The middle and the side for a band on one of them; the left and the right for a band on one of those.
            if (isMidSide != (way.way == Placement::mid || way.way == Placement::side))
                continue;

            CAPTURE ((int) way.way);
            const auto channel = way.way == Placement::right ? 1 : 0;

            for (const auto hertz : { 300.0, 1000.0, 3000.0 })
            {
                CAPTURE (hertz);
                const auto in = makeStereoSine (hertz, numSamples, way.left, way.right);
                auto out = in;
                auto current = values;
                playEq (*makePrepared (values), out, 512, current);
                const auto measured = probe::inDecibels (probe::getToneLevel (out, channel, hertz, testRate, 3 * linearLatency, 9600)
                                                         / probe::getToneLevel (in, channel, hertz, testRate, 3 * linearLatency, 9600));
                CHECK (std::abs (measured - 20.0 * std::log10 (Eq::getMagnitude (values, hertz, testRate, way.way))) < 0.1);
            }
        }
    }

    // A band on the side leaves noise alike on both sides as it came, L later, to the bit; one on the middle, noise opposite on
    // them; one on the left, the right; one on the right, the left.
    auto alike = probe::makeNoise (3 * linearLatency, 21, 0.5f), opposite = alike, oneSide = probe::makeNoise (3 * linearLatency, 22, 0.5f);

    for (int i = 0; i < alike.getNumSamples(); ++i)
    {
        alike.setSample (1, i, alike.getSample (0, i));
        opposite.setSample (1, i, -opposite.getSample (0, i));
    }

    struct Untouched
    {
        Placement placement;
        const probe::Sound* sound;
        std::vector<int> channels; // those left as they came
    };

    for (const auto& [placement, sound, channels] :
         { Untouched { Placement::side, &alike, { 0, 1 } }, Untouched { Placement::mid, &opposite, { 0, 1 } },
           Untouched { Placement::left, &oneSide, { 1 } }, Untouched { Placement::right, &oneSide, { 0 } } })
    {
        CAPTURE ((int) placement);
        const auto values = inLinear (withBands ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1, placement } }));
        auto out = *sound;
        auto current = values;
        playEq (*makePrepared (values), out, 512, current);

        for (const auto channel : channels)
        {
            CAPTURE (channel);
            auto differences = 0;

            for (int i = 0; i < out.getNumSamples(); ++i)
                if (! juce::exactlyEqual (out.getSample (channel, i),
                                          i < linearLatency ? 0.0f : sound->getSample (channel, i - linearLatency)))
                    ++differences;

            CHECK (differences == 0);
        }
    }
}

TEST_CASE ("The EQ as a de-esser in Linear dips as deep as in Minimum, on the burst as it comes out")
{
    // Part A's de-esser case: De-Ess's band, a 7 kHz burst 20 dB over quiet noise. In Linear its shape at its Gain (0 dB) is in
    // the kernels and what its Dynamic moves plays after them, its ear hearing what came in as late as they play it.
    constexpr int burstFrom = 24000, burstTo = 48000, numSamples = 72000 + linearLatency, block = 48;
    std::vector<float> values;

    for (const auto& [name, preset] : getPresetValues())
        if (name == juce::File::addTrailingSeparator ("Dynamic") + "De-Ess.phazitepreset")
            values = preset;

    REQUIRE (values.size() == Eq::info.parameters.size());
    auto in = probe::makeNoise (numSamples, 17, 0.01f);
    const auto burst = makeStereoSine (7000.0, numSamples, 0.25f, 0.25f);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = burstFrom; i < burstTo; ++i)
            in.addSample (channel, i, burst.getSample (channel, i));

    struct Played
    {
        probe::Sound out;
        float deepest = 0.0f;
        int deepestAt = 0; // from where the burst comes out
        double dip = 0.0;  // the burst's level out against in, in its middle, in decibels
    };

    const auto through = [&] (bool isLinear)
    {
        const auto late = isLinear ? linearLatency : 0;
        auto eq = makePrepared (isLinear ? inLinear (values) : values, block);
        Played played { in };
        auto current = isLinear ? inLinear (values) : values;
        playEq (*eq, played.out, block, current, {},
                [&] (int end)
                {
                    if (const auto moved = eq->getGainMoved (5); moved < played.deepest)
                    {
                        played.deepest = moved;
                        played.deepestAt = end - late - burstFrom;
                    }
                });

        played.dip = probe::inDecibels (probe::getToneLevel (played.out, 0, 7000.0, testRate, burstFrom + late + 9600, 9600)
                                        / probe::getToneLevel (in, 0, 7000.0, testRate, burstFrom + 9600, 9600));
        return played;
    };

    const auto minimum = through (false), linear = through (true);
    MESSAGE ("Minimum: its band down " << minimum.deepest << " dB at most, " << minimum.deepestAt << " samples into the burst, the burst "
                                       << minimum.dip << " dB; Linear: " << linear.deepest << " dB, " << linear.deepestAt
                                       << " samples into it as it comes out, the burst " << linear.dip << " dB");
    CHECK (std::abs (linear.dip - minimum.dip) < 0.5);
    CHECK (std::abs (linear.deepest - minimum.deepest) < 0.5f);
    CHECK (std::abs (linear.deepestAt - minimum.deepestAt) <= (int) (0.003 * testRate));

    // Before it, the noise as it came, only later.
    CHECK (probe::getLargestDifference (probe::getPart (linear.out, linearLatency, burstFrom), probe::getPart (in, 0, burstFrom)) < 1.0e-5);
}

TEST_CASE ("The EQ in Linear's cost, counted: multiply-adds a sample for each set of kernels, and a design's work spread evenly")
{
    const auto prepared = [] (std::initializer_list<BandSetting> bands) { return makePrepared (inLinear (withBands (bands)), 1); };

    // One kernel for both sides; one on the middle, the side a delay alone (a band on Mid alone); two (the sides' and the
    // crossings', bands on Mid and on Side); four. Each kernel a side's sum takes is 31 partitions of 257 complex points, four
    // real multiply-adds each, every 256 samples; beside them, an FFT of 512 points of each side in and one of each side out
    // every 256 samples.
    const auto one = prepared ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f } });
    const auto middle = prepared ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1, Placement::mid } });
    const auto two = prepared ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1, Placement::mid },
                                 { 5, Eq::BandType::bell, 3000.0f, 3.0f, 1.0f, 1, Placement::side } });
    const auto four = prepared ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1, Placement::left },
                                  { 5, Eq::BandType::bell, 3000.0f, 3.0f, 1.0f, 1, Placement::mid } });
    const auto none = prepared ({});
    const auto perKernel = (4.0 * 31 * 257) / 256.0;
    MESSAGE ("multiply-adds a sample: one kernel " << one->getLinearPhase().getMultiplyAddsPerSample (2) << ", one on the middle "
                                                   << middle->getLinearPhase().getMultiplyAddsPerSample (2) << ", two "
                                                   << two->getLinearPhase().getMultiplyAddsPerSample (2) << ", four "
                                                   << four->getLinearPhase().getMultiplyAddsPerSample (2) << ", every band flat "
                                                   << none->getLinearPhase().getMultiplyAddsPerSample (2));
    CHECK (one->getLinearPhase().getNumKernelsPlayed() == 1);
    CHECK (middle->getLinearPhase().getNumKernelsPlayed() == 1);
    CHECK (two->getLinearPhase().getNumKernelsPlayed() == 2);
    CHECK (four->getLinearPhase().getNumKernelsPlayed() == 4);
    CHECK (none->getLinearPhase().getNumKernelsPlayed() == 0); // a delay alone
    CHECK (one->getLinearPhase().getMultiplyAddsPerSample (2) == (int) std::ceil (2.0 * perKernel));
    CHECK (middle->getLinearPhase().getMultiplyAddsPerSample (2) == (int) std::ceil (perKernel + 2.0 * 257 / 256.0));
    CHECK (two->getLinearPhase().getMultiplyAddsPerSample (2) == (int) std::ceil (4.0 * perKernel));
    CHECK (four->getLinearPhase().getMultiplyAddsPerSample (2) == (int) std::ceil (4.0 * perKernel));
    CHECK (none->getLinearPhase().getMultiplyAddsPerSample (2) <= 5);

    // A design: so much work, spread over the L / 2 samples after the line it starts on, none of a sample's share more than a
    // 64th of it, played a sample at a time; and nothing designed while nothing changes.
    for (const auto& [name, bands] :
         { std::pair { "eight bands, every slope", withBands ({ { 0, Eq::BandType::lowCut, 30.0f, 0.0f, 0.71f, 3 },
                                                                { 1, Eq::BandType::lowShelf, 100.0f, 3.0f, 0.71f },
                                                                { 2, Eq::BandType::bell, 250.0f, -2.0f, 1.0f },
                                                                { 3, Eq::BandType::bell, 800.0f, 2.0f, 1.0f },
                                                                { 4, Eq::BandType::notch, 2500.0f, 0.0f, 4.0f },
                                                                { 5, Eq::BandType::bell, 6000.0f, 1.0f, 1.0f },
                                                                { 6, Eq::BandType::highShelf, 10000.0f, -2.0f, 0.71f },
                                                                { 7, Eq::BandType::highCut, 18000.0f, 0.0f, 0.71f, 2 } }) },
           std::pair { "two bands, one on the left", withBands ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f, 1, Placement::left },
                                                                  { 5, Eq::BandType::bell, 3000.0f, 3.0f, 1.0f, 1, Placement::mid } }) } })
    {
        CAPTURE (name);
        auto values = inLinear (bands);
        auto eq = makePrepared (values, 1);
        auto silence = probe::makeSilence (3 * linearLatency);
        auto worked = eq->getLinearPhase().getWorkDone();
        std::int64_t largestShare = 0, total = 0;
        auto designs = 0;
        playEq (
            *eq, silence, 1, values,
            [&] (int start)
            {
                if (start == linearLatency / 2)
                    values[(size_t) Eq::getParameterIndex (3, Eq::gain)] = 4.0f;
            },
            [&] (int)
            {
                const auto now = eq->getLinearPhase().getWorkDone();
                largestShare = std::max (largestShare, now - worked);
                total += now - worked;
                worked = now;
                designs = eq->getLinearPhase().getDesignsStarted();
            });

        const auto cost = eq->getLinearPhase().getDesignCost();
        MESSAGE (std::string (name) << ": a design's work " << cost << " units, done " << total << ", its largest share in a sample "
                                    << largestShare << " (a " << (double) cost / (double) largestShare << "th)");
        CHECK (designs == 1);
        CHECK (total <= cost);
        CHECK (largestShare * 64 <= cost);
    }
}

// A measure of time: with the benchmark gates, which run nightly on a quiet machine, not in ctest.
TEST_CASE ("Benchmark gate: the EQ in Linear against Minimum, eight bands on, standing still and with a band automated"
           * doctest::test_suite ("benchmark-gate") * doctest::skip())
{
    const auto values = withBands ({ { 0, Eq::BandType::lowCut, 30.0f, 0.0f, 0.71f, 1 },
                                     { 1, Eq::BandType::lowShelf, 100.0f, 2.0f, 0.71f },
                                     { 2, Eq::BandType::bell, 250.0f, -2.0f, 1.0f },
                                     { 3, Eq::BandType::bell, 800.0f, 2.0f, 1.0f },
                                     { 4, Eq::BandType::bell, 2500.0f, -1.0f, 1.0f },
                                     { 5, Eq::BandType::bell, 6000.0f, 1.0f, 1.0f },
                                     { 6, Eq::BandType::highShelf, 10000.0f, 2.0f, 0.71f },
                                     { 7, Eq::BandType::highCut, 18000.0f, 0.0f, 0.71f } });
    const auto noise = probe::makeNoise (24000, 3, 0.25f);

    for (const auto automated : { false, true })
    {
        std::array<double, 2> seconds {};

        for (const auto isLinear : { false, true })
        {
            auto current = isLinear ? inLinear (values) : values;
            auto eq = makePrepared (current, 512);
            const auto start = std::chrono::steady_clock::now();

            for (int turn = 0; turn < 40; ++turn)
            {
                auto buffer = noise;
                playEq (*eq, buffer, 512, current,
                        [&] (int at)
                        {
                            if (automated)
                                current[(size_t) Eq::getParameterIndex (3, Eq::frequency)] =
                                    (float) (800.0 * std::pow (2.0, std::sin (twoPi * (turn * 24000 + at) / 96000.0)));
                        });
            }

            seconds[isLinear ? 1 : 0] = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        }

        MESSAGE ("20 s at 48 kHz, eight bands" << std::string (automated ? ", one automated" : ", standing still") << ": Minimum in "
                                               << seconds[0] << " s, Linear in " << seconds[1] << " s, " << seconds[1] / seconds[0]
                                               << " times as long");
        CHECK (std::isfinite (seconds[1] / seconds[0]));
    }
}

TEST_CASE ("The EQ in Linear after a reset: kernels that differ from the values come in whole L / 2 later, which costs the far pre-ring")
{
    // Prepared with every band off, played at once with the steepest low cut on full-scale noise that starts at the reset,
    // against an EQ prepared with the cut: until the cut's kernels are in, only their leading half of what came in can sound.
    auto off = getDefaults();

    for (int band = 0; band < Eq::numBands; ++band)
        set (off, band, Eq::on, 0.0f);

    for (const auto frequency : { 30.0f, 100.0f, 1000.0f })
    {
        CAPTURE (frequency);
        auto cut = inLinear (withBands ({ { 0, Eq::BandType::lowCut, frequency, 0.0f, 0.71f, 3 } }, off));
        const auto noise = probe::makeNoise (3 * linearLatency, 11, 1.0f);
        auto late = noise, right = noise;
        auto values = cut;
        playEq (*makePrepared (inLinear (off)), late, 512, values);
        playEq (*makePrepared (cut), right, 512, values);
        const auto before = probe::getLargestDifference (late, right, 0, linearLatency / 2);
        const auto after = probe::getLargestDifference (late, right, linearLatency / 2);
        MESSAGE ("a 48 dB low cut at " << frequency << " Hz: " << probe::inDecibels (before)
                                       << " dBFS at most from the cut's own sound in the first L / 2 samples, " << after << " after");
        CHECK (before < 0.002); // -54 dBFS; measured -58 dBFS at 30 Hz, -92 at 100 Hz
        CHECK (after < 1.0e-5);
    }
}

TEST_CASE ("A dynamic EQ band with a Gain is a hair off Minimum's curve in Linear while it moves: two bells in a row, not one")
{
    // In Linear its Gain is in the kernels and its moved amount plays after them as a bell of its own; Minimum plays one bell of
    // their sum. A bell at 1 kHz with a Gain of +6 dB moved 8 dB down, its Threshold far under the sine through it: the sine's
    // level through each mode, Linear's L later, once the band has moved all the way.
    constexpr int numSamples = 24000 + linearLatency, from = 12000, length = 12000;

    for (const auto q : { 0.7f, 1.0f, 4.0f })
    {
        CAPTURE (q);
        auto values = withBands ({ { 3, Eq::BandType::bell, 1000.0f, 6.0f, q } });
        values[(size_t) Eq::getParameterIndex (3, Eq::dynamic)] = -8.0f;
        values[(size_t) Eq::getParameterIndex (3, Eq::threshold)] = -60.0f;
        auto largest = 0.0;

        for (const auto hertz : { 400.0, 700.0, 900.0, 1000.0, 1150.0, 1500.0, 2500.0 })
        {
            CAPTURE (hertz);
            const auto in = makeStereoSine (hertz, numSamples, 0.25f, 0.25f);
            const auto levelThrough = [&] (bool isLinear)
            {
                auto current = isLinear ? inLinear (values) : values;
                auto out = in;
                playEq (*makePrepared (current, 480), out, 480, current);
                const auto late = isLinear ? linearLatency : 0;
                return probe::inDecibels (probe::getToneLevel (out, 0, hertz, testRate, from + late, length)
                                          / probe::getToneLevel (in, 0, hertz, testRate, from, length));
            };

            const auto minimum = levelThrough (false), linear = levelThrough (true);
            largest = std::max (largest, std::abs (linear - minimum));

            if (juce::exactlyEqual (hertz, 1000.0))
                CHECK (std::abs (minimum + 2.0) < 0.05); // moved all the way: one bell of the sum, +6 - 8 dB
        }

        MESSAGE ("Q " << q << ": a sine through Linear at most " << largest << " dB from one through Minimum");
        CHECK (largest < 0.1); // the curves alone: 0.07 dB apart at most
    }
}

TEST_CASE ("On a mono track, the EQ in Linear plays one kernel, of the bands that play on the channel in Minimum")
{
    // A bell on Stereo, a low shelf on Mid and a high shelf on Left play on a mono channel; a bell on Side and a notch on Right
    // don't. After L of silence (its kernels for one channel designed and put in), an impulse.
    const auto values = inLinear (withBands ({ { 2, Eq::BandType::bell, 1000.0f, 6.0f, 1.0f },
                                               { 1, Eq::BandType::lowShelf, 200.0f, -4.0f, 0.71f, 1, Placement::mid },
                                               { 6, Eq::BandType::highShelf, 6000.0f, 3.0f, 0.71f, 1, Placement::left },
                                               { 4, Eq::BandType::bell, 3000.0f, -9.0f, 2.0f, 1, Placement::side },
                                               { 5, Eq::BandType::notch, 500.0f, 0.0f, 2.0f, 1, Placement::right } }));
    auto eq = makePrepared (values);
    auto sound = probe::makeSilence (3 * linearLatency + 64, 1);
    sound.setSample (0, linearLatency, 1.0f);
    auto current = values;
    playEq (*eq, sound, 512, current);
    std::vector<double> response;

    for (int i = linearLatency; i < sound.getNumSamples(); ++i)
        response.push_back (sound.getSample (0, i));

    CHECK (eq->getLinearPhase().getNumKernelsPlayed() == 1);
    auto largest = 0.0;

    for (const auto f : getFrequencies (12, 200.0))
    {
        auto expected = 1.0;

        for (const auto band : { 2, 1, 6 })
            expected *= Eq::getBandShape (values, band, testRate).getMagnitude (f, testRate);

        largest = std::max (largest, std::abs (responseDecibels (response, f) - 20.0 * std::log10 (expected)));
    }

    MESSAGE ("on a mono track: at most " << largest << " dB from the bands that play on it");
    CHECK (largest < 0.05);
}
