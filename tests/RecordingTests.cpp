// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording MIDI and Capture (src/model/transport/internal/Recording.h, Take.h, Capture.h): the settings and their
// commands, takes recorded through Phazite's engine as the app's audio device would play it, and what Capture makes of
// notes played a moment ago.
#include "EnginePlayback.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "engine/nodes/DeviceRunner.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Capture.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "ui/pianoroll/PianoRoll.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

TEST_CASE ("Overdub and quantizing recordings are the app's settings, which their commands switch")
{
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());

    auto song = phazite::createSong();
    phazite::Selection selection;
    selection.setSong (song.get());
    phazite::CommandRegistry registry;
    phazite::addRecordingCommands (registry, selection, audio);

    const auto* overdub = registry.find ("transport.overdub");
    const auto* quantize = registry.find ("transport.quantizeRecordings");
    REQUIRE (overdub != nullptr);
    REQUIRE (quantize != nullptr);
    CHECK (overdub->isTicked()); // at first
    CHECK_FALSE (quantize->isTicked());

    REQUIRE (registry.invoke ("transport.overdub"));
    CHECK_FALSE (phazite::isOverdubOn (audio));

    REQUIRE (registry.invoke ("transport.quantizeRecordings"));
    CHECK (phazite::isRecordQuantizeOn (audio));
    CHECK (quantize->isTicked());

    // Back as they were, for the other tests.
    phazite::setOverdubOn (audio, true);
    phazite::setRecordQuantizeOn (audio, false);
    CHECK (overdub->isTicked());
    CHECK_FALSE (quantize->isTicked());
    selection.setSong (nullptr);
}

TEST_CASE ("Recording needs a track for live MIDI to play")
{
    phazite::test::TestSong song;
    auto& songState = *phazite::findSong (song.context);
    phazite::routeLiveMidiInput (songState);
    CHECK_FALSE (phazite::canRecord (songState)); // no instrument anywhere
    CHECK_FALSE (phazite::canToggleRecording (songState));

    phazite::startRecording (songState);
    CHECK_FALSE (phazite::isRecording (songState));

    song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    phazite::routeLiveMidiInput (songState);
    CHECK (phazite::canRecord (songState));
}

//==============================================================================
namespace
{
// Note starts, in seconds, on a grid of beats at a tempo: each beat in a list, where 0.5 is an eighth after the first.
std::vector<double> atTempo (double bpm, std::initializer_list<double> beats, double jitterSeconds = 0.0, int seed = 1)
{
    juce::Random random (seed);
    std::vector<double> starts;

    for (const auto beat : beats)
        starts.push_back (beat * 60.0 / bpm + (random.nextDouble() * 2.0 - 1.0) * jitterSeconds);

    return starts;
}

} // namespace

TEST_CASE ("Capture's tempo is the one at which the notes fall best on a grid, more of them on beats")
{
    using phazite::detectTempo;

    // Eighths and quarters at 100 BPM.
    CHECK (detectTempo (atTempo (100.0, { 0, 1, 1.5, 2, 3, 3.5, 4, 5, 6, 6.5, 7 })).value_or (0.0) == doctest::Approx (100.0));

    // A sixteenth beat at 128, played up to 15 ms early or late.
    CHECK (detectTempo (atTempo (128.0, { 0, 0.75, 1, 1.5, 2, 2.25, 2.75, 3, 3.5, 4, 4.75, 5, 5.5, 6, 6.25, 6.75, 7, 7.5 }, 0.015))
               .value_or (0.0)
           == doctest::Approx (128.0).epsilon (0.01));

    // Quarter notes at 90: not dotted eighths at 135.
    CHECK (detectTempo (atTempo (90.0, { 0, 1, 2, 3, 4, 5, 6, 7 })).value_or (0.0) == doctest::Approx (90.0));

    // Quarter notes at 60 are eighths at 120: one octave only.
    CHECK (detectTempo (atTempo (60.0, { 0, 1, 2, 3, 4, 5 })).value_or (0.0) == doctest::Approx (120.0));

    // Too few notes, too short a time, or notes at random: no tempo.
    CHECK_FALSE (detectTempo (atTempo (120.0, { 0, 1, 2 })).has_value());
    CHECK_FALSE (detectTempo (atTempo (120.0, { 0, 0.5, 1, 1.5 })).has_value());

    juce::Random random (7);
    std::vector<double> scattered;

    for (int i = 0; i < 24; ++i)
        scattered.push_back (i * 0.37 + random.nextDouble() * 0.3);

    CHECK_FALSE (detectTempo (scattered).has_value());
}

//==============================================================================
// Recording MIDI and Capture through Phazite's engine (model/transport/internal/Take.h), driven a block at a time: a note
// played between two of the test's calls comes in where the engine was, to within a block.
namespace
{
using phazite::test::EnginePlayback;
using phazite::test::TestSong;

// A song at 120 BPM (a beat is half a second) whose first track has an instrument, which live MIDI plays.
struct EngineTake
{
    EngineTake()
    {
        song.setTempo (120.0);
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        song.setSwitch ("transport.countIn", false);
        phazite::routeLiveMidiInput (*phazite::findSong (song.context)); // live MIDI plays the track with an instrument
        playback.play (0.01);
        REQUIRE (phazite::getLiveMidiTrack (song.context) == song.firstTrack);
        song.context.getUndoManager()->clearUndoHistory();
    }

    bool isRecording() const { return song.commands.find ("transport.record")->isTicked(); }

    // A note played live: so long after the last, for so long, as the engine plays on.
    void playNote (int pitch, double after, double seconds)
    {
        auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
        playback.play (after);
        audio.playLive (juce::MidiMessage::noteOn (1, pitch, 0.8f));
        playback.play (seconds);
        audio.playLive (juce::MidiMessage::noteOff (1, pitch));
    }

    // The notes of the track's clips, in time order.
    std::vector<phazite::NoteData> getNotes() const
    {
        std::vector<phazite::ClipID> clips;

        for (const auto& clip : phazite::getClips (song.context, song.firstTrack))
            clips.push_back (clip.id);

        auto notes = phazite::getNotes (song.context, clips);
        std::sort (notes.begin(), notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
        return notes;
    }

    TestSong song;
    EnginePlayback playback;
};

constexpr double withinABlock = 0.03; // in beats at 120 BPM, a block of 256 frames at 48 kHz is 0.011
} // namespace

TEST_CASE ("What is played while recording becomes a clip on live MIDI's track, as one undo step")
{
    EngineTake take;
    take.song.run ("transport.record");
    REQUIRE (take.isRecording());

    take.playNote (60, 0.25, 0.25); // a quarter of a second in, for a quarter of a second
    take.playback.play (0.25);
    take.song.run ("transport.record");
    CHECK_FALSE (take.isRecording());
    CHECK (phazite::getPlayheadBeat (take.song.context).inBeats() == doctest::Approx (0.0)); // back where recording started

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].kind == phazite::ClipType::midi);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (1.5).epsilon (withinABlock));

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].pitch == 60);
    CHECK (notes[0].velocity == 102);
    CHECK (notes[0].start.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (notes[0].length.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (take.song.context.getUndoManager()->getUndoDescription() == juce::String ("Record"));

    phazite::undo (*phazite::findSong (take.song.context));
    CHECK (phazite::getClips (take.song.context, take.song.firstTrack).empty());
}

TEST_CASE ("The song's own loop doesn't loop a take, and a looped range folds what each time round played")
{
    SUBCASE ("The song's loop: the note stays where it was played, past the loop's end")
    {
        EngineTake take;
        take.song.setTempo (240.0); // a bar a second, the loop of a song without clips
        take.song.run ("transport.loopSong");
        take.song.run ("transport.record");
        take.playNote (64, 1.25, 0.25); // a quarter of a second into the second bar
        take.playback.play (0.1);
        take.song.run ("transport.record");

        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 1);
        CHECK (notes[0].start.inBeats() == doctest::Approx (5.0).epsilon (withinABlock));
        CHECK (phazite::isLoopOn (take.song.context));
    }

    SUBCASE ("A range looped on purpose, a second long: each time round's notes in one clip over it")
    {
        EngineTake take;
        phazite::loopBeatRange (*phazite::findSong (take.song.context), 0.0, 2.0);
        take.song.run ("transport.record");
        take.playNote (60, 0.25, 0.25); // at half a beat, the first time round
        take.playNote (62, 1.1, 0.2);   // at 1.6 s: 1.2 beats into the second time round
        take.playback.play (0.5);
        take.song.run ("transport.record");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (2.0));

        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 2);
        CHECK (notes[0].pitch == 60);
        CHECK (notes[0].start.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));
        CHECK (notes[1].pitch == 62);
        CHECK (notes[1].start.inBeats() == doctest::Approx (1.2).epsilon (withinABlock));
    }
}

