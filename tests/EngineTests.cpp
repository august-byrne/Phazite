// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own playback engine (src/engine/; docs/tracktion-migration.md, T2), tested on its own, without a project or
// Tracktion: how the audio thread is handed what it plays, and Phazite's devices as the engine plays them.
#include "TestHelpers.h"
#include "devices/motion/Phaser.h"
#include "devices/synth/Synth.h"
#include "engine/core/Handover.h"
#include "engine/nodes/DeviceRunner.h"

#include <array>
#include <memory>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace engine = phazite::engine;
constexpr double rate = 48000.0;
constexpr int blockSize = 256;

// Counts how many of it exist, to see when the handover frees one.
struct Counted
{
    static inline int alive = 0;
    int value = 0;

    explicit Counted (int v)
        : value (v)
    {
        ++alive;
    }
    ~Counted() { --alive; }
};

std::vector<float> getDefaults (const dev::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}
} // namespace

TEST_CASE ("The handover frees what it replaced only once the audio thread has finished a block since")
{
    {
        engine::Handover<Counted> handover;
        CHECK (handover.get() == nullptr);

        handover.give (std::make_unique<Counted> (1));
        CHECK (handover.get()->value == 1);

        // A block takes what was given last as it starts, and keeps it to its end whatever is given meanwhile.
        {
            const engine::Handover<Counted>::Block block { handover };
            REQUIRE (block.get() != nullptr);
            CHECK (block.get()->value == 1);

            handover.give (std::make_unique<Counted> (2));
            CHECK (block.get()->value == 1);
            CHECK (Counted::alive == 2);
        }

        // That block has ended, but another may have taken the first as it started: it goes after one more.
        handover.give (std::make_unique<Counted> (3));
        CHECK (Counted::alive >= 2);

        {
            const engine::Handover<Counted>::Block block { handover };
            CHECK (block.get()->value == 3);
        }

        {
            const engine::Handover<Counted>::Block block { handover };
        }

        handover.give (nullptr);
        CHECK (Counted::alive == 1); // only the third, kept until the audio thread has finished a block since

        {
            const engine::Handover<Counted>::Block block { handover };
            CHECK (block.get() == nullptr);
        }

        {
            const engine::Handover<Counted>::Block block { handover };
        }

        handover.give (nullptr);
        CHECK (Counted::alive == 0);
    }

    CHECK (Counted::alive == 0);
}

TEST_CASE ("A device plays through its runner without any engine around it")
{
    engine::DeviceRunner runner (dev::Synth::info, std::make_unique<dev::Synth>());
    runner.prepare (rate, blockSize);
    REQUIRE (runner.isInstrument());

    const auto values = getDefaults (dev::Synth::info);
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::array<dev::NoteEvent, 1> note { dev::NoteEvent { dev::NoteEvent::Type::noteOn, 100, 60, 0.8f } };

    engine::DeviceRunner::Block block;
    block.values = values;
    block.events = note;
    block.isPlaying = true;
    runner.process (buffer, 0, blockSize, block);

    // Silent before its note, sounding after it.
    CHECK (buffer.getMagnitude (0, 0, 100) == 0.0f);
    CHECK (buffer.getMagnitude (0, 100, blockSize - 100) > 0.001f);
    CHECK (buffer.getMagnitude (1, 100, blockSize - 100) > 0.001f);
}

TEST_CASE ("An effect's runner passes what comes in on unchanged at no mix")
{
    engine::DeviceRunner runner (dev::Phaser::info, std::make_unique<dev::Phaser>());
    runner.prepare (rate, blockSize);
    REQUIRE_FALSE (runner.isInstrument());
    REQUIRE (runner.getLatencySamples() == 0);

    auto values = getDefaults (dev::Phaser::info);
    const auto mix = dev::findParameter (dev::Phaser::info, dev::mixParameterID);
    REQUIRE (mix >= 0);
    values[(size_t) mix] = 0.0f;

    juce::AudioBuffer<float> buffer (2, blockSize), input (2, blockSize);
    juce::Random random (7);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < blockSize; ++i)
            input.setSample (channel, i, random.nextFloat() - 0.5f);

    engine::DeviceRunner::Block block;
    block.values = values;

    // The first block ramps the mix down from where it started; the next play none of the effect.
    for (int round = 0; round < 3; ++round)
    {
        buffer.makeCopyOf (input);
        runner.process (buffer, 0, blockSize, block);
    }

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < blockSize; ++i)
            REQUIRE (buffer.getSample (channel, i) == input.getSample (channel, i));
}

namespace
{
// An instrument that plays a level of 0.5 while a note is held, and counts the blocks it is asked to play.
class HeldLevel final : public dev::Processor
{
public:
    static inline const dev::DeviceInfo info { .typeID = "testHeldLevel", .name = "Held Level", .kind = dev::DeviceKind::instrument };

    int blocksPlayed = 0;

    void prepare (double, int) override {}
    void reset() override { held = 0; }

    void process (const dev::ProcessContext& context) override
    {
        ++blocksPlayed;
        int done = 0;

        for (const auto& event : context.events)
        {
            fill (context, done, event.sample);
            done = event.sample;
            held += event.type == dev::NoteEvent::Type::noteOn ? 1 : event.type == dev::NoteEvent::Type::noteOff ? -1 : 0;
        }

        fill (context, done, context.numSamples);
    }

private:
    int held = 0;

    void fill (const dev::ProcessContext& context, int from, int to) const
    {
        for (int channel = 0; channel < context.numChannels; ++channel)
            juce::FloatVectorOperations::fill (context.channels[channel] + from, held > 0 ? 0.5f : 0.0f, to - from);
    }
};
} // namespace

TEST_CASE ("A device's runner sleeps after a quarter of a second of silence, and wakes for its next note at once")
{
    auto device = std::make_unique<HeldLevel>();
    auto& counting = *device;
    engine::DeviceRunner runner (HeldLevel::info, std::move (device));
    runner.prepare (rate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    std::array<dev::NoteEvent, 1> event {};
    engine::DeviceRunner::Block block;

    const auto play = [&] (dev::NoteEvent::Type type)
    {
        event[0] = { type, 10, 60, 0.8f };
        block.events = event;
        runner.process (buffer, 0, blockSize, block);
        block.events = {};
    };

    play (dev::NoteEvent::Type::noteOn);
    CHECK (buffer.getSample (0, 9) == 0.0f);
    CHECK (buffer.getSample (0, 10) == 0.5f);
    play (dev::NoteEvent::Type::noteOff);
    CHECK (buffer.getSample (0, 10) == 0.0f);

    // Silent: played for a quarter of a second, then no more.
    const auto quarter = (int) (rate / 4) / blockSize + 1;

    for (int i = 0; i < quarter; ++i)
        runner.process (buffer, 0, blockSize, block);

    const auto played = counting.blocksPlayed;

    for (int i = 0; i < 10; ++i)
        runner.process (buffer, 0, blockSize, block);

    CHECK (counting.blocksPlayed == played);

    // Its next note plays in the block it comes in.
    play (dev::NoteEvent::Type::noteOn);
    CHECK (counting.blocksPlayed == played + 1);
    CHECK (buffer.getSample (0, 10) == 0.5f);

    // A render never sleeps.
    play (dev::NoteEvent::Type::noteOff);
    block.isRendering = true;

    for (int i = 0; i < quarter + 10; ++i)
        runner.process (buffer, 0, blockSize, block);

    CHECK (counting.blocksPlayed == played + 2 + quarter + 10);
}
