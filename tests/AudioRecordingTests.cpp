// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording audio (src/model/transport/RecordingModel.h, internal/AudioRecorder.h, internal/Take.h; roadmap step 3.4): a
// track's input, how it is heard and armed, and takes recorded through Phazite's engine, whose device's output comes back
// into its input a block later, as a cable from an audio interface's output to its input would bring it: the metronome's
// clicks, recorded, show where Phazite puts what comes in.
#include "EnginePlayback.h"
#include "RealTimePlayback.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Take.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

namespace
{
using phazite::test::RealTimePlayback;

// Where the clicks start: each first sample that isn't silent after a quiet stretch. The metronome's clicks start on their
// beats, and the silence between them comes back in, and into the file, as silence.
std::vector<juce::int64> findClicks (const std::vector<float>& song, double rate)
{
    std::vector<juce::int64> clicks;
    const auto quiet = (juce::int64) (0.05 * rate);
    juce::int64 lastLoud = -quiet;

    for (size_t i = 0; i < song.size(); ++i)
    {
        if (std::abs (song[i]) > 1.0e-6f)
        {
            if ((juce::int64) i - lastLoud >= quiet)
                clicks.push_back ((juce::int64) i);

            lastLoud = (juce::int64) i;
        }
    }

    return clicks;
}

} // namespace

TEST_CASE ("A track's input lists the device's inputs, alone and in pairs, is one undo step, and is heard while armed")
{
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    auto song = phazite::createSong();
    const RealTimePlayback playback; // a device with two inputs
    setSong (context, song.get());
    const auto track = phazite::getTrackIDs (*song).front();

    const auto inputs = phazite::getAudioInputs (context);
    REQUIRE (inputs.size() == 3);
    CHECK (inputs[0].name == juce::String ("In 1"));
    CHECK (inputs[1].name == juce::String ("In 2"));
    CHECK (inputs[2].name == juce::String ("In 1+2"));
    CHECK (inputs[2].numChannels == 2);

    // Without an input, a track can't be armed.
    CHECK (phazite::canRecordAudio (context, track));
    CHECK (phazite::getTrackInput (context, track).isEmpty());
    phazite::setArmed (context, track, true);
    CHECK_FALSE (phazite::isArmed (context, track));

    phazite::setTrackInput (context, track, "1+2");
    CHECK (phazite::getTrackInput (context, track) == juce::String ("1+2"));
    CHECK (phazite::getLastInput (context) == juce::String ("1+2"));
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Change Input"));
    phazite::setArmed (context, track, true);
    CHECK (phazite::isArmed (context, track));
    CHECK (phazite::getArmedTracks (context) == std::vector { track });
    CHECK (phazite::getInputMonitoring (context, track) == phazite::InputMonitoring::whileArmed);
    phazite::setInputMonitoring (context, track, phazite::InputMonitoring::never);
    CHECK (phazite::getInputMonitoring (context, track) == phazite::InputMonitoring::never);
    CHECK (song->getUndoManager().getUndoDescription() == juce::String ("Change Input Monitoring"));

    // Undoing the input leaves the track without one, and disarmed; arming is no undo step and isn't saved.
    phazite::undo (*song);
    phazite::undo (*song);
    CHECK (phazite::getTrackInput (context, track).isEmpty());
    CHECK_FALSE (phazite::isArmed (context, track));
    CHECK_FALSE (song->getState().toXmlString().contains ("armed"));

    // A track with an instrument plays what is played on MIDI instead.
    phazite::setTrackInput (context, track, "1");
    REQUIRE (phazite::setInstrument (*song, track, phazite::makeOwnDeviceState (*song, "phaziteSynth")).isValid());
    CHECK_FALSE (phazite::canRecordAudio (context, track));
    phazite::setArmed (context, track, true);
    CHECK_FALSE (phazite::isArmed (context, track));
    setSong (context, nullptr);
}

