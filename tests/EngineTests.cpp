// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own playback engine (src/engine/; docs/tracktion-migration.md, T2), tested on its own, without a project or
// Tracktion: how the audio thread is handed what it plays, Phazite's devices as the engine plays them, the tempo map, and
// the transport and its metronome, to the sample.
#include "TestHelpers.h"
#include "devices/motion/Phaser.h"
#include "devices/synth/Synth.h"
#include "engine/core/Engine.h"
#include "engine/core/Handover.h"
#include "engine/core/TempoMap.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/DeviceRunner.h"
#include "engine/transport/Click.h"

#include <array>
#include <memory>
#include <optional>
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

//==============================================================================
TEST_CASE ("The tempo map places each beat in samples, beats of the time signature's own at a tempo in quarter notes")
{
    const engine::TempoMap plain ({ { 0.0, 120.0 } }, { { 0.0, 4, 4 } }, rate);
    CHECK (plain.toSamples (1.0) == doctest::Approx (24000.0));
    CHECK (plain.toBeats (36000.0) == doctest::Approx (1.5));
    CHECK (plain.getTempoAt (3.0) == doctest::Approx (120.0));
    CHECK (plain.getBarStart (5.5) == doctest::Approx (4.0));
    CHECK (plain.toSamples (-1.0) == doctest::Approx (-24000.0)); // before the song's start, as its first tempo goes on

    // A tempo change: the beats after it are as long as it says.
    const engine::TempoMap slower ({ { 0.0, 120.0 }, { 4.0, 60.0 } }, {}, rate);
    CHECK (slower.toSamples (5.0) == doctest::Approx (4 * 24000.0 + 48000.0));
    CHECK (slower.toBeats (4 * 24000.0 + 24000.0) == doctest::Approx (4.5));
    CHECK (slower.getSamplesPerBeat (3.0) == doctest::Approx (24000.0));
    CHECK (slower.getSamplesPerBeat (4.0) == doctest::Approx (48000.0));

    // In 6/8 a beat is an eighth, half a quarter note; in 3/4 a bar has three.
    const engine::TempoMap eighths ({ { 0.0, 120.0 } }, { { 0.0, 6, 8 } }, rate);
    CHECK (eighths.toSamples (1.0) == doctest::Approx (12000.0));
    CHECK (eighths.getBarStart (7.0) == doctest::Approx (6.0));

    const engine::TempoMap waltz ({ { 0.0, 90.0 } }, { { 0.0, 3, 4 } }, rate);
    CHECK (waltz.getBarStart (4.5) == doctest::Approx (3.0));
    CHECK (waltz.toSamples (1.0) == doctest::Approx (32000.0));
}

namespace
{
// A plan at 120 BPM in 4/4 at the test's rate, with the metronome on.
std::unique_ptr<engine::Plan> makeClickPlan (std::optional<engine::Plan::BeatRange> loop = {}, double bpm = 120.0)
{
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, bpm } }, { { 0.0, 4, 4 } }, rate);
    plan->loop = loop;
    plan->metronome = true;
    return plan;
}

// What the metronome should play: a click starting at each of these samples (an accented one at those marked).
juce::AudioBuffer<float> expectClicks (int numSamples, std::initializer_list<std::pair<int, bool>> clicks)
{
    juce::AudioBuffer<float> expected (2, numSamples);
    expected.clear();
    const auto click = engine::makeClick (rate, false), accent = engine::makeClick (rate, true);

    for (const auto& [start, accented] : clicks)
    {
        const auto& sound = accented ? accent : click;
        const auto length = std::min (sound.getNumSamples(), numSamples - start);

        for (int channel = 0; channel < 2; ++channel)
        {
            // A click stops where the next starts.
            expected.clear (channel, start, numSamples - start);
            expected.copyFrom (channel, start, sound, 0, 0, length);
        }
    }

    return expected;
}

void requireSame (const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& expected)
{
    REQUIRE (actual.getNumSamples() == expected.getNumSamples());

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < actual.getNumSamples(); ++i)
            if (actual.getSample (channel, i) != expected.getSample (channel, i))
                FAIL ("sample " << i << " on channel " << channel << ": " << actual.getSample (channel, i) << ", expected "
                                << expected.getSample (channel, i));
}
} // namespace

