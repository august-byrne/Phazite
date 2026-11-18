// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices of roadmap step 2.10, as DSP (src/devices/): the synth's unison. The filter's have a file of their own
// (FilterTests.cpp), as the delay's do (DelayTests.cpp), Stereo's (StereoTests.cpp), the phaser's and the flanger's
// (MotionEffectTests.cpp), the multiband's (MultibandTests.cpp) and the saturator's (SaturatorTests.cpp).
#include "MotionTestHelpers.h"
#include "TestHelpers.h"
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
} // namespace

TEST_CASE ("The devices of step 2.10 describe themselves, with type and parameter IDs of their own")
{
    std::set<std::string_view> types;

    for (const auto* info : { &devices::Saturator::info, &devices::FilterEffect::info, &devices::Stereo::info, &devices::Multiband::info,
                              &devices::Synth::info })
    {
        INFO (std::string (info->typeID));
        CHECK (devices::isValid (*info));
        CHECK (types.insert (info->typeID).second);
    }

    // The synth's unison plays one voice at first.
    CHECK (devices::Synth::info.parameters[devices::Synth::unisonVoices].defaultValue == doctest::Approx (0.0f));

    // Stereo changes nothing until turned, and has no mix; nor has the multiband, whose LMH Mix blends its bands
    // with the dry sound before its master band, so that the master still limits what comes out.
    CHECK (devices::findParameter (devices::Stereo::info, devices::mixParameterID) < 0);
    CHECK (devices::findParameter (devices::Multiband::info, devices::mixParameterID) < 0);
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