TEST_CASE ("Recorded from before a looped range, the notes played before it go into a clip of their own, where they were played")
{
    // Known bug 10 until 2026-11-10: they went into the range's clip, all on its first beat without Overdub. Here beats 4 to
    // 8 (2 to 4 seconds) are looped, and recording starts at the song's start.
    const auto start = [] (EngineTake& take, bool overdub)
    {
        phazite::setOverdubOn (phazite::getAudioIO (phazite::test::getEngineContext()), overdub);
        phazite::loopBeatRange (*phazite::findSong (take.song.context), 4.0, 8.0);
        take.song.run ("transport.record");
        REQUIRE (take.isRecording());
    };

    const juce::ScopeGuard overdubOn { [] { phazite::setOverdubOn (phazite::getAudioIO (phazite::test::getEngineContext()), true); } };

    SUBCASE ("Stopped in the range before going round: one clip of what was played, its notes where they were played")
    {
        EngineTake take;
        start (take, true);
        take.playNote (60, 0.5, 0.25);  // at beat 1
        take.playNote (62, 1.75, 0.25); // at 2.5 seconds: beat 5
        take.playback.play (0.25);      // to beat 6
        take.song.run ("transport.record");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (6.0).epsilon (withinABlock));
        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 2);
        CHECK (notes[0].start.inBeats() == doctest::Approx (1.0).epsilon (withinABlock));
        CHECK (notes[1].start.inBeats() == doctest::Approx (5.0).epsilon (withinABlock));
    }

    SUBCASE ("Stopped before the range: one clip of what was played")
    {
        EngineTake take;
        start (take, false);
        take.playNote (60, 0.5, 0.25); // at beat 1
        take.playback.play (0.75);     // to beat 3
        take.song.run ("transport.record");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (3.0).epsilon (withinABlock));
        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 1);
        CHECK (notes[0].start.inBeats() == doctest::Approx (1.0).epsilon (withinABlock));
    }

    // Round once and part of the second time round.
    const auto roundOnce = [&start] (bool overdub)
    {
        EngineTake take;
        start (take, overdub);
        take.playNote (60, 0.5, 0.25);  // at beat 1
        take.playNote (62, 1.75, 0.25); // at 2.5 seconds: beat 5
        take.playNote (64, 2.0, 0.25);  // at 4.75 seconds: beat 9.5, beat 5.5 the second time round
        take.playback.play (0.25);      // to beat 10.5
        take.song.run ("transport.record");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 2);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (4.0));
        CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (clips[1].range.getEnd().inBeats() == doctest::Approx (8.0));

        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 3);
        CHECK (notes[0].pitch == 60);
        CHECK (notes[0].clip == clips[0].id);
        CHECK (notes[0].start.inBeats() == doctest::Approx (1.0).epsilon (withinABlock));
        CHECK (notes[1].pitch == 62);
        CHECK (notes[1].clip == clips[1].id);
        CHECK (notes[1].start.inBeats() == doctest::Approx (5.0).epsilon (withinABlock));
        CHECK (notes[2].pitch == 64);
        CHECK (notes[2].clip == clips[1].id);
        CHECK (notes[2].start.inBeats() == doctest::Approx (5.5).epsilon (withinABlock));
    };

    SUBCASE ("Round once, with Overdub: the notes before the range in a clip of their own, the rest folded into the range's clip")
    {
        roundOnce (true);
    }

    SUBCASE ("Round once, without Overdub: the same, in place of what was there")
    {
        roundOnce (false);
    }

    SUBCASE ("Recorded from within the range, as before: every time round's notes in the range's clip")
    {
        EngineTake take;
        phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (5.0));
        start (take, true);
        take.playNote (60, 0.5, 0.25);  // at beat 6
        take.playNote (62, 1.25, 0.25); // at 4.5 seconds: beat 9, beat 5 the second time round
        take.playback.play (0.25);
        take.song.run ("transport.record");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
        CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (8.0));
        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 2);
        CHECK (notes[0].pitch == 62);
        CHECK (notes[0].start.inBeats() == doctest::Approx (5.0).epsilon (withinABlock));
        CHECK (notes[1].pitch == 60);
        CHECK (notes[1].start.inBeats() == doctest::Approx (6.0).epsilon (withinABlock));
    }
}

TEST_CASE ("While recording from before a looped range, the notes before it show apart from the range's once recording has gone round")
{
    EngineTake take;
    phazite::loopBeatRange (*phazite::findSong (take.song.context), 4.0, 8.0);
    take.song.run ("transport.record");
    take.playNote (60, 0.5, 0.25); // at beat 1

    // The first time round: from where recording started, as it will be if it stops before going round.
    auto recordings = phazite::getRecordingsInProgress (take.song.context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (1.5).epsilon (withinABlock));
    REQUIRE (recordings[0].notes.size() == 1);
    CHECK (recordings[0].notes[0].start == doctest::Approx (1.0).epsilon (withinABlock));

    // Round once: the note before the range over the part before it, and the one played since folded into the range.
    take.playNote (62, 3.75, 0.25); // at 4.5 seconds: beat 9, beat 5 the second time round
    recordings = phazite::getRecordingsInProgress (take.song.context);
    REQUIRE (recordings.size() == 2);
    std::sort (recordings.begin(), recordings.end(), [] (const auto& a, const auto& b) { return a.range.getStart() < b.range.getStart(); });
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (4.0));
    CHECK (recordings[0].take == 1);
    REQUIRE (recordings[0].notes.size() == 1);
    CHECK (recordings[0].notes[0].start == doctest::Approx (1.0).epsilon (withinABlock));
    CHECK (recordings[1].range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (recordings[1].range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (recordings[1].take == 2);
    REQUIRE (recordings[1].notes.size() == 1);
    CHECK (recordings[1].notes[0].start == doctest::Approx (1.0).epsilon (withinABlock)); // beat 5, from the range's start
    take.song.run ("transport.record");
}

TEST_CASE ("Punched in, a take keeps only the notes played in the looped range")
{
    EngineTake take;
    phazite::loopBeatRange (*phazite::findSong (take.song.context), 1.0, 3.0);
    phazite::setPunchOn (take.song.context, true);
    take.song.run ("transport.record");
    take.playNote (60, 0.25, 0.1); // at half a beat: before the range
    take.playNote (62, 0.4, 0.1);  // at 1.5 beats
    take.playNote (64, 0.85, 0.1); // at 3.2 beats: after it
    take.playback.play (0.2);
    take.song.run ("transport.record");

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (1.0));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (3.0));

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].pitch == 62);
    CHECK (notes[0].start.inBeats() == doctest::Approx (1.5).epsilon (withinABlock));
    phazite::setPunchOn (take.song.context, false);
}

TEST_CASE ("While recording, the notes played show on live MIDI's track as they come in, over where their clip will go")
{
    // As Ableton Live draws a MIDI clip growing as it records (RecordingModel.h: getRecordingsInProgress()).
    EngineTake take;
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    take.song.run ("transport.record");
    take.playback.play (0.25);
    CHECK (phazite::getRecordingsInProgress (take.song.context).empty()); // nothing played yet

    // A note held: from where recording started, the note reaching where it has got to.
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    take.playback.play (0.25);
    auto recordings = phazite::getRecordingsInProgress (take.song.context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].track == take.song.firstTrack);
    CHECK (recordings[0].source == phazite::RecordSource::midi);
    CHECK (recordings[0].take == 1);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (1.0).epsilon (withinABlock));
    REQUIRE (recordings[0].notes.size() == 1);
    CHECK (recordings[0].notes[0].pitch == 60);
    CHECK (recordings[0].notes[0].start == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (recordings[0].notes[0].end == doctest::Approx (recordings[0].range.getLength().inBeats()));

    // Let go of: it ends there, and the recording grows on.
    audio.playLive (juce::MidiMessage::noteOff (1, 60));
    take.playback.play (0.25);
    recordings = phazite::getRecordingsInProgress (take.song.context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (1.5).epsilon (withinABlock));
    REQUIRE (recordings[0].notes.size() == 1);
    CHECK (recordings[0].notes[0].end == doctest::Approx (1.0).epsilon (withinABlock));

    // Stopped, the clip it made is there instead, as it was shown.
    take.song.run ("transport.record");
    CHECK (phazite::getRecordingsInProgress (take.song.context).empty());
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (1.5).epsilon (withinABlock));
}

