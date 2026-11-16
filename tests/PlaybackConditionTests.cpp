// SPDX-License-Identifier: AGPL-3.0-or-later
// What playback promises wherever it plays, checked at each of the playback conditions (PlaybackConditions.h: rates, block
// sizes and tempos, among them tempos whose beats fall between samples and blocks that start on a beat's first sample):
// every beat of the metronome and the count-in clicks once; a loop plays the same each time round, and a note that starts
// on its end never plays in it; a file longer than what is read ahead plays without a gap from a new place and from the
// loop's start; and a song played live sounds as its render does. The first three drive Phazite's engine on its own, as
// EngineTests.cpp does; the last plays a song made through the model.
#include "EnginePlayback.h"
#include "PlaybackConditions.h"
#include "RenderHarness.h"
#include "TestSong.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/synth/Synth.h"
#include "engine/audio/AudioRegion.h"
#include "engine/core/Engine.h"
#include "engine/nodes/DeviceNode.h"
#include "engine/transport/Click.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace engine = phazite::engine;
using phazite::test::atEveryCondition;
using phazite::test::PlaybackCondition;
using phazite::test::TestSong;

// A plan at a condition's rate and tempo, in 4/4, made for its blocks.
std::unique_ptr<engine::Plan> makePlan (const PlaybackCondition& condition)
{
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, condition.getTempo() } }, { { 0.0, 4, 4 } }, condition.getSampleRate());
    plan->maximumBlockSize = condition.blockSize;
    return plan;
}

// What an engine plays next, for so many samples, as a device would ask for it: in the condition's blocks, after a first
// block of another length if one is given (so that the blocks after it start where a test wants one to); with something
// done after each block, if given (the reading thread's turn).
juce::AudioBuffer<float> playBlocks (engine::Engine& player, const PlaybackCondition& condition, int numSamples, int firstBlock = 0,
                                     const std::function<void()>& afterEachBlock = {})
{
    juce::AudioBuffer<float> played (2, numSamples);
    played.clear();

    for (int done = 0; done < numSamples;)
    {
        const auto length = std::min (done == 0 && firstBlock > 0 ? firstBlock : condition.blockSize, numSamples - done);
        float* channels[] = { played.getWritePointer (0, done), played.getWritePointer (1, done) };
        player.process (channels, 2, length);
        done += length;

        if (afterEachBlock != nullptr)
            afterEachBlock();
    }

    return played;
}

// The first sample at which two stretches of audio differ on either side, or -1 if they are the same to the last bit.
int findFirstDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples())
        return std::min (a.getNumSamples(), b.getNumSamples());

    for (int i = 0; i < a.getNumSamples(); ++i)
        for (int channel = 0; channel < 2; ++channel)
            if (! juce::exactlyEqual (a.getSample (channel, i), b.getSample (channel, i)))
                return i;

    return -1;
}

//==============================================================================
// What the metronome plays from the song's start at a condition: a click from the first sample at or after each beat, each
// bar's first the accented one, each stopping where the next starts. The count-in's bar and the song after it click on the
// same samples, counted from the count-in's start.
juce::AudioBuffer<float> expectClicks (const PlaybackCondition& condition, int numSamples)
{
    juce::AudioBuffer<float> expected (2, numSamples);
    expected.clear();
    const auto click = engine::makeClick (condition.getSampleRate(), false), accent = engine::makeClick (condition.getSampleRate(), true);

    for (int beat = 0; condition.getFirstSampleOf (beat) < numSamples; ++beat)
    {
        const auto start = (int) condition.getFirstSampleOf (beat);
        const auto& sound = beat % 4 == 0 ? accent : click;

        for (int channel = 0; channel < 2; ++channel)
        {
            expected.clear (channel, start, numSamples - start);
            expected.copyFrom (channel, start, sound, 0, 0, std::min (sound.getNumSamples(), numSamples - start));
        }
    }

    return expected;
}