//==============================================================================
// Takes driven a block at a time (EnginePlayback.h): the device's outputs come back into its inputs a block later, which it
// reports as its inputs' delay, so that the metronome's clicks, recorded, land where they played, to the sample.
namespace
{
using phazite::test::EnginePlayback;
using phazite::test::TestSong;

// A song at 120 BPM with the metronome on, whose recordings go into its project's Audio folder, its first track
// recording input 1, unheard (its sound would come back into its input), armed.
struct EngineStudio
{
    EngineStudio()
    {
        song.setTempo (120.0);
        song.setSwitch ("transport.metronome", true);
        song.setSwitch ("transport.countIn", false);
        phazite::setTrackInput (song.context, song.firstTrack, "1");
        phazite::setInputMonitoring (song.context, song.firstTrack, phazite::InputMonitoring::never);
        phazite::setArmed (song.context, song.firstTrack, true);
        REQUIRE (phazite::isArmed (song.context, song.firstTrack));
        song.context.getUndoManager()->clearUndoHistory();
    }

    bool isRecording() const { return song.commands.find ("transport.record")->isTicked(); }

    // Records for so long, from where playback starts or the song is.
    void record (double seconds)
    {
        song.run ("transport.record");
        REQUIRE (isRecording());
        playback.play (seconds);
        song.run ("transport.record");
        CHECK_FALSE (isRecording());
    }

    // Where the clicks start in the song as Phazite's engine renders it, from its start.
    std::vector<juce::int64> renderClicks (double seconds)
    {
        const auto audio = phazite::renderWithOwnEngine (*phazite::findSong (song.context), (int) (seconds * EnginePlayback::sampleRate),
                                                         EnginePlayback::sampleRate, EnginePlayback::blockSize);
        return findClicks ({ audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples() }, EnginePlayback::sampleRate);
    }

    TestSong song;
    EnginePlayback playback { EnginePlayback::Input::loopback };
};

// The clicks' first sample that isn't silent, on a beat (EnginePlayback::sampleRate): a click starts at no level.
juce::int64 clickOn (double beat)
{
    return (juce::int64) std::llround (beat * 24000.0) + 1;
}
} // namespace

TEST_CASE ("What comes into an armed track's input while recording becomes a clip there, to the sample")
{
    EngineStudio studio;
    phazite::movePlayhead (studio.song.context, phazite::BeatPosition::fromBeats (0.5)); // between beats
    studio.record (2.2);
    CHECK (studio.song.context.getUndoManager()->getUndoDescription() == juce::String ("Record"));

    const auto clips = phazite::getClips (studio.song.context, studio.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].kind == phazite::ClipType::audio);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.5));
    CHECK (clips[0].range.getLength().inBeats() == doctest::Approx (4.4 - 256.0 / 24000.0)); // a block less: it came in late

    // Each click where it played, the first beat after the start too: the input's delay is known, and so is where the
    // take started.
    const auto clicks = studio.renderClicks (2.6);
    REQUIRE (clicks.size() == 4);

    for (size_t i = 0; i < clicks.size(); ++i)
    {
        CAPTURE (i);
        CHECK (clicks[i] == clickOn ((double) i + 1.0));
    }

    // Undone, it is gone.
    phazite::undo (*phazite::findSong (studio.song.context));
    CHECK (phazite::getClips (studio.song.context, studio.song.firstTrack).empty());
}

TEST_CASE ("A take started while the song plays lands as one started from a stop does")
{
    EngineStudio studio;
    studio.song.run ("transport.playStop");
    studio.playback.play (0.3); // into the first beat
    studio.record (1.2);

    const auto clips = phazite::getClips (studio.song.context, studio.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.6)); // where the song was as recording started

    const auto clicks = studio.renderClicks (1.6);
    REQUIRE (clicks.size() == 2);
    CHECK (clicks[0] == clickOn (1.0));
    CHECK (clicks[1] == clickOn (2.0));
}

