// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own playback engine (src/engine/; docs/design.md, section 23), tested on its own, without a project: how the
// audio thread is handed what it plays, Phazite's devices as the engine plays them (their runners and nodes), the tempo
// map, the transport and its metronome, to the sample, and audio files and the clips that play them.
#include "TestDevices.h"
#include "TestHelpers.h"
#include "devices/modulation/Modulators.h"
#include "devices/motion/Phaser.h"
#include "devices/synth/Synth.h"
#include "engine/audio/AudioRegion.h"
#include "engine/automation/Curve.h"
#include "engine/core/Engine.h"
#include "engine/core/Handover.h"
#include "engine/core/TempoMap.h"
#include "engine/core/Workers.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/DeviceNode.h"
#include "engine/nodes/DeviceRunner.h"
#include "engine/nodes/LoudnessMeter.h"
#include "engine/transport/Click.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
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
// Devices through their runners and nodes, with the devices made for tests (TestDevices.h): an effect (a gain), an
// instrument that plays the velocity of the note it holds, and an effect with latency and a mix (a delay).
namespace
{
using phazite::test::TestDelay;
using phazite::test::TestGain;
using phazite::test::TestTone;

constexpr int deviceBlock = 512;

// A buffer of so many samples on so many channels, each at a level.
juce::AudioBuffer<float> makeLevel (int numChannels, int numSamples, float level)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);

    for (int channel = 0; channel < numChannels; ++channel)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), level, numSamples);

    return buffer;
}
} // namespace

TEST_CASE ("An effect's runner gets its parameters' values and stereo audio, and reports its latency and tail")
{
    auto device = std::make_unique<TestGain>();
    auto& gain = *device;
    engine::DeviceRunner runner (TestGain::info, std::move (device));
    runner.prepare (rate, deviceBlock);
    CHECK (gain.prepared);
    CHECK_FALSE (runner.isInstrument());
    CHECK (runner.getLatencySamples() == 64);
    CHECK (runner.getTailSeconds() == doctest::Approx (1.5));

    std::vector<float> values { -6.0206f, 0.0f };
    engine::DeviceRunner::Block block;
    block.values = values;
    auto buffer = makeLevel (2, 256, 1.0f);
    runner.process (buffer, 0, 256, block);
    CHECK (buffer.getSample (0, 10) == doctest::Approx (0.5f).epsilon (0.001));
    CHECK (buffer.getSample (1, 255) == doctest::Approx (0.5f).epsilon (0.001));
    REQUIRE (gain.lastValues.size() == 2);
    CHECK (gain.lastValues[0] == doctest::Approx (-6.0206f));

    // A choice reaches the DSP as a whole number, however it was set.
    values[1] = 0.7f;

    // A mono channel is played in stereo, a stretch at a time, and gets the average back.
    auto mono = makeLevel (1, 1300, 1.0f);
    runner.process (mono, 0, 1300, block);
    CHECK (gain.lastValues[1] == doctest::Approx (1.0f));
    CHECK (gain.lastNumChannels == 2);
    CHECK (mono.getSample (0, 0) == doctest::Approx (-0.5f).epsilon (0.001));
    CHECK (mono.getSample (0, 1299) == doctest::Approx (-0.5f).epsilon (0.001));
}

TEST_CASE ("MIDI reaches a device as events at their samples, and an instrument plays them into silence")
{
    using Type = dev::NoteEvent::Type;

    // What devices use of MIDI: notes (a note-on of velocity 0 is an off), all notes off, the pitch wheel and controllers.
    const auto bend = engine::toNoteEvent (juce::MidiMessage::pitchWheel (1, 16383), 96);
    const auto on = engine::toNoteEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 64), 240);
    const auto off = engine::toNoteEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 0), 475);
    const auto controller = engine::toNoteEvent (juce::MidiMessage::controllerEvent (1, 74, 127), 504);
    const auto allOff = engine::toNoteEvent (juce::MidiMessage::allNotesOff (1), 0);
    REQUIRE (bend.has_value());
    REQUIRE (on.has_value());
    REQUIRE (off.has_value());
    REQUIRE (controller.has_value());
    REQUIRE (allOff.has_value());

    CHECK (bend->type == Type::pitchBend);
    CHECK (bend->sample == 96);
    CHECK (bend->value == doctest::Approx (8191.0f / 8192.0f));
    CHECK (on->type == Type::noteOn);
    CHECK (on->sample == 240);
    CHECK (on->number == 60);
    CHECK (on->value == doctest::Approx (64.0f / 127.0f));
    CHECK (off->type == Type::noteOff);
    CHECK (controller->type == Type::controller);
    CHECK (controller->number == 74);
    CHECK (controller->value == doctest::Approx (1.0f));
    CHECK (allOff->type == Type::allNotesOff);
    CHECK_FALSE (engine::toNoteEvent (juce::MidiMessage::programChange (1, 3), 505).has_value());

    // An instrument gets them at their samples, and plays into silence: the track's audio isn't heard.
    auto device = std::make_unique<TestTone>();
    auto& tone = *device;
    engine::DeviceRunner runner (TestTone::info, std::move (device));
    runner.prepare (rate, deviceBlock);
    REQUIRE (runner.isInstrument());

    std::vector<dev::NoteEvent> events { *allOff, *bend, *on, *off, *controller };
    const auto values = getDefaults (TestTone::info);
    engine::DeviceRunner::Block block;
    block.values = values;
    block.events = events;
    auto buffer = makeLevel (2, deviceBlock, 0.3f);
    runner.process (buffer, 0, deviceBlock, block);

    REQUIRE (tone.received.size() == 5);
    CHECK (tone.received[0].type == Type::allNotesOff);
    CHECK (tone.received[1].sample == 96);
    CHECK (tone.received[2].sample == 240);
    CHECK (tone.received[3].sample == 475);
    CHECK (tone.received[4].sample == 504);

    CHECK (buffer.getSample (0, 239) == doctest::Approx (0.0f));
    CHECK (buffer.getSample (0, 300) == doctest::Approx (64.0f / 127.0f));
    CHECK (buffer.getSample (1, 300) == doctest::Approx (64.0f / 127.0f));
    CHECK (buffer.getSample (1, 480) == doctest::Approx (0.0f));
}

TEST_CASE ("An effect's runner blends its sound with what came in at its mix, lined up with its latency")
{
    engine::DeviceRunner runner (TestDelay::info, std::make_unique<TestDelay>());
    runner.prepare (rate, deviceBlock);
    std::vector<float> values { 1.0f };
    engine::DeviceRunner::Block block;
    block.values = values;

    const auto play = [&] (float level, float mix, int length)
    {
        values[0] = mix;
        auto buffer = makeLevel (2, length, level);
        runner.process (buffer, 0, length, block);
        return buffer;
    };

    // At 100 %, only the effect: what came in, upside down.
    CHECK (play (1.0f, 1.0f, 512).getSample (0, 511) == doctest::Approx (-1.0f));

    // At 0 %, only what came in; half way, the two cancel. (Each first block ramps from the last mix.)
    play (1.0f, 0.0f, 512);
    CHECK (play (1.0f, 0.0f, 512).getSample (1, 300) == doctest::Approx (1.0f));
    play (1.0f, 0.5f, 512);
    CHECK (std::abs (play (1.0f, 0.5f, 512).getSample (0, 300)) < 1.0e-6f);

    // The sound that came in is delayed as much as the effect's, so a click and its echo come out together.
    runner.reset();
    play (0.0f, 0.5f, 512);
    auto click = makeLevel (2, 256, 0.0f);
    click.setSample (0, 0, 1.0f);
    click.setSample (1, 0, 1.0f);
    runner.process (click, 0, 256, block);
    CHECK (click.getSample (0, 0) == doctest::Approx (0.0f));
    CHECK (click.getSample (0, TestDelay::latency) == doctest::Approx (0.0f)); // half the click and half of it upside down
}