TEST_CASE ("Round a looped range, the notes shown fold into it, and fill it once recording has gone round")
{
    EngineTake take;
    phazite::loopBeatRange (*phazite::findSong (take.song.context), 0.0, 2.0); // a second
    take.song.run ("transport.record");
    take.playNote (60, 0.25, 0.25); // at half a beat, the first time round

    auto recordings = phazite::getRecordingsInProgress (take.song.context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].take == 1);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (1.0).epsilon (withinABlock));

    take.playNote (62, 1.0, 0.1); // at 1.5 s: a beat into the second time round
    recordings = phazite::getRecordingsInProgress (take.song.context);
    REQUIRE (recordings.size() == 1);
    CHECK (recordings[0].take == 2);
    CHECK (recordings[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (recordings[0].range.getEnd().inBeats() == doctest::Approx (2.0));
    REQUIRE (recordings[0].notes.size() == 2);
    CHECK (recordings[0].notes[0].start == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (recordings[0].notes[1].start == doctest::Approx (1.0).epsilon (withinABlock));
    take.song.run ("transport.record");
}

TEST_CASE ("Overdub adds a take's notes to the clip there, and without it they take its place")
{
    const auto record = [] (bool overdub)
    {
        EngineTake take;
        const auto there = take.song.addMidiClip (take.song.firstTrack, 0.0, 4.0);
        take.song.addNote (there, 48, 2.0, 1.0);
        auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
        phazite::setOverdubOn (audio, overdub);
        take.song.run ("transport.record");
        take.playNote (60, 0.25, 0.25);
        take.playback.play (0.25);
        take.song.run ("transport.record");
        phazite::setOverdubOn (audio, true);

        std::vector<int> pitches;

        for (const auto& note : take.getNotes())
            pitches.push_back (note.pitch);

        return std::pair { phazite::getClips (take.song.context, take.song.firstTrack).size(), pitches };
    };

    CHECK (record (true) == std::pair { size_t (1), std::vector { 60, 48 } }); // in the clip, beside its note
    CHECK (record (false)
           == std::pair { size_t (2), std::vector { 60, 48 } }); // the clip cut where the take was: its end, with its note, stays
}

TEST_CASE ("Overdub puts each note in the clip it starts in, and one between clips in a clip of its own there: every note plays")
{
    // Clips at beats 0 to 2 and 4 to 6, notes played at beats half, 2.5 and 4.5 (120 BPM): all went into the first clip,
    // which can't grow past the second, so the later ones lay outside it and never played.
    EngineTake take;
    const auto first = take.song.addMidiClip (take.song.firstTrack, 0.0, 2.0);
    const auto second = take.song.addMidiClip (take.song.firstTrack, 4.0, 2.0);
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    phazite::setOverdubOn (audio, true);
    take.song.run ("transport.record");
    take.playNote (60, 0.25, 0.1);
    take.playNote (62, 0.9, 0.1);
    take.playNote (64, 0.9, 0.1);
    take.playback.play (0.25);
    take.song.run ("transport.record");

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 3);
    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 3);

    for (const auto& note : notes)
    {
        CAPTURE (note.pitch);
        const auto clip = std::find_if (clips.begin(), clips.end(), [&note] (const auto& each) { return each.id == note.clip; });
        REQUIRE (clip != clips.end());
        CHECK (clip->range.getStart() <= note.start);
        CHECK (note.start < clip->range.getEnd());
    }

    CHECK (notes[0].clip == first);
    CHECK (notes[2].clip == second);
    CHECK (notes[1].clip != first);
    CHECK (notes[1].clip != second);
}

TEST_CASE ("Quantized round a looped range, a note played just before its end goes round to its start, in the range's clip")
{
    // Round beats 0 to 2 (a second), a note 15 ms before the end the second time round: quantized to its sixteenth, it was
    // on the range's end, which grew the clip past it, where it never played.
    EngineTake take;
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    phazite::setRecordQuantizeOn (audio, true);
    const juce::ScopeGuard quantizeOff { [&audio] { phazite::setRecordQuantizeOn (audio, false); } }; // the app's setting
    phazite::loopBeatRange (*phazite::findSong (take.song.context), 0.0, 2.0);
    take.song.run ("transport.record");
    take.playNote (60, 1.985, 0.01);
    take.playback.play (0.25);
    take.song.run ("transport.record");

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (2.0));
    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].start.inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("A take starts after a bar of count-in")
{
    EngineTake take;
    take.song.setSwitch ("transport.countIn", true);
    take.song.setSwitch ("transport.metronome", false);
    const juce::ScopeGuard countInOff { [&take] { take.song.setSwitch ("transport.countIn", false); } }; // the app's setting

    take.song.run ("transport.record");
    const auto counted = take.playback.play (2.0); // a bar of 4/4 at 120 BPM
    CHECK (phazite::getPlayheadBeat (take.song.context).inBeats() == doctest::Approx (0.0).epsilon (withinABlock)); // waiting
    CHECK (counted.getMagnitude (0, 0, counted.getNumSamples()) > 0.1f);                                            // its clicks
    take.playNote (60, 0.25, 0.25);
    take.playback.play (0.25);
    take.song.run ("transport.record");

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].start.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));
}

TEST_CASE ("The playhead moved while recording: what was played before is kept, and recording goes on from the new place")
{
    // As Ableton Live records on after a jump. Until 2026-11-13 the jump ended the take in the engine while Record stayed lit,
    // and nothing played after it was kept.
    EngineTake take;
    take.song.run ("transport.record");
    take.playNote (60, 0.25, 0.25); // at half a beat
    take.playback.play (0.25);      // to beat 1.5

    phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (8.0));
    CHECK (take.isRecording());
    CHECK (phazite::isRecording (take.song.context));
    take.playNote (64, 0.25, 0.25); // half a beat after the new place
    take.playback.play (0.25);
    CHECK (take.isRecording());
    CHECK (phazite::getPlayheadBeat (take.song.context).inBeats() == doctest::Approx (9.5).epsilon (withinABlock));
    take.song.run ("transport.record");
    CHECK_FALSE (take.isRecording());
    CHECK_FALSE (phazite::isRecording (take.song.context));

    // A clip for each stretch recorded, each with its note where it was played.
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clips[0].range.getEnd().inBeats() == doctest::Approx (1.5).epsilon (withinABlock));
    CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (8.0));
    CHECK (clips[1].range.getEnd().inBeats() == doctest::Approx (9.5).epsilon (withinABlock));

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 2);
    CHECK (notes[0].pitch == 60);
    CHECK (notes[0].start.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (notes[1].pitch == 64);
    CHECK (notes[1].start.inBeats() == doctest::Approx (8.5).epsilon (withinABlock));

    // Each stretch its own Record step: undone, the last goes and the first stays.
    CHECK (take.song.context.getUndoManager()->getUndoDescription() == juce::String ("Record"));
    phazite::undo (*phazite::findSong (take.song.context));
    CHECK (phazite::getClips (take.song.context, take.song.firstTrack).size() == 1);
}

TEST_CASE ("A note played a moment before a take starts, as its count-in ends, starts the take")
{
    // Known bug 92: a take kept a note-on only at or after its first sample, so a downbeat played a few milliseconds early
    // after the count-in was lost. Ableton Live, Logic Pro and Cubase keep it, on the take's first beat.
    EngineTake take;
    take.song.setSwitch ("transport.countIn", true);
    take.song.setSwitch ("transport.metronome", false);
    const juce::ScopeGuard countInOff { [&take] { take.song.setSwitch ("transport.countIn", false); } }; // the app's setting

    take.song.run ("transport.record");
    take.playNote (60, 1.97, 0.25); // 30 ms before the bar of count-in ends
    take.playNote (62, 0.25, 0.25); // and one in the take
    take.playback.play (0.25);
    take.song.run ("transport.record");

    auto notes = take.getNotes();
    REQUIRE (notes.size() == 2);
    CHECK (notes[0].pitch == 60);
    CHECK (notes[0].start.inBeats() == doctest::Approx (0.0));
    CHECK (notes[0].length.inBeats() == doctest::Approx (0.44).epsilon (withinABlock)); // to where it was let go
    CHECK (notes[1].pitch == 62);
    CHECK (notes[1].start.inBeats() == doctest::Approx (0.94).epsilon (withinABlock));

    // Played half a second before, it was the count-in's, not the take's.
    take.song.run ("transport.record");
    take.playNote (64, 1.5, 0.1);
    take.playback.play (1.0);
    take.song.run ("transport.record");
    notes = take.getNotes();
    CHECK (std::none_of (notes.begin(), notes.end(), [] (const auto& note) { return note.pitch == 64; }));
}

namespace
{
// The controller elements of a MIDI clip, as a MIDI file's import makes them: their kind, number, value and beat in the song,
// and their MIDI channel.
struct RecordedControl
{
    juce::String kind;
    int number = 0, value = 0;
    double beat = 0.0;
    int channel = 1;
};

// A controller or a bend played into live MIDI's track as a MIDI input's driver plays it (AudioIO.h), into the engine: the
// computer keyboard plays notes alone.
void playFromMidiInput (const juce::MidiMessage& message)
{
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    const auto* session = host.getPlayingSession();
    const auto event = phazite::engine::toNoteEvent (message, 0);
    REQUIRE (session != nullptr);
    REQUIRE (event.has_value());
    host.getEngine().playLive ((std::int64_t) session->getLiveTrack().getRaw(), *event);
}

std::vector<RecordedControl> getControls (const phazite::Song& song, phazite::ClipID clip)
{
    const auto clipState = phazite::findClipState (song, clip);
    std::vector<RecordedControl> controls;

    for (const auto& child : clipState)
        if (child.hasType (phazite::schema::CONTROL))
            controls.push_back ({ child[phazite::schema::controlKind].toString(), (int) child[phazite::schema::controlNumber],
                                  (int) child[phazite::schema::controlValue],
                                  phazite::getContentStart (clipState).inBeats() + (double) child[phazite::schema::controlBeat],
                                  phazite::readChannel (child, phazite::schema::controlChannel) });

    return controls;
}
} // namespace

