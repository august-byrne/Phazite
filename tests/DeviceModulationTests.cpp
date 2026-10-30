// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's devices moving their modulated parameters themselves (roadmap A5, part 4; model/devices/internal/
// DeviceModulation.h): an LFO's waves as Tracktion's LFO makes them, followed within each block from where Tracktion's
// LFO is as the block starts, so that a device plays the same at any block size; a modulator switched off moving
// nothing; and what modulates each device kept up to date as modulation changes.
#include "TestDevices.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/modulation/Modulation.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace tracktion::literals;

namespace
{
namespace devices = phazite::devices;
using phazite::test::TestGain;

constexpr double testRate = 48000.0;

te::PluginRenderContext makeContext (juce::AudioBuffer<float>& buffer, te::TimePosition start)
{
    const auto length = te::TimeDuration::fromSamples (buffer.getNumSamples(), testRate);
    return { &buffer, 0, buffer.getNumSamples(), nullptr, 0.0, te::TimeRange (start, length), true, false, false, false };
}

te::LFOModifier& getLfo (const phazite::ModulatorAccess& modulator)
{
    auto* lfo = dynamic_cast<te::LFOModifier*> (modulator.modifier.get());
    REQUIRE (lfo != nullptr);
    return *lfo;
}

void set (te::AutomatableParameter& parameter, float value)
{
    parameter.setParameter (value, juce::sendNotification);
}

// The test gain, moved by a tenth of its range by a square LFO at 10 Hz from 0 to 1, its cycle running free from its
// start: 0 dB for half of each cycle, and 7.2 dB for the other half.
struct Modulated
{
    Modulated()
    {
        device = &phazite::test::createTestDevice (*edit, TestGain::typeID, keep);
        lfo = phazite::addModulator (*te::getAudioTracks (*edit)[0], phazite::ModulatorType::lfo);
        REQUIRE (lfo.has_value());

        auto& wave = getLfo (*lfo);
        set (*wave.waveParam, (float) devices::LfoWave::square);
        set (*wave.syncTypeParam, (float) te::ModifierCommon::free);
        set (*wave.rateTypeParam, (float) te::ModifierCommon::hertz);
        set (*wave.rateParam, 10.0f);
        set (*wave.depthParam, 1.0f);
        set (*wave.offsetParam, 0.0f);
        set (*wave.bipolarParam, 0.0f);
        set (*wave.phaseParam, 0.0f);
        phazite::setModulation (*lfo, *device->findParameter ("gain"), 0.1f);
    }

    // The gain each sample gets over half a second, played on a constant 1 in blocks of a size.
    std::vector<float> play (int blockSize)
    {
        getLfo (*lfo).baseClassInitialise (testRate, blockSize);
        device->baseClassInitialise ({ 0_tp, testRate, blockSize });
        std::vector<float> gains;
        juce::AudioBuffer<float> buffer (2, blockSize);

        for (int done = 0; done < (int) (0.5 * testRate); done += blockSize)
        {
            const auto start = te::TimePosition::fromSamples (done, testRate);
            edit->updateModifierTimers (start, blockSize); // as playback moves the modulators on as each block starts

            for (int channel = 0; channel < 2; ++channel)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), 1.0f, blockSize);

            device->applyToBufferWithAutomation (makeContext (buffer, start)); // as playback moves its parameters on too

            for (int i = 0; i < blockSize; ++i)
                gains.push_back (buffer.getSample (0, i));
        }

        device->baseClassDeinitialise();
        getLfo (*lfo).baseClassDeinitialise();
        return gains;
    }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    phazite::DevicePlugin* device = nullptr;
    std::optional<phazite::ModulatorAccess> lfo;
};

const auto high = juce::Decibels::decibelsToGain (7.2f);
} // namespace