TEST_CASE ("Runners sleep once what comes in and what they play have been silent a while, and wake at the first sound")
{
    auto effectDevice = std::make_unique<TestGain>();
    auto& gain = *effectDevice;
    engine::DeviceRunner effect (TestGain::info, std::move (effectDevice));
    auto instrumentDevice = std::make_unique<TestTone>();
    auto& tone = *instrumentDevice;
    engine::DeviceRunner instrument (TestTone::info, std::move (instrumentDevice));
    effect.prepare (rate, deviceBlock);
    instrument.prepare (rate, deviceBlock);

    const std::vector<float> gainValues { -6.0206f, 0.0f };
    const auto toneValues = getDefaults (TestTone::info);
    engine::DeviceRunner::Block effectBlock, instrumentBlock;
    effectBlock.values = gainValues;
    instrumentBlock.values = toneValues;
    juce::AudioBuffer<float> buffer (2, deviceBlock);

    // A quarter of a second of silence (24 blocks), then each sleeps.
    for (int b = 0; b < 30; ++b)
    {
        buffer.clear();
        effect.process (buffer, 0, deviceBlock, effectBlock);
        instrument.process (buffer, 0, deviceBlock, instrumentBlock);
    }

    CHECK (gain.processed == 24);
    CHECK (tone.processed == 24);

    // The first sound wakes the effect, which treats it whole.
    buffer = makeLevel (2, deviceBlock, 1.0f);
    effect.process (buffer, 0, deviceBlock, effectBlock);
    CHECK (gain.processed == 25);
    CHECK (buffer.getSample (0, 0) == doctest::Approx (0.5f).epsilon (0.001));

    // A note wakes the instrument, which plays it from its sample.
    std::array<dev::NoteEvent, 1> note { dev::NoteEvent { dev::NoteEvent::Type::noteOn, 48, 60, 1.0f } };
    instrumentBlock.events = note;
    buffer.clear();
    instrument.process (buffer, 0, deviceBlock, instrumentBlock);
    CHECK (tone.processed == 25);
    CHECK (buffer.getSample (0, 47) == doctest::Approx (0.0f));
    CHECK (buffer.getSample (0, 48) == doctest::Approx (1.0f));
}

TEST_CASE ("A continuous parameter that moves between blocks glides there over the next, while choices jump")
{
    auto device = std::make_unique<TestGain>();
    auto& gain = *device;
    engine::DeviceRunner runner (TestGain::info, std::move (device));
    runner.prepare (rate, deviceBlock);
    constexpr auto step = engine::DeviceRunner::smoothingStep;
    std::vector<float> values { 0.0f, 0.0f };
    engine::DeviceRunner::Block block;
    block.values = values;
    juce::AudioBuffer<float> buffer;

    const auto play = [&]
    {
        buffer = makeLevel (2, deviceBlock, 1.0f);
        runner.process (buffer, 0, deviceBlock, block);
    };

    play();
    CHECK (buffer.getSample (0, 0) == doctest::Approx (1.0f));
    CHECK (gain.processed == 1); // a block that changes nothing plays whole

    // From 0 dB to -6 dB: a step of the way each stretch, there by the block's end.
    values[0] = -6.0206f;
    play();
    CHECK (gain.processed == 1 + deviceBlock / step);
    CHECK (buffer.getSample (0, 0) == doctest::Approx (juce::Decibels::decibelsToGain (-6.0206f * step / deviceBlock)).epsilon (0.001));
    CHECK (buffer.getSample (0, deviceBlock - 1) == doctest::Approx (0.5f).epsilon (0.001));

    for (int i = step; i < deviceBlock; i += step)
        CHECK (buffer.getSample (0, i) < buffer.getSample (0, i - step));

    // Settled: the next block plays whole again. A choice that changes jumps, with no glide.
    play();
    CHECK (gain.processed == 2 + deviceBlock / step);
    values[1] = 1.0f;
    play();
    CHECK (gain.processed == 3 + deviceBlock / step);
    CHECK (buffer.getSample (0, 0) == doctest::Approx (-0.5f).epsilon (0.001));
}