// The first blocks to play the clicks in: none (the device's blocks from the first sample on), and for each of the next
// few beats, one after which a block starts on the beat's first sample, and one after which a block ends on it.
std::vector<int> getFirstBlocks (const PlaybackCondition& condition, int numBeats)
{
    std::vector<int> firstBlocks { 0 };

    for (int beat = 1; beat <= numBeats; ++beat)
        for (const auto sample : { condition.getFirstSampleOf (beat), condition.getFirstSampleOf (beat) + 1 })
            if (const auto firstBlock = (int) (sample % condition.blockSize);
                std::find (firstBlocks.begin(), firstBlocks.end(), firstBlock) == firstBlocks.end())
                firstBlocks.push_back (firstBlock);

    return firstBlocks;
}

// Says which beat a sample that differs is in, and fails if there is one.
void checkClicks (const PlaybackCondition& condition, const juce::AudioBuffer<float>& played, const juce::AudioBuffer<float>& expected)
{
    const auto differsAt = findFirstDifference (played, expected);
    const auto inBeat = differsAt / condition.getBeatSamples();
    CAPTURE (differsAt);
    CAPTURE (inBeat);
    CHECK_MESSAGE (differsAt == -1, "the clicks differ from a click on each beat's first sample");
}
} // namespace

TEST_CASE ("At every condition the metronome clicks once on each beat, on its first sample, wherever its blocks start")
{
    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            const auto length = (int) (8.5 * condition.getBeatSamples());
            const auto expected = expectClicks (condition, length);

            for (const auto firstBlock : getFirstBlocks (condition, 4))
            {
                CAPTURE (firstBlock);
                engine::Engine player;
                player.prepare (condition.getSampleRate(), condition.blockSize);
                auto plan = makePlan (condition);
                plan->metronome = true;
                player.setPlan (std::move (plan));
                player.play();
                checkClicks (condition, playBlocks (player, condition, length, firstBlock), expected);
            }
        });
}

TEST_CASE ("At every condition the count-in clicks once on each beat of its bar, and the song's beats after it, wherever its blocks start")
{
    // From bar 2, the metronome on: a bar of count-in, four clicks, then the song's, from the bar's first. A beat of the
    // count-in is as long as the song's there, so all of them click where the song's would from its start.
    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            const auto length = (int) (6.5 * condition.getBeatSamples());
            const auto expected = expectClicks (condition, length);

            for (const auto firstBlock : getFirstBlocks (condition, 5))
            {
                CAPTURE (firstBlock);
                engine::Engine player;
                player.prepare (condition.getSampleRate(), condition.blockSize);
                auto plan = makePlan (condition);
                plan->metronome = true;
                player.setPlan (std::move (plan));
                player.locate (4.0);
                player.play (true);
                checkClicks (condition, playBlocks (player, condition, length, firstBlock), expected);
            }
        });
}

TEST_CASE ("At every condition the metronome and the count-in click as late as the song's sound comes out")
{
    // With 1,000 samples of delay compensation on the master (a limiter's look-ahead, a plug-in's latency) the song comes out
    // 1,000 samples late, and so does each click, from the song's start and after a count-in from bar 2: they clicked at once,
    // ahead of the song, and notes played to them were recorded early (known bug 76; a take places them by the song's delay,
    // model/transport/internal/Take.h).
    constexpr int latency = 1000;

    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            const auto length = (int) (6.5 * condition.getBeatSamples());
            const auto unshifted = expectClicks (condition, length - latency);
            juce::AudioBuffer<float> expected (2, length);
            expected.clear();

            for (int channel = 0; channel < 2; ++channel)
                expected.copyFrom (channel, latency, unshifted, channel, 0, length - latency);

            for (const auto countIn : { false, true })
            {
                CAPTURE (countIn);
                engine::Engine player;
                player.prepare (condition.getSampleRate(), condition.blockSize);
                auto plan = makePlan (condition);
                plan->metronome = true;
                engine::Plan::Strip master;
                master.state = std::make_shared<engine::StripState>();
                master.state->prepare (condition.getSampleRate(), condition.blockSize);
                master.latency = latency;
                plan->strips.push_back (std::move (master));
                player.setPlan (std::move (plan));

                if (countIn)
                    player.locate (4.0);

                player.play (countIn);
                checkClicks (condition, playBlocks (player, condition, length), expected);
            }
        });
}