TEST_CASE ("Phazite's LFO waves are Tracktion's: the same value at each point of the cycle, whatever its shape")
{
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    const auto modulator = phazite::addModulator (*te::getAudioTracks (*edit)[0], phazite::ModulatorType::lfo);
    REQUIRE (modulator.has_value());
    auto& lfo = getLfo (*modulator);

    // Following the song's position, a sixteenth a cycle, so that where the cycle is follows from the time alone.
    set (*lfo.syncTypeParam, (float) te::ModifierCommon::transport);
    set (*lfo.rateTypeParam, (float) te::ModifierCommon::sixteenth);
    set (*lfo.rateParam, 1.0f);
    set (*lfo.depthParam, 0.8f);
    set (*lfo.offsetParam, 0.1f);

    for (int wave = 0; wave <= (int) devices::LfoWave::eightStepsDown; ++wave)
    {
        for (const auto bipolar : { false, true })
        {
            CAPTURE (wave);
            CAPTURE (bipolar);
            set (*lfo.waveParam, (float) wave);
            set (*lfo.bipolarParam, bipolar ? 1.0f : 0.0f);
            const devices::LfoShape shape { (devices::LfoWave) wave, 0.8f, 0.1f, bipolar };
            CHECK (devices::isPeriodic (shape.wave));

            for (int step = 0; step < 37; ++step)
            {
                edit->updateModifierTimers (te::TimePosition::fromSeconds (step * 0.0137), 512);
                CHECK (lfo.getCurrentValue() == doctest::Approx (devices::getLfoValue (shape, lfo.getCurrentPhase())).epsilon (1.0e-4));
            }
        }
    }

    // Random values and noise follow the LFO's own random numbers.
    CHECK_FALSE (devices::isPeriodic (devices::LfoWave::random));
    CHECK_FALSE (devices::isPeriodic (devices::LfoWave::noise));
    CHECK (devices::movePhase (0.75f, 0.5) == doctest::Approx (0.25f));
    CHECK (devices::movePhase (0.25f, -0.5) == doctest::Approx (0.75f));
}

TEST_CASE ("Modulation moves a parameter in its control's range, and keeps it to its range and steps")
{
    const devices::ParameterInfo gain { .id = "gain",
                                        .name = "Gain",
                                        .minimum = -60.0f,
                                        .maximum = 12.0f,
                                        .unit = devices::Unit::decibels };
    const devices::ParameterInfo cutoff { .id = "cutoff",
                                          .name = "Cutoff",
                                          .minimum = 20.0f,
                                          .maximum = 20000.0f,
                                          .defaultValue = 1000.0f,
                                          .unit = devices::Unit::hertz,
                                          .midpoint = 1000.0f };

    // A tenth of the gain's 72 dB, either way; never past its ends.
    CHECK (devices::modulate (gain, devices::getRange (gain), 0.0f, 0.1f) == doctest::Approx (7.2f));
    CHECK (devices::modulate (gain, devices::getRange (gain), 0.0f, -0.1f) == doctest::Approx (-7.2f));
    CHECK (devices::modulate (gain, devices::getRange (gain), 0.0f, 0.5f) == doctest::Approx (12.0f));
    CHECK (devices::modulate (gain, devices::getRange (gain), 0.0f, 0.0f) == doctest::Approx (0.0f));

    // A skewed range is moved as its control spans it: from its middle (1 kHz), a quarter up is halfway to the top on the
    // knob, not on the hertz.
    const auto range = devices::getRange (cutoff);
    const auto moved = devices::modulate (cutoff, range, 1000.0f, 0.25f);
    CHECK (range.convertTo0to1 (moved) == doctest::Approx (0.75f));
    CHECK (moved < 10010.0f);
}

