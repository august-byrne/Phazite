// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording audio (src/model/transport/RecordingModel.h, internal/AudioRecorder.h; roadmap step 3.4): a track's input,
// how it is heard and armed, and takes recorded in real time through Tracktion's hosted audio device, whose output comes
// back into its input a block later, as a cable from an audio interface's output to its input would bring it: the
// metronome's clicks, recorded, show where Phazite puts what comes in.
#include "RealTimePlayback.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

namespace
{
using phazite::test::RealTimePlayback;

// An Edit whose recordings go into a project folder of their own (its Audio folder), with one track recording input 1,
// unheard (its sound would come back into its input), armed.
struct Studio
{
    explicit Studio (double bpm = 120.0)
    {
        // The project's file is there, as a project's always is: the paths of the files its clips play are relative to it.
        const auto file = temp.folder.getChildFile ("Song.phazite");
        REQUIRE (file.create());
        edit->editFileRetriever = [file] { return file; };
        phazite::setTempo (*edit, bpm);
        edit->getUndoManager().clearUndoHistory();
        phazite::setCountInOn (*edit, false);
        phazite::setMetronomeOn (*edit, true);
        edit->getTransport().looping = false;
        track = te::getAudioTracks (*edit)[0];
        track->state.setProperty (phazite::schema::phaziteInput, "1", nullptr);
        track->state.setProperty (phazite::schema::phaziteMonitoring, phazite::schema::monitorNever, nullptr);
    }

    // Arms the track, once the device's inputs are there, and waits for Tracktion to have made them as the track needs.
    void arm()
    {
        recorder->setArmed (*track, true);
        RealTimePlayback::wait (300);
        REQUIRE (recorder->canRecord());
    }

    // Records for so long, from where playback starts.
    void record (int milliseconds)
    {
        phazite::startRecording (*edit);
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return phazite::isRecording (*edit); }));
        RealTimePlayback::wait (milliseconds);
        phazite::stopRecording (*edit);
    }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::AudioTrack* track = nullptr;
    std::unique_ptr<phazite::AudioRecorder> recorder; // made once playback runs
};

// The clips' audio as the song plays it: each clip's file read from its offset, at its place.
std::vector<float> renderTrack (te::AudioTrack& track, double seconds, double rate)
{
    std::vector<float> song ((size_t) (seconds * rate), 0.0f);

    for (auto* clip : track.getClips())
        if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
        {
            const auto file = audio->getSourceFileReference().getFile();
            auto stream = file.createInputStream();
            REQUIRE (stream != nullptr);
            std::unique_ptr<juce::AudioFormatReader> reader (juce::WavAudioFormat().createReaderFor (stream.release(), true));
            REQUIRE (reader != nullptr);
            const auto position = audio->getPosition();
            const auto from = (juce::int64) std::llround (position.getStart().inSeconds() * rate);
            const auto length = (int) std::llround (position.getLength().inSeconds() * rate);
            juce::AudioBuffer<float> buffer (1, length);
            reader->read (&buffer, 0, length, (juce::int64) std::llround (position.getOffset().inSeconds() * rate), true, false);

            for (int i = 0; i < length; ++i)
                if (from + i < (juce::int64) song.size())
                    song[(size_t) (from + i)] += buffer.getSample (0, i);
        }

    return song;
}

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

// Keeps the message thread busy, as a slow computer's can be, between Tracktion starting to play for a recording and
// saying that recording started: blocks go by meanwhile.
struct SlowToHear : te::TransportControl::Listener
{
    explicit SlowToHear (te::TransportControl& t)
        : transport (t)
    {
        transport.addListener (this);
    }

    ~SlowToHear() override { transport.removeListener (this); }

    void startVideo() override { std::this_thread::sleep_for (std::chrono::milliseconds (50)); } // once it plays

    te::TransportControl& transport;
};
} // namespace

TEST_CASE ("A track's input lists the device's inputs, alone and in pairs, is one undo step, and is heard while armed")
{
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const RealTimePlayback playback (phazite::test::getEngine()); // a device with two inputs
    setEdit (context, edit.get());
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);

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
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Input"));
    phazite::setArmed (context, track, true);
    CHECK (phazite::isArmed (context, track));
    CHECK (phazite::getArmedTracks (context) == std::vector { track });
    CHECK (phazite::getInputMonitoring (context, track) == phazite::InputMonitoring::whileArmed);
    phazite::setInputMonitoring (context, track, phazite::InputMonitoring::never);
    CHECK (phazite::getInputMonitoring (context, track) == phazite::InputMonitoring::never);
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Input Monitoring"));

    // Undoing the input leaves the track without one, and disarmed; arming is no undo step and isn't saved.
    phazite::undo (*edit);
    phazite::undo (*edit);
    CHECK (phazite::getTrackInput (context, track).isEmpty());
    CHECK_FALSE (phazite::isArmed (context, track));
    CHECK_FALSE (edit->state.toXmlString().contains ("armed"));

    // A track with an instrument plays what is played on MIDI instead.
    phazite::setTrackInput (context, track, "1");
    auto* audioTrack = te::getAudioTracks (*edit)[0];
    audioTrack->pluginList.insertPlugin (edit->getPluginCache().createNewPlugin ("phaziteSynth", {}), 0, nullptr);
    CHECK_FALSE (phazite::canRecordAudio (context, track));
    phazite::setArmed (context, track, true);
    CHECK_FALSE (phazite::isArmed (context, track));
    setEdit (context, nullptr);
}