//==============================================================================
namespace
{
// An instrument that plays a level of 0.5 while a note is held, and keeps each note event it is given: with the song's
// sample it is on, and how many times the song had gone back by then (the loop's rounds).
class NoteLogger final : public dev::Processor
{
public:
    static inline const dev::DeviceInfo info { .typeID = "testNoteLogger", .name = "Note Logger", .kind = dev::DeviceKind::instrument };

    struct Logged
    {
        dev::NoteEvent::Type type = dev::NoteEvent::Type::noteOn;
        int pitch = 0;
        std::int64_t songSample = 0;
        int round = 0;
    };

    std::vector<Logged> events;

    void prepare (double, int) override { events.reserve (4096); }
    void reset() override { held = 0; }

    void process (const dev::ProcessContext& context) override
    {
        using Type = dev::NoteEvent::Type;

        if (context.songSample < lastSongSample)
            ++round;

        lastSongSample = context.songSample;
        int done = 0;

        for (const auto& event : context.events)
        {
            fill (context, done, event.sample);
            done = event.sample;

            if (event.type == Type::noteOn)
                ++held;
            else if (event.type == Type::noteOff)
                held = std::max (0, held - 1);
            else if (event.type == Type::allNotesOff)
                held = 0;

            events.push_back ({ event.type, event.number, context.songSample + event.sample, round });
        }

        fill (context, done, context.numSamples);
    }

private:
    int held = 0, round = 0;
    std::int64_t lastSongSample = 0;

    void fill (const dev::ProcessContext& context, int from, int to) const
    {
        for (int channel = 0; channel < context.numChannels; ++channel)
            juce::FloatVectorOperations::fill (context.channels[channel] + from, held > 0 ? 0.5f : 0.0f, to - from);
    }
};

// A track of a plan that plays notes on a note logger, and the master it plays into.
void addLoggedTrack (engine::Plan& plan, const PlaybackCondition& condition, std::unique_ptr<NoteLogger> logger,
                     std::vector<engine::NoteSequence::Note> notes)
{
    engine::Plan::Strip track, master;
    track.id = 1;

    for (auto* strip : { &track, &master })
    {
        strip->state = std::make_shared<engine::StripState>();
        strip->state->prepare (condition.getSampleRate(), condition.blockSize);
    }

    track.notes = std::make_shared<const engine::NoteSequence> (std::move (notes), std::vector<engine::NoteSequence::Control> {});
    auto node = std::make_shared<engine::DeviceNode> (NoteLogger::info, std::move (logger));
    node->getRunner().prepare (condition.getSampleRate(), condition.blockSize);
    track.devices.push_back ({ std::move (node), {} });
    track.hasInstrument = true;
    master.inputs.push_back ({ 0, nullptr });
    plan.strips.push_back (std::move (track));
    plan.strips.push_back (std::move (master));
}

// A note's start or end as it was played: on a sample, counted from where a test counts (the song's start, a loop's).
struct Played
{
    dev::NoteEvent::Type type;
    int pitch;
    std::int64_t sample;

    bool operator== (const Played&) const = default;
};

// For a failure's message: "on 60 at 0, off 60 at 10286..."
juce::String describe (const std::vector<Played>& played)
{
    using Type = dev::NoteEvent::Type;
    juce::StringArray texts;

    for (const auto& note : played)
        texts.add (juce::String (note.type == Type::noteOn    ? "on "
                                 : note.type == Type::noteOff ? "off "
                                                              : "other ")
                   + juce::String (note.pitch) + " at " + juce::String (note.sample));

    return texts.joinIntoString (", ");
}
} // namespace

