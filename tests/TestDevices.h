// SPDX-License-Identifier: AGPL-3.0-or-later
// Devices made for tests, which songs hold and Phazite's engine plays as its own (registered as such:
// model/devices/internal/Devices.h): an effect (a gain), an instrument that plays the velocity of the note it holds, an
// effect with latency and a mix (a delay), an effect that plays its sidechain, and one that takes a share of the time.
#pragma once

#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Mixer.h"

#include <array>
#include <vector>

namespace phazite::test
{
inline constexpr std::string_view testGainModes[] = { "Normal", "Inverted" };

inline constexpr devices::ParameterInfo testGainParameters[] = {
    { .id = "gain", .name = "Gain", .minimum = -60.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = devices::Unit::decibels },
    { .id = "mode",
      .name = "Mode",
      .minimum = 0.0f,
      .maximum = 1.0f,
      .defaultValue = 0.0f,
      .unit = devices::Unit::choice,
      .choices = testGainModes },
};

// Scales the sound by its gain, upside down when inverted; 64 samples of latency and a 1.5 s tail, as if it had them.
struct TestGain final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestGain";
    static constexpr devices::DeviceInfo info { .typeID = typeID,
                                                .name = "Test Gain",
                                                .kind = devices::DeviceKind::effect,
                                                .parameters = testGainParameters };

    void prepare (double, int) override { prepared = true; }
    void reset() override {}

    void process (const devices::ProcessContext& block) override
    {
        const auto gain = juce::Decibels::decibelsToGain (block.parameters[0]) * (block.parameters[1] > 0.5f ? -1.0f : 1.0f);

        for (int channel = 0; channel < block.numChannels; ++channel)
            for (int i = 0; i < block.numSamples; ++i)
                block.channels[channel][i] *= gain;

        lastValues.assign (block.parameters.begin(), block.parameters.end());
        lastNumChannels = block.numChannels;
        beats[(size_t) processed % beats.size()] = block.beat;
        ++processed;
    }

    int getLatencySamples() const override { return 64; }
    double getTailSeconds() const override { return 1.5; }

    bool prepared = false;
    std::vector<float> lastValues;
    int lastNumChannels = 0;
    int processed = 0;               // stretches
    std::array<double, 64> beats {}; // where the song was as each stretch began, the last 64 (by processed, round)
};

inline constexpr devices::ParameterInfo testToneParameters[] = {
    { .id = "level", .name = "Level", .minimum = 0.0f, .maximum = 1.0f, .defaultValue = 1.0f, .unit = devices::Unit::percent },
};

// Plays the velocity of the note it holds, and keeps every event it was given.
struct TestTone final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestTone";
    static constexpr devices::DeviceInfo info { .typeID = typeID,
                                                .name = "Test Tone",
                                                .kind = devices::DeviceKind::instrument,
                                                .parameters = testToneParameters };

    void prepare (double, int) override {}
    void reset() override { held = 0.0f; }

    void process (const devices::ProcessContext& block) override
    {
        ++processed;
        received.insert (received.end(), block.events.begin(), block.events.end());
        size_t next = 0;

        for (int i = 0; i < block.numSamples; ++i)
        {
            for (; next < block.events.size() && block.events[next].sample <= i; ++next)
            {
                const auto& event = block.events[next];

                if (event.type == devices::NoteEvent::Type::noteOn)
                    held = event.value;
                else if (event.type == devices::NoteEvent::Type::noteOff || event.type == devices::NoteEvent::Type::allNotesOff)
                    held = 0.0f;
            }

            block.channels[0][i] += held;
            block.channels[1][i] += held;
        }
    }

    float held = 0.0f;
    std::vector<devices::NoteEvent> received;
    int processed = 0; // stretches
};

inline constexpr devices::ParameterInfo testDelayParameters[] = {
    { .id = devices::mixParameterID, .name = "Mix", .defaultValue = 1.0f, .unit = devices::Unit::percent },
};