TEST_CASE ("What comes into an armed track's input while recording becomes a clip there, in time, as one undo step")
{
    Studio studio;
    const RealTimePlayback playback (phazite::test::getEngine(), RealTimePlayback::Input::loopback);
    studio.recorder = std::make_unique<phazite::AudioRecorder> (*studio.edit);
    studio.arm();

    // Two seconds and a bit: four beats of clicks.
    studio.record (2200);
    CHECK_FALSE (phazite::isRecording (*studio.edit));
    CHECK (studio.edit->getUndoManager().getUndoDescription() == juce::String ("Record"));

    const auto clips = studio.track->getClips();
    REQUIRE (clips.size() == 1);
    auto* clip = dynamic_cast<te::WaveAudioClip*> (clips.getFirst());
    REQUIRE (clip != nullptr);
    CHECK (clip->getPosition().getStart().inSeconds() == doctest::Approx (0.0).epsilon (0.001));
    CHECK (clip->getPosition().getLength().inSeconds() > 1.8);
    const auto file = clip->getSourceFileReference().getFile();
    CHECK (file.getParentDirectory().getFullPathName() == studio.temp.folder.getChildFile ("Audio").getFullPathName());
    CHECK (file.getFileName() == juce::String ("Track 1 Take 1.wav"));

    // Each click on its beat: the device's output comes back into its input a block (256 frames at 48 kHz) later, and
    // Tracktion's clock puts each block that comes in where the song played the block before.
    const auto rate = 48000.0;
    const auto clicks = findClicks (renderTrack (*studio.track, 2.2, rate), rate);
    REQUIRE (clicks.size() >= 3);
    juce::String found; // all of them, in a failure's report

    for (auto click : clicks)
        found << juce::String (click) << " ";

    CAPTURE (found);

    for (size_t beat = 0; beat < clicks.size(); ++beat)
    {
        CAPTURE (beat);
        CHECK (clicks[beat] == (juce::int64) (beat * 24000));
    }

    // Undone, it is gone (its file stays, for redo).
    phazite::undo (*studio.edit);
    CHECK (studio.track->getClips().isEmpty());
}

TEST_CASE ("A take has all of its start however long the message thread takes to hear that recording started")
{
    Studio studio;
    const RealTimePlayback playback (phazite::test::getEngine(), RealTimePlayback::Input::loopback);
    studio.recorder = std::make_unique<phazite::AudioRecorder> (*studio.edit);
    studio.arm();
    const SlowToHear slow (studio.edit->getTransport()); // some 9 blocks
    studio.record (1200);

    const auto clips = studio.track->getClips();
    REQUIRE (clips.size() == 1);
    CHECK (clips.getFirst()->getPosition().getStart().inSeconds() == doctest::Approx (0.0).epsilon (0.001));
    const auto clicks = findClicks (renderTrack (*studio.track, 1.2, 48000.0), 48000.0);
    REQUIRE (clicks.size() >= 2);
    CHECK (clicks[0] == 0); // the first beat's, on its beat, as in the test above
    CHECK (clicks[1] == 24000);
}

TEST_CASE ("A take started while the song plays lands as one started from a stop does")
{
    Studio studio;
    const RealTimePlayback playback (phazite::test::getEngine(), RealTimePlayback::Input::loopback);
    studio.recorder = std::make_unique<phazite::AudioRecorder> (*studio.edit);
    studio.arm();
    phazite::playOrStop (*studio.edit);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&studio] { return studio.edit->getTransport().isPlaying(); }));
    RealTimePlayback::wait (300); // into the first beat
    studio.record (1200);

    const auto clips = studio.track->getClips();
    REQUIRE (clips.size() == 1);
    CHECK (clips.getFirst()->getPosition().getStart().inSeconds() > 0.1);
    CHECK (clips.getFirst()->getPosition().getStart().inSeconds() < 0.5);

    // The second and third beats' clicks, on their beats.
    const auto clicks = findClicks (renderTrack (*studio.track, 1.2, 48000.0), 48000.0);
    REQUIRE (clicks.size() >= 2);
    CHECK (clicks[0] == 24000);
    CHECK (clicks[1] == 48000);
}