TEST_CASE ("At every condition a note starts and ends on the sample nearest its beat, wherever its blocks start")
{
    // Six short notes on and between the beats, played from the song's start in the device's blocks, and again for each
    // note's start and end with a first block after which a block starts on its sample, and one after which a block ends
    // on it.
    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            using Type = dev::NoteEvent::Type;
            const auto tempo = makePlan (condition)->tempo;
            std::vector<engine::NoteSequence::Note> notes;
            std::vector<Played> expected;
            std::vector<int> firstBlocks { 0 };
            auto pitch = 60;

            for (const auto start : { 0.5, 1.0, 1.75, 2.0, 2.5, 3.0 })
            {
                notes.push_back ({ start, start + 0.125, pitch, 0.8f });

                for (const auto& [type, beat] : { std::pair { Type::noteOn, start }, std::pair { Type::noteOff, start + 0.125 } })
                {
                    const auto sample = std::llround (tempo.toSamples (beat));
                    expected.push_back ({ type, pitch, sample });

                    for (const auto blockStart : { sample, sample + 1 })
                        if (const auto firstBlock = (int) (blockStart % condition.blockSize);
                            std::find (firstBlocks.begin(), firstBlocks.end(), firstBlock) == firstBlocks.end())
                            firstBlocks.push_back (firstBlock);
                }

                ++pitch;
            }

            const auto expectedNotes = describe (expected);
            CAPTURE (expectedNotes);

            for (const auto firstBlock : firstBlocks)
            {
                auto plan = makePlan (condition);
                auto logger = std::make_unique<NoteLogger>();
                const auto* logged = logger.get();
                addLoggedTrack (*plan, condition, std::move (logger), notes);

                engine::Engine player;
                player.prepare (condition.getSampleRate(), condition.blockSize);
                player.setPlan (std::move (plan));
                player.play();
                playBlocks (player, condition, (int) (3.5 * condition.getBeatSamples()), firstBlock);

                std::vector<Played> played;

                for (const auto& event : logged->events)
                    if (event.type != Type::allNotesOff)
                        played.push_back ({ event.type, event.pitch, event.songSample });

                const auto playedNotes = describe (played);
                CAPTURE (firstBlock);
                CAPTURE (playedNotes);
                CHECK_MESSAGE (played == expected, "each note should start and end on the sample nearest its beat");
            }
        });
}

