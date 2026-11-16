// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's engine (src/engine/) between what the message thread asks of it and what it plays: the transport asked for
// faster than blocks come, live notes and the transport while it has no plan (a render has the song's plug-ins), a mono
// output, the workers' denormals, an instrument holding a silent note, an audio clip changed under the playhead, and the
// plans a stalled block still plays. Each was a bug the quality audit found (known bugs 79, 80, 82, 81, 37, 77 and 83).
#include "HostedDevice.h"
#include "TestAudioDevice.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "engine/audio/AudioRegion.h"
#include "engine/audio/FileReader.h"
#include "engine/core/Engine.h"
#include "engine/core/Workers.h"
#include "engine/nodes/DeviceNode.h"
#include "model/devices/internal/Devices.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace engine = phazite::engine;
constexpr double rate = 48000.0;
constexpr int blockSize = 512;

// A plan at 120 BPM in 4/4, at the tests' rate.
std::unique_ptr<engine::Plan> makePlan()
{
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, 120.0 } }, { { 0.0, 4, 4 } }, rate);
    plan->maximumBlockSize = blockSize;
    return plan;
}

// A device of a test's own, made into a node prepared at the tests' rate.
std::shared_ptr<engine::DeviceNode> makeNode (const dev::DeviceInfo& info, std::unique_ptr<dev::Processor> processor)
{
    auto node = std::make_shared<engine::DeviceNode> (info, std::move (processor));
    node->getRunner().prepare (rate, blockSize);
    return node;
}

std::shared_ptr<engine::StripState> makeStripState()
{
    auto state = std::make_shared<engine::StripState>();
    state->prepare (rate, blockSize);
    return state;
}

// Adds a track (ID 1) that plays its notes on an instrument, or nothing, with its audio clips, and the master after it, with
// a device on it if given.
void addTrack (engine::Plan& plan, std::shared_ptr<engine::DeviceNode> instrument, std::vector<engine::NoteSequence::Note> notes = {},
               std::vector<engine::AudioRegion> audio = {}, std::shared_ptr<engine::DeviceNode> onMaster = {})
{
    engine::Plan::Strip track, master;
    track.id = 1;
    track.state = makeStripState();
    master.state = makeStripState();

    if (instrument != nullptr)
    {
        track.devices.push_back ({ std::move (instrument), {} });
        track.hasInstrument = true;
    }

    if (! notes.empty())
        track.notes = std::make_shared<const engine::NoteSequence> (std::move (notes), std::vector<engine::NoteSequence::Control> {});

    track.audio = std::move (audio);

    if (onMaster != nullptr)
        master.devices.push_back ({ std::move (onMaster), {} });

    master.inputs.push_back ({ 0, nullptr });
    plan.strips.push_back (std::move (track));
    plan.strips.push_back (std::move (master));
}

// Plays so many samples in the tests' blocks into two channels (or one).
juce::AudioBuffer<float> playBlocks (engine::Engine& player, int numSamples, int numOutputs = 2)
{
    juce::AudioBuffer<float> played (2, numSamples);
    played.clear();

    for (int done = 0; done < numSamples; done += blockSize)
    {
        float* channels[] = { played.getWritePointer (0, done), played.getWritePointer (1, done) };
        player.process (channels, numOutputs, std::min (blockSize, numSamples - done));
    }

    return played;
}

// An instrument that plays a level of 0.5 while it holds a note (on its left side alone, or its right alone, if asked),
// and keeps each event it was given.
struct Holder final : dev::Processor
{
    static constexpr dev::DeviceInfo info { .typeID = "testHolder", .name = "Holder", .kind = dev::DeviceKind::instrument };
    enum class Side
    {
        both,
        left,
        right
    };

    explicit Holder (Side s = Side::both)
        : side (s)
    {
    }

    void prepare (double, int) override { received.reserve (8192); }
    void reset() override { held = 0; }

    void process (const dev::ProcessContext& block) override
    {
        auto done = 0;

        for (const auto& event : block.events)
        {
            fill (block, done, event.sample);
            done = event.sample;
            received.push_back (event);

            if (event.type == dev::NoteEvent::Type::noteOn)
                ++held;
            else if (event.type == dev::NoteEvent::Type::noteOff)
                held = std::max (0, held - 1);
            else if (event.type == dev::NoteEvent::Type::allNotesOff)
                held = 0;
        }

        fill (block, done, block.numSamples);
    }

    void fill (const dev::ProcessContext& block, int from, int to) const
    {
        for (int channel = 0; channel < block.numChannels; ++channel)
        {
            const auto plays = side == Side::both || (side == Side::left) == (channel == 0);
            juce::FloatVectorOperations::fill (block.channels[channel] + from, plays && held > 0 ? 0.5f : 0.0f, to - from);
        }
    }

    int countReceived (dev::NoteEvent::Type type) const
    {
        return (int) std::count_if (received.begin(), received.end(), [type] (const dev::NoteEvent& event) { return event.type == type; });
    }

    const Side side;
    int held = 0;
    std::vector<dev::NoteEvent> received;
};
} // namespace