TEST_CASE ("Recording round a looped range of any length makes takes of one clip, the last whole one playing")
{
    EngineStudio studio;
    phazite::loopBeatRange (*phazite::findSong (studio.song.context), 0.0, 2.0); // a second: a loop of any length
    studio.record (2.5);                                                         // twice round and a half

    const auto clips = phazite::getClips (studio.song.context, studio.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (2.0));
    const auto takes = phazite::getClipTakes (studio.song.context, clips[0].id);
    CHECK (takes.count == 3);
    CHECK (takes.playing == 2);
    CHECK (phazite::isLoopOn (studio.song.context));

    // The second time round's clicks, on their beats.
    const auto clicks = studio.renderClicks (1.0);
    REQUIRE (clicks.size() == 2);
    CHECK (clicks[0] == clickOn (0.0));
    CHECK (clicks[1] == clickOn (1.0));

    phazite::setClipTake (studio.song.context, clips[0].id, 1);
    CHECK (phazite::getClipTakes (studio.song.context, clips[0].id).playing == 1);
    CHECK (studio.song.context.getUndoManager()->getUndoDescription() == juce::String ("Choose Take"));
}

TEST_CASE ("Recorded from before a looped range, a take keeps its first time round whole, the part before the range a clip of its own")
{
    // Known bug 10 until 2026-11-10: what was played before the range was a take of the range's clip, which played it there.
    // The engine plays on from where recording starts to the range's end before it first goes round, as Bitwig records a
    // first take longer than the rest. Here beats 4 to 8 (2 to 4 seconds) are looped, and recording starts at the song's
    // start.
    EngineStudio studio;
    auto& context = studio.song.context;
    const auto track = studio.song.firstTrack;
    auto& song = *phazite::findSong (context);
    phazite::loopBeatRange (song, 4.0, 8.0);
    constexpr auto lateBlock = 256.0 / 24000.0;  // in beats: what came in a block late ends a block early
    constexpr auto firstFrame = 256.0 / 48000.0; // in seconds: the file's first frame came in a block after recording started

    // Where in its file a clip starts playing, in seconds.
    const auto playedFrom = [&song] (phazite::ClipID clip) { return phazite::getFilePart (phazite::findClipState (song, clip)).start; };

    // The clicks of the song rendered, each on its beat, from the first to the last given.
    const auto checkClicks = [&studio] (double seconds, int firstBeat, int lastBeat)
    {
        const auto clicks = studio.renderClicks (seconds);
        REQUIRE (clicks.size() == (size_t) (lastBeat - firstBeat + 1));

        for (size_t i = 0; i < clicks.size(); ++i)
        {
            CAPTURE (i);
            CHECK (clicks[i] == clickOn ((double) firstBeat + (double) i));
        }
    };

    SUBCASE ("Stopped in the range before going round: one clip of what was played, where it was played")
    {
        studio.record (3.0); // to beat 6
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (6.0 - lateBlock));
        CHECK (clips[0].takes == 0);
        CHECK (playedFrom (clips[0].id) == doctest::Approx (firstFrame));
        checkClicks (3.2, 0, 5);
    }

    SUBCASE ("Stopped before the range: one clip of what was played, where it was played")
    {
        studio.record (1.0); // to beat 2
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (2.0 - lateBlock));
        CHECK (clips[0].takes == 0);
        checkClicks (1.2, 0, 1);
    }

    SUBCASE ("Round once and part of the second time round: the part before the range, and the range's takes, the first playing")
    {
        studio.record (5.0); // to beat 10, beat 6 the second time round
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 2);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (4.0));
        CHECK (clips[0].takes == 0);
        CHECK (playedFrom (clips[0].id) == doctest::Approx (firstFrame));

        // The first time round's part in the range is the first take, the last whole one; the second is the second time round.
        CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (clips[1].range.getEnd().inBeats() == doctest::Approx (8.0));
        CHECK (clips[1].takes == 2);
        CHECK (clips[1].take == 1);
        CHECK (playedFrom (clips[1].id) == doctest::Approx (2.0 + firstFrame));
        checkClicks (4.2, 0, 7);

        phazite::setClipTake (context, clips[1].id, 2);
        CHECK (playedFrom (clips[1].id) == doctest::Approx (4.0 + firstFrame));
    }

    SUBCASE ("Round twice and part of the third time round: the second time round's take playing, the last whole one")
    {
        studio.record (7.0); // to beat 14, beat 6 the third time round
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 2);
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (4.0));
        CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (clips[1].range.getEnd().inBeats() == doctest::Approx (8.0));
        CHECK (clips[1].takes == 3);
        CHECK (clips[1].take == 2);
        CHECK (playedFrom (clips[1].id) == doctest::Approx (4.0 + firstFrame));
        checkClicks (4.2, 0, 7);
    }

    SUBCASE ("Recorded from a moment before the range, too short to keep: no clip of it, and the range's takes as from its start")
    {
        phazite::movePlayhead (context, phazite::BeatPosition::fromBeats (4.0 - 0.01)); // 5 ms before
        studio.record (2.5);                                                            // to beat 8.99, beat 4.99 the second time round
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (clips[0].takes == 2);
        CHECK (clips[0].take == 1);
    }

    // Recorded from beat 5, within the range, for so long: the take that shows while it records, just before it stops
    // (RecordingModel.h), which is the finished clip's last. Every time round is a take, the first too, as Logic Pro and Bitwig
    // keep a partial first pass: its file starts with silence from the range's start, so that each take lies where the range
    // does, as long as it, one after another.
    const auto recordFromWithin = [&studio, &context] (double seconds)
    {
        phazite::movePlayhead (context, phazite::BeatPosition::fromBeats (5.0));
        studio.song.run ("transport.record");
        REQUIRE (studio.isRecording());
        studio.playback.play (seconds);
        const auto shown = phazite::getRecordingsInProgress (context);
        REQUIRE (shown.size() == 1);
        studio.song.run ("transport.record");
        return shown[0].take;
    };

    SUBCASE ("Recorded from within the range, stopped before going round: one clip of what was played, without the silence before it")
    {
        const auto shownTake = recordFromWithin (1.0); // from beat 5 to beat 7
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (5.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (7.0 - lateBlock));
        CHECK (clips[0].takes == 0);
        CHECK (shownTake == 1);
        CHECK (playedFrom (clips[0].id) == doctest::Approx (0.5)); // past the silence from the range's start
        checkClicks (3.6, 5, 6);
    }

    SUBCASE ("Recorded from within the range, stopped in the second time round: two takes, the first holding the first time round")
    {
        const auto shownTake = recordFromWithin (3.0); // from beat 5 to beat 11, beat 7 the second time round
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (8.0));
        CHECK (clips[0].takes == 2);
        CHECK (clips[0].take == 2); // neither is whole: the last plays
        CHECK (shownTake == clips[0].takes);
        CHECK (playedFrom (clips[0].id) == doctest::Approx (2.0));
        checkClicks (4.2, 4, 6);

        // The first take: silence from the range's start, then what was played from beat 5, where it was played.
        phazite::setClipTake (context, clips[0].id, 1);
        CHECK (playedFrom (clips[0].id) == doctest::Approx (0.0));
        checkClicks (4.2, 5, 7);
    }

    SUBCASE ("Recorded from within the range, stopped in the third time round: three takes, the whole second one playing")
    {
        const auto shownTake = recordFromWithin (5.0); // from beat 5 to beat 15, beat 7 the third time round
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].takes == 3);
        CHECK (clips[0].take == 2);
        CHECK (shownTake == clips[0].takes);
        CHECK (playedFrom (clips[0].id) == doctest::Approx (2.0));
        checkClicks (4.2, 4, 7);
    }
}