TEST_CASE ("A device's node keeps its values set by hand in their ranges, plays them, and nothing while it is off")
{
    engine::DeviceNode node (TestGain::info, std::make_unique<TestGain>());
    auto& gain = dynamic_cast<TestGain&> (node.getRunner().getProcessor());
    node.getRunner().prepare (rate, deviceBlock);

    // Its parameters start at their defaults; a value is kept to its parameter's range, a choice's to a whole number.
    CHECK (node.getValue (0) == doctest::Approx (0.0f));
    CHECK (node.getValue (1) == doctest::Approx (0.0f));
    node.setValue (0, -100.0f);
    CHECK (node.getValue (0) == doctest::Approx (-60.0f));
    node.setValue (0, -6.0206f);
    node.setValue (1, 0.7f);
    CHECK (node.getValue (1) == doctest::Approx (1.0f));

    // It plays them, and keeps what it played for the controls.
    auto buffer = makeLevel (2, deviceBlock, 1.0f);
    node.process (buffer, 0, deviceBlock, {});
    CHECK (buffer.getSample (0, 0) == doctest::Approx (-0.5f).epsilon (0.001));
    CHECK (node.getPlayedValue (0) == doctest::Approx (-6.0206f));
    CHECK (node.getPlayedValue (1) == doctest::Approx (1.0f));

    // Off, it lets the sound through untouched, without playing.
    const auto played = gain.processed;
    node.setOn (false);
    buffer = makeLevel (2, deviceBlock, 1.0f);
    node.process (buffer, 0, deviceBlock, {});
    CHECK (gain.processed == played);
    CHECK (buffer.getSample (0, 0) == doctest::Approx (1.0f));

    node.setOn (true);
    buffer = makeLevel (2, deviceBlock, 1.0f);
    node.process (buffer, 0, deviceBlock, {});
    CHECK (gain.processed > played);
    CHECK (buffer.getSample (0, 0) == doctest::Approx (-0.5f).epsilon (0.001));
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
            if (! juce::exactlyEqual (actual.getSample (channel, i), expected.getSample (channel, i)))
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

TEST_CASE ("Asked more than its queue holds while no device plays it, the engine plays the transport as last asked once one does")
{
    engine::Engine player;
    player.prepare (rate, 256);
    player.setPlan (makeClickPlan());

    // Played and stopped, again and again, then put between beats and played: past 64, the queue drops what is asked.
    for (int i = 0; i < 40; ++i)
    {
        player.play();
        player.stop();
    }

    player.locate (3.5);
    player.play();

    // A device starts (prepares it): the next beat, a bar's first, clicks half a beat later.
    player.prepare (rate, 256);
    CHECK (player.getPlayheadBeat() == doctest::Approx (3.5));
    juce::AudioBuffer<float> played (2, 24000);
    played.clear();

    for (int done = 0; done < played.getNumSamples(); done += 256)
    {
        float* channels[] = { played.getWritePointer (0, done), played.getWritePointer (1, done) };
        player.process (channels, 2, std::min (256, played.getNumSamples() - done));
    }

    requireSame (played, expectClicks (24000, { { 12000, true } }));
}

TEST_CASE ("The engine tells of blocks that started more than two blocks' length after the one before, once")
{
    engine::Engine player;
    player.prepare (rate, 480); // 10 ms
    juce::AudioBuffer<float> block (2, 480);
    float* channels[] = { block.getWritePointer (0), block.getWritePointer (1) };

    // Back to back: none late.
    player.process (channels, 2, 480);
    player.process (channels, 2, 480);
    auto times = player.takeBlockTimes();
    CHECK (times.late == 0);
    CHECK (times.blockLength == doctest::Approx (0.01));
    CHECK (times.longestGap < 0.02);

    // One 30 ms after the one before: late, and told of once.
    juce::Thread::sleep (30);
    player.process (channels, 2, 480);
    times = player.takeBlockTimes();
    CHECK (times.late == 1);
    CHECK (times.longestGap >= 0.029);
    CHECK (times.longestBlock < times.longestGap);
    CHECK (player.takeBlockTimes().late == 0);

    // A device started again (prepared): its first block is no gap.
    juce::Thread::sleep (30);
    player.prepare (rate, 480);
    player.process (channels, 2, 480);
    CHECK (player.takeBlockTimes().late == 0);
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
        if (! juce::exactlyEqual (after.getSample (0, i), 0.0f))
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

// An effect that rings at a level of 0.25 from the first sound it hears until it is reset: a tail without an end.
class Ringer final : public dev::Processor
{
public:
    static inline const dev::DeviceInfo info { .typeID = "testRinger", .name = "Ringer", .kind = dev::DeviceKind::effect };

    void prepare (double, int) override {}
    void reset() override { ringing = false; }
    bool sleepsOnSilence() const noexcept override { return false; }

    void process (const dev::ProcessContext& context) override
    {
        for (int i = 0; i < context.numSamples; ++i)
        {
            ringing = ringing || ! juce::exactlyEqual (context.channels[0][i], 0.0f);

            for (int channel = 0; channel < context.numChannels; ++channel)
                context.channels[channel][i] = ringing ? 0.25f : 0.0f;
        }
    }

private:
    bool ringing = false;
};

// An instrument that keeps each note event it is given, at its sample from the first block's start.
class EventRecorder final : public dev::Processor
{
public:
    static inline const dev::DeviceInfo info { .typeID = "testEventRecorder",
                                               .name = "Event Recorder",
                                               .kind = dev::DeviceKind::instrument };

    std::vector<dev::NoteEvent> events;

    void prepare (double, int) override {}
    void reset() override {}

    void process (const dev::ProcessContext& context) override
    {
        for (auto event : context.events)
        {
            event.sample += done;
            events.push_back (event);
        }

        done += context.numSamples;
    }

private:
    int done = 0;
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
    strip.devices.push_back ({ std::move (node), {} });
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

TEST_CASE ("A slide note reaches the instrument as a slide to its pitch over its length in seconds, a portamento note as one gliding in")
{
    auto plan = makeClickPlan();
    plan->metronome = false;
    auto recorder = std::make_unique<EventRecorder>();
    auto* recorded = recorder.get();

    // C4 from beat 0 to 4, a slide to E4 at beat 1 a beat long, and G4 a portamento note at beat 4; a beat is 24000 samples.
    plan->strips.push_back (makeTrack (1, EventRecorder::info, std::move (recorder),
                                       { { 0.0, 4.0, 60, 0.8f }, { 1.0, 2.0, 64, 0.8f, true }, { 4.0, 5.0, 67, 0.8f, false, true } },
                                       blockSize));
    plan->strips.push_back (makeMaster (*plan, blockSize));

    engine::Engine player;
    player.prepare (rate, blockSize);
    player.setPlan (std::move (plan));
    player.render (24000 * 6);

    using Type = dev::NoteEvent::Type;
    std::vector<dev::NoteEvent> events;
    std::copy_if (recorded->events.begin(), recorded->events.end(), std::back_inserter (events),
                  [] (const dev::NoteEvent& event) { return event.type != Type::allNotesOff; });

    REQUIRE (events.size() == 5);
    CHECK ((events[0].type == Type::noteOn && events[0].number == 60 && events[0].sample == 0));
    CHECK ((events[1].type == Type::slide && events[1].number == 64 && events[1].sample == 24000));
    CHECK (events[1].value == doctest::Approx (0.5f)); // a beat at 120 BPM, in seconds
    CHECK ((events[2].type == Type::noteOff && events[2].number == 60 && events[2].sample == 96000));
    CHECK ((events[3].type == Type::portamentoOn && events[3].number == 67 && events[3].sample == 96000));
    CHECK ((events[4].type == Type::noteOff && events[4].number == 67 && events[4].sample == 120000));
}

TEST_CASE ("The engine played from where one note ends and the next starts plays the next alone, however its beat rounds")
{
    // At 140 BPM, beat 8 is at sample 164571.43: on sample 164571, a little before the beat.
    auto plan = makeClickPlan ({}, 140.0);
    plan->metronome = false;
    REQUIRE (plan->tempo.toBeats ((double) std::llround (plan->tempo.toSamples (8.0))) < 8.0);

    plan->strips.push_back (
        makeTrack (1, Ticker::info, std::make_unique<Ticker> (0), { { 7.0, 8.0, 60, 0.8f }, { 8.0, 9.0, 62, 0.8f } }, blockSize));
    plan->strips.push_back (makeMaster (*plan, blockSize));

    engine::Engine player;
    player.prepare (rate, blockSize);
    player.setPlan (std::move (plan));
    player.locate (8.0);
    const auto played = player.render (blockSize);

    CHECK (played.getSample (0, 0) == 0.5f); // the second note alone: the first, which ends there, doesn't start again
    CHECK (played.getMagnitude (0, 1, blockSize - 1) == 0.0f);
}

TEST_CASE ("Playback started or jumping starts from silence: a tail rings on after a stop, but not into what plays next")
{
    auto plan = makeClickPlan();
    plan->metronome = false;
    auto track = makeTrack (1, Ticker::info, std::make_unique<Ticker> (0), { { 0.0, 1.0, 60, 0.8f } }, blockSize);
    auto ringer = std::make_shared<engine::DeviceNode> (Ringer::info, std::make_unique<Ringer>());
    ringer->getRunner().prepare (rate, blockSize);
    track.devices.push_back ({ std::move (ringer), {} });
    plan->strips.push_back (std::move (track));
    plan->strips.push_back (makeMaster (*plan, blockSize));

    engine::Engine player;
    player.prepare (rate, blockSize);
    player.setPlan (std::move (plan));

    // The note at the song's start sets it ringing; a jump while playing cuts it off, where no note is.
    CHECK (player.render (blockSize).getSample (0, blockSize - 1) == 0.25f);
    player.locate (8.0);
    CHECK (player.render (blockSize).getMagnitude (0, 0, blockSize) == 0.0f);

    // Back to the note, it rings again, and on after a stop.
    player.locate (0.0);
    CHECK (player.render (blockSize).getSample (0, blockSize - 1) == 0.25f);
    player.stop();
    juce::AudioBuffer<float> stopped (2, blockSize);
    float* channels[] = { stopped.getWritePointer (0), stopped.getWritePointer (1) };
    player.process (channels, 2, blockSize);
    CHECK (stopped.getSample (0, blockSize - 1) == 0.25f);

    // Played again, where no note is, it is silent from the first sample.
    player.locate (8.0);
    CHECK (player.render (blockSize).getMagnitude (0, 0, blockSize) == 0.0f);
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

namespace
{
// A file's samples in memory: on its first channel the sample's number in a thousandths' cycle, on any other the same negated;
// or one value throughout.
struct MemoryReader final : engine::SampleReader
{
    MemoryReader (int channels, double sampleRate, std::int64_t samples, std::optional<float> constant = {})
        : numChannels (channels),
          fileRate (sampleRate),
          length (samples),
          value (constant)
    {
    }

    int getNumChannels() const override { return numChannels; }
    double getSampleRate() const override { return fileRate; }
    std::int64_t getLength() const override { return length; }

    static float at (int channel, std::int64_t sample) { return (channel == 0 ? 1.0f : -1.0f) * (float) (sample % 1000) / 1000.0f; }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (channel, i, value.has_value() ? *value : at (channel, start + i));

        return true;
    }

    int numChannels;
    double fileRate;
    std::int64_t length;
    std::optional<float> value;
};

// A mono file of a full-scale sine at a frequency, starting at its upward zero crossing.
struct SineReader final : engine::SampleReader
{
    SineReader (double sampleRate, double frequency, std::int64_t samples)
        : fileRate (sampleRate),
          hertz (frequency),
          length (samples)
    {
    }

    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return fileRate; }
    std::int64_t getLength() const override { return length; }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
    {
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * hertz * (double) (start + i) / fileRate));

        return true;
    }

    double fileRate, hertz;
    std::int64_t length;
};

// How a stretch of a channel holds a sine at a frequency: its level, in decibels against full scale, and how far everything
// else in it (aliases, images, noise) is under that, in decibels; by least squares.
struct ToneFit
{
    double decibels = 0.0, othersUnder = 0.0;
};

ToneFit fitTone (const float* samples, int numSamples, double frequency, double sampleRate)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
        const auto s = std::sin (phase), c = std::cos (phase);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += samples[i] * s;
        yc += samples[i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    const auto a = (ys * cc - yc * sc) / determinant, b = (yc * ss - ys * sc) / determinant;
    auto residual = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
        const auto error = samples[i] - (a * std::sin (phase) + b * std::cos (phase));
        residual += error * error;
    }

    const auto amplitude = std::sqrt (a * a + b * b);
    const auto tonePower = 0.5 * amplitude * amplitude * numSamples;
    return { 20.0 * std::log10 (std::max (amplitude, 1.0e-12)), 10.0 * std::log10 (tonePower / std::max (residual, 1.0e-30)) };
}