//==============================================================================
TEST_CASE ("The transport asked for faster than blocks come takes the state last asked, and the playhead follows it")
{
    // 200 jumps between two blocks, which a queue of 64 commands couldn't hold (known bug 79): the playhead stayed where the
    // last was asked for, while the song played on from the 64th; then a stop after another 200, which it lost.
    engine::Engine player;
    player.prepare (rate, blockSize);
    player.setPlan (makePlan());
    playBlocks (player, blockSize);

    for (int i = 1; i <= 200; ++i)
        player.locate (i * 0.25);

    player.play();
    playBlocks (player, 10 * blockSize);
    const auto beatsPerBlock = 2.0 * blockSize / rate;
    CHECK (player.getPlayheadBeat() >= 50.0 + 8.0 * beatsPerBlock); // it plays on from beat 50, and is shown doing so

    for (int i = 1; i <= 200; ++i)
        player.locate (i * 0.125);

    player.stop();
    playBlocks (player, 2 * blockSize);
    const auto stoppedAt = player.getPlayheadBeat();
    CHECK (stoppedAt == doctest::Approx (25.0));
    playBlocks (player, 4 * blockSize);
    CHECK (player.getPlayheadBeat() == doctest::Approx (stoppedAt)); // stopped: it doesn't move
}

TEST_CASE ("While the engine has no plan, live notes are dropped as they come but their ends are kept, and the transport waits")
{
    // A render has the song's plug-ins, so the engine plays no plan meanwhile (known bug 80): the notes played then sounded
    // as one burst once the plan came back, the ends past 1,024 events were lost, and the transport past 63 commands froze.
    engine::Engine player;
    player.prepare (rate, blockSize);
    auto holder = std::make_unique<Holder>();
    auto* instrument = holder.get();
    const auto node = makeNode (Holder::info, std::move (holder));
    const auto givePlan = [&player, &node]
    {
        auto plan = makePlan();
        addTrack (*plan, node);
        player.setPlan (std::move (plan));
    };

    givePlan();
    player.playLive (1, { dev::NoteEvent::Type::noteOn, 0, 60, 1.0f });
    playBlocks (player, 2 * blockSize);
    REQUIRE (instrument->held == 1);

    // The render borrows the plug-ins: blocks without a plan, while notes and the transport are played.
    player.setPlan (nullptr);
    playBlocks (player, blockSize);

    for (int chunk = 0; chunk < 11; ++chunk)
    {
        for (int i = 0; i < 100; ++i)
            player.playLive (1, { dev::NoteEvent::Type::noteOn, 0, 30 + i % 20, 1.0f });

        playBlocks (player, blockSize);
    }

    player.playLive (1, { dev::NoteEvent::Type::noteOff, 0, 60, 0.0f });
    playBlocks (player, blockSize);

    for (int i = 0; i < 100; ++i)
        player.locate (i + 1.0);

    playBlocks (player, blockSize);

    // The plan back: no burst of notes, the held one ended, and the playhead where it was last put.
    const auto before = instrument->countReceived (dev::NoteEvent::Type::noteOn);
    givePlan();
    const auto played = playBlocks (player, 2 * blockSize);
    CHECK (instrument->countReceived (dev::NoteEvent::Type::noteOn) == before);
    CHECK (instrument->held == 0);
    CHECK (played.getMagnitude (0, 2 * blockSize) < 1.0e-6f);
    CHECK (player.getPlayheadBeat() == doctest::Approx (100.0));
}

TEST_CASE ("Live MIDI leaving a track ends the notes and the pedal held live there, and nothing of the song's")
{
    // Live MIDI moves to another track (another track selected, a MIDI input unplugged) while the song plays a held note on
    // the track it played: an all-notes-off there cut the song's note too, which nothing started again, and a pedal-up
    // let go of the song's pedal. Now only the notes held live end, each as a note's end, which a take and Capture record
    // as the end of the note played; and the pedal only if it is held down live.
    using Type = dev::NoteEvent::Type;
    engine::Engine player;
    player.prepare (rate, blockSize);
    auto holder = std::make_unique<Holder>();
    auto* instrument = holder.get();
    auto plan = makePlan();
    addTrack (*plan, makeNode (Holder::info, std::move (holder)), { { 0.0, 16.0, 60, 1.0f } });
    player.setPlan (std::move (plan));
    player.play();
    playBlocks (player, 4 * blockSize);
    REQUIRE (instrument->held == 1);

    std::vector<engine::Engine::PlayedEvent> played (64);
    const auto takePlayed = [&player, &played]
    {
        std::vector<engine::Engine::PlayedEvent> taken (played.begin(), played.begin() + player.takePlayed (played));
        return taken;
    };

    takePlayed();
    player.playLive (1, { Type::noteOn, 0, 64, 1.0f });
    playBlocks (player, 4 * blockSize);
    REQUIRE (instrument->held == 2);

    player.endLive (1);
    playBlocks (player, 4 * blockSize);
    CHECK (instrument->held == 1); // the song's note sounds on
    CHECK (instrument->countReceived (Type::allNotesOff) == 0);
    CHECK (instrument->countReceived (Type::controller) == 0); // no pedal held live: none let go

    const auto recorded = takePlayed();
    REQUIRE (recorded.size() == 2);
    CHECK (recorded[1].event.type == Type::noteOff);
    CHECK (recorded[1].event.number == 64);

    // A pedal held live is let go.
    constexpr int sustainPedal = 64;
    player.playLive (1, { Type::controller, 0, sustainPedal, 1.0f });
    playBlocks (player, 2 * blockSize);
    player.endLive (1);
    playBlocks (player, 2 * blockSize);
    REQUIRE (instrument->countReceived (Type::controller) == 2);
    CHECK (instrument->received.back().type == Type::controller);
    CHECK (instrument->received.back().value < 0.5f);
    CHECK (instrument->held == 1);
}