TEST_CASE ("The sustain pedal, the mod wheel and the pitch bend played while recording go into the take, and play back")
{
    // Known bug 93: a take kept notes only, so a part played with the pedal held lost its sustain once recorded. Here the
    // pedal goes down at a quarter of a second (half a beat), a note sounds from beat 1 to 1.5, the mod wheel and the bend
    // move at beat 2, and the pedal comes up at beat 3.
    EngineTake take;
    take.song.run ("transport.record");
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.25, 0.25);
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 1, 100));
    playFromMidiInput (juce::MidiMessage::pitchWheel (1, 12288));
    take.playback.play (0.5);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.5);
    take.song.run ("transport.record");

    auto& song = *phazite::findSong (take.song.context);
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    REQUIRE (controls.size() == 5);
    CHECK (controls[0].kind == juce::String (phazite::schema::controlIsController));
    CHECK (controls[0].number == 64);
    CHECK (controls[0].value == 127);
    CHECK (controls[0].beat == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (controls[1].number == 1);
    CHECK (controls[1].value == 100);
    CHECK (controls[1].beat == doctest::Approx (2.0).epsilon (withinABlock));
    CHECK (controls[2].kind == juce::String (phazite::schema::controlIsPitchWheel));
    CHECK (controls[2].value == 12288);
    CHECK (controls[3].number == 64);
    CHECK (controls[3].value == 0);
    CHECK (controls[3].beat == doctest::Approx (3.0).epsilon (withinABlock));

    // The bend, still away from its middle as the take stopped, back to it just before the clip's end.
    CHECK (controls[4].kind == juce::String (phazite::schema::controlIsPitchWheel));
    CHECK (controls[4].value == 8192);
    CHECK (controls[4].beat == doctest::Approx (4.0 - 1.0 / 960.0).epsilon (withinABlock));

    // Played back, the pedal holds the note on after its key came up (the synth lets go of it within 0.2 s), until it does.
    const auto played = phazite::renderWithOwnEngine (song, 96000, EnginePlayback::sampleRate, EnginePlayback::blockSize);
    const auto levelBetween = [&played] (double from, double to)
    { return played.getMagnitude (0, (int) (from * 48000.0), (int) ((to - from) * 48000.0)); };
    const auto held = levelBetween (1.1, 1.45);
    CHECK (held > 0.01f);
    CHECK (levelBetween (1.85, 2.0) < held * 0.01f);

    // Undone with the take, they go too.
    phazite::undo (song);
    CHECK (phazite::getClips (take.song.context, take.song.firstTrack).empty());
}

TEST_CASE ("A take started with the pedal held down starts with it down, and Capture keeps the pedal with the notes")
{
    // Known bug 93, as Capture had it too.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);

    SUBCASE ("A take")
    {
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127)); // well before recording starts
        take.playback.play (0.5);
        take.song.run ("transport.record");
        take.playNote (60, 0.25, 0.25);
        take.playback.play (0.25);
        take.song.run ("transport.record");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        const auto controls = getControls (song, clips[0].id);
        REQUIRE (controls.size() == 2);
        CHECK (controls[0].number == 64);
        CHECK (controls[0].value == 127);
        CHECK (controls[0].beat == doctest::Approx (0.0)); // where the take starts

        // Still held as it stopped: let go of just before the clip's end, where the take stopped.
        CHECK (controls[1].number == 64);
        CHECK (controls[1].value == 0);
        CHECK (controls[1].beat == doctest::Approx (1.5 - 1.0 / 960.0).epsilon (withinABlock));
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
        take.playback.play (0.05); // played, not left in the engine's queue for the next test
    }

    SUBCASE ("Capture, stopped: the pedal down from the clip's start, and up where it came up")
    {
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
        take.playback.play (0.1);

        for (const auto pitch : { 60, 64, 67, 72 })
            take.playNote (pitch, 0.25, 0.25); // quarter notes at 120 BPM

        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
        take.playback.play (0.1);
        take.song.run ("transport.capture");

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        const auto notes = take.getNotes();
        REQUIRE (notes.size() == 4);
        const auto controls = getControls (song, clips[0].id);
        REQUIRE (controls.size() == 2);
        CHECK (controls[0].value == 127);
        CHECK (controls[0].beat == doctest::Approx (clips[0].range.getStart().inBeats()));
        CHECK (notes.front().start.inBeats() == doctest::Approx (clips[0].range.getStart().inBeats()));
        CHECK (controls[1].value == 0);
        CHECK (controls[1].beat > notes.back().start.inBeats());
    }
}

TEST_CASE ("Only what is played in a take makes its clip: a pedal held from before alone leaves the track's clips as they were")
{
    // A reviewer, of known bug 93's fix: the pedal held from before the take (a foot resting on it while a vocal is recorded)
    // made a clip of its own on live MIDI's track, in place of the clips there without Overdub. Ableton Live records controllers
    // alone, so a mod wheel moved in the take makes its clip.
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    const juce::ScopeGuard overdubOn { [&audio] { phazite::setOverdubOn (audio, true); } };

    for (const auto overdub : { true, false })
    {
        CAPTURE (overdub);
        EngineTake take;
        auto& song = *phazite::findSong (take.song.context);
        phazite::setOverdubOn (audio, overdub);
        const auto there = take.song.addMidiClip (take.song.firstTrack, 0.0, 8.0);
        take.song.addNote (there, 48, 1.0, 1.0);
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
        take.playback.play (0.25);
        take.song.run ("transport.record");
        take.playback.play (1.0);
        take.song.run ("transport.record");
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
        take.playback.play (0.05);

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].id == there);
        CHECK (clips[0].range.getLength().inBeats() == doctest::Approx (8.0));
        CHECK (getControls (song, there).empty());
        CHECK (take.getNotes().size() == 1);
    }

    // The mod wheel alone, a value repeated once (which changes nothing): a clip of it.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    take.song.run ("transport.record");
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 1, 64));
    take.playback.play (0.1);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 1, 64));
    take.playback.play (0.1);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 1, 70));
    take.playback.play (0.25);
    take.song.run ("transport.record");

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    REQUIRE (controls.size() == 2);
    CHECK (controls[0].number == 1);
    CHECK (controls[0].value == 64);
    CHECK (controls[1].value == 70);
}

TEST_CASE ("A note played just before the playhead is moved while recording is kept by the take before the jump alone")
{
    // A reviewer, of known bug 92's fix: the take after the jump kept note-ons from a moment before its start, moved to it,
    // so a note played in the last tenth of a second before the jump was in both takes.
    EngineTake take;
    take.song.run ("transport.record");
    take.playback.play (0.5);
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    take.playback.play (0.05);
    phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (8.0));
    take.playback.play (0.25);
    audio.playLive (juce::MidiMessage::noteOff (1, 60));
    take.playback.play (0.25);
    take.song.run ("transport.record");

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].start.inBeats() == doctest::Approx (1.0).epsilon (withinABlock));
}

TEST_CASE ("A pedal still down as a take stops is let go of at its clip's end: the next clip's note isn't held")
{
    // A reviewer, of known bug 93's fix: the song plays its clips' controllers as one stream, and resets none at a clip's end,
    // so a pedal held down as the take stopped held the notes of every clip after it. The synth lets go of a note within 0.2 s
    // of its key coming up.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    take.song.run ("transport.record");
    take.playback.play (0.1);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.15, 0.25); // beats 0.5 to 1
    take.playback.play (0.5);       // to beat 2, still held
    take.song.run ("transport.record");
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.05);

    // A clip after it, with a short note at beat 4 (two seconds in).
    const auto after = take.song.addMidiClip (take.song.firstTrack, 4.0, 4.0);
    take.song.addNote (after, 64, 4.0, 0.25);
    const auto played = phazite::renderWithOwnEngine (song, 4 * 48000, EnginePlayback::sampleRate, EnginePlayback::blockSize);
    const auto levelBetween = [&played] (double from, double to)
    { return played.getMagnitude (0, (int) (from * 48000.0), (int) ((to - from) * 48000.0)); };
    CHECK (levelBetween (0.6, 0.95) > 0.01f);                          // held by the pedal in the take's clip
    CHECK (levelBetween (2.6, 3.0) < levelBetween (2.0, 2.1) * 0.01f); // the note after it let go of
}