TEST_CASE ("At every condition a loop plays the same each time round, and a note that starts on its end never plays in it")
{
    // Loops whose ends fall on beats that round to their samples either way, where the tempo puts beats between samples.
    // In each: a note from the loop's start, one held over its end (which ends on the loop's last sample, and starts again
    // where it started), and around it a note that ends on the loop's start and one that starts on its end, which are the
    // bars' before and after: neither sounds in the loop. With the metronome on, played three times round from its start.
    struct Loop
    {
        int start, end; // in beats
    };

    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            for (const auto& loop : { Loop { 0, 1 }, Loop { 0, 2 }, Loop { 1, 3 }, Loop { 1, 4 }, Loop { 2, 5 } })
            {
                CAPTURE (loop.start);
                CAPTURE (loop.end);
                const auto start = (double) loop.start, end = (double) loop.end;
                auto plan = makePlan (condition);
                plan->loop = engine::Plan::BeatRange { start, end };
                plan->metronome = true;

                // Where the engine puts the loop's ends: on the samples nearest them.
                const auto loopStart = condition.getNearestSampleTo (loop.start), loopEnd = condition.getNearestSampleTo (loop.end);
                REQUIRE (std::llround (plan->tempo.toSamples (start)) == loopStart);
                REQUIRE (std::llround (plan->tempo.toSamples (end)) == loopEnd);
                const auto length = (int) (loopEnd - loopStart);

                constexpr int before = 59, first = 60, over = 61, after = 62;
                std::vector<engine::NoteSequence::Note> notes;

                if (loop.start > 0)
                    notes.push_back ({ start - 0.5, start, before, 0.8f });

                notes.push_back ({ start, start + 0.5, first, 0.8f });
                notes.push_back ({ end - 0.25, end + 1.0, over, 0.8f });
                notes.push_back ({ end, end + 1.0, after, 0.8f });

                auto logger = std::make_unique<NoteLogger>();
                const auto* logged = logger.get();
                addLoggedTrack (*plan, condition, std::move (logger), std::move (notes));

                engine::Engine player;
                player.prepare (condition.getSampleRate(), condition.blockSize);
                player.setPlan (std::move (plan));
                player.locate (start);
                player.play();
                const auto played = playBlocks (player, condition, 3 * length);

                // The notes each round plays, by their samples from the loop's start.
                using Type = dev::NoteEvent::Type;

                std::array<std::vector<Played>, 3> rounds;
                auto strays = 0;

                for (const auto& event : logged->events)
                {
                    if (event.type == Type::allNotesOff)
                        continue;

                    if ((event.type == Type::noteOn && (event.pitch == before || event.pitch == after)) || event.round > 2)
                        ++strays;
                    else
                        rounds[(size_t) event.round].push_back ({ event.type, event.pitch, event.songSample - loopStart });
                }

                CHECK_MESSAGE (strays == 0, "a note of the bar before the loop or of the bar after it started in the loop");

                const auto firstRound = describe (rounds[0]), secondRound = describe (rounds[1]), thirdRound = describe (rounds[2]);
                CAPTURE (firstRound);
                CAPTURE (secondRound);
                CAPTURE (thirdRound);

                const auto isPlayed = [&rounds] (Type type, int pitch, std::int64_t sample)
                { return std::find (rounds[0].begin(), rounds[0].end(), Played { type, pitch, sample }) != rounds[0].end(); };

                CHECK_MESSAGE (isPlayed (Type::noteOn, first, 0), "the loop's first note should start on its first sample");
                CHECK_MESSAGE (isPlayed (Type::noteOff, over, length - 1), "a note held over the loop's end should end on its last sample");
                CHECK_MESSAGE (rounds[1] == rounds[0], "the second time round should play the notes the first did, on the same samples");
                CHECK_MESSAGE (rounds[2] == rounds[0], "the third time round should play the notes the first did, on the same samples");

                // And what is heard: the notes and the clicks, the same each time round to the last bit.
                REQUIRE (played.getMagnitude (0, 0, length) > 0.4f);

                for (int round = 1; round < 3; ++round)
                {
                    auto differsAt = -1;

                    for (int i = 0; i < length && differsAt < 0; ++i)
                        if (! juce::exactlyEqual (played.getSample (0, round * length + i), played.getSample (0, i)))
                            differsAt = i;

                    CAPTURE (round);
                    CAPTURE (differsAt);
                    CHECK_MESSAGE (differsAt == -1, "a time round the loop should sound as the first did");
                }
            }
        });
}

//==============================================================================
namespace
{
// A file's samples made up as they are read: each the sample's number in a thousandths' cycle, so that every stretch of it
// tells where in the file it is from. Mono, at the engine's rate.
struct CountingReader final : engine::SampleReader
{
    CountingReader (double sampleRate, std::int64_t samples)
        : fileRate (sampleRate),
          length (samples)
    {
    }

    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return fileRate; }
    std::int64_t getLength() const override { return length; }

    static float at (std::int64_t sample) { return (float) (sample % 1000) / 1000.0f; }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
    {
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (0, i, at (start + i));

        return true;
    }

    double fileRate;
    std::int64_t length;
};
} // namespace