TEST_CASE ("One output channel plays both sides of the mix, each at half")
{
    // On a mono device, or with one output chosen, the left side alone went out, and what was panned right was lost (known
    // bug 82).
    for (const auto side : { Holder::Side::left, Holder::Side::right })
    {
        CAPTURE ((int) side);
        const auto play = [side] (int numOutputs)
        {
            engine::Engine player;
            player.prepare (rate, blockSize);
            auto plan = makePlan();
            addTrack (*plan, makeNode (Holder::info, std::make_unique<Holder> (side)), { { 0.0, 4.0, 60, 1.0f } });
            player.setPlan (std::move (plan));
            player.play();
            return playBlocks (player, 4 * blockSize, numOutputs);
        };

        const auto stereo = play (2);
        const auto mono = play (1);
        const auto expected = 0.5f * (stereo.getSample (0, 3 * blockSize) + stereo.getSample (1, 3 * blockSize));
        REQUIRE (expected > 0.1f);
        CHECK (mono.getSample (0, 3 * blockSize) == doctest::Approx (expected));
    }
}

TEST_CASE ("The engine's workers flush denormals to zero, as the audio thread does")
{
    // What the engine itself plays on a worker (a container's crossover, a modulator's follower) slowed down as it died away,
    // and could play otherwise than on the audio thread (known bug 81). Jobs of two tasks, each waiting a while for the other
    // to start, until a worker has taken one (a worker just started may miss the first jobs, which the calling thread then
    // plays alone).
    engine::Workers workers;
    workers.setNumThreads (2);

    struct Job
    {
        std::atomic<int> started { 0 };
        std::thread::id caller;
        std::atomic<int> onWorkers { 0 }, flushedOnWorkers { 0 };
    } job;

    job.caller = std::this_thread::get_id();

    for (int tries = 0; tries < 1000 && job.onWorkers.load() == 0; ++tries)
    {
        job.started = 0;
        workers.run (
            2,
            [] (void* context, int) noexcept
            {
                auto& running = *static_cast<Job*> (context);
                running.started.fetch_add (1);

                for (int waits = 0; waits < 100000 && running.started.load() < 2; ++waits)
                    std::this_thread::yield();

                if (std::this_thread::get_id() != running.caller)
                {
                    running.onWorkers.fetch_add (1);

                    if (juce::FloatVectorOperations::areDenormalsDisabled())
                        running.flushedOnWorkers.fetch_add (1);
                }
            },
            &job);
    }

    REQUIRE (job.onWorkers.load() > 0);
    CHECK (job.flushedOnWorkers.load() == job.onWorkers.load());
}

namespace
{
// An instrument whose note is silent for its first second while held, then sounds at 0.5: as a held pad under a slow LFO
// that closes its filter, or a sample with a gap in it, is.
struct LateSounder final : dev::Processor
{
    static constexpr dev::DeviceInfo info { .typeID = "testLateSounder", .name = "Late Sounder", .kind = dev::DeviceKind::instrument };

    void prepare (double sampleRate, int) override { silentFor = (int) sampleRate; }
    void reset() override { held = 0; }

    void process (const dev::ProcessContext& block) override
    {
        // Each note held as often as it was started (a key held live over the same note in the song is two), from the
        // first started after none was held.
        for (const auto& event : block.events)
        {
            if (event.type == dev::NoteEvent::Type::noteOn)
            {
                if (held++ == 0)
                    sinceStart = 0;
            }
            else if (event.type == dev::NoteEvent::Type::noteOff)
            {
                held = std::max (0, held - 1);
            }
            else if (event.type == dev::NoteEvent::Type::allNotesOff)
            {
                held = 0;
            }
        }

        for (int i = 0; i < block.numSamples; ++i, ++sinceStart)
            for (int channel = 0; channel < block.numChannels; ++channel)
                block.channels[channel][i] = held > 0 && sinceStart >= silentFor ? 0.5f : 0.0f;
    }

    int held = 0, sinceStart = 0, silentFor = 48000;
};
} // namespace