TEST_CASE ("A sustain pedal recorded with a part shows in the piano roll's Sustain Pedal lane, plays, and once deleted plays no more")
{
    // A pedal recorded, seen in its lane, heard holding the note, and deleted there. The pedal goes down at half a beat, a
    // note sounds from beat 1 to 1.5, and the pedal comes up at beat 3; the synth lets go of a note within 0.2 s of its key
    // coming up.
    EngineTake take;
    take.song.run ("transport.record");
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.25, 0.25);
    take.playback.play (0.75);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.5);
    take.song.run ("transport.record");

    auto& song = *phazite::findSong (take.song.context);
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const phazite::ControlLane pedal { phazite::ControlKind::controller, 64, 1 };

    // The piano roll shows the clip, on its velocities: the chooser says the clip holds the pedal, and lists it first.
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::PianoRoll pianoRoll { commands, take.song.context };
    phazite::test::TestWindow window { pianoRoll, 1176, 564 };
    pianoRoll.showClip (clips[0].id);
    pianoRoll.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) });
    auto* chooser = dynamic_cast<juce::TextButton*> (phazite::test::findComponentWithTitle (pianoRoll, "Lane: Velocity"));
    REQUIRE (chooser != nullptr);
    CHECK (chooser->getTooltip().contains ("These clips also hold Sustain Pedal (CC 64)"));

    // Chosen, its lane shows the pedal held down from half a beat to beat 3: filled under its line there, and not after.
    REQUIRE (phazite::test::runMenuItem (pianoRoll.getLaneMenu(), { "Sustain Pedal (CC 64)" }));
    auto* lane = phazite::test::findComponentWithTitle (pianoRoll, "Controller lane");
    REQUIRE (lane != nullptr);
    REQUIRE (lane->isShowing());
    const auto values = phazite::getControlValues (take.song.context, { clips[0].id }, pedal);
    REQUIRE (values.size() == 2);
    CHECK (values[0].value == 127);
    CHECK (values[1].value == 0);

    const auto pixelAt = [&pianoRoll, lane] (double beat)
    {
        const auto image = lane->createComponentSnapshot (lane->getLocalBounds());
        // Between the grid's lines and the lane's own.
        return image.getPixelAt (juce::roundToInt (pianoRoll.getView().beatToX (phazite::BeatPosition::fromBeats (beat))),
                                 lane->getHeight() * 3 / 5);
    };

    const auto down = pixelAt (2.1), up = pixelAt (3.6);
    CHECK (down != up);

    // It plays: the pedal holds the note on after its key came up, until it does.
    const auto levelBetween = [&song] (double from, double to)
    {
        const auto played = phazite::renderWithOwnEngine (song, 96000, EnginePlayback::sampleRate, EnginePlayback::blockSize);
        return played.getMagnitude (0, (int) (from * 48000.0), (int) ((to - from) * 48000.0));
    };

    const auto held = levelBetween (1.1, 1.45);
    CHECK (held > 0.01f);

    // Delete All, from the chooser: the lane shows nothing held, and the note stops as its key comes up.
    REQUIRE (phazite::test::runMenuItem (pianoRoll.getLaneMenu(), { "Delete All Sustain Pedal Values" }));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getControlValues (take.song.context, { clips[0].id }, pedal).empty());
    CHECK (pixelAt (2.1) == up);
    CHECK (levelBetween (1.1, 1.45) < held * 0.01f);
    CHECK (take.song.context.getUndoManager()->getUndoDescription() == juce::String ("Delete Sustain Pedal"));
}

TEST_CASE ("With Overdub, a controller recorded into a clip takes the place of its earlier values there, from its first to its last")
{
    // A reviewer, of known bug 93's fix: an earlier take's pedal-ups stayed among the new pedal-downs. Ableton Live's overdub
    // of a clip's envelope takes the place of what was there; the clip's other controllers and its notes stay.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    const auto there = take.song.addMidiClip (take.song.firstTrack, 0.0, 8.0);
    take.song.addNote (there, 48, 0.0, 1.0);
    phazite::addControl (song, there, phazite::BeatPosition::fromBeats (0.5), phazite::ControlKind::controller, 64, 127);
    phazite::addControl (song, there, phazite::BeatPosition::fromBeats (1.5), phazite::ControlKind::controller, 64, 0);
    phazite::addControl (song, there, phazite::BeatPosition::fromBeats (1.5), phazite::ControlKind::controller, 1, 90);

    take.song.run ("transport.record");
    take.playback.play (0.5); // to beat 1
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.1, 0.1);
    take.playback.play (0.3); // to beat 2
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.1);
    take.song.run ("transport.record");

    std::vector<std::pair<double, int>> pedal;
    auto modWheel = 0;

    for (const auto& control : getControls (song, there))
    {
        if (control.number == 64)
            pedal.emplace_back (control.beat, control.value);
        else if (control.number == 1)
            ++modWheel;
    }

    std::sort (pedal.begin(), pedal.end());
    REQUIRE (pedal.size() == 3);
    CHECK (pedal[0].first == doctest::Approx (0.5));
    CHECK (pedal[0].second == 127);
    CHECK (pedal[1].first == doctest::Approx (1.0).epsilon (withinABlock));
    CHECK (pedal[1].second == 127);
    CHECK (pedal[2].first == doctest::Approx (2.0).epsilon (withinABlock));
    CHECK (pedal[2].second == 0);
    CHECK (modWheel == 1);
    CHECK (take.getNotes().size() == 2);
}

TEST_CASE ("Notes, a pedal and a program change played on MIDI channel 3 go into the take on channel 3")
{
    // Each kept on the channel it was played on, as a keyboard set to channel 3 plays them: the note's end paired with its
    // start by its channel and pitch (a note of the same pitch on channel 1 is another), the pedal on channel 3 and let go of
    // there, the program change kept as played, never let go of.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    take.song.run ("transport.record");
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (3, 64, 127));
    playFromMidiInput (juce::MidiMessage::programChange (3, 9));
    playFromMidiInput (juce::MidiMessage::noteOn (3, 60, 0.8f));
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::noteOn (1, 60, 0.5f));
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::noteOff (1, 60));
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::noteOff (3, 60));
    playFromMidiInput (juce::MidiMessage::controllerEvent (3, 64, 0));
    take.playback.play (0.25);
    take.song.run ("transport.record");

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 2);
    CHECK (notes[0].channel == 3);
    CHECK (notes[0].start.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));
    CHECK (notes[0].length.inBeats() == doctest::Approx (1.5).epsilon (withinABlock)); // to its own end, past channel 1's
    CHECK (notes[1].channel == 1);
    CHECK (notes[1].length.inBeats() == doctest::Approx (0.5).epsilon (withinABlock));

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    REQUIRE (controls.size() == 3);
    CHECK (controls[0].number == 64);
    CHECK (controls[0].value == 127);
    CHECK (controls[0].channel == 3);
    CHECK (controls[1].kind == juce::String (phazite::schema::controlIsProgramChange));
    CHECK (controls[1].value == 9);
    CHECK (controls[1].channel == 3);
    CHECK (controls[2].number == 64);
    CHECK (controls[2].value == 0);
    CHECK (controls[2].channel == 3);
}

TEST_CASE ("A pedal held on channel 3 and a bend on channel 1 as a take ends are each let go of on their own channel")
{
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    take.song.run ("transport.record");
    take.playback.play (0.1);
    playFromMidiInput (juce::MidiMessage::controllerEvent (3, 64, 127));
    playFromMidiInput (juce::MidiMessage::pitchWheel (1, 12288));
    take.playNote (60, 0.15, 0.25);
    take.playback.play (0.5);
    take.song.run ("transport.record");
    playFromMidiInput (juce::MidiMessage::controllerEvent (3, 64, 0));
    playFromMidiInput (juce::MidiMessage::pitchWheel (1, 8192));
    take.playback.play (0.05);

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    std::vector<std::tuple<juce::String, int, int>> released; // kind, value, channel, of those let go of at the take's end

    for (const auto& control : getControls (song, clips[0].id))
        if (control.beat > 1.5) // played at beat 0.2, let go of just before the take's end, at beat 1.8
            released.emplace_back (control.kind, control.value, control.channel);

    std::sort (released.begin(), released.end());
    CHECK (released
           == std::vector<std::tuple<juce::String, int, int>> { { phazite::schema::controlIsController, 0, 3 },
                                                                { phazite::schema::controlIsPitchWheel, 8192, 1 } });
}

TEST_CASE ("With Overdub, a controller recorded on one MIDI channel takes the place of its earlier values on that channel alone")
{
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    const auto there = take.song.addMidiClip (take.song.firstTrack, 0.0, 8.0);
    take.song.addNote (there, 48, 0.0, 1.0);
    phazite::addControl (song, there, phazite::BeatPosition::fromBeats (1.5), phazite::ControlKind::controller, 1, 90, 1);
    phazite::addControl (song, there, phazite::BeatPosition::fromBeats (1.5), phazite::ControlKind::controller, 1, 50, 2);

    // The mod wheel on channel 1 moved from beat 1 to beat 2.
    take.song.run ("transport.record");
    take.playback.play (0.5);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 1, 20));
    take.playNote (60, 0.1, 0.1);
    take.playback.play (0.3);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 1, 30));
    take.playback.play (0.1);
    take.song.run ("transport.record");

    std::vector<std::pair<int, int>> modWheel; // value, channel

    for (const auto& control : getControls (song, there))
        if (control.number == 1)
            modWheel.emplace_back (control.value, control.channel);

    std::sort (modWheel.begin(), modWheel.end());
    CHECK (modWheel == std::vector<std::pair<int, int>> { { 20, 1 }, { 30, 1 }, { 50, 2 } });
}