TEST_CASE ("A device follows an LFO through each block, so that it plays the same at any block size")
{
    // Each from its LFO's start.
    Modulated first, second;
    phazite::updateDeviceModulation (*first.device);
    phazite::updateDeviceModulation (*second.device);
    REQUIRE (first.device->getModulation() != nullptr);

    const auto small = first.play (128);
    const auto large = second.play (2048);
    const auto length = (size_t) (0.5 * testRate);
    REQUIRE (small.size() >= length);
    REQUIRE (large.size() >= length);

    // High for the first half of each 4800-sample cycle and 0 dB for the other, each change within 32 samples of where
    // the LFO turns, even in the middle of a block of 2048.
    CHECK (large[0] == doctest::Approx (high).epsilon (0.001));
    CHECK (large[2399] == doctest::Approx (high).epsilon (0.001));
    CHECK (large[2400] == doctest::Approx (1.0f).epsilon (0.001));
    CHECK (large[4799] == doctest::Approx (1.0f).epsilon (0.001));
    CHECK (large[4800] == doctest::Approx (high).epsilon (0.001));

    auto differences = 0;

    for (size_t i = 0; i < length; ++i)
        if (std::abs (small[i] - large[i]) > 0.001f)
            ++differences;

    CHECK (differences == 0);
}

TEST_CASE ("A modulator switched off moves nothing, and a device without modulation of its own keeps Tracktion's")
{
    Modulated off;
    phazite::updateDeviceModulation (*off.device);
    phazite::setModulatorOn (*off.lfo, false);

    const auto unmoved = off.play (512);
    CHECK (std::all_of (unmoved.begin(), unmoved.end(), [] (float g) { return std::abs (g - 1.0f) < 1.0e-4f; }));

    // Without a plan of its own, the device plays the value Tracktion's modulation gives it, once a block: high as the
    // blocks start in the LFO's first half, then gliding down over the block that starts in its second.
    Modulated tracktions;
    const auto gains = tracktions.play (512);
    CHECK (gains[0] == doctest::Approx (high).epsilon (0.001));
    CHECK (std::any_of (gains.begin(), gains.end(), [] (float g) { return g < 0.999f * high; }));
}

TEST_CASE ("What modulates each of the project's devices follows its modulation as it changes, and stays as it is otherwise")
{
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    te::Plugin::Ptr keep;
    auto& device = phazite::test::createTestDevice (*edit, TestGain::typeID, keep);
    track->pluginList.insertPlugin (keep, 0, nullptr);
    auto& gain = *device.findParameter ("gain");

    phazite::DeviceModulationKeeper keeper (*edit);
    CHECK (device.getModulation() == nullptr);

    const auto lfo = phazite::addModulator (*track, phazite::ModulatorType::lfo);
    const auto macro = phazite::addModulator (*track, phazite::ModulatorType::macro);
    REQUIRE (lfo.has_value());
    REQUIRE (macro.has_value());
    phazite::setModulation (*lfo, gain, 0.25f);
    phazite::setModulation (*macro, gain, -0.5f);
    phazite::test::dispatchPendingMessages();

    const auto* first = device.getModulation();
    REQUIRE (first != nullptr);
    REQUIRE (first->routes.size() == 2);
    REQUIRE (first->sources.size() == 2);
    CHECK (first->isModulated[0]);
    CHECK_FALSE (first->isModulated[1]); // the mode
    CHECK (first->sources[(size_t) first->routes[0].source].lfo != nullptr);
    CHECK (first->sources[(size_t) first->routes[1].source].macro != nullptr);
    CHECK (first->routes[1].amount == doctest::Approx (-0.5f));

    // Asked again with nothing changed, it plays on as it was.
    phazite::updateDeviceModulation (*edit);
    CHECK (device.getModulation() == first);

    // A new amount; then none, once nothing modulates it.
    phazite::setModulation (*lfo, gain, -0.75f);
    phazite::test::dispatchPendingMessages();
    REQUIRE (device.getModulation() != nullptr);
    CHECK (device.getModulation()->routes[0].amount == doctest::Approx (-0.75f));

    phazite::removeModulation (*lfo, gain);
    phazite::removeModulation (*macro, gain);
    phazite::test::dispatchPendingMessages();
    CHECK (device.getModulation() == nullptr);
}