TEST_CASE ("An instrument holding a note doesn't sleep however long the note is silent")
{
    // A note held for four seconds, silent for its first: longer than a device stays awake in silence, so the instrument
    // slept and played nothing more until its next note (known bug 37).
    engine::Engine player;
    player.prepare (rate, blockSize);
    auto plan = makePlan();
    addTrack (*plan, makeNode (LateSounder::info, std::make_unique<LateSounder>()), { { 0.0, 8.0, 60, 1.0f } });
    player.setPlan (std::move (plan));
    player.play();
    const auto played = playBlocks (player, (int) (2.0 * rate));
    CHECK (played.getSample (0, (int) (1.5 * rate)) > 0.1f);
}

TEST_CASE ("An instrument holding a note twice, live and in the song, doesn't sleep when one of them lets go")
{
    // The song holds C4 for four seconds, and a key plays the same C4 live for a moment: its end left the runner holding
    // no note, and the instrument slept with the song's note still down.
    engine::Engine player;
    player.prepare (rate, blockSize);
    auto plan = makePlan();
    addTrack (*plan, makeNode (LateSounder::info, std::make_unique<LateSounder>()), { { 0.0, 8.0, 60, 1.0f } });
    player.setPlan (std::move (plan));
    player.play();
    playBlocks (player, 10 * blockSize);
    player.playLive (1, { dev::NoteEvent::Type::noteOn, 0, 60, 1.0f });
    playBlocks (player, 10 * blockSize);
    player.playLive (1, { dev::NoteEvent::Type::noteOff, 0, 60, 0.0f });
    const auto played = playBlocks (player, (int) (2.0 * rate));
    CHECK (played.getSample (0, (int) (1.5 * rate)) > 0.1f);
}

namespace
{
// A file's samples made up as they are read: each the sample's number (from a first of its own) in a thousandths' cycle,
// so that every stretch of it tells where in the file it is from. Mono, at the tests' rate.
struct CountingReader final : engine::SampleReader
{
    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return rate; }
    std::int64_t getLength() const override { return 10 * (std::int64_t) rate; }
    explicit CountingReader (std::int64_t firstSample = 0)
        : first (firstSample)
    {
    }

    static float at (std::int64_t sample) { return (float) (sample % 1000) / 1000.0f; }
    const std::int64_t first;

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
    {
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (0, i, at (first + start + i));

        return true;
    }
};
} // namespace

TEST_CASE ("An audio clip moved, slipped or rendered again under the playhead plays from where its new place has it")
{
    // A region's state is kept by its clip's ID from plan to plan: it played on from where it was in its file until the
    // next stop or jump (known bug 77). Each change, half a second into the clip, as a new plan; after the fade in a jump
    // has (40 samples), every sample is the file's where the region now has it.
    struct Change
    {
        const char* name;
        std::int64_t start;
        double fileStart;
        bool otherFile;
    };

    for (const auto& change : { Change { "moved later", 1234, 0.0, false }, Change { "slipped in its file", 0, 3210.0, false },
                                Change { "rendered again", 0, 0.0, true } })
    {
        CAPTURE (change.name);
        engine::Engine player;
        player.prepare (rate, blockSize);
        player.setPlayingAsRender (true); // its file read as it plays
        const auto makeFile = [&player] (std::int64_t first)
        { return std::make_shared<engine::AudioFile> (std::make_unique<CountingReader> (first), &player.getBlocksPlayed()); };
        auto file = makeFile (0);
        const auto state = std::make_shared<engine::RegionState> (*file, rate);

        const auto givePlan = [&player, &state] (std::shared_ptr<engine::AudioFile> played, std::int64_t start, double fileStart)
        {
            engine::AudioRegion region;
            region.file = std::move (played);
            region.state = state;
            region.start = start;
            region.length = 8 * (std::int64_t) rate;
            region.fileStart = fileStart;
            auto plan = makePlan();
            addTrack (*plan, nullptr, {}, { region });
            player.setPlan (std::move (plan));
        };

        givePlan (file, 0, 0.0);
        player.play();
        const auto changedAt = 47 * blockSize; // half a second in
        playBlocks (player, changedAt);
        givePlan (change.otherFile ? makeFile (500) : file, change.start, change.fileStart);
        const auto played = playBlocks (player, 4 * blockSize);

        auto wrong = 0, firstWrong = -1;

        for (int i = 48; i < played.getNumSamples(); ++i)
        {
            const auto songSample = changedAt + i;
            const auto expected = CountingReader::at ((std::int64_t) ((double) (songSample - change.start) + change.fileStart)
                                                      + (change.otherFile ? 500 : 0));

            if (std::abs (played.getSample (0, i) - expected) > 1.0e-4f && wrong++ == 0)
                firstWrong = i;
        }

        CAPTURE (firstWrong);
        CHECK (wrong == 0);
    }
}

namespace
{
// A file of one level throughout, mono at the tests' rate, ten seconds long.
struct LevelReader final : engine::SampleReader
{
    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return rate; }
    std::int64_t getLength() const override { return 10 * (std::int64_t) rate; }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t, int numSamples) override
    {
        juce::FloatVectorOperations::fill (buffer.getWritePointer (0), 0.5f, numSamples);
        return true;
    }
};

