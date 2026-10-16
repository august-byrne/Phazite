// SPDX-License-Identifier: AGPL-3.0-or-later
// Devices made for tests, run through the same thin Tracktion plug-in as Phazite's own (model/devices/
// DevicePlugin.h): an effect (a gain), an instrument that plays the velocity of the note it holds, an effect with
// latency and a mix (a delay), and an effect that plays its sidechain.
#pragma once

#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/dsp/DelayLine.h"
#include "model/devices/internal/DevicePlugin.h"

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
    }

    int getLatencySamples() const override { return 64; }
    double getTailSeconds() const override { return 1.5; }

    bool prepared = false;
    std::vector<float> lastValues;
    int lastNumChannels = 0;
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

// Registers them with the tests' engine, once.
inline void registerTestDevices()
{
    [[maybe_unused]] static const bool registered = []
    {
        auto& plugins = getEngine().getPluginManager();
        plugins.createBuiltInType<DevicePluginOf<TestGain>>();
        plugins.createBuiltInType<DevicePluginOf<TestTone>>();
        plugins.createBuiltInType<DevicePluginOf<TestDelay>>();
        plugins.createBuiltInType<DevicePluginOf<TestSidechain>>();
        return true;
    }();
}

// A new test device of that type in the Edit, kept alive by keep.
inline DevicePlugin& createTestDevice (te::Edit& edit, const char* type, te::Plugin::Ptr& keep)
{
    registerTestDevices();
    keep = edit.getPluginCache().createNewPlugin (type, {});
    auto* device = dynamic_cast<DevicePlugin*> (keep.get());
    REQUIRE (device != nullptr);
    return *device;
}
} // namespace phazite::test