TEST_CASE ("While recording from before a looped range, the part before it shows as a clip of its own once recording has gone round")
{
    // Ableton Live draws a recording growing where it goes (RecordingModel.h: getRecordingsInProgress()). Beats 4 to 8 are
    // looped; a tone on another track plays before them, which comes back into the input with the clicks.
    EngineStudio studio;
    auto& context = studio.song.context;
    const auto track = studio.song.firstTrack;
    phazite::test::TemporaryFolder sounds;
    const auto tone = studio.song.addTrack ("Tone");
    studio.song.addAudioClip (tone, phazite::test::writeSound (sounds.folder.getChildFile ("Tone.wav"), 2.0), 0.0); // beats 0 to 4
    phazite::loopBeatRange (*phazite::findSong (context), 4.0, 8.0);
    EnginePlayback::waitForFiles();

    // The first time round: one clip, from where recording started, as it will be if it stops before going round.
    studio.song.run ("transport.record");
    REQUIRE (studio.isRecording());
    studio.playback.play (3.0); // to beat 6
    auto recordings = phazite::getRecordingsInProgress (context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (6.0 - 256.0 / 24000.0));
    CHECK (recordings[0].take == 1);

    // Round once: the part before the range, whole, and this time round from the range's start, its second take.
    studio.playback.play (2.0); // to beat 10, beat 6 the second time round
    recordings = phazite::getRecordingsInProgress (context);
    REQUIRE (recordings.size() == 2);
    std::sort (recordings.begin(), recordings.end(), [] (const auto& a, const auto& b) { return a.range.getStart() < b.range.getStart(); });
    CHECK (recordings[0].track == track);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (4.0));
    CHECK (recordings[0].take == 1);
    CHECK (recordings[1].track == track);
    CHECK (recordings[1].range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (recordings[1].range.getEnd().inBeats() == doctest::Approx (6.0 - 256.0 / 24000.0));
    CHECK (recordings[1].take == 2);

    // The part before the range draws the first time round's sound, the tone (between the clicks too); the range, this time
    // round's, the clicks alone.
    const auto levels = [&context, track] (double from, double to)
    {
        return phazite::getRecordingLevels (
            context, track, phazite::BeatRange (phazite::BeatPosition::fromBeats (from), phazite::BeatPosition::fromBeats (to)), 4);
    };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&levels] { return levels (5.0, 6.0)[0] > 0.0f; }));
    CHECK (levels (2.0, 3.0)[1] > 0.1f);  // a quarter of a beat after a click: the tone
    CHECK (levels (5.0, 6.0)[1] == 0.0f); // the same, this time round: silence
    studio.song.run ("transport.record");

    // Stopped, the clips it made are where they showed.
    const auto clips = phazite::getClips (context, track);
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (4.0));
    CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("A take whose writing thread stalls stays in time, with silence where its sound was lost, and Phazite says where")
{
    // Known bug 12 until 2026-11-10: what came in after a stall longer than the inputs' buffers (8 seconds) played early by
    // as long as was lost, without a word. Ableton Live leaves "a gap in the recorded sample", Audacity "will insert zeroes
    // into the recording to keep the other good parts synchronized" and labels where, and Pro Tools stops with an alert.
    EngineStudio studio;
    auto& context = studio.song.context;
    const auto track = studio.song.firstTrack;
    const auto trackName = phazite::getTrackName (*phazite::findSong (context), track);
    juce::StringArray problems;
    phazite::setProblemReporter (context, [&problems] (const juce::String& title, const juce::String& message)
                                 { problems.add (title + ": " + message); });

    // Records with the drive stalled for ten seconds: the input's buffer holds the first eight, less the block that no longer
    // fits, and the rest is lost (from 7.99 seconds, beat 4 of bar 4, for 2.0 seconds).
    const auto recordStalled = [&studio, &context]
    {
        studio.song.run ("transport.record");
        REQUIRE (studio.isRecording());
        auto* take = phazite::getSession (context)->getTake();
        REQUIRE (take != nullptr);
        take->holdWriting (true);
        studio.playback.play (10.0);
        return take;
    };

    // The drive catches up: the writing thread let go, and what waited in the buffer written (its click on beat 15 drawn).
    const auto catchUp = [&context, track] (phazite::Take& take)
    {
        take.holdWriting (false);
        const auto span = phazite::BeatRange (phazite::BeatPosition::fromBeats (15.0), phazite::BeatPosition::fromBeats (15.5));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getRecordingLevels (context, track, span, 1)[0] > 0.0f; }));
    };

    // Each click on its beat, as far as the last, but those of beats 16 to 19, which were lost.
    const auto clicksTo = [] (int lastBeat)
    {
        std::vector<juce::int64> clicks;

        for (int beat = 0; beat <= lastBeat; ++beat)
            if (beat < 16 || beat > 19)
                clicks.push_back (clickOn (beat));

        return clicks;
    };

    // What says where it was lost: one message.
    const auto checkSaid = [&problems] (const juce::String& where)
    {
        REQUIRE (problems.size() == 1);
        CHECK (problems[0].startsWith ("Part of the recording was lost: "));
        CHECK (problems[0].endsWith (": " + where + "."));
    };

    SUBCASE ("The drive catches up, and the song plays on: what came in after the stall is where it was played")
    {
        catchUp (*recordStalled());
        studio.playback.play (3.0);
        studio.song.run ("transport.record");

        // One clip of all thirteen seconds (26 beats, less the block that came in late).
        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (26.0 - 256.0 / 24000.0));
        CHECK (studio.renderClicks (13.0) == clicksTo (25));
        checkSaid (trackName + " at bar 4, beat 4 (2.0 s)");
    }

    SUBCASE ("Stopped while the drive still stalls: what was lost at the end, which the engine noted no gap for, is silence too")
    {
        recordStalled();
        studio.song.run ("transport.record");

        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (20.0 - 256.0 / 24000.0));
        CHECK (studio.renderClicks (10.0) == clicksTo (19));
        checkSaid (trackName + " at bar 4, beat 4 (2.0 s)");
    }

    SUBCASE ("Round a looped range: where it was lost in the range, and in which take")
    {
        phazite::loopBeatRange (*phazite::findSong (context), 0.0, 8.0); // four seconds: lost from the second time round
        recordStalled();
        studio.song.run ("transport.record");

        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].takes == 3);
        CHECK (clips[0].take == 2);
        checkSaid (trackName + " at bar 2, beat 4 of take 2 (2.0 s)");
    }

    SUBCASE ("Lost before a looped range that recording then went round: said where, before the range, whose clip keeps it")
    {
        phazite::loopBeatRange (*phazite::findSong (context), 24.0, 32.0); // 12 to 16 seconds
        catchUp (*recordStalled());
        studio.playback.play (7.0); // to beat 34, beat 26 the second time round
        studio.song.run ("transport.record");

        const auto clips = phazite::getClips (context, track);
        REQUIRE (clips.size() == 2);
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (24.0));
        CHECK (clips[1].takes == 2);
        checkSaid (trackName + " at bar 4, beat 4 (2.0 s)");
    }

    SUBCASE ("Nothing lost, nothing said")
    {
        studio.record (1.0);
        CHECK (phazite::getClips (context, track).size() == 1);
        CHECK (problems.isEmpty());
    }
}