// What a region plays from a sample of the song for so many, a block at a time, as a render plays it.
juce::AudioBuffer<float> playRegion (const engine::AudioRegion& region, std::int64_t from, int numSamples, double engineRate = rate)
{
    juce::AudioBuffer<float> out (2, numSamples), scratch (2, 8 * 64 + 16);
    out.clear();

    for (int done = 0; done < numSamples; done += 64)
    {
        const auto count = std::min (64, numSamples - done);
        float* channels[] = { out.getWritePointer (0, done), out.getWritePointer (1, done) };
        region.play (channels, from + done, count, engineRate, true, scratch);
    }

    return out;
}

engine::AudioRegion makeRegion (std::shared_ptr<engine::AudioFile> file, std::int64_t start, std::int64_t length, double engineRate = rate)
{
    engine::AudioRegion region;
    region.state = std::make_shared<engine::RegionState> (*file, engineRate);
    region.file = std::move (file);
    region.start = start;
    region.length = length;
    return region;
}
} // namespace

TEST_CASE ("A fade's curves start silent and end at their level: straight, convex quick at first, concave slow, S-curves between")
{
    using engine::FadeShape;
    const auto gain = [] (FadeShape shape, float bend, float along) { return engine::getFadeGain ({ shape, bend }, along); };

    for (const auto shape : { FadeShape::curve, FadeShape::sCurve })
    {
        for (const auto bend : { -1.0f, -0.5f, 0.0f, 0.3f, 1.0f })
        {
            CHECK (gain (shape, bend, 0.0f) == doctest::Approx (0.0f));
            CHECK (gain (shape, bend, 1.0f) == doctest::Approx (1.0f));

            // Rising all the way.
            for (auto along = 0.05f; along < 1.0f; along += 0.05f)
                CHECK (gain (shape, bend, along) > gain (shape, bend, along - 0.05f));
        }

        CHECK (gain (shape, 0.0f, 0.3f) == doctest::Approx (0.3f));
    }

    // Bent all the way: a quarter of a sine, quick at first or slow; an S-curve slow at both ends, or quick the other way.
    CHECK (gain (FadeShape::curve, 1.0f, 0.5f) == doctest::Approx (std::sqrt (0.5f)));
    CHECK (gain (FadeShape::curve, -1.0f, 0.5f) == doctest::Approx (1.0f - std::sqrt (0.5f)));
    CHECK (gain (FadeShape::sCurve, 1.0f, 0.5f) == doctest::Approx (0.5f));
    CHECK (gain (FadeShape::sCurve, 1.0f, 0.25f) < 0.25f);
    CHECK (gain (FadeShape::sCurve, 1.0f, 0.75f) > 0.75f);
    CHECK (gain (FadeShape::sCurve, -1.0f, 0.25f) > 0.25f);
    CHECK (gain (FadeShape::sCurve, -1.0f, 0.75f) < 0.75f);

    // Half way bent is half way from straight.
    CHECK (gain (FadeShape::curve, 0.5f, 0.5f) == doctest::Approx (0.25f + 0.5f * std::sqrt (0.5f)));
}

TEST_CASE ("An audio region plays its part of a file on its samples, at its gain, through its fades, and nothing outside")
{
    auto file = std::make_shared<engine::AudioFile> (std::make_unique<MemoryReader> (2, rate, 1000), nullptr);
    auto region = makeRegion (file, 100, 400);
    region.fileStart = 200.0;
    region.gain = 0.5f;
    region.fadeIn = 100;
    region.fadeOut = 100;
    region.fadeOutCurve = { engine::FadeShape::curve, 1.0f }; // convex

    const auto out = playRegion (region, 0, 600);
    CHECK (out.getMagnitude (0, 100) == 0.0f);
    CHECK (out.getMagnitude (500, 100) == 0.0f);

    // In the middle, the file from its start in the file, on both channels.
    for (const auto sample : { 200, 300, 399 })
    {
        const auto inFile = 200 + (sample - 100);
        CHECK (out.getSample (0, sample) == doctest::Approx (0.5f * MemoryReader::at (0, inFile)));
        CHECK (out.getSample (1, sample) == doctest::Approx (0.5f * MemoryReader::at (1, inFile)));
    }

    // Half way through the linear fade in and the convex fade out.
    CHECK (out.getSample (0, 150) == doctest::Approx (0.5f * 0.5f * MemoryReader::at (0, 250)));
    CHECK (out.getSample (0, 450) == doctest::Approx (0.5f * std::sqrt (0.5f) * MemoryReader::at (0, 550)));
}

TEST_CASE ("An audio region started inside fades in over 40 samples rather than click, and a mono file plays on both sides")
{
    auto file = std::make_shared<engine::AudioFile> (std::make_unique<MemoryReader> (1, rate, 1000, 0.8f), nullptr);
    const auto region = makeRegion (file, 0, 1000);

    const auto out = playRegion (region, 300, 200);
    CHECK (out.getSample (0, 0) < 0.1f);
    CHECK (out.getSample (0, 20) == doctest::Approx (0.8f * 21.0f / 40.0f));
    CHECK (out.getSample (0, 40) == doctest::Approx (0.8f));
    CHECK (out.getSample (1, 100) == doctest::Approx (0.8f));

    // Played on from where it played to, it doesn't fade again.
    const auto next = playRegion (region, 500, 64);
    CHECK (next.getSample (0, 0) == doctest::Approx (0.8f));
}

TEST_CASE ("An audio region of a file at another rate plays it at its own speed, in time from its first sample")
{
    auto file = std::make_shared<engine::AudioFile> (std::make_unique<MemoryReader> (1, rate / 2.0, 1000, 0.6f), nullptr);
    const auto region = makeRegion (file, 0, 2000);

    const auto out = playRegion (region, 0, 1000);
    CHECK (out.getSample (0, 500) == doctest::Approx (0.6f).epsilon (1.0e-4));
    CHECK (out.getSample (1, 999) == doctest::Approx (0.6f).epsilon (1.0e-4));

    // A low tone at 44.1 kHz comes out at 48 kHz where it was, sample for sample: the resampling hides its latency.
    constexpr double hertz = 100.0;
    auto tone = std::make_shared<engine::AudioFile> (std::make_unique<SineReader> (44100.0, hertz, 44100), nullptr);
    const auto played = playRegion (makeRegion (tone, 0, 48000), 0, 48000);
    auto worst = 0.0;

    for (int i = 2000; i < 46000; ++i)
        worst = std::max (worst, std::abs (played.getSample (0, i) - std::sin (juce::MathConstants<double>::twoPi * hertz * i / rate)));

    CHECK (worst < 0.005); // under a tenth of a sample late or early

    // A region whose state was made for another rate plays nothing rather than at the wrong speed.
    auto wrong = makeRegion (tone, 0, 48000);
    wrong.state = std::make_shared<engine::RegionState>();
    CHECK (playRegion (wrong, 0, 1000).getMagnitude (0, 1000) == 0.0f);
}