TEST_CASE ("The engine's metronome clicks on each beat's sample, a bar's first louder, whatever the block size")
{
    for (const auto blocks : { 64, 256, 1000 })
    {
        CAPTURE (blocks);
        engine::Engine player;
        player.prepare (rate, blocks);
        player.setPlan (makeClickPlan());

        // Two seconds at 120 BPM: four beats, the first a bar's.
        requireSame (player.render (96000), expectClicks (96000, { { 0, true }, { 24000, false }, { 48000, false }, { 72000, false } }));
    }
}

TEST_CASE ("The engine's loop starts again at its start on the sample its end is on")
{
    engine::Engine player;
    player.prepare (rate, 256);

    // A loop of beats 2 to 3: from 1.5, it clicks on beat 2, then on each start again.
    player.setPlan (makeClickPlan (engine::Plan::BeatRange { 2.0, 3.0 }));
    player.locate (1.5);
    requireSame (player.render (12000 + 3 * 24000),
                 expectClicks (12000 + 3 * 24000, { { 12000, false }, { 36000, false }, { 60000, false } }));

    // Past its end, it starts at the loop's start.
    player.stop();
    player.locate (7.0);
    requireSame (player.render (24000), expectClicks (24000, { { 0, false } }));
}

TEST_CASE ("The engine stops and locates as the message thread asks, at its next block")
{
    engine::Engine player;
    player.prepare (rate, 256);
    player.setPlan (makeClickPlan());

    // A locate between beats: the next beat, a bar's first, clicks half a beat later.
    player.locate (3.5);
    CHECK (player.getPlayheadBeat() == doctest::Approx (3.5)); // shown at once
    requireSame (player.render (24000), expectClicks (24000, { { 12000, true } }));
    CHECK (player.isPlaying());

    player.stop();
    CHECK_FALSE (player.isPlaying());
    juce::AudioBuffer<float> silence (2, 1024);
    float* channels[] = { silence.getWritePointer (0), silence.getWritePointer (1) };
    player.process (channels, 2, 1024);
    CHECK (silence.getMagnitude (0, 1024) == 0.0f);
}

TEST_CASE ("A new plan with another tempo leaves the playhead on its beat")
{
    engine::Engine player;
    player.prepare (rate, 256);
    player.setPlan (makeClickPlan());
    player.render (141 * 256); // to sample 36096, beat 1.504: a render plays whole blocks

    // At 60 BPM a beat is a second: from beat 1.5, the next click is half a second on.
    player.setPlan (makeClickPlan ({}, 60.0));
    player.stop();
    const auto after = player.render (48000);

    auto first = -1;

    for (int i = 0; i < after.getNumSamples() && first < 0; ++i)
        if (after.getSample (0, i) != 0.0f)
            first = i;

    // The click's first sample is silent (its attack starts at 0): its second is the first heard.
    // Beat 1.504 is at sample 72192 at 60 BPM, and beat 2 at 96000.
    CHECK (first - 1 == 96000 - 72192);
}

//==============================================================================
namespace
{
// An instrument that plays a single sample of 0.5 for each note it is played, as many samples late as it says it looks ahead.
class Ticker final : public dev::Processor
{
public:
    static inline const dev::DeviceInfo info { .typeID = "testTicker", .name = "Ticker", .kind = dev::DeviceKind::instrument };

    explicit Ticker (int lookAhead)
        : latency (lookAhead)
    {
    }

    void prepare (double, int) override {}
    void reset() override { numDue = 0; }
    int getLatencySamples() const override { return latency; }