TEST_CASE ("The audio settings' correction moves what an input records earlier")
{
    Studio studio;
    const RealTimePlayback playback (phazite::test::getEngine(), RealTimePlayback::Input::loopback);
    studio.recorder = std::make_unique<phazite::AudioRecorder> (*studio.edit);
    studio.arm();
    phazite::setRecordingCorrection (phazite::test::getEngine(), 256.0 / 48.0); // a block, in milliseconds
    studio.record (1200);
    phazite::setRecordingCorrection (phazite::test::getEngine(), 0.0);

    // The second and third beats' clicks a block early (the first beat's starts before the take, which cuts it).
    const auto clicks = findClicks (renderTrack (*studio.track, 1.2, 48000.0), 48000.0);
    REQUIRE (clicks.size() >= 2);
    CHECK (clicks[clicks.size() - 2] == 24000 - 256);
    CHECK (clicks.back() == 48000 - 256);
}

TEST_CASE ("Recording round a looped range makes takes of one clip, the last whole one playing, and another is one undo step")
{
    Studio studio (110.0); // a bar of 4/4 is a little over two seconds, as Tracktion's looped recording needs
    const auto bar = 240.0 / 110.0;
    phazite::loopRange (*studio.edit, { te::TimePosition(), te::TimePosition::fromSeconds (bar) });
    const RealTimePlayback playback (phazite::test::getEngine(), RealTimePlayback::Input::loopback);
    studio.recorder = std::make_unique<phazite::AudioRecorder> (*studio.edit);
    studio.arm();

    // Twice round and a half.
    studio.record ((int) (2.5 * bar * 1000.0));

    const auto clips = studio.track->getClips();
    REQUIRE (clips.size() == 1);
    auto* clip = dynamic_cast<te::WaveAudioClip*> (clips.getFirst());
    REQUIRE (clip != nullptr);
    CHECK (clip->getPosition().time.getStart().inSeconds() == doctest::Approx (0.0).epsilon (0.001));
    CHECK (clip->getPosition().time.getEnd().inSeconds() == doctest::Approx (bar).epsilon (0.001));

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, studio.edit.get());
    const auto id = phazite::getClipID (*clip);
    const auto takes = phazite::getClipTakes (context, id);
    CHECK (takes.count == 3);
    CHECK (takes.playing == 2);
    const auto offset = clip->getPosition().getOffset().inSeconds();

    phazite::setClipTake (context, id, 1);
    CHECK (phazite::getClipTakes (context, id).playing == 1);
    CHECK (clip->getPosition().getOffset().inSeconds() == doctest::Approx (offset - bar).epsilon (0.0001));
    CHECK (studio.edit->getUndoManager().getUndoDescription() == juce::String ("Choose Take"));
    phazite::undo (*studio.edit);
    CHECK (phazite::getClipTakes (context, id).playing == 2);
    setEdit (context, nullptr);
}

TEST_CASE ("Punched in, only what falls in the looped range is kept, in place of what was there, cross-fading at its edges")
{
    Studio studio;
    phazite::test::TemporaryFolder sounds;
    const auto before =
        studio.track->insertWaveClip ("Before", phazite::test::writeSound (sounds.folder.getChildFile ("Before.wav"), 6.0),
                                      { { te::TimePosition(), te::TimePosition::fromSeconds (6.0) }, te::TimeDuration() }, false);
    REQUIRE (before != nullptr);
    phazite::loopRange (*studio.edit, { te::TimePosition::fromSeconds (2.0), te::TimePosition::fromSeconds (4.0) });
    phazite::setPunchOn (*studio.edit, true);
    REQUIRE (phazite::canPunch (*studio.edit));
    const RealTimePlayback playback (phazite::test::getEngine(), RealTimePlayback::Input::loopback);
    studio.recorder = std::make_unique<phazite::AudioRecorder> (*studio.edit);
    studio.arm();
    studio.edit->getUndoManager().clearUndoHistory();

    studio.record (4800);
    CHECK (phazite::isLoopOn (*studio.edit)); // back on after the take

    // What was there, cut at the range's edges with a fade, and the take over the range reaching a little past them.
    std::vector<te::TimeRange> ranges;

    for (auto* clip : studio.track->getClips())
        ranges.push_back (clip->getPosition().time);

    std::sort (ranges.begin(), ranges.end(), [] (auto a, auto b) { return a.getStart() < b.getStart(); });
    REQUIRE (ranges.size() == 3);
    CHECK (ranges[0].getStart().inSeconds() == doctest::Approx (0.0));
    CHECK (ranges[0].getEnd().inSeconds() == doctest::Approx (2.005).epsilon (0.0001));
    CHECK (ranges[1].getStart().inSeconds() == doctest::Approx (1.995).epsilon (0.0001));
    CHECK (ranges[1].getEnd().inSeconds() == doctest::Approx (4.005).epsilon (0.0001));
    CHECK (ranges[2].getStart().inSeconds() == doctest::Approx (3.995).epsilon (0.0001));
    CHECK (ranges[2].getEnd().inSeconds() == doctest::Approx (6.0));

    // Undone, the take is gone and what was there is whole again.
    phazite::undo (*studio.edit);
    REQUIRE (studio.track->getClips().size() == 1);
    CHECK (studio.track->getClips().getFirst()->getPosition().getLength().inSeconds() == doctest::Approx (6.0));
}