TEST_CASE ("A file at another rate keeps its top octave at its level, and nothing of it comes back as false tones")
{
    // Each tone at full scale, measured once the region has played a while.
    const auto measure = [] (double fileRate, double hertz)
    {
        auto file = std::make_shared<engine::AudioFile> (std::make_unique<SineReader> (fileRate, hertz, (std::int64_t) fileRate), nullptr);
        const auto played = playRegion (makeRegion (file, 0, 48000), 0, 40000);
        return fitTone (played.getReadPointer (0, 4000), 32768, hertz, rate);
    };

    // 44.1 kHz, the most common rate of samples, in a 48 kHz project: whole to 20 kHz (JUCE's Lagrange interpolation lost
    // 1.2 dB at 15 kHz and 4.4 dB at 20 kHz, and its false tones were 17.5 dB and 4.7 dB under them; docs/dsp-audit.md).
    for (const auto hertz : { 1000.0, 10000.0, 15000.0, 20000.0 })
    {
        CAPTURE (hertz);
        const auto fit = measure (44100.0, hertz);
        CHECK (std::abs (fit.decibels) < 0.01);
        CHECK (fit.othersUnder > 100.0);
    }

    // 96 kHz, in the same project: what lies above 24 kHz is gone, not folded down (Lagrange interpolation dropped every other
    // sample there, so 30 kHz came back at 18 kHz at full level).
    for (const auto hertz : { 30000.0, 40000.0 })
    {
        CAPTURE (hertz);
        auto file = std::make_shared<engine::AudioFile> (std::make_unique<SineReader> (96000.0, hertz, 96000), nullptr);
        const auto played = playRegion (makeRegion (file, 0, 48000), 0, 40000);
        CHECK (played.getMagnitude (0, 4000, 32768) < 1.0e-5f); // more than 100 dB down
    }

    CHECK (std::abs (measure (96000.0, 15000.0).decibels) < 0.01);
}

TEST_CASE ("A file at another rate played from inside, or after a jump, fades in and plays in time from there")
{
    constexpr double hertz = 100.0;
    auto file = std::make_shared<engine::AudioFile> (std::make_unique<SineReader> (44100.0, hertz, 88200), nullptr);
    const auto region = makeRegion (file, 0, 96000);
    const auto ideal = [] (std::int64_t sample) { return std::sin (juce::MathConstants<double>::twoPi * hertz * (double) sample / rate); };

    // On from 9600, then a jump on to 28800, and back to 19200 (as to a loop's start): each where a sample of the file plays,
    // since a jump starts the file at the sample nearest (half a sample late or early at most, 11 microseconds here).
    for (const auto from : { std::int64_t { 9600 }, std::int64_t { 28800 }, std::int64_t { 19200 } })
    {
        CAPTURE (from);
        const auto played = playRegion (region, from, 6000);
        CHECK (std::abs (played.getSample (0, 0)) < 0.05f);
        auto worst = 0.0;

        for (int i = 2000; i < 6000; ++i)
            worst = std::max (worst, std::abs (played.getSample (0, i) - ideal (from + i)));

        CHECK (worst < 0.005);
    }
}

TEST_CASE ("A long audio file is read ahead of where it plays, each clip's start kept, and what it played long ago let go of")
{
    // At 1 kHz, 200 s is 7 chunks, of which it reads one ahead.
    std::atomic<std::uint64_t> blocks { 0 };
    engine::AudioFile file (std::make_unique<MemoryReader> (1, 1000.0, 200000), &blocks);
    const auto chunk = engine::AudioFile::chunkSamples;

    auto count = 64;
    CHECK (file.getSamples (0, 0, count) == nullptr); // not read yet: silence
    CHECK (count == 64);

    file.keep (3 * chunk + 10);
    CHECK (file.readNext()); // the clip's start first
    CHECK (file.readNext()); // then where it plays
    CHECK (! file.readNext());

    count = 64;
    const auto* samples = file.getSamples (0, 3 * chunk + 10, count);
    REQUIRE (samples != nullptr);
    CHECK (samples[0] == doctest::Approx (MemoryReader::at (0, 3 * chunk + 10)));

    count = 64;
    REQUIRE (file.getSamples (0, 0, count) != nullptr);

    // Played on to chunk 5: it reads that, and lets go of the first, which goes once the engine has played on.
    count = 64;
    CHECK (file.getSamples (0, 5 * chunk, count) == nullptr);
    CHECK (file.readNext());
    CHECK (! file.readNext());

    count = 64;
    CHECK (file.getSamples (0, 5 * chunk, count) != nullptr);
    count = 64;
    CHECK (file.getSamples (0, 3 * chunk + 10, count) != nullptr); // kept
    count = 64;
    CHECK (file.getSamples (0, 0, count) == nullptr);

    // The last samples of a chunk: as many as are left of it.
    count = 64;
    file.getSamples (0, 6 * chunk - 10, count);
    CHECK (count == 10);
}

TEST_CASE ("A lane's line eases as far as it bends: quick or slow at first up to half way, then held at one end, and at the ends a jump")
{
    const auto valueAt = [] (float bend, double along)
    {
        const engine::Curve curve ({ { 0.0, 0.0f, bend }, { 1.0, 1.0f, 0.0f } });
        return curve.getValueAt (along);
    };

    // Straight; and bent half way, the most a lane's handle bends it, a tenth of the way at the middle (slow at first) or
    // nine tenths (quick at first).
    CHECK (valueAt (0.0f, 0.3) == doctest::Approx (0.3f));
    CHECK (valueAt (0.5f, 0.5) == doctest::Approx (0.1f));
    CHECK (valueAt (-0.5f, 0.5) == doctest::Approx (0.9f));

    // A bend and the same the other way are each other's mirror image, and rise all the way.
    for (const auto bend : { -0.9f, -0.3f, 0.2f, 0.5f, 0.8f })
    {
        for (const auto along : { 0.1, 0.4, 0.7 })
            CHECK (valueAt (bend, along) == doctest::Approx (1.0f - valueAt (-bend, 1.0 - along)).epsilon (1.0e-5));

        for (auto along = 0.05; along < 1.0; along += 0.05)
            CHECK (valueAt (bend, along) >= valueAt (bend, along - 0.05));
    }

    // Further, it holds for a part of the line: slow at first, at its start; quick at first, at its end; eased in the rest.
    CHECK (valueAt (0.75f, 0.4) == doctest::Approx (0.0f));
    CHECK (valueAt (0.75f, 0.75) == doctest::Approx (0.1f));
    CHECK (valueAt (-0.75f, 0.6) == doctest::Approx (1.0f));

    // All the way, a hold and a jump: at the line's end, or at its start.
    CHECK (valueAt (1.0f, 0.99) == doctest::Approx (0.0f));
    CHECK (valueAt (-1.0f, 0.01) == doctest::Approx (1.0f));
}

TEST_CASE ("A fader follows its automation on the song's control points and in a line between, the same at any block size")
{
    // Three beats at 120 BPM in 4/4 (24000 samples a beat): a bent line, one bent past half way, and a hold with a jump.
    const engine::Curve curve (
        { { 0.0, 0.2f, 0.0f }, { 1.0, 1.0f, 0.3f }, { 2.0, 0.5f, -0.8f }, { 2.5, 0.9f, 1.0f }, { 3.0, 0.3f, 0.0f } });
    constexpr int length = 72000;
    constexpr double samplesPerBeat = 24000.0;

    const auto playThrough = [&curve] (int samplesInBlock)
    {
        engine::Channel channel;
        channel.prepare (rate);
        juce::AudioBuffer<float> audio (2, length);

        for (int channelIndex = 0; channelIndex < 2; ++channelIndex)
            juce::FloatVectorOperations::fill (audio.getWritePointer (channelIndex), 1.0f, length);

        for (int done = 0; done < length; done += samplesInBlock)
        {
            engine::Channel::Automation automation;
            automation.volume = &curve;
            automation.sample = done;
            automation.beat = done / samplesPerBeat;
            automation.beatsPerSample = 1.0 / samplesPerBeat;
            float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
            channel.process (channels, 2, std::min (samplesInBlock, length - done), automation);
        }

        return audio;
    };

    const auto reference = playThrough (256);

    for (const auto samplesInBlock : { 32, 100, 517 })
    {
        const auto other = playThrough (samplesInBlock);
        auto largest = 0.0f;

        for (int i = 0; i < length; ++i)
            largest = std::max (largest, std::abs (other.getSample (0, i) - reference.getSample (0, i)));

        CHECK (largest < 1.0e-5f);
    }

    // On each control point, exactly the lane's level (its positions on the fader's travel).
    for (const auto point : { 4800, 24000, 30016, 54016, 61984 })
    {
        const auto position = curve.getValueAt (point / samplesPerBeat);
        const auto gain = juce::Decibels::decibelsToGain (engine::faderPositionToDecibels (position), engine::Channel::silenceDb);
        CHECK (reference.getSample (0, point - 1) == doctest::Approx (gain).epsilon (1.0e-4));
    }
}