TEST_CASE ("Capture keeps the notes and the pedal on the MIDI channel they were played on")
{
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    playFromMidiInput (juce::MidiMessage::controllerEvent (2, 64, 127));
    take.playback.play (0.1);

    for (const auto pitch : { 60, 64, 67, 72 })
    {
        take.playback.play (0.25);
        playFromMidiInput (juce::MidiMessage::noteOn (2, pitch, 0.8f));
        take.playback.play (0.25);
        playFromMidiInput (juce::MidiMessage::noteOff (2, pitch));
    }

    take.playback.play (0.1);
    take.song.run ("transport.capture");

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 4);
    CHECK (std::all_of (notes.begin(), notes.end(), [] (const phazite::NoteData& note) { return note.channel == 2; }));
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    REQUIRE (controls.size() == 2); // down from the clip's start, let go of at its end: the pedal still held
    CHECK (controls[0].value == 127);
    CHECK (controls[0].channel == 2);
    CHECK (controls[1].value == 0);
    CHECK (controls[1].channel == 2);
    playFromMidiInput (juce::MidiMessage::controllerEvent (2, 64, 0));
    take.playback.play (0.05);
}

TEST_CASE ("A take longer than the notes played are kept for still starts with the pedal held as it started")
{
    // A reviewer, 2026-11-15: what was played before the take goes after 30 seconds (PlaybackHost::getPlayedEvents()), so a
    // take longer than that, recorded with the pedal down from before it, played back dry. The engine's clock is driven, a
    // block at a time.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playback.play (0.5);
    take.song.run ("transport.record");
    take.playback.play (31.0);
    take.playNote (60, 0.25, 0.25);
    take.playback.play (0.25);
    take.song.run ("transport.record");
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.05);

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    REQUIRE_FALSE (controls.empty());
    CHECK (controls.front().number == 64);
    CHECK (controls.front().value == 127);
    CHECK (controls.front().beat == doctest::Approx (0.0));
}

TEST_CASE (
    "With Overdub, a pedal held at the take's end leaves the clip there holding what it held, and what was held from before replaces nothing")
{
    // A reviewer, 2026-11-15: the pedal-up written at the take's end went into the clip that was there and goes on, so its
    // notes after the take lost their sustain; and the pedal held from before the take made the replaced span reach from
    // the take's start to its end, taking away the clip's own pedalling between.
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    phazite::setOverdubOn (audio, true);
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    const auto there = take.song.addMidiClip (take.song.firstTrack, 0.0, 16.0);
    take.song.addNote (there, 48, 0.0, 16.0);

    const auto pedalOf = [&song, there]
    {
        std::vector<std::pair<double, int>> pedal;

        for (const auto& control : getControls (song, there))
            if (control.number == 64)
                pedal.emplace_back (control.beat, control.value);

        std::sort (pedal.begin(), pedal.end());
        return pedal;
    };

    SUBCASE ("Pressed in the take and held as it stopped, in a clip that holds it down to beat 8: still down there")
    {
        phazite::addControl (song, there, phazite::BeatPosition::fromBeats (1.0), phazite::ControlKind::controller, 64, 127);
        phazite::addControl (song, there, phazite::BeatPosition::fromBeats (8.0), phazite::ControlKind::controller, 64, 0);
        phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (2.0));
        take.song.run ("transport.record");
        take.playback.play (0.25); // beat 2.5
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
        take.playNote (60, 0.1, 0.1);
        take.playback.play (0.55); // to beat 4, held
        take.song.run ("transport.record");

        const auto pedal = pedalOf();
        REQUIRE (pedal.size() == 4);
        CHECK (pedal[0] == std::pair (1.0, 127));
        CHECK (pedal[1].first == doctest::Approx (2.5).epsilon (withinABlock));
        CHECK (pedal[1].second == 127);
        CHECK (pedal[2].first == doctest::Approx (4.0).epsilon (withinABlock));
        CHECK (pedal[2].second == 127); // what the clip held there
        CHECK (pedal[3] == std::pair (8.0, 0));
    }

    SUBCASE ("Held from before the take, notes overdubbed: the clip's own pedalling between stays")
    {
        phazite::addControl (song, there, phazite::BeatPosition::fromBeats (1.0), phazite::ControlKind::controller, 64, 127);
        phazite::addControl (song, there, phazite::BeatPosition::fromBeats (3.0), phazite::ControlKind::controller, 64, 0);
        phazite::addControl (song, there, phazite::BeatPosition::fromBeats (3.5), phazite::ControlKind::controller, 64, 127);
        phazite::addControl (song, there, phazite::BeatPosition::fromBeats (8.0), phazite::ControlKind::controller, 64, 0);
        playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
        take.playback.play (0.1);
        phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (2.0));
        take.song.run ("transport.record");
        take.playNote (60, 0.25, 0.1);
        take.playback.play (1.0); // to about beat 4.7
        take.song.run ("transport.record");

        const auto pedal = pedalOf();
        for (const auto beat : { 1.0, 3.0, 3.5, 8.0 })
            CHECK (std::any_of (pedal.begin(), pedal.end(), [beat] (const auto& each) { return std::abs (each.first - beat) < 1.0e-9; }));
    }

    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.05);
}

TEST_CASE ("Round a looped range, a pedal held from before the range on into it is never let go of between the two clips")
{
    // A reviewer, 2026-11-15: the part before the range got a pedal-up a tick before the range's start, where the range's
    // clip puts it down again: what only the pedal held let go in that tick. Beats 4 to 8 are looped, recording from the
    // song's start.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    phazite::loopBeatRange (song, 4.0, 8.0);
    take.song.run ("transport.record");
    take.playback.play (1.0); // beat 2
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.5, 0.25);  // at beat 3
    take.playNote (62, 2.75, 0.25); // at 4.5 seconds: beat 9, beat 5 the second time round
    take.playback.play (0.25);
    take.song.run ("transport.record");
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.05);

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 2);
    const auto before = getControls (song, clips[0].id);
    CHECK (std::none_of (before.begin(), before.end(), [] (const auto& control) { return control.number == 64 && control.value == 0; }));
    const auto inRange = getControls (song, clips[1].id);
    CHECK (std::any_of (inRange.begin(), inRange.end(), [] (const auto& control)
                        { return control.number == 64 && control.value == 127 && std::abs (control.beat - 4.0) < 1.0e-6; }));
}

TEST_CASE ("A take after a jump that lands where the take before ended carries its pedal on: nothing lets go of it at the seam")
{
    // Known bug 175: the take before was placed before the next had recorded anything, so it wrote its pedal-up a tick before
    // its end, where the next one's clip puts the pedal down again: what only the pedal held let go in that tick. As between
    // the parts of one take, nothing lets go there now, and Undo of the second take puts it back.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    take.song.run ("transport.record");
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.1, 0.25);
    take.playback.play (0.25);

    // Where the take ends (what was heard last, as its clip will): the playhead put there.
    const auto* recording = engine.getRecording();
    const auto* session = phazite::getSession (take.song.context);
    REQUIRE (recording != nullptr);
    REQUIRE (recording->hasStarted());
    const auto& tempo = engine.getPlan()->tempo;
    const auto end = recording->getStartSample() + recording->getLength() - session->getOutputLatency()
                     - phazite::engine::Engine::getOutputLatency (*engine.getPlan());
    phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (tempo.toBeats ((double) end)));
    take.playNote (62, 0.1, 0.25);
    take.playback.play (0.25);
    take.song.run ("transport.record");
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.05);

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 2);
    REQUIRE (clips[0].range.getEnd().inBeats() == doctest::Approx (clips[1].range.getStart().inBeats()).epsilon (1.0e-9));
    const auto pedalUp = [] (const RecordedControl& control) { return control.number == 64 && control.value == 0; };
    const auto first = getControls (song, clips[0].id);
    CHECK (std::none_of (first.begin(), first.end(), pedalUp));
    const auto second = getControls (song, clips[1].id);
    CHECK (std::any_of (second.begin(), second.end(), [start = clips[1].range.getStart().inBeats()] (const RecordedControl& control)
                        { return control.number == 64 && control.value == 127 && std::abs (control.beat - start) < 1.0e-9; }));

    // The second take undone: the first lets go of its pedal at its end again, as nothing follows it.
    phazite::undo (song);
    const auto alone = getControls (song, clips[0].id);
    CHECK (std::any_of (alone.begin(), alone.end(), pedalUp));
}