// An engine playing as a render does (its files read as they play), and a plan of a track whose audio clips play a file
// of one level, each from a sample for so many (a state of its own each), with a loop if given.
struct LevelClips
{
    LevelClips()
    {
        player.prepare (rate, blockSize);
        player.setPlayingAsRender (true);
    }

    void givePlan (std::vector<std::pair<std::int64_t, std::int64_t>> clips, std::optional<engine::Plan::BeatRange> loop = {})
    {
        std::vector<engine::AudioRegion> regions;

        for (const auto& [start, length] : clips)
        {
            engine::AudioRegion region;
            region.file = file;
            region.state = std::make_shared<engine::RegionState> (*file, rate);
            region.start = start;
            region.length = length;
            regions.push_back (std::move (region));
        }

        auto plan = makePlan();
        plan->loop = loop;
        addTrack (*plan, nullptr, {}, std::move (regions));
        player.setPlan (std::move (plan));
    }

    // So many samples in blocks of 64, shorter than a fade.
    juce::AudioBuffer<float> play (int numSamples)
    {
        juce::AudioBuffer<float> played (2, numSamples);
        played.clear();

        for (int done = 0; done < numSamples; done += 64)
        {
            float* channels[] = { played.getWritePointer (0, done), played.getWritePointer (1, done) };
            player.process (channels, 2, std::min (64, numSamples - done));
        }

        return played;
    }

    engine::Engine player;
    std::shared_ptr<engine::AudioFile> file =
        std::make_shared<engine::AudioFile> (std::make_unique<LevelReader>(), &player.getBlocksPlayed());
};

// The largest change from one sample to the next, on the first side.
float findLargestStep (const juce::AudioBuffer<float>& played)
{
    auto largest = 0.0f;

    for (int i = 1; i < played.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (played.getSample (0, i) - played.getSample (0, i - 1)));

    return largest;
}
} // namespace

TEST_CASE ("A stop's fade plays on only the audio clips that were playing")
{
    // The audio clips fade out after a stop from where the song stopped; it played every clip there, so that one starting
    // a millisecond after the stop played its start as a tick, a new plan given with the stop (a preview's next loop)
    // played from where the last stopped, and a stop while a jump faded out faded out the clips where it would land.
    const auto stopAt = 20 * 64;

    SUBCASE ("A clip that starts just after the stop")
    {
        LevelClips clips;
        clips.givePlan ({ { stopAt + 48, (std::int64_t) rate } });
        clips.player.play();
        clips.play (stopAt);
        clips.player.stop();
        CHECK (clips.play (20 * 64).getMagnitude (0, 20 * 64) == 0.0f);
    }

    SUBCASE ("A new plan given with the stop")
    {
        LevelClips clips;
        clips.givePlan ({});
        clips.player.play();
        clips.play (stopAt);
        clips.player.stop();
        clips.givePlan ({ { 0, (std::int64_t) rate } });
        clips.player.locate (0.0);
        CHECK (clips.play (20 * 64).getMagnitude (0, 20 * 64) == 0.0f);
    }

    SUBCASE ("A stop while a jump fades out")
    {
        LevelClips clips;
        clips.givePlan ({ { 3 * (std::int64_t) rate, (std::int64_t) rate } }); // where the jump lands, at beat 6
        clips.player.play();
        clips.play (stopAt);
        clips.player.locate (6.0);
        clips.play (64);
        clips.player.stop();
        CHECK (clips.play (20 * 64).getMagnitude (0, 20 * 64) == 0.0f);
    }
}

TEST_CASE ("A jump during the fade in after another fades out from where the fade in was")
{
    // Two jumps 4 ms apart: the second faded out from full level, though what played was still a third of the way in.
    LevelClips clips;
    clips.givePlan ({ { 0, 8 * (std::int64_t) rate } });
    clips.player.play();
    auto played = clips.play ((int) rate / 2);
    clips.player.locate (8.0);
    const auto first = clips.play (3 * 64); // its fade out (144 samples), and 48 of its fade in
    clips.player.locate (12.0);
    const auto second = clips.play (20 * 64);

    juce::AudioBuffer<float> whole (1, played.getNumSamples() + first.getNumSamples() + second.getNumSamples());
    auto at = 0;

    for (const auto* part : std::array<const juce::AudioBuffer<float>*, 3> { &played, &first, &second })
    {
        whole.copyFrom (0, at, *part, 0, 0, part->getNumSamples());
        at += part->getNumSamples();
    }

    CHECK (findLargestStep (whole) < 0.05f);
}

TEST_CASE ("An audio clip that ends within a sample of the loop's end isn't faded out before it")
{
    // A preview's loop whose stretched length rounds one sample past the loop's end: it was cut at the loop's end, and
    // faded out every time round, though it plays on whole into its start again.
    LevelClips clips;
    const auto loopEnd = (std::int64_t) rate / 2; // beat 1 at 120 BPM
    clips.givePlan ({ { 0, loopEnd + 1 } }, engine::Plan::BeatRange { 0.0, 1.0 });
    clips.player.play();
    const auto played = clips.play ((int) loopEnd + 2000);
    CHECK (played.getSample (0, (int) loopEnd - 1) == doctest::Approx (0.5f));
    CHECK (findLargestStep (played) < 0.05f);
}