TEST_CASE ("Punched in, only what falls in the looped range is kept, in place of what was there")
{
    EngineStudio studio;
    phazite::test::TemporaryFolder sounds;
    studio.song.addAudioClip (studio.song.firstTrack, phazite::test::writeSound (sounds.folder.getChildFile ("Before.wav"), 6.0), 0.0);
    phazite::loopBeatRange (*phazite::findSong (studio.song.context), 4.0, 8.0); // from 2 s to 4 s
    phazite::setPunchOn (studio.song.context, true);
    REQUIRE (phazite::canPunch (studio.song.context));
    studio.song.context.getUndoManager()->clearUndoHistory();

    studio.record (4.8);
    CHECK (phazite::isLoopOn (studio.song.context)); // the take didn't loop, and the loop is as it was

    // What was there, cut at the range's edges with a fade, and the take over the range reaching a little past them (half
    // of 10 ms, in beats at 120 BPM).
    auto clips = phazite::getClips (studio.song.context, studio.song.firstTrack);
    REQUIRE (clips.size() == 3);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (4.01).epsilon (0.0001));
    CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (3.99).epsilon (0.0001));
    CHECK (clips[1].range.getEnd().inBeats() == doctest::Approx (8.01).epsilon (0.0001));
    CHECK (clips[2].range.getStart().inBeats() == doctest::Approx (7.99).epsilon (0.0001));
    CHECK (clips[2].range.getEnd().inBeats() == doctest::Approx (12.0));

    phazite::undo (*phazite::findSong (studio.song.context));
    clips = phazite::getClips (studio.song.context, studio.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getLength().inBeats() == doctest::Approx (12.0));
}