    void process (const dev::ProcessContext& context) override
    {
        for (const auto& event : context.events)
            if (event.type == dev::NoteEvent::Type::noteOn && numDue < (int) due.size())
                due[(size_t) numDue++] = event.sample + latency;

        for (int i = 0; i < numDue;)
        {
            if (due[(size_t) i] < context.numSamples)
            {
                for (int channel = 0; channel < context.numChannels; ++channel)
                    context.channels[channel][due[(size_t) i]] += 0.5f;

                due[(size_t) i] = due[(size_t) --numDue];
            }
            else
            {
                due[(size_t) i++] -= context.numSamples;
            }
        }
    }

private:
    int latency = 0;
    std::array<int, 64> due {};
    int numDue = 0;
};

// A track of a plan: its notes, into one instrument, which is made and prepared for it.
engine::Plan::Strip makeTrack (std::int64_t id, const dev::DeviceInfo& info, std::unique_ptr<dev::Processor> instrument,
                               std::vector<engine::NoteSequence::Note> notes, int maximumBlockSize)
{
    engine::Plan::Strip strip;
    strip.id = id;
    strip.state = std::make_shared<engine::StripState>();
    strip.state->prepare (rate, maximumBlockSize);
    strip.notes = std::make_shared<const engine::NoteSequence> (std::move (notes), std::vector<engine::NoteSequence::Control> {});
    auto node = std::make_shared<engine::DeviceNode> (info, std::move (instrument));
    node->getRunner().prepare (rate, maximumBlockSize);
    strip.latency = node->getRunner().getLatencySamples();
    strip.devices.push_back (std::move (node));
    strip.hasInstrument = true;
    return strip;
}

// The master of a plan's tracks, each delayed to line up with the latest.
engine::Plan::Strip makeMaster (const engine::Plan& plan, int maximumBlockSize)
{
    engine::Plan::Strip master;
    master.state = std::make_shared<engine::StripState>();
    master.state->prepare (rate, maximumBlockSize);

    for (const auto& strip : plan.strips)
        master.latency = std::max (master.latency, strip.latency);

    for (int i = 0; i < (int) plan.strips.size(); ++i)
    {
        const auto delay = master.latency - plan.strips[(size_t) i].latency;
        master.inputs.push_back ({ i, delay > 0 ? std::make_shared<engine::Delay> (delay) : nullptr });
    }

    return master;
}
} // namespace

TEST_CASE ("The engine plays a track's notes on their samples, and ends what sounds at the loop's end and starts it again")
{
    auto plan = makeClickPlan (engine::Plan::BeatRange { 1.0, 2.0 });
    plan->metronome = false;

    // A note from beat 0.5 to 3, around the loop's start and end; at 120 BPM a beat is 24000 samples.
    plan->strips.push_back (makeTrack (1, HeldLevel::info, std::make_unique<HeldLevel>(), { { 0.5, 3.0, 60, 0.8f } }, blockSize));
    plan->strips.push_back (makeMaster (*plan, blockSize));

    engine::Engine player;
    player.prepare (rate, blockSize);
    player.setPlan (std::move (plan));
    const auto played = player.render (24000 * 3);

    CHECK (played.getSample (0, 11999) == 0.0f);
    CHECK (played.getSample (0, 12000) == 0.5f); // the note starts
    CHECK (played.getSample (0, 47998) == 0.5f);
    CHECK (played.getSample (0, 47999) == 0.0f); // the loop's last sample: the note ends there
    CHECK (played.getSample (0, 48000) == 0.5f); // and starts again with the loop
    CHECK (played.getSample (1, 48000) == 0.5f);
}

TEST_CASE ("The engine's master lines its tracks up, each delayed as much as the latest is late")
{
    auto plan = makeClickPlan();
    plan->metronome = false;

    // A note on beat 1 on two tracks, one of whose instruments plays 100 samples late.
    plan->strips.push_back (makeTrack (1, Ticker::info, std::make_unique<Ticker> (100), { { 1.0, 1.5, 60, 0.8f } }, blockSize));
    plan->strips.push_back (makeTrack (2, Ticker::info, std::make_unique<Ticker> (0), { { 1.0, 1.5, 60, 0.8f } }, blockSize));
    plan->strips.push_back (makeMaster (*plan, blockSize));
    REQUIRE (engine::Engine::getOutputLatency (*plan) == 100);

    // A render plays ahead by the master's latency, so both ticks land together on the beat's sample.
    engine::Engine player;
    player.prepare (rate, blockSize);
    player.setPlan (std::move (plan));
    const auto played = player.render (48000);

    CHECK (played.getSample (0, 23999) == 0.0f);
    CHECK (played.getSample (0, 24000) == 1.0f);
    CHECK (played.getSample (0, 24001) == 0.0f);
}

TEST_CASE ("A channel's pan keeps a sound's power, at its level in the middle")
{
    const auto middle = engine::getPanGains (0.0f);
    CHECK (middle.left == doctest::Approx (1.0f));
    CHECK (middle.right == doctest::Approx (1.0f));

    const auto right = engine::getPanGains (1.0f);
    CHECK (right.left == doctest::Approx (0.0f).epsilon (1.0e-6));
    CHECK (right.right == doctest::Approx (std::sqrt (2.0f)));

    for (const auto pan : { -0.8f, -0.3f, 0.25f, 0.6f })
    {
        const auto gains = engine::getPanGains (pan);
        CHECK (gains.left * gains.left + gains.right * gains.right == doctest::Approx (2.0f));
    }
}