namespace
{
// An effect that waits, in its block, until it is let go: as a block that stalls in a driver, under a debugger, or with
// the machine suspended. Static, as the test's thread and the block's share it.
struct Staller final : dev::Processor
{
    static constexpr const char* typeID = "testStaller";
    static constexpr dev::DeviceInfo info { .typeID = typeID, .name = "Staller", .kind = dev::DeviceKind::effect };
    static inline std::atomic<bool> stalls { false }, stalled { false }, inBlock { false };

    void prepare (double, int) override {}
    void reset() override {}
    bool sleepsOnSilence() const noexcept override { return false; }

    void process (const dev::ProcessContext&) override
    {
        if (! stalls.load())
            return;

        inBlock.store (true);
        stalled.store (true);

        while (stalls.load())
            std::this_thread::yield();

        inBlock.store (false);
    }
};
} // namespace

TEST_CASE ("The plans a session lets go of while a block stalls are freed only once the block has finished")
{
    // letGoOfPlans() waited for two blocks or half a second, then freed the plans, though a block that took longer still
    // played one (known bug 83). A plan whose master's state only it holds, played by the tests' audio device on a thread
    // of its own, stalls in a block; meanwhile the plan is let go of.
    phazite::test::HostedDevice device;
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    auto& player = host.getEngine();
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, 120.0 } }, { { 0.0, 4, 4 } }, phazite::test::HostedDevice::sampleRate);
    plan->maximumBlockSize = phazite::test::HostedDevice::blockSize;
    engine::Plan::Strip master;
    master.state = std::make_shared<engine::StripState>();
    master.state->prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);
    const std::weak_ptr<engine::StripState> masterState = master.state;
    auto staller = std::make_shared<engine::DeviceNode> (Staller::info, std::make_unique<Staller>());
    staller->getRunner().prepare (phazite::test::HostedDevice::sampleRate, phazite::test::HostedDevice::blockSize);
    master.devices.push_back ({ staller, {} }); // the test keeps the node too, so that the stalled block's device stays
    plan->strips.push_back (std::move (master));
    player.setPlan (std::move (plan));

    auto* audio = phazite::test::TestAudioDevice::findIn (phazite::test::getEngineContext().getAudioDeviceManager());
    REQUIRE (audio != nullptr);
    Staller::stalled = false;
    Staller::stalls = true;
    std::thread audioThread (
        [audio]
        {
            juce::AudioBuffer<float> buffer (2, phazite::test::HostedDevice::blockSize);
            buffer.clear();
            audio->process (buffer);
        });

    while (! Staller::stalled.load())
        std::this_thread::yield();

    host.dropPlan();
    CHECK_MESSAGE (! masterState.expired(), "the plan a stalled block plays was freed under it");
    Staller::stalls = false;
    audioThread.join();

    // Freed once the block has finished, and another after it, as the next plan is given.
    juce::AudioBuffer<float> buffer (2, phazite::test::HostedDevice::blockSize);
    buffer.clear();
    audio->process (buffer);
    player.setPlan (nullptr);
    CHECK (masterState.expired());
    phazite::test::dispatchPendingMessages();
}

TEST_CASE ("A render takes the project's plug-ins only once a block that stalls in them has finished")
{
    // lendToRender() let go of the plans after half a second, whatever the audio thread did, and the render then played
    // the plug-ins the stalled block was still in: two threads in one plug-in. Now it waits on, however long the block
    // takes (the window waits with it). A track's effect stalls a block of the tests' audio device on a thread of its own;
    // it is let go once the session waits past the half second.
    [[maybe_unused]] static const bool registered = []
    {
        phazite::registerDeviceType (Staller::info, []() -> std::unique_ptr<dev::Processor> { return std::make_unique<Staller>(); });
        return true;
    }();

    phazite::test::TestSong song;
    phazite::test::HostedDevice device;
    auto& songItself = song.getProject().getSong();
    REQUIRE (phazite::insertDeviceState (songItself, song.firstTrack, phazite::makeOwnDeviceState (songItself, Staller::typeID)).isValid());
    phazite::test::settle();
    auto* session = phazite::findSession (songItself);
    REQUIRE (session != nullptr);
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    auto* audio = phazite::test::TestAudioDevice::findIn (phazite::test::getEngineContext().getAudioDeviceManager());
    REQUIRE (audio != nullptr);

    // The device plays on, as a device does, once the block that stalls has finished.
    Staller::stalled = false;
    Staller::stalls = true;
    std::atomic<bool> done { false };
    std::thread audioThread (
        [audio, &done]
        {
            juce::AudioBuffer<float> buffer (2, phazite::test::HostedDevice::blockSize);

            do
            {
                buffer.clear();
                audio->process (buffer);
            } while (! done.load());
        });

    while (! Staller::stalled.load())
        std::this_thread::yield();

    std::thread letGo (
        [&host, &done]
        {
            while (! done.load() && ! host.isWaitingForAudioThread())
                std::this_thread::yield();

            Staller::stalls = false;
        });

    session->lendToRender (true);
    CHECK_MESSAGE (! Staller::inBlock.load(), "the render took the plug-ins while a block was still playing them");
    done = true;
    letGo.join();
    audioThread.join();
    session->lendToRender (false);
}