TEST_CASE ("While recording, an armed track's input shows where it will go, growing, with its waveform as it comes in")
{
    // As Ableton Live draws a clip growing as it records (RecordingModel.h: getRecordingsInProgress()).
    EngineStudio studio;
    auto& context = studio.song.context;
    const auto track = studio.song.firstTrack;
    phazite::movePlayhead (context, phazite::BeatPosition::fromBeats (0.5));
    studio.song.run ("transport.record");
    REQUIRE (studio.isRecording());
    studio.playback.play (1.0); // to 2.5 beats

    // From where recording started to what has come in, which came in a block late.
    auto recordings = phazite::getRecordingsInProgress (context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].track == track);
    CHECK (recordings[0].source == phazite::RecordSource::input);
    CHECK (recordings[0].take == 1);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.5));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (2.5 - 256.0 / 24000.0));

    // Its waveform, in quarter beats, once the writing thread has written it: the clicks on beats 1 and 2, where they were
    // played, and silence between them. (Its levels are kept for 256 frames at a time, so the column before a click, which
    // ends where the click starts, has it too.)
    const auto span = phazite::BeatRange (phazite::BeatPosition::fromBeats (0.5), phazite::BeatPosition::fromBeats (2.5));
    const auto levels = [&context, track, span] { return phazite::getRecordingLevels (context, track, span, 8); };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&levels] { return levels()[6] > 0.0f; }));
    const auto shown = levels();
    CHECK (shown[0] == 0.0f);
    CHECK (shown[2] > 0.1f); // from beat 1
    CHECK (shown[4] == 0.0f);
    CHECK (shown[6] > 0.1f); // from beat 2

    // Stopped, the clip it made is there instead, as it was shown.
    studio.song.run ("transport.record");
    CHECK (phazite::getRecordingsInProgress (context).empty());
    const auto clips = phazite::getClips (context, track);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.5));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (2.5 - 256.0 / 24000.0));
}