TEST_CASE ("A device's automation moves its parameter on the song's control points, wherever its blocks start, but while it is written")
{
    const auto& info = dev::Phaser::info;
    const auto depth = dev::findParameter (info, "depth");
    REQUIRE (depth >= 0);
    engine::DeviceNode node (info, std::make_unique<dev::Phaser>());
    node.getRunner().prepare (rate, 512);
    const auto curve = std::make_shared<const engine::Curve> (std::vector<engine::CurvePoint> { { 0.0, 0.0f, 0.0f }, { 1.0, 1.0f, 0.0f } });
    const engine::ParameterCurve automation[] = { { depth, curve } };
    juce::AudioBuffer<float> buffer (2, 512);
    constexpr double samplesPerBeat = 24000.0;
    std::int64_t sample = 0;

    for (const auto samplesInBlock : { 100, 37, 512, 1, 64, 300 })
    {
        buffer.clear();
        engine::DeviceRunner::Block block;
        block.isPlaying = true;
        block.sample = sample;
        block.automationBeat = block.beat = sample / samplesPerBeat;
        block.beatsPerSample = 1.0 / samplesPerBeat;
        node.process (buffer, 0, samplesInBlock, block, automation);
        sample += samplesInBlock;

        // Its last stretch's: on the control point at or before the block's last sample.
        const auto point = (sample - 1) / engine::controlPointSpacing * engine::controlPointSpacing;
        CHECK (node.getPlayedBaseValue (depth) == doctest::Approx (curve->getValueAt (point / samplesPerBeat)));
    }

    // Written by hand, it plays its value set by hand.
    node.setValue (depth, 0.25f);
    node.setWriting (depth, true);
    engine::DeviceRunner::Block block;
    block.isPlaying = true;
    block.sample = sample;
    block.automationBeat = block.beat = sample / samplesPerBeat;
    block.beatsPerSample = 1.0 / samplesPerBeat;
    node.process (buffer, 0, 512, block, automation);
    CHECK (node.getPlayedBaseValue (depth) == doctest::Approx (0.25f));
}

namespace
{
// A device that takes its blocks whole (as a plug-in in its helper does), and keeps the changes it was given, each at its
// sample from the song's start.
class WholeBlocks final : public dev::Processor
{
public:
    static inline const dev::ParameterInfo parameters[] = { { .id = "a", .name = "A", .defaultValue = 0.5f },
                                                            { .id = "b", .name = "B", .defaultValue = 0.5f } };
    static inline const dev::DeviceInfo info { .typeID = "testWholeBlocks", .name = "Whole Blocks", .parameters = parameters };

    std::vector<dev::ParameterChange> changes;
    int calls = 0;

    void prepare (double, int) override { changes.reserve (4096); }
    void reset() override {}
    bool takesWholeBlocks() const noexcept override { return true; }

    void process (const dev::ProcessContext& context) override
    {
        ++calls;

        for (const auto& change : context.changes)
            changes.push_back ({ change.parameter, (int) played + change.sample, change.value });

        played += context.numSamples;
    }

private:
    std::int64_t played = 0;
};
} // namespace

TEST_CASE ("A device that takes its blocks whole gets each in one call, its automation's values on the song's control points as changes")
{
    auto device = std::make_unique<WholeBlocks>();
    auto& whole = *device;
    engine::DeviceNode node (WholeBlocks::info, std::move (device));
    node.getRunner().prepare (rate, 512);
    node.setValue (0, 0.25f);
    const auto curve = std::make_shared<const engine::Curve> (std::vector<engine::CurvePoint> { { 0.0, 0.0f, 0.0f }, { 1.0, 1.0f, 0.0f } });
    const engine::ParameterCurve automation[] = { { 0, curve } };
    juce::AudioBuffer<float> buffer (2, 512);
    constexpr double samplesPerBeat = 24000.0;
    std::int64_t sample = 0;

    const auto play = [&] (int numSamples, std::span<const engine::ParameterCurve> automated)
    {
        engine::DeviceRunner::Block block;
        block.isPlaying = true;
        block.sample = sample;
        block.automationBeat = block.beat = sample / samplesPerBeat;
        block.beatsPerSample = 1.0 / samplesPerBeat;
        buffer.clear();
        node.process (buffer, 0, numSamples, block, automated);
        sample += numSamples;
    };

    // Blocks of any size, each in one call: A at its curve's value on the control point at or before each block's start,
    // then on each control point after, where the curve rises.
    for (const auto samplesInBlock : { 100, 37, 512, 1, 64, 300 })
        play (samplesInBlock, automation);

    CHECK (whole.calls == 6);
    std::set<int> samples;

    for (const auto& change : whole.changes)
    {
        CHECK (change.parameter == 0);
        const auto point = change.sample / engine::controlPointSpacing * engine::controlPointSpacing;
        CHECK (change.value == doctest::Approx (curve->getValueAt (point / samplesPerBeat)));
        samples.insert (change.sample);
    }

    for (int point = engine::controlPointSpacing; point < sample; point += engine::controlPointSpacing)
        CHECK (samples.contains (point));

    // Without its automation, A goes back to its value set by hand, once; B, never moved, never goes.
    whole.changes.clear();
    play (256, {});
    play (256, {});
    REQUIRE (whole.changes.size() == 1);
    CHECK (whole.changes[0].parameter == 0);
    CHECK (whole.changes[0].sample == (int) sample - 512);
    CHECK (whole.changes[0].value == doctest::Approx (0.25f));
}

namespace
{
// An instrument that takes its blocks whole and may be handed them early, as a plug-in instrument in its helper is: it
// plays a level while a note is held, worked out as it is handed the block, and notes what it was asked when.
class EarlyLevel final : public dev::Processor
{
public:
    static inline const dev::DeviceInfo info { .typeID = "testEarlyLevel", .name = "Early Level", .kind = dev::DeviceKind::instrument };

    EarlyLevel (bool handedEarly, float playedLevel, int trackNumber, std::vector<std::string>* calls)
        : early (handedEarly),
          level (playedLevel),
          track (trackNumber),
          log (calls)
    {
    }

    void prepare (double, int maximumBlockSize) override { played.setSize (2, maximumBlockSize); }
    void reset() override { held = 0; }
    bool takesWholeBlocks() const noexcept override { return true; }
    bool startsEarly() const noexcept override { return early; }

    void start (const dev::ProcessContext& context) override
    {
        note ("start");
        play (context);
        started = true;
    }

    void process (const dev::ProcessContext& context) override
    {
        note ("process");

        if (! std::exchange (started, false))
            play (context);

        for (int channel = 0; channel < context.numChannels; ++channel)
            juce::FloatVectorOperations::copy (context.channels[channel], played.getReadPointer (channel), context.numSamples);
    }

private:
    const bool early;
    const float level;
    const int track;
    std::vector<std::string>* log;
    juce::AudioBuffer<float> played;
    int held = 0;
    bool started = false;

    void note (const char* what)
    {
        if (log != nullptr)
            log->push_back (what + std::string (" ") + std::to_string (track));
    }

    void play (const dev::ProcessContext& context)
    {
        int done = 0;

        for (const auto& event : context.events)
        {
            fill (done, event.sample);
            done = event.sample;
            held += event.type == dev::NoteEvent::Type::noteOn ? 1 : event.type == dev::NoteEvent::Type::noteOff ? -1 : 0;
        }

        fill (done, context.numSamples);
    }

    void fill (int from, int to)
    {
        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (played.getWritePointer (channel) + from, held > 0 ? level : 0.0f, to - from);
    }
};
} // namespace