TEST_CASE ("At every condition a long audio file plays without a gap from where the playhead was put and from the loop's start")
{
    // A file of 70 s from the song's start, longer than is read whole, of which the engine keeps a few seconds read ahead of
    // where it plays; beats 40 to 56 looped. Its reading thread is the test's, which reads all that is wanted after each
    // block. Stopped at the song's start, the playhead is put at beat 52, further on than was read, and the song played
    // from there a little later: to the loop's end, round it, and half way round again. Every sample is the file's at the
    // song's sample it plays, but for the 48 after playback starts and after each jump back, where what plays fades in (an
    // audio region fades in over 40 samples rather than click), and those before the loop's end, where it fades out
    // (engine::Engine::fadeSeconds).
    constexpr int fadeSamples = 48;

    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            const auto rate = condition.getSampleRate();
            engine::Engine player;
            player.prepare (rate, condition.blockSize);
            auto file = std::make_shared<engine::AudioFile> (
                std::make_unique<CountingReader> (rate, 70 * (std::int64_t) condition.sampleRate), &player.getBlocksPlayed());
            file->keep (0);

            auto plan = makePlan (condition);
            plan->loop = engine::Plan::BeatRange { 40.0, 56.0 };
            const auto loopStart = condition.getNearestSampleTo (40), loopEnd = condition.getNearestSampleTo (56);
            const auto playFrom = condition.getNearestSampleTo (52);
            REQUIRE (loopEnd < file->getLength());
            REQUIRE ((double) file->getLength() > engine::AudioFile::shortSeconds * rate);

            engine::Plan::Strip track, master;
            track.id = 1;

            for (auto* strip : { &track, &master })
            {
                strip->state = std::make_shared<engine::StripState>();
                strip->state->prepare (rate, condition.blockSize);
            }

            engine::AudioRegion region;
            region.state = std::make_shared<engine::RegionState> (*file, rate);
            region.file = file;
            region.length = file->getLength();
            track.audio.push_back (std::move (region));
            master.inputs.push_back ({ 0, nullptr });
            plan->strips.push_back (std::move (track));
            plan->strips.push_back (std::move (master));
            player.setPlan (std::move (plan));

            const auto readAhead = [&file]
            {
                while (file->readNext())
                {
                }
            };

            playBlocks (player, condition, 4 * condition.blockSize, 0, readAhead);
            player.locate (52.0);
            playBlocks (player, condition, 4 * condition.blockSize, 0, readAhead);
            player.play();
            const auto loopLength = loopEnd - loopStart;
            const auto played = playBlocks (player, condition, (int) (loopEnd - playFrom + loopLength + loopLength / 2), 0, readAhead);

            auto songSample = playFrom;
            auto sinceJump = 0, wrong = 0, jumps = 0, firstWrong = -1;
            const auto fadeOut = (int) std::lround (engine::Engine::fadeSeconds * rate);

            for (int i = 0; i < played.getNumSamples(); ++i)
            {
                const auto expected = CountingReader::at (songSample), got = played.getSample (0, i);
                const auto right = sinceJump < fadeSamples || songSample >= loopEnd - fadeOut ? got >= 0.0f && got <= expected + 1.0e-6f
                                                                                              : std::abs (got - expected) <= 1.0e-6f;

                if (! right && wrong++ == 0)
                    firstWrong = i;

                ++sinceJump;

                if (++songSample == loopEnd)
                {
                    songSample = loopStart;
                    sinceJump = 0;
                    ++jumps;
                }
            }

            CAPTURE (firstWrong);
            CHECK_MESSAGE (wrong == 0, "samples that aren't the file's where the song is: a gap, or a late start");
            CHECK (jumps == 2);
        });
}