TEST_CASE ("While recording round a looped range, the input shows this time round as its take; punched in, what falls in the range")
{
    SUBCASE ("Round a range a second long")
    {
        EngineStudio studio;
        phazite::loopBeatRange (*phazite::findSong (studio.song.context), 0.0, 2.0);
        studio.song.run ("transport.record");
        studio.playback.play (1.5); // half way round the second time

        const auto recordings = phazite::getRecordingsInProgress (studio.song.context);
        REQUIRE (recordings.size() == 1);
        CHECK (recordings[0].take == 2);
        CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (1.0 - 256.0 / 24000.0));

        // This time round's click on its first beat, where it played.
        const auto span = phazite::BeatRange (phazite::BeatPosition(), phazite::BeatPosition::fromBeats (1.0));
        const auto levels = [&studio, span] { return phazite::getRecordingLevels (studio.song.context, studio.song.firstTrack, span, 4); };
        REQUIRE (phazite::test::dispatchMessagesUntil ([&levels] { return levels()[0] > 0.0f; }));
        CHECK (levels()[2] == 0.0f);
        studio.song.run ("transport.record");
    }

    SUBCASE ("Punched in over beats 4 to 8: nothing before it, then from its start")
    {
        EngineStudio studio;
        phazite::loopBeatRange (*phazite::findSong (studio.song.context), 4.0, 8.0);
        phazite::setPunchOn (studio.song.context, true);
        studio.song.run ("transport.record");
        studio.playback.play (1.0); // to beat 2
        CHECK (phazite::getRecordingsInProgress (studio.song.context).empty());

        studio.playback.play (2.0); // to beat 6
        const auto recordings = phazite::getRecordingsInProgress (studio.song.context);
        REQUIRE (recordings.size() == 1);
        CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (6.0 - 256.0 / 24000.0));
        studio.song.run ("transport.record");
        phazite::setPunchOn (studio.song.context, false);
    }
}

