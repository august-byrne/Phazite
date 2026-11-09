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
#include "model/transport/internal/Tempo.h"

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