namespace
{
// A file of silence, mono at the tests' rate, a minute long, longer than is read whole, which notes the chunks it reads, in
// the order it reads them, by their first samples.
struct NotingReader final : engine::SampleReader
{
    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return rate; }
    std::int64_t getLength() const override { return 60 * (std::int64_t) rate; }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int) override
    {
        buffer.clear();
        starts.push_back (start);
        return true;
    }

    bool hasRead (std::int64_t sample) const
    {
        const auto chunk = sample / engine::AudioFile::chunkSamples * engine::AudioFile::chunkSamples;
        return std::find (starts.begin(), starts.end(), chunk) != starts.end();
    }

    std::vector<std::int64_t> starts;
};

// The chunk a sample of a file is in, by its first sample.
std::int64_t chunkOf (std::int64_t sample)
{
    return sample / engine::AudioFile::chunkSamples * engine::AudioFile::chunkSamples;
}

// Reads all that a file wants read, as its reading thread would.
void readAllWanted (engine::AudioFile& file)
{
    while (file.readNext())
    {
    }
}
} // namespace

TEST_CASE ("A jump into a part of a long file not read yet has that part read first ahead of all else the file wants")
{
    // Known bug 36: the reading thread learnt where a jump lands only once the engine played there, after the jump's fade,
    // and then read it after the chunks it reads ahead elsewhere (the nearest of every place it plays first). A jump asked
    // for tells the files there at once, and the next chunk read is the one it lands in.
    engine::Engine player;
    player.prepare (rate, blockSize);
    auto reader = std::make_unique<NotingReader>();
    auto& noted = *reader;
    auto file = std::make_shared<engine::AudioFile> (std::move (reader), &player.getBlocksPlayed());
    engine::AudioRegion region;
    region.file = file;
    region.state = std::make_shared<engine::RegionState> (*file, rate);
    region.length = file->getLength();
    auto plan = makePlan();
    addTrack (*plan, nullptr, {}, { region });
    player.setPlan (std::move (plan));

    // Playing from the start, read ahead of as it plays; then a jump to 40 s in (beat 80 at 120 BPM), not read yet.
    player.play();

    for (int block = 0; block < 20; ++block)
    {
        playBlocks (player, blockSize);
        readAllWanted (*file);
    }

    const auto landing = 40 * (std::int64_t) rate;
    REQUIRE_FALSE (noted.hasRead (landing));
    playBlocks (player, blockSize);
    player.locate (80.0);
    CHECK (file->readNext());
    CHECK (noted.starts.back() == chunkOf (landing));
}

TEST_CASE ("A version's bounce compared with the song is read ahead at the loop's start and first where a jump lands")
{
    // Known bug 39: the song's audio clips were read ahead at the loop's start, and where playback will start, but a
    // version's bounce heard against the song (A/B) only where it played: each time round the loop, and after a jump, it
    // was silent until it was read. A loop from 40 s to 42 s in (beats 80 to 84 at 120 BPM), the version's tempo the same.
    engine::Engine player;
    player.prepare (rate, blockSize);
    auto reader = std::make_unique<NotingReader>();
    auto& noted = *reader;
    auto file = std::make_shared<engine::AudioFile> (std::move (reader), &player.getBlocksPlayed());
    auto plan = makePlan();
    plan->loop = engine::Plan::BeatRange { 80.0, 84.0 };
    addTrack (*plan, nullptr);
    engine::Plan::Comparison comparison;
    comparison.bounce.file = file;
    comparison.bounce.state = std::make_shared<engine::RegionState> (*file, rate);
    comparison.bounce.length = file->getLength();
    comparison.tempo = plan->tempo;
    comparison.state = std::make_shared<engine::ComparisonState>();
    comparison.state->prepare (rate, blockSize);
    plan->comparison = std::move (comparison);
    player.setPlan (std::move (plan));

    // Stopped at the start, the loop's start is read ahead of, with where playback will start.
    playBlocks (player, blockSize);
    readAllWanted (*file);
    CHECK (noted.hasRead (0));
    CHECK (noted.hasRead (40 * (std::int64_t) rate));

    // A jump to 50 s in (beat 100) is read first.
    player.locate (100.0);
    CHECK (file->readNext());
    CHECK (noted.starts.back() == chunkOf (50 * (std::int64_t) rate));
}

namespace
{
// A file of one level a minute long, read on the reading thread, which notes when it read each chunk (milliseconds by
// juce::Time::getMillisecondCounterHiRes(), by its first sample), for the test's thread to look at.
struct TimedReader final : engine::SampleReader
{
    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return rate; }
    std::int64_t getLength() const override { return 60 * (std::int64_t) rate; }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
    {
        juce::FloatVectorOperations::fill (buffer.getWritePointer (0), 0.5f, numSamples);
        const std::lock_guard lock (mutex);
        reads.emplace_back (start, juce::Time::getMillisecondCounterHiRes());
        return true;
    }