TEST_CASE ("A jump while a control is held places the take before once the drag ends, a step that its Escape keeps, the pedal carried on")
{
    // Known bug 175's review: a jump during a drag placed the take before at once, inside the drag's step, so the drag's
    // Escape took it back; and the take after the jump mended the seam only from what the take before had let go of as it
    // was placed by then. Now that one waits for the drag's end as Stop's take does, and the next, placed after it, still
    // carries the pedal on across the seam.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    take.song.run ("transport.record");
    take.playback.play (0.25);
    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 127));
    take.playNote (60, 0.1, 0.25);
    take.playback.play (0.25);

    auto volume = phazite::getVolume (take.song.context, take.song.firstTrack);
    volume.beginGesture();
    volume.setValue (0.3f);

    const auto* recording = engine.getRecording();
    const auto* session = phazite::getSession (take.song.context);
    REQUIRE (recording != nullptr);
    const auto end = recording->getStartSample() + recording->getLength() - session->getOutputLatency()
                     - phazite::engine::Engine::getOutputLatency (*engine.getPlan());
    phazite::movePlayhead (take.song.context, phazite::BeatPosition::fromBeats (engine.getPlan()->tempo.toBeats ((double) end)));
    CHECK (phazite::getClips (take.song.context, take.song.firstTrack).empty()); // it waits for the drag's end
    take.playNote (62, 0.1, 0.25);
    take.playback.play (0.25);
    take.song.run ("transport.record");

    volume.cancelGesture(); // Escape: the drag goes, the takes stay
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 2);
    CHECK (take.song.context.getUndoManager()->getUndoDescription() == juce::String ("Record"));
    const auto first = getControls (song, clips[0].id);
    CHECK (std::none_of (first.begin(), first.end(),
                         [] (const RecordedControl& control) { return control.number == 64 && control.value == 0; }));

    playFromMidiInput (juce::MidiMessage::controllerEvent (1, 64, 0));
    take.playback.play (0.05);
}

TEST_CASE ("A take keeps at most one value a millisecond of each note's pressure, however densely a keyboard sends it")
{
    // Known bug 173's review: a keyboard that sends every held key's pressure all the time would fill a take with as many
    // values; each controller, a note's pressure by its note, keeps at most one a millisecond (Take::controlSeconds), the
    // latest at the time of the first. Four keys held for a second, each one's pressure sent four times a block, as it comes
    // in at the block's start (the test plays faster than real time): one of them kept a block.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    take.song.run ("transport.record");
    take.playback.play (0.1);

    for (const auto pitch : { 60, 64, 67, 72 })
        audio.playLive (juce::MidiMessage::noteOn (1, pitch, 0.8f));

    const auto blocks = (int) (1.0 * EnginePlayback::sampleRate / EnginePlayback::blockSize);
    auto sent = 0;

    for (int block = 0; block < blocks; ++block)
    {
        for (int i = 0; i < 4; ++i)
        {
            for (const auto pitch : { 60, 64, 67, 72 })
            {
                playFromMidiInput (juce::MidiMessage::aftertouchChange (1, pitch, (block * 4 + i) % 128));
                ++sent;
            }
        }

        take.playback.playSamples (EnginePlayback::blockSize);
    }

    for (const auto pitch : { 60, 64, 67, 72 })
        audio.playLive (juce::MidiMessage::noteOff (1, pitch));

    take.playback.play (0.1);
    take.song.run ("transport.record");

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    const auto kept = std::count_if (controls.begin(), controls.end(), [] (const RecordedControl& control)
                                     { return control.kind == juce::String (phazite::schema::controlIsAftertouch); });
    CAPTURE (sent);
    CHECK (kept >= 4 * blocks * 3 / 4); // what was played is kept, in time: about a value a block for each note (two that
                                        // came less than a millisecond apart across blocks of a loaded machine, one)
    CHECK (kept <= 4 * blocks + 8);
    CHECK (kept <= 4 * (1000 + 2)); // a second of it: a thousand values for each note at most
}

TEST_CASE (
    "The keys' pressure, a note's and the channel's, is heard live, recorded into the take, saved, and played back where it was played")
{
    // Known bug 173: the engine dropped aftertouch and channel pressure, so neither was heard live, played back, nor kept by
    // a take. A stand-in instrument that keeps what it is given; a note held while the channel's pressure sweeps up and back,
    // and the note's own pressure.
    using Type = phazite::devices::NoteEvent::Type;
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    const auto toneID = phazite::test::addTestDevice (song, take.song.firstTrack, phazite::test::TestTone::typeID);
    take.playback.play (0.05);
    const auto tone = [&take, toneID]() -> phazite::test::TestTone&
    {
        auto* session = phazite::findPlayingSession (*phazite::findSong (take.song.context));
        REQUIRE (session != nullptr);
        auto* node = session->getPlayer().findDevice ((std::int64_t) toneID.getRaw());
        REQUIRE (node != nullptr);
        return dynamic_cast<phazite::test::TestTone&> (node->getRunner().getProcessor());
    };

    const auto pressures = [] (const std::vector<phazite::devices::NoteEvent>& events)
    {
        std::vector<std::pair<Type, int>> found;

        for (const auto& event : events)
            if (event.type == Type::channelPressure || event.type == Type::aftertouch)
                found.emplace_back (event.type, juce::roundToInt (event.value * 127.0f));

        return found;
    };

    const std::vector<int> sweep { 20, 60, 100, 127, 90, 40 };
    std::vector<std::pair<Type, int>> played;

    take.song.run ("transport.record");
    take.playback.play (0.25);
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    take.playback.play (0.1);

    for (const auto value : sweep)
    {
        playFromMidiInput (juce::MidiMessage::channelPressureChange (1, value));
        take.playback.play (0.05);
        played.emplace_back (Type::channelPressure, value);
    }

    playFromMidiInput (juce::MidiMessage::aftertouchChange (1, 60, 70));
    played.emplace_back (Type::aftertouch, 70);
    take.playback.play (0.1);
    audio.playLive (juce::MidiMessage::noteOff (1, 60));
    take.playback.play (0.2);
    take.song.run ("transport.record");

    // Heard live, each as it was played; then the pressure let go of, as the next test's take would start with it held.
    CHECK (pressures (tone().received) == played);
    playFromMidiInput (juce::MidiMessage::channelPressureChange (1, 0));
    take.playback.play (0.05);

    // Recorded beside the note, each where it was heard, and the channel's pressure let go of at the clip's end.
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    std::vector<std::pair<juce::String, int>> kept;

    for (const auto& control : controls)
        kept.emplace_back (control.kind, control.value);

    REQUIRE (kept.size() == played.size() + 1);

    for (size_t i = 0; i < played.size(); ++i)
    {
        CAPTURE (i);
        CHECK (kept[i].first
               == juce::String (played[i].first == Type::aftertouch ? phazite::schema::controlIsAftertouch
                                                                    : phazite::schema::controlIsChannelPressure));
        CHECK (kept[i].second == played[i].second);
    }

    CHECK (controls.back().kind == juce::String (phazite::schema::controlIsChannelPressure));
    CHECK (controls.back().value == 0);
    CHECK (controls.back().beat == doctest::Approx (clips[0].range.getEnd().inBeats() - 1.0 / 960.0));
    CHECK (controls[played.size() - 1].number == 60);

    // Saved and opened again, then played from the song's start: the instrument is given each again where it was recorded,
    // to the sample.
    take.song.reopen();
    take.playback.play (0.05);
    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    tone().received.clear();
    take.song.run ("transport.playStop");
    const auto startClock = engine.getClock();
    std::vector<std::pair<std::int64_t, phazite::devices::NoteEvent>> heard;

    for (int block = 0; block < 300; ++block)
    {
        const auto at = engine.getClock() - startClock;
        tone().received.clear();
        take.playback.playSamples (EnginePlayback::blockSize);

        for (const auto& event : tone().received)
            if (event.type == Type::channelPressure || event.type == Type::aftertouch)
                heard.emplace_back (at + event.sample, event);
    }

    take.song.run ("transport.playStop");

    // First, as playback starts, the channel's pressure chased: none yet, where the clip holds it later (Engine.h).
    REQUIRE (heard.size() == controls.size() + 1);
    CHECK (heard.front().first == 0);
    CHECK (heard.front().second.type == Type::channelPressure);
    CHECK (heard.front().second.value == 0.0f);
    heard.erase (heard.begin());
    const auto& tempo = engine.getPlan()->tempo;

    for (size_t i = 0; i < heard.size(); ++i)
    {
        CAPTURE (i);
        CHECK (juce::roundToInt (heard[i].second.value * 127.0f) == controls[i].value);
        CHECK (heard[i].first == std::llround (tempo.toSamples (controls[i].beat)));
    }
}