TEST_CASE ("Instruments handed their blocks early get each before the tracks play, and sound as they would one after another")
{
    // Three tracks of the instrument, each its notes, and the master: played by one thread and by four, handed their
    // blocks early and not.
    const auto render = [] (bool early, int numThreads, std::vector<std::string>* log)
    {
        auto plan = makeClickPlan();
        plan->metronome = false;

        for (int track = 1; track <= 3; ++track)
            plan->strips.push_back (makeTrack (track, EarlyLevel::info,
                                               std::make_unique<EarlyLevel> (early, 0.1f * (float) track, track, log),
                                               { { 0.5, 1.5, 60, 0.8f }, { 1.0 + 0.25 * track, 2.5, 64, 0.8f } }, blockSize));

        plan->strips.push_back (makeMaster (*plan, blockSize));
        engine::Engine player;
        player.prepare (rate, blockSize);
        player.setNumThreads (numThreads);
        player.setPlan (std::move (plan));
        return player.render (24000 * 3);
    };

    // Each block, every instrument handed it first, in the tracks' order; then each track takes what its instrument played.
    std::vector<std::string> log;
    log.reserve (6 * (24000 * 3 / blockSize + 1));
    const auto early = render (true, 1, &log);
    REQUIRE (log.size() >= 12);
    CHECK (log.size() % 6 == 0);

    for (size_t block = 0; block < 2; ++block)
        for (size_t i = 0; i < 6; ++i)
            CHECK (log[block * 6 + i] == (i < 3 ? "start " : "process ") + std::to_string (i % 3 + 1));

    // The same to the last bit as one after another, on one thread or four.
    const auto inTurn = render (false, 1, nullptr);
    const auto onFour = render (true, 4, nullptr);
    CHECK (early.getMagnitude (0, 0, early.getNumSamples()) > 0.1f); // they play

    for (const auto* other : { &inTurn, &onFour })
    {
        auto differing = 0;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < early.getNumSamples(); ++i)
                differing += juce::exactlyEqual (early.getSample (channel, i), other->getSample (channel, i)) ? 0 : 1;

        CHECK (differing == 0);
    }
}

namespace
{
// A modulator's parameters at their defaults, with some set.
std::vector<float> getModulatorValues (dev::ModulatorKind kind, std::initializer_list<std::pair<std::string_view, float>> set = {})
{
    std::vector<float> values;

    for (const auto& parameter : dev::getModulatorParameters (kind))
        values.push_back (parameter.defaultValue);

    for (const auto& [id, value] : set)
        values[(size_t) dev::findModulatorParameter (kind, id)] = value;

    return values;
}

// Where a song at 120 BPM in 4/4 is at a beat.
dev::SongPlace placeAt (double beat)
{
    return { beat / 2.0, beat, 120.0, 4 };
}
} // namespace

TEST_CASE ("An LFO runs free at its rate from its start, or follows the transport, or starts again with each note")
{
    using dev::ModulatorKind;
    constexpr double secondsPerPoint = engine::controlPointSpacing / rate;

    // Free at 2 Hz: a sine from its cycle's start, moving on by a control point's time each time.
    dev::Modulator lfo (ModulatorKind::lfo);
    lfo.prepare (rate, 1);
    const auto free = getModulatorValues (ModulatorKind::lfo, { { "rateType", 0.0f }, { "rate", 2.0f } });

    for (int point = 0; point < 3000; ++point)
    {
        const auto expected = (std::sin (juce::MathConstants<double>::twoPi * 2.0 * point * secondsPerPoint) + 1.0) / 2.0;
        REQUIRE (lfo.next (free, {}, placeAt (0.0), secondsPerPoint) == doctest::Approx (expected).epsilon (1.0e-4));
    }

    // Synced to the transport, a bar a cycle, a saw: where the song is in its bar, wherever it was before.
    const auto synced = getModulatorValues (ModulatorKind::lfo, { { "syncType", 1.0f }, { "wave", 2.0f } });

    for (const auto beat : { 0.0, 1.0, 2.5, 7.0, 13.0 })
        CHECK (lfo.next (synced, {}, placeAt (beat), secondsPerPoint) == doctest::Approx (std::fmod (beat / 4.0, 1.0)).epsilon (1.0e-5));

    // Synced to notes: a note starts its cycle again.
    lfo.reset();
    const auto noted = getModulatorValues (ModulatorKind::lfo, { { "syncType", 2.0f }, { "wave", 2.0f }, { "rateType", 0.0f } });

    for (int point = 0; point < 100; ++point)
        lfo.next (noted, {}, placeAt (0.0), secondsPerPoint);

    CHECK (lfo.next (noted, {}, placeAt (0.0), secondsPerPoint) > 0.05f);
    lfo.noteOn (60, 100);
    CHECK (lfo.next (noted, {}, placeAt (0.0), secondsPerPoint) == doctest::Approx (0.0f));
}

TEST_CASE ("A step sequencer, a breakpoint envelope and a random source move as they are set, the random one the same each time")
{
    using dev::ModulatorKind;
    constexpr double secondsPerPoint = engine::controlPointSpacing / rate;

    // Four steps a quarter note each, synced to the transport: the step the song is on.
    dev::Modulator steps (ModulatorKind::steps);
    steps.prepare (rate, 1);
    const auto stepValues = getModulatorValues (ModulatorKind::steps, { { "syncType", 1.0f }, { "numSteps", 4.0f }, { "rateType", 8.0f } });
    const std::vector<float> pattern { -1.0f, -0.25f, 0.5f, 1.0f };

    for (const auto& [beat, expected] : { std::pair (0.5, -1.0f), std::pair (1.2, -0.25f), std::pair (3.9, 1.0f), std::pair (6.0, 0.5f) })
        CHECK (steps.next (stepValues, pattern, placeAt (beat), secondsPerPoint) == doctest::Approx (expected));

    // The breakpoint envelope as it is made: up to 1 at a quarter of its cycle, to 0.5 at half, held, and down to 0.
    dev::Modulator envelope (ModulatorKind::breakpoints);
    envelope.prepare (rate, 1);
    const auto envelopeValues = getModulatorValues (ModulatorKind::breakpoints, { { "syncType", 1.0f } });

    for (const auto& [beat, expected] :
         { std::pair (0.5, 0.5f), std::pair (1.0, 1.0f), std::pair (1.5, 0.75f), std::pair (2.5, 0.5f), std::pair (3.5, 0.25f) })
        CHECK (envelope.next (envelopeValues, {}, placeAt (beat), secondsPerPoint) == doctest::Approx (expected).epsilon (1.0e-5));

    // Random values, the same each time for the same seed, another for another; held for a cycle each, from 0 to 1.
    const auto randomValues = getModulatorValues (ModulatorKind::random, { { "rateType", 0.0f }, { "rate", 50.0f } });
    const auto play = [&randomValues] (std::uint32_t seed)
    {
        dev::Modulator source (ModulatorKind::random);
        source.prepare (rate, seed);
        std::vector<float> values;

        for (int point = 0; point < 2000; ++point)
            values.push_back (source.next (randomValues, {}, placeAt (0.0), secondsPerPoint));

        return values;
    };

    const auto first = play (7);
    CHECK (first == play (7));
    CHECK (first != play (8));
    CHECK (std::all_of (first.begin(), first.end(), [] (float value) { return value >= 0.0f && value <= 1.0f; }));
    CHECK (std::set<float> (first.begin(), first.end()).size() > 20);
}

TEST_CASE ("An envelope follower rises and falls with what it hears, and a note follower follows the notes")
{
    using dev::ModulatorKind;

    // At 0.5 for a tenth of a second, its attack: it rises all but 37% of the way (a time constant: exp (-1) left); then in
    // silence, it falls.
    dev::Modulator follower (ModulatorKind::envelopeFollower);
    follower.prepare (rate, 1);
    const auto values = getModulatorValues (ModulatorKind::envelopeFollower);
    juce::AudioBuffer<float> sound (2, 4800);
    sound.clear();

    for (int channel = 0; channel < 2; ++channel)
        juce::FloatVectorOperations::fill (sound.getWritePointer (channel), 0.5f, 4800);

    follower.listen (sound.getArrayOfReadPointers(), 2, 4800, values);
    CHECK (follower.next (values, {}, placeAt (0.0), 0.0) == doctest::Approx (0.5 * (1.0 - std::exp (-1.0))).epsilon (0.01));

    sound.clear();
    follower.listen (sound.getArrayOfReadPointers(), 2, 4800, values);
    const auto fallen = follower.next (values, {}, placeAt (0.0), 0.0);
    CHECK (fallen < 0.45f);
    CHECK (fallen > 0.2f);

    // The note follower: -1 for the lowest note, 1 for the highest, in a line between; relative, a root and a spread.
    dev::Modulator notes (ModulatorKind::noteFollower);
    notes.prepare (rate, 1);
    const auto absolute = getModulatorValues (ModulatorKind::noteFollower);
    const auto relative = getModulatorValues (ModulatorKind::noteFollower, { { "mode", 1.0f } });

    for (const auto note : { 0, 32, 64, 96, 127 })
    {
        notes.noteOn (note, 100);
        CHECK (notes.next (absolute, {}, placeAt (0.0), 0.0) == doctest::Approx ((float) note / 63.5f - 1.0f));
    }

    notes.noteOn (72, 100);
    CHECK (notes.next (relative, {}, placeAt (0.0), 0.0) == doctest::Approx (1.0f));
}