    int countReads() const
    {
        const std::lock_guard lock (mutex);
        return (int) reads.size();
    }

    std::optional<double> findReadTime (std::int64_t chunkStart) const
    {
        const std::lock_guard lock (mutex);

        for (const auto& [start, at] : reads)
            if (start == chunkStart)
                return at;

        return {};
    }

    mutable std::mutex mutex;
    std::vector<std::pair<std::int64_t, double>> reads;
};
} // namespace

TEST_CASE ("A jump while the reading thread waits has the place it lands in read at once and not when the thread next looks")
{
    // The reading thread, done with what it reads ahead, looks again every 20 ms; it learnt of a jump into a part of a long
    // file not read yet only as the engine landed there, after the jump's fade, and read it when it next looked (known bug
    // 36: up to 20 ms of silence more than the read takes). The jump asked for now wakes it, with the place to read first.
    engine::Engine player;
    player.prepare (rate, blockSize);
    engine::FileReader reading;
    auto timed = std::make_unique<TimedReader>();
    auto& reads = *timed;
    auto file = std::make_shared<engine::AudioFile> (std::move (timed), &player.getBlocksPlayed());
    engine::AudioRegion region;
    region.file = file;
    region.state = std::make_shared<engine::RegionState> (*file, rate);
    region.length = file->getLength();
    auto plan = makePlan();
    addTrack (*plan, nullptr, {}, { region });
    player.setPlan (std::move (plan));
    reading.add (file);
    player.play();
    playBlocks (player, blockSize);

    // Until it has read ahead (10 s, in 15 chunks) and waits.
    const auto ahead = (int) std::ceil (10.0 * rate / engine::AudioFile::chunkSamples);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&reads, ahead] { return reads.countReads() >= ahead; }));

    // The jump, 40 s in (beat 80), asked for with no block played meanwhile: the place it lands in is read at once, ahead of
    // the engine's landing there. The old reading thread learnt of it only from a block played there, so it waited for ever
    // (here for the limit, a second: how soon is what is tested).
    const auto landing = chunkOf (40 * (std::int64_t) rate);
    const auto asked = juce::Time::getMillisecondCounterHiRes();
    player.locate (80.0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&reads, landing] { return reads.findReadTime (landing).has_value(); }, 1000));
    MESSAGE ("the reading thread read where a jump lands " << *reads.findReadTime (landing) - asked << " ms after it was asked for");

    // Then the clip is silent for the jump's fade alone: from where the jump is taken, its fade in (3 ms) at most, never a
    // block of silence while its file is read.
    while (player.getLocatesTaken().load() != player.getLocatesAsked())
        playBlocks (player, 64);

    const auto played = playBlocks (player, 8 * blockSize);
    auto silent = 0;

    for (int i = 0; i < played.getNumSamples(); ++i)
        silent += std::abs (played.getSample (0, i)) < 1.0e-6f ? 1 : 0;

    CAPTURE (silent);
    CHECK (silent <= (int) std::lround (engine::Engine::fadeSeconds * rate) + 64);
}

TEST_CASE ("A delay made longer than its ring reads the past from the ring before and its work in the block of the switch is the block's")
{
    // The review of known bug 167 (B): the ring of a delay made longer than its ring copied what the ring before held, all
    // of it, on the audio thread as it first played, one for every path into a sum that grew, in one block (50 tracks at
    // 48 kHz from 8192 to 16,384 samples: about 4.9 MB). Now it copies nothing: it reads the part of its reach that predates
    // it from the ring before, so a block's work is the block's, however much the rings hold. A count played through a
    // delay of 8192 samples for a while, then through one of 16,384 on a new ring that takes that one's place.
    auto first = std::make_shared<engine::DelayLine> (8192 + 16384);
    engine::Delay before (first, 8192, 144);
    first->reset(); // as a start from a stop makes it: silence, all its room heard
    juce::AudioBuffer<float> in (2, 64), out (2, 64);
    std::int64_t count = 0;

    const auto playBlock = [&] (engine::Delay& delay)
    {
        for (int i = 0; i < 64; ++i)
            for (int channel = 0; channel < 2; ++channel)
                in.setSample (channel, i, (float) (count + i + 1));

        out.clear();
        delay.addDelayed (in.getArrayOfReadPointers(), out.getArrayOfWritePointers(), 64);
        count += 64;
    };

    for (int block = 0; block < 600; ++block)
        playBlock (before);

    engine::Delay after (std::make_shared<engine::DelayLine> (16384 + 16384, first), 16384, 144);
    const auto from = count;
    playBlock (after);

    // At most the block read and the block written, on each side; and what it played the count 16,384 samples before.
    CAPTURE (after.getSamplesTouched());
    CHECK (after.getSamplesTouched() <= 2 * 64);
    CHECK (out.getSample (0, 0) == doctest::Approx ((double) (from + 1 - 16384)));
    CHECK (out.getSample (1, 63) == doctest::Approx ((double) (from + 64 - 16384)));
}