//==============================================================================
TEST_CASE ("At every condition a song played live sounds as its render does")
{
    // Two bars at the condition's tempo. In the first, a beat on the drum sampler and off-beat stabs on the synth, which is
    // silent between them for longer than a device stays awake in silence. In the second, a single hit through a delay
    // whose echoes come half a note apart and nothing else, so that the delay is silent between them for longer than that
    // too; and a tone from a file, which live is read ahead and at another rate than its own resampled.
    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            TestSong song;
            song.setTempo (condition.getTempo());
            const auto drums = song.firstTrack;
            song.setInstrument (drums, dev::DrumSampler::typeID);
            const auto beat = song.addMidiClip (drums, 0.0, 4.0);

            for (int beatNumber = 0; beatNumber < 4; ++beatNumber)
            {
                song.addNote (beat, beatNumber % 2 == 0 ? 36 : 38, beatNumber, 0.25);
                song.addNote (beat, 42, beatNumber + 0.5, 0.25, 80);
            }

            const auto synth = song.addTrack ("Synth");
            song.setInstrument (synth, dev::Synth::typeID);
            const auto stabs = song.addMidiClip (synth, 0.0, 4.0);
            song.addChord (stabs, { 60, 64, 67 }, 0.5, 0.25);
            song.addChord (stabs, { 62, 65, 69 }, 3.5, 0.25);

            const auto echoes = song.addTrack ("Echoes");
            song.addAudioClip (echoes, phazite::test::writeHit (song.getSoundsFolder().getChildFile ("Hit.wav")), 4.0);
            const auto delay = song.addEffect (echoes, dev::Delay::typeID);
            song.setParameter (delay, "time", 11.0f); // 1/2
            song.setParameter (delay, "feedback", 0.5f);
            song.setParameter (delay, "mix", 0.5f);

            const auto tone = song.addTrack ("Tone");
            song.addAudioClip (tone, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 0.3, 330.0, 0.1f), 4.25);
            phazite::test::settle();

            const auto rate = condition.getSampleRate();
            const auto beatSeconds = 60.0 / condition.getTempo();
            const auto seconds = std::ceil ((8.0 * beatSeconds + 0.5) * 10.0) / 10.0;
            const auto rendered = phazite::test::renderOffline (song.context, seconds, rate, condition.blockSize);

            // The render has what the song is there to play: the hit's first echo, on beat 6, after silence.
            const auto fingerprint = phazite::test::getFingerprint (rendered, condition.getFingerprintBlock());
            const auto echoAt = 6.0 * beatSeconds;
            REQUIRE (phazite::test::isLouder (fingerprint, (size_t) ((echoAt + 0.025) * 10.0), (size_t) (echoAt * 10.0) - 1, 10.0f,
                                              "the hit's first echo, half a note after it"));

            const auto live = phazite::test::playLive (song.context, seconds, rate, condition.blockSize);
            const auto differences = phazite::test::compareAudio (rendered, live, rate);
            const auto apart = phazite::test::getDifference (rendered, live, rate);
            INFO (("Played live, the song sounds otherwise than its render (sample by sample, up to "
                   + phazite::test::formatLevel (apart.peakDb) + " dBFS apart, at " + juce::String (apart.seconds, 4) + " s):\n  "
                   + differences.joinIntoString ("\n  "))
                      .toStdString());
            CHECK (differences.isEmpty());
        });
}

//==============================================================================
namespace
{
// A sine of 997 Hz at half of full scale, mono at the engine's rate, eight seconds long, at its peak on a sample of a test's
// choosing: where the transport cuts it, so that a cut without a fade is a step as large as the sine.
struct SineReader final : engine::SampleReader
{
    SineReader (double sampleRate, std::int64_t peakAt)
        : fileRate (sampleRate),
          peak (peakAt)
    {
    }

    static constexpr double frequency = 997.0, amplitude = 0.5;

    int getNumChannels() const override { return 1; }
    double getSampleRate() const override { return fileRate; }
    std::int64_t getLength() const override { return (std::int64_t) (8.0 * fileRate); }

    bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
    {
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (
                0, i,
                (float) (amplitude * std::cos (juce::MathConstants<double>::twoPi * frequency * (double) (start + i - peak) / fileRate)));

        return true;
    }

    double fileRate;
    std::int64_t peak;
};

// The largest change from one sample to the next, and where, on the first side.
std::pair<float, int> findLargestStep (const juce::AudioBuffer<float>& played)
{
    auto largest = 0.0f;
    auto at = -1;

    for (int i = 1; i < played.getNumSamples(); ++i)
        if (const auto step = std::abs (played.getSample (0, i) - played.getSample (0, i - 1)); step > largest)
        {
            largest = step;
            at = i;
        }

    return { largest, at };
}
} // namespace