// Puts out what comes in 64 samples late and upside down, and says so; its plug-in applies its mix.
struct TestDelay final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestDelay";
    static constexpr int latency = 64;
    static constexpr devices::DeviceInfo info { .typeID = typeID,
                                                .name = "Test Delay",
                                                .kind = devices::DeviceKind::effect,
                                                .parameters = testDelayParameters };

    void prepare (double, int) override
    {
        for (auto& line : lines)
            line.prepare (latency);
    }

    void reset() override
    {
        for (auto& line : lines)
            line.reset();
    }

    void process (const devices::ProcessContext& block) override
    {
        for (int channel = 0; channel < block.numChannels; ++channel)
        {
            for (int i = 0; i < block.numSamples; ++i)
            {
                lines[(size_t) channel].push (block.channels[channel][i]);
                block.channels[channel][i] = -lines[(size_t) channel].read (latency);
            }
        }
    }

    int getLatencySamples() const override { return latency; }

    std::array<devices::dsp::DelayLine, 2> lines;
};

// Puts out its sidechain's sound in place of what comes in (silence without one), so a render shows what reaches it.
struct TestSidechain final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestSidechain";
    static constexpr devices::DeviceInfo info { .typeID = typeID,
                                                .name = "Test Sidechain",
                                                .kind = devices::DeviceKind::effect,
                                                .hasSidechain = true };

    void prepare (double, int) override {}
    void reset() override {}

    void process (const devices::ProcessContext& block) override
    {
        for (int channel = 0; channel < block.numChannels; ++channel)
            for (int i = 0; i < block.numSamples; ++i)
                block.channels[channel][i] = channel < block.numSidechainChannels ? block.sidechain[channel][i] : 0.0f;
    }
};

// Takes a fifth of the time of what it plays, as a device busy working out its sound would, and leaves the sound as it
// is; it never sleeps on silence. For the CPU meters.
struct TestBusy final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestBusy";
    static constexpr double share = 0.2;
    static constexpr devices::DeviceInfo info { .typeID = typeID, .name = "Test Busy", .kind = devices::DeviceKind::effect };

    void prepare (double rate, int) override { sampleRate = rate; }
    void reset() override {}

    void process (const devices::ProcessContext& block) override
    {
        const auto until =
            juce::Time::getHighResolutionTicks() + juce::Time::secondsToHighResolutionTicks (share * block.numSamples / sampleRate);

        while (juce::Time::getHighResolutionTicks() < until)
        {
        }
    }

    bool sleepsOnSilence() const noexcept override { return false; }

    double sampleRate = 44100.0;
};

// Registers them as devices songs can hold, once.
inline void registerTestDevices()
{
    [[maybe_unused]] static const bool registered = []
    {
        const auto add = [] (const devices::DeviceInfo& info, DeviceFactory make) { registerDeviceType (info, make); };
        add (TestGain::info, []() -> std::unique_ptr<devices::Processor> { return std::make_unique<TestGain>(); });
        add (TestTone::info, []() -> std::unique_ptr<devices::Processor> { return std::make_unique<TestTone>(); });
        add (TestDelay::info, []() -> std::unique_ptr<devices::Processor> { return std::make_unique<TestDelay>(); });
        add (TestSidechain::info, []() -> std::unique_ptr<devices::Processor> { return std::make_unique<TestSidechain>(); });
        add (TestBusy::info, []() -> std::unique_ptr<devices::Processor> { return std::make_unique<TestBusy>(); });
        return true;
    }();
}

// A new test device of that type at the end of a track's devices (an instrument in place of its instrument), as a model
// function adds one, in the caller's undo step; its ID.
inline DeviceID addTestDevice (Song& song, TrackID track, const char* type)
{
    registerTestDevices();
    const auto id = insertDeviceState (song, track, makeOwnDeviceState (song, type));
    REQUIRE (id.isValid());
    return id;
}
} // namespace phazite::test