TEST_CASE ("Capture keeps the keys' pressure played with the notes, and lets go of the channel's at its clip's end")
{
    // Known bug 173, as Capture had it: the pressure played while stopped, among four quarter notes at 120 BPM.
    EngineTake take;
    auto& song = *phazite::findSong (take.song.context);
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    take.playback.play (0.1);
    playFromMidiInput (juce::MidiMessage::channelPressureChange (1, 90));
    take.playback.play (0.05);
    playFromMidiInput (juce::MidiMessage::aftertouchChange (1, 60, 50));
    take.playback.play (0.1);
    audio.playLive (juce::MidiMessage::noteOff (1, 60));

    for (const auto pitch : { 64, 67, 72 })
        take.playNote (pitch, 0.25, 0.25);

    take.playback.play (0.1);
    take.song.run ("transport.capture");
    playFromMidiInput (juce::MidiMessage::channelPressureChange (1, 0));
    take.playback.play (0.05);

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto controls = getControls (song, clips[0].id);
    REQUIRE (controls.size() == 3);
    CHECK (controls[0].kind == juce::String (phazite::schema::controlIsChannelPressure));
    CHECK (controls[0].value == 90);
    CHECK (controls[1].kind == juce::String (phazite::schema::controlIsAftertouch));
    CHECK (controls[1].number == 60);
    CHECK (controls[1].value == 50);
    CHECK (controls[2].kind == juce::String (phazite::schema::controlIsChannelPressure));
    CHECK (controls[2].value == 0);
    CHECK (controls[2].beat == doctest::Approx (clips[0].range.getEnd().inBeats() - 1.0 / 960.0));
}

TEST_CASE ("A MIDI file's aftertouch and channel pressure, which its import keeps, play")
{
    using Type = phazite::devices::NoteEvent::Type;
    TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto toneID = phazite::test::addTestDevice (song, s.firstTrack, phazite::test::TestTone::typeID);

    juce::MidiMessageSequence events;
    events.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0.0);
    events.addEvent (juce::MidiMessage::channelPressureChange (1, 80), 480.0);
    events.addEvent (juce::MidiMessage::aftertouchChange (1, 60, 30), 960.0);
    events.addEvent (juce::MidiMessage::noteOff (1, 60), 1920.0);
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (960);
    midi.addTrack (events);
    const auto file = s.getSoundsFolder().getChildFile ("Pressed.mid");
    file.getParentDirectory().createDirectory();
    {
        juce::FileOutputStream stream (file);
        REQUIRE (midi.writeTo (stream));
    }

    {
        const phazite::UndoStep step (song, "Import");
        REQUIRE (phazite::importFile (song, s.firstTrack, file, phazite::BeatPosition()).isValid());
    }

    EnginePlayback playback;
    playback.play (0.05);
    auto* session = phazite::findPlayingSession (song);
    REQUIRE (session != nullptr);
    auto& tone = dynamic_cast<phazite::test::TestTone&> (
        session->getPlayer().findDevice ((std::int64_t) toneID.getRaw())->getRunner().getProcessor());
    tone.received.clear();
    s.run ("transport.playStop");
    playback.play (1.2);
    s.run ("transport.playStop");

    std::vector<std::pair<Type, int>> heard;

    for (const auto& event : tone.received)
        if (event.type == Type::channelPressure || event.type == Type::aftertouch)
            heard.emplace_back (event.type, event.type == Type::aftertouch ? event.number : juce::roundToInt (event.value * 127.0f));

    // (The channel's pressure chased first as playback starts: none yet.)
    CHECK (heard
           == std::vector<std::pair<Type, int>> { { Type::channelPressure, 0 }, { Type::channelPressure, 80 }, { Type::aftertouch, 60 } });
}

TEST_CASE ("Controllers moved without notes clear nothing: they go into the clips there, or a clip of their own where there are none")
{
    // A reviewer, 2026-11-15 (the owner's decision): a bend wheel bumped while a vocal recorded replaced the instrument
    // track's clips there with a clip of the one value, without Overdub. Notes still take the clips' place without it.
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    phazite::setOverdubOn (audio, false);
    const juce::ScopeGuard overdubOn { [&audio] { phazite::setOverdubOn (audio, true); } };

    const auto bump = [] (EngineTake& take)
    {
        take.song.run ("transport.record");
        take.playback.play (0.5); // beat 1
        playFromMidiInput (juce::MidiMessage::pitchWheel (1, 12288));
        take.playback.play (0.1);
        playFromMidiInput (juce::MidiMessage::pitchWheel (1, 8192));
        take.playback.play (0.4);
        take.song.run ("transport.record");
    };

    SUBCASE ("A clip there: kept, the bend in it")
    {
        EngineTake take;
        auto& song = *phazite::findSong (take.song.context);
        const auto there = take.song.addMidiClip (take.song.firstTrack, 0.0, 8.0);
        take.song.addNote (there, 48, 0.0, 1.0);
        bump (take);

        const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].id == there);
        CHECK (clips[0].range.getLength().inBeats() == doctest::Approx (8.0));
        CHECK (take.getNotes().size() == 1);
        const auto controls = getControls (song, there);
        REQUIRE (controls.size() == 2);
        CHECK (controls[0].kind == juce::String (phazite::schema::controlIsPitchWheel));
        CHECK (controls[0].value == 12288);
    }

    SUBCASE ("No clip there: one of its own")
    {
        EngineTake take;
        bump (take);
        CHECK (phazite::getClips (take.song.context, take.song.firstTrack).size() == 1);
    }
}
TEST_CASE ("Capture while stopped sets the tempo from the notes and puts them at bar 1")
{
    EngineTake take;

    // Quarter notes at 100 BPM, each half a beat long.
    for (const auto pitch : { 60, 62, 64, 65, 67, 69 })
        take.playNote (pitch, 0.3, 0.3);

    take.song.run ("transport.capture");
    CHECK (phazite::getTempoHandle (take.song.context).getValue() == doctest::Approx (100.0));

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (clips[0].range.getLength().inBeats() == doctest::Approx (8.0)); // two whole bars

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 6);

    for (size_t i = 0; i < notes.size(); ++i)
        CHECK (notes[i].start.inBeats() == doctest::Approx ((double) i).epsilon (withinABlock));

    CHECK (take.song.context.getUndoManager()->getUndoDescription() == juce::String ("Capture"));
}

TEST_CASE ("Capture while stopped, in a song with clips, keeps its tempo and goes after the track's clips")
{
    EngineTake take;
    take.song.addMidiClip (take.song.firstTrack, 0.0, 4.0);

    for (const auto pitch : { 60, 64, 67, 72 })
        take.playNote (pitch, 0.25, 0.25); // quarter notes at 120 BPM

    take.song.run ("transport.capture");
    CHECK (phazite::getTempoHandle (take.song.context).getValue() == doctest::Approx (120.0));

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 2);
    CHECK (clips[1].range.getStart().inBeats() == doctest::Approx (4.0)); // bar 2, after the clip in bar 1

    const auto notes = phazite::getNotes (take.song.context, { clips[1].id });
    REQUIRE (notes.size() == 4);
    CHECK (notes.front().start.inBeats() == doctest::Approx (4.0));
    CHECK (notes.back().start.inBeats() == doctest::Approx (7.0).epsilon (withinABlock));
}

TEST_CASE ("Capture takes the notes played, not the guide notes a click in the piano roll or a preset tried in the browser plays")
{
    // Known bug 147: the guide notes were played as recordable, so Capture (and a take) took them as played; and a test that
    // played them without playing blocks left them in the engine's queue for the next test's Capture.
    EngineTake take;
    const auto clip = take.song.addMidiClip (take.song.firstTrack, 0.0, 4.0);

    for (const auto pitch : { 60, 64, 67, 72 })
    {
        // As a click on the piano roll's keys plays its guide note (a preset tried in the browser plays one the same way).
        REQUIRE (phazite::auditionNotes (take.song.context, clip, { pitch + 13 }, 100) == take.song.firstTrack);
        take.playNote (pitch, 0.25, 0.25);
        phazite::stopAuditioning (take.song.context, take.song.firstTrack);
    }

    take.playback.play (0.25);
    take.song.run ("transport.capture");
    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 2);
    auto pitches = std::vector<int>();

    for (const auto& note : phazite::getNotes (take.song.context, { clips[1].id }))
        pitches.push_back (note.pitch);

    std::sort (pitches.begin(), pitches.end());
    CHECK (pitches == std::vector<int> { 60, 64, 67, 72 }); // the notes played alone
}

TEST_CASE ("Capture while the song plays keeps the notes where they were played")
{
    EngineTake take;
    take.playNote (67, 0.1, 0.1); // before playback: not captured
    take.song.run ("transport.playStop");
    take.playNote (60, 1.0, 0.25); // two beats in
    take.playback.play (0.25);
    take.song.run ("transport.capture");

    const auto clips = phazite::getClips (take.song.context, take.song.firstTrack);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (0.0)); // the bar the note is in

    const auto notes = take.getNotes();
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].pitch == 60);
    CHECK (notes[0].start.inBeats() == doctest::Approx (2.0).epsilon (withinABlock));
}