TEST_CASE ("At every condition a stop, the loop's end and a jump fade what plays rather than cut it")
{
    // An audio clip of a sine from the song's start, cut at its peak by the transport: stopped, at the loop's end (beats 1 to
    // 3), and by a jump to beat 2.5 while it plays. They cut it in a step as large as the sine (the audit's engine finding
    // 10): now what sounds fades out over a few milliseconds and what follows fades in, as other DAWs do, so that no
    // sample steps further from the one before than twice the most a sine of its frequency moves.
    // And a stop that comes while the jump fades out (known bug 162): it took the jump at once, cutting its fade short in a
    // step from where the fade had got to (0.24 of full scale at 48 kHz, half way through the fade), as the fade in after it
    // started from silence; now the stop waits for the fade's end and is taken where the jump lands.
    enum class Cut
    {
        stop,
        loop,
        jump,
        stopInJump
    };

    atEveryCondition (
        [] (const PlaybackCondition& condition)
        {
            const auto rate = condition.getSampleRate();
            const auto blocksIn = (int) std::ceil (0.25 * rate / condition.blockSize);
            const auto cutAt = (std::int64_t) blocksIn * condition.blockSize;
            const auto loopEnd = condition.getNearestSampleTo (3);
            const auto mostASineMoves = (float) (SineReader::amplitude * juce::MathConstants<double>::twoPi * SineReader::frequency / rate);

            for (const auto cut : { Cut::stop, Cut::loop, Cut::jump, Cut::stopInJump })
            {
                CAPTURE ((int) cut);
                engine::Engine player;
                player.prepare (rate, condition.blockSize);
                player.setPlayingAsRender (true); // the file read as it plays
                const auto peak = cut == Cut::loop ? loopEnd - 1 : cutAt - 1;
                auto file = std::make_shared<engine::AudioFile> (std::make_unique<SineReader> (rate, peak), &player.getBlocksPlayed());

                auto plan = makePlan (condition);

                if (cut == Cut::loop)
                    plan->loop = engine::Plan::BeatRange { 1.0, 3.0 };

                engine::Plan::Strip track, master;
                track.id = 1;

                for (auto* strip : { &track, &master })
                {
                    strip->state = std::make_shared<engine::StripState>();
                    strip->state->prepare (rate, condition.blockSize);
                }

                engine::AudioRegion region;
                region.state = std::make_shared<engine::RegionState> (*file, rate);
                region.file = file;
                region.length = file->getLength();
                track.audio.push_back (std::move (region));
                master.inputs.push_back ({ 0, nullptr });
                plan->strips.push_back (std::move (track));
                plan->strips.push_back (std::move (master));
                player.setPlan (std::move (plan));
                player.play();

                const auto before = (int) (cut == Cut::loop ? loopEnd : cutAt);
                auto played = playBlocks (player, condition, before);

                if (cut == Cut::stop)
                    player.stop();
                else if (cut == Cut::jump || cut == Cut::stopInJump)
                    player.locate (2.5);

                // Half way through the jump's fade, in a block of its own, the stop.
                juce::AudioBuffer<float> inFade (2, 0);

                if (cut == Cut::stopInJump)
                {
                    inFade = playBlocks (player, condition, (int) std::lround (engine::Engine::fadeSeconds * rate / 2.0));
                    player.stop();
                }

                const auto after = playBlocks (player, condition, (int) (0.1 * rate));
                juce::AudioBuffer<float> whole (2, before + inFade.getNumSamples() + after.getNumSamples());

                for (int channel = 0; channel < 2; ++channel)
                {
                    whole.copyFrom (channel, 0, played, channel, 0, before);
                    whole.copyFrom (channel, before, inFade, channel, 0, inFade.getNumSamples());
                    whole.copyFrom (channel, before + inFade.getNumSamples(), after, channel, 0, after.getNumSamples());
                }

                const auto largest = findLargestStep (whole);
                const auto step = largest.first;
                const auto at = largest.second;
                CAPTURE (at);
                CAPTURE (step);
                CHECK (step <= 2.0f * mostASineMoves);

                // A stop ends it once it has faded out; the loop and the jump play on.
                if (cut == Cut::stop || cut == Cut::stopInJump)
                    CHECK (after.getMagnitude (0, after.getNumSamples() / 2, after.getNumSamples() / 2) < 1.0e-6f);
                else
                    CHECK (after.getMagnitude (0, after.getNumSamples() / 2, after.getNumSamples() / 2) > 0.4f);
            }
        });
}
