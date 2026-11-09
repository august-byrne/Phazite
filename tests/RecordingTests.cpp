// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording MIDI and Capture (src/model/transport/internal/Recording.h, Take.h, Capture.h): the settings and their
// commands, takes recorded through Phazite's engine as the app's audio device would play it, and what Capture makes of
// notes played a moment ago.
#include "EnginePlayback.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Capture.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

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