TEST_CASE ("A modulator moves a fader on the song's control points, the same at any block size")
{
    // A note held for two beats, at 0.5, its volume swung by a bipolar LFO at 3 Hz by half the fader's travel.
    const auto play = [] (int samplesInBlock)
    {
        auto plan = makeClickPlan();
        plan->metronome = false;
        auto track = makeTrack (1, HeldLevel::info, std::make_unique<HeldLevel>(), { { 0.0, 2.0, 60, 0.8f } }, samplesInBlock);
        auto lfo = std::make_shared<engine::ModulatorNode> (dev::ModulatorKind::lfo);
        lfo->prepare (rate, samplesInBlock, 1);

        for (const auto& [id, value] : { std::pair ("rateType", 0.0f), std::pair ("rate", 3.0f), std::pair ("bipolar", 1.0f) })
            lfo->setParameter (dev::findModulatorParameter (dev::ModulatorKind::lfo, id), value);

        track.modulators.push_back (lfo);
        track.volumeModulation.push_back ({ 0, 0.5f });
        plan->strips.push_back (std::move (track));
        plan->strips.push_back (makeMaster (*plan, samplesInBlock));

        engine::Engine player;
        player.prepare (rate, samplesInBlock);
        player.setPlan (std::move (plan));
        return player.render (48000);
    };

    // It swings: loud at the top of the LFO's cycle (1/12 s), quiet at its bottom (1/4 s).
    const auto reference = play (256);
    CHECK (reference.getMagnitude (0, 3800, 400) > 0.9f);
    CHECK (reference.getMagnitude (0, 11800, 400) < 0.1f);

    for (const auto samplesInBlock : { 64, 333 })
    {
        const auto other = play (samplesInBlock);
        auto largest = 0.0f;

        for (int i = 0; i < 48000; ++i)
            largest = std::max (largest, std::abs (other.getSample (0, i) - reference.getSample (0, i)));

        CHECK (largest < 1.0e-5f);
    }
}

TEST_CASE ("The engine's loudness meter reads a sine as EBU R128 does, gates silence out, and starts again when reset")
{
    // A 997 Hz sine at a tenth of full scale on both channels is -20 LUFS (BS.1770's K-weighting is +0.69 dB there,
    // which its -0.691 takes back), its true peak -20 dBTP.
    engine::LoudnessMeter meter;
    meter.prepare (rate, 512);
    juce::AudioBuffer<float> sine (2, 512);
    const auto play = [&meter, &sine] (double seconds, float amplitude, int& sample)
    {
        for (int done = 0; done < (int) (seconds * rate); done += 512)
        {
            for (int i = 0; i < 512; ++i, ++sample)
                for (int channel = 0; channel < 2; ++channel)
                    sine.setSample (channel, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * 997.0 * sample / rate));

            meter.process (sine.getArrayOfReadPointers(), 2, 512);
        }
    };

    int sample = 0;
    CHECK_FALSE (meter.getReadings().momentary.has_value());
    play (4.0, 0.1f, sample);
    const auto loud = meter.getReadings();
    REQUIRE (loud.momentary.has_value());
    REQUIRE (loud.shortTerm.has_value());
    REQUIRE (loud.integrated.has_value());
    REQUIRE (loud.truePeak.has_value());
    CHECK (*loud.momentary == doctest::Approx (-20.0f).epsilon (0.005));
    CHECK (*loud.shortTerm == doctest::Approx (-20.0f).epsilon (0.005));
    CHECK (*loud.integrated == doctest::Approx (-20.0f).epsilon (0.005));
    CHECK (*loud.truePeak == doctest::Approx (-20.0f).epsilon (0.01));

    // Silence below the gate leaves the integrated loudness as it was, but for the gating blocks that overlap the sine's
    // end, which R128 counts; and 10 dB quieter for as long as it was loud, it counts (it is within 10 LU of the rest),
    // between the two.
    play (2.0, 0.0f, sample);
    CHECK (*meter.getReadings().integrated == doctest::Approx (-20.0f).epsilon (0.01));
    play (4.0, 0.0316f, sample);
    const auto between = *meter.getReadings().integrated;
    CHECK (between < -20.5f);
    CHECK (between > -29.5f);

    // Reset: only what it hears after it counts, the loud start long gone.
    meter.reset();
    play (4.0, 0.0316f, sample);
    const auto after = meter.getReadings();
    REQUIRE (after.integrated.has_value());
    REQUIRE (after.truePeak.has_value());
    CHECK (*after.integrated == doctest::Approx (-30.0f).epsilon (0.005));
    CHECK (*after.truePeak == doctest::Approx (-30.0f).epsilon (0.01));
}

TEST_CASE ("The engine's workers run each task of a job once, on the audio thread and themselves, job after job")
{
    engine::Workers workers;
    workers.setNumThreads (4);
    std::array<std::atomic<int>, 64> runs {};

    struct Job
    {
        std::array<std::atomic<int>, 64>* runs;
        std::atomic<int> total { 0 };
    } job { &runs };

    for (int round = 0; round < 2000; ++round)
    {
        for (auto& count : runs)
            count.store (0);

        const auto numTasks = 1 + round % 64;
        workers.run (
            numTasks,
            [] (void* context, int index) noexcept
            {
                auto& running = *static_cast<Job*> (context);
                (*running.runs)[(size_t) index].fetch_add (1);
                running.total.fetch_add (1);
            },
            &job);

        for (int i = 0; i < 64; ++i)
            REQUIRE (runs[(size_t) i].load() == (i < numTasks ? 1 : 0));
    }
}

TEST_CASE ("The engine's workers take a long job's tasks beside the thread that starts it, wherever real-time threads aren't allowed too")
{
    // Tasks of 20 microseconds each, long enough for a worker asleep to wake: some run on a worker. (On Linux without
    // real-time rights, JUCE started no thread at all for a real-time one, and the audio thread took every task.)
    engine::Workers workers;
    workers.setNumThreads (4);

    struct Job
    {
        std::thread::id caller;
        std::atomic<int> elsewhere { 0 };
    } job;

    job.caller = std::this_thread::get_id();

    for (int round = 0; round < 50; ++round)
    {
        workers.run (
            64,
            [] (void* context, int) noexcept
            {
                auto& running = *static_cast<Job*> (context);
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds (20);

                while (std::chrono::steady_clock::now() < until)
                {
                }

                if (std::this_thread::get_id() != running.caller)
                    running.elsewhere.fetch_add (1);
            },
            &job);
    }

    CHECK (job.elsewhere.load() > 0);
}

#if JUCE_WINDOWS
extern "C" __declspec (dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec (dllimport) unsigned long __stdcall GetPriorityClass (void*);

TEST_CASE ("The engine's workers leave the app's priority class as it is, so that none of its other threads plays above other programs")
{
    constexpr unsigned long high = 0x80, realtime = 0x100; // HIGH_PRIORITY_CLASS, REALTIME_PRIORITY_CLASS
    engine::Workers workers;
    workers.setNumThreads (4);
    const auto priorityClass = GetPriorityClass (GetCurrentProcess());
    CHECK (priorityClass != high);
    CHECK (priorityClass != realtime);
}
#endif