TEST_CASE ("An input recorded on a new track adds a track called Audio after the selected one, armed with it, as one undo step")
{
    TestSong song;
    EnginePlayback playback; // a device with two inputs
    auto& context = song.context;
    auto& selection = getSelection (context);
    selection.selectOnly (song.firstTrack);
    song.context.getUndoManager()->clearUndoHistory();

    const auto track = phazite::addTrackForInput (context, "1+2");
    REQUIRE (track.isValid());
    const auto tracks = phazite::getTrackIDs (*phazite::findSong (context));
    REQUIRE (tracks.size() == 2);
    CHECK (tracks[1] == track); // after the selected track
    CHECK (phazite::getTrackName (*phazite::findSong (context), track) == juce::String ("Audio"));
    CHECK (phazite::getTrackInput (context, track) == juce::String ("1+2"));
    CHECK (phazite::getLastInput (context) == juce::String ("1+2"));
    CHECK (phazite::isArmed (context, track));
    CHECK (selection.getTracks() == std::vector { track });
    CHECK (context.getUndoManager()->getUndoDescription() == juce::String ("Add Audio Track"));
    CHECK (phazite::describeRecordTargets (context) == juce::String ("Audio (In 1+2)"));

    // The next is called Audio 2; an input the device doesn't have adds none.
    const auto second = phazite::addTrackForInput (context, "1");
    CHECK (phazite::getTrackName (*phazite::findSong (context), second) == juce::String ("Audio 2"));
    CHECK (phazite::describeRecordTargets (context) == juce::String ("Audio (In 1+2) and Audio 2 (In 1)"));
    CHECK_FALSE (phazite::addTrackForInput (context, "3+4").isValid());

    // Undone, each goes, and nothing stays armed.
    phazite::undo (*phazite::findSong (context));
    phazite::undo (*phazite::findSong (context));
    CHECK (phazite::getTrackIDs (*phazite::findSong (context)).size() == 1);
    CHECK (phazite::getArmedTracks (context).empty());
}

TEST_CASE ("A track hears its input while it is armed, or always, as its monitoring says, stopped or playing")
{
    TestSong song;
    EnginePlayback playback (EnginePlayback::Input::tone);
    phazite::setTrackInput (song.context, song.firstTrack, "1");
    const auto level = [&playback]
    {
        playback.play (0.05); // the change, and the fader easing
        const auto heard = playback.play (0.1);
        return heard.getMagnitude (0, 0, heard.getNumSamples());
    };

    CHECK (level() == 0.0f); // not armed

    phazite::setArmed (song.context, song.firstTrack, true);
    CHECK (level() > 0.1f);
    song.run ("transport.playStop");
    CHECK (level() > 0.1f);
    song.run ("transport.playStop");

    phazite::setInputMonitoring (song.context, song.firstTrack, phazite::InputMonitoring::never);
    CHECK (level() == 0.0f);

    phazite::setArmed (song.context, song.firstTrack, false);
    phazite::setInputMonitoring (song.context, song.firstTrack, phazite::InputMonitoring::always);
    CHECK (level() > 0.1f);
}

TEST_CASE ("The audio settings' correction moves what an input records earlier")
{
    // On the tests' audio device, in real time, which has the inputs that keep the correction: its output comes back a
    // block later, which it reports as its delay, so that the clicks would land on their beats, and the correction puts
    // them a block earlier.
    TestSong song;
    const RealTimePlayback playback (RealTimePlayback::Input::loopback);
    song.setTempo (120.0);
    song.setSwitch ("transport.metronome", true);
    song.setSwitch ("transport.countIn", false);
    phazite::setTrackInput (song.context, song.firstTrack, "1");
    phazite::setInputMonitoring (song.context, song.firstTrack, phazite::InputMonitoring::never);
    phazite::setArmed (song.context, song.firstTrack, true);
    RealTimePlayback::wait (300); // the inputs opened
    REQUIRE (phazite::isArmed (song.context, song.firstTrack));
    auto& audioIO = phazite::getAudioIO (phazite::test::getEngineContext());
    audioIO.setRecordingCorrection (256.0 / 48.0); // a block, in milliseconds

    song.run ("transport.record");
    RealTimePlayback::wait (1200);
    song.run ("transport.record");
    audioIO.setRecordingCorrection (0.0);

    const auto audio = phazite::renderWithOwnEngine (*phazite::findSong (song.context), (int) (1.2 * RealTimePlayback::sampleRate),
                                                     RealTimePlayback::sampleRate, 256);
    const auto clicks =
        findClicks ({ audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples() }, RealTimePlayback::sampleRate);
    REQUIRE (clicks.size() >= 2);
    CHECK (clicks[clicks.size() - 2] == clickOn (1.0) - 256);
    CHECK (clicks.back() == clickOn (2.0) - 256);
}
