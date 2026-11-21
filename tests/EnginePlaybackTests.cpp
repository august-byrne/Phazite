// SPDX-License-Identifier: AGPL-3.0-or-later
// The song as Phazite's own engine plays it (model/playback/): the plan the model compiles from the song, and the transport
// the model's commands drive through it while it plays the project.
#include "EnginePlayback.h"
#include "PlaybackConditions.h"
#include "RenderHarness.h"
#include "TestDevices.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/delay/Delay.h"
#include "devices/dsp/ClearedSamples.h"
#include "devices/dsp/NoteValues.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Motion.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "engine/transport/Click.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/LevelSource.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TempoValues.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using phazite::test::EnginePlayback;
using phazite::test::TestSong;

namespace
{
// The first sample something sounds at, or -1.
int findFirstSound (const juce::AudioBuffer<float>& audio, int from = 0)
{
    for (int i = from; i < audio.getNumSamples(); ++i)
        if (! juce::exactlyEqual (audio.getSample (0, i), 0.0f))
            return i;

    return -1;
}

// Where a pan (-1 left to 1 right) is along its knob's travel, as automation keeps it: 0 left, 0.5 centre, 1 right.
float panPosition (float pan)
{
    return (pan + 1.0f) / 2.0f;
}
} // namespace

TEST_CASE ("Phazite's engine plays and stops as the transport's commands ask, back to where playback starts")
{
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 16.0);
    EnginePlayback playback;

    song.run ("transport.playStop");
    CHECK (phazite::isPlaying (song.context));
    playback.play (1.0); // two beats at 120 BPM
    CHECK (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (2.0).epsilon (0.02));

    // Stopping goes back to where playback starts.
    song.run ("transport.playStop");
    CHECK_FALSE (phazite::isPlaying (song.context));
    CHECK (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (0.0));

    // A click in the ruler puts the playhead there, and Play / Pause goes on from where it stops.
    phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (4.0));
    song.run ("transport.playPause");
    playback.play (0.5);
    song.run ("transport.playPause");
    playback.play (0.1);
    CHECK (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (5.0).epsilon (0.02));
}

TEST_CASE ("Phazite's engine loops the range the song loops, and stops at the song's end without a loop")
{
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 4.0); // a bar: the song ends at beat 4
    EnginePlayback playback;

    phazite::loopRange (song.context, { phazite::BeatPosition::fromBeats (1.0), phazite::BeatPosition::fromBeats (2.0) });
    song.run ("transport.playStop");
    playback.play (3.0);
    const auto beat = phazite::getPlayheadBeat (song.context).inBeats();
    CHECK (beat >= 1.0);
    CHECK (beat < 2.1);

    // Without the loop, playback stops at the song's end, and goes back: the session's timer sees the playhead cross it.
    const auto followTheSong = [] { phazite::test::dispatchMessagesUntil ([] { return false; }, 100); }; // at 30 Hz

    song.run ("transport.playStop");
    song.setSwitch ("transport.loop", false);
    song.run ("transport.playStop");
    playback.play (1.9);
    followTheSong();
    CHECK (phazite::isPlaying (song.context));
    playback.play (0.2);
    CHECK (phazite::test::dispatchMessagesUntil ([&song] { return ! phazite::isPlaying (song.context); }));
    CHECK (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("The playhead heard is where the engine was as long before as its sound takes to reach the speakers")
{
    // The window showed where the engine played, ahead of what was heard by the delay compensation and the audio output's
    // latency (docs/archive/performance-audit.md, section 16, option 7). An effect 64 samples late delays the song's sound by as much.
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 16.0);
    auto& state = *phazite::findSong (song.context);
    phazite::test::addTestDevice (state, song.firstTrack, phazite::test::TestDelay::typeID);
    EnginePlayback playback;

    song.run ("transport.playStop");
    playback.play (0.5);
    const auto* session = phazite::findPlayingSession (state);
    REQUIRE (session != nullptr);
    CHECK (session->getHeardDelay() == doctest::Approx (phazite::test::TestDelay::latency * 1000.0 / EnginePlayback::sampleRate));

    // At any moment, the playhead heard is the engine's that much earlier.
    const auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    const auto now = phazite::devices::Clock::getMilliseconds(); // the engine's clock
    CHECK (phazite::getHeardPlayheadBeat (song.context, now).inBeats()
           == doctest::Approx (engine.getPlayheadBeatAt (now - session->getHeardDelay())));
    song.run ("transport.playStop");
}

TEST_CASE ("Phazite's engine clicks the metronome on each beat's sample")
{
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 16.0);
    song.setSwitch ("transport.metronome", true);
    EnginePlayback playback;

    song.run ("transport.playStop");
    const auto played = playback.play (1.0);

    // Clicks at 0 and half a second, each heard from its second sample (its first is silent).
    CHECK (findFirstSound (played) == 1);
    CHECK (findFirstSound (played, 12000) == 24000 + 1);
}

TEST_CASE ("Phazite's engine plays a track's notes through Phazite's synth, which the track's meter and the master's show")
{
    TestSong song;
    song.setTempo (120.0);
    song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 4.0);
    song.addChord (clip, { 60, 64, 67 }, 0.0, 2.0);
    EnginePlayback playback;
    auto level = phazite::getLevel (song.context, song.firstTrack);
    auto master = phazite::getMasterLevel (song.context);

    song.run ("transport.playStop");
    const auto played = playback.play (0.5);
    CHECK (played.getMagnitude (0, 0, played.getNumSamples()) > 0.01f);

    // Its meters read Phazite's engine.
    level.refresh();
    master.refresh();
    REQUIRE (level.isMeasuring());
    CHECK (level.getAndClearLevel (0) > -40.0f);
    CHECK (master.getAndClearLevel (1) > -40.0f);

    // A value set by hand reaches what plays at once, with no new plan: the track turned all the way down is silent once its
    // fader has eased there.
    song.setVolume (song.firstTrack, -100.0f);
    playback.play (0.05);
    const auto quiet = playback.play (0.2);
    CHECK (quiet.getMagnitude (0, 0, quiet.getNumSamples()) == 0.0f);
}

TEST_CASE ("A new plan compiles again the notes of the tracks whose clips changed only, and plays every change of them")
{
    TestSong song;
    auto& state = *phazite::findSong (song.context);
    const auto other = song.addTrack ("Other");
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 4.0);
    song.addNote (clip, 60, 0.0, 1.0);
    song.addNote (song.addMidiClip (other, 0.0, 4.0), 48, 0.0, 1.0);

    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    const auto compile = [session] { return session->getPlayer().compile (48000.0, 256, std::nullopt); };
    const auto notesOf = [] (const phazite::engine::Plan& plan, phazite::TrackID track)
    {
        for (const auto& strip : plan.strips)
            if (strip.id == (std::int64_t) track.getRaw())
                return strip.notes;

        FAIL ("no strip for the track");
        return std::shared_ptr<const phazite::engine::NoteSequence>();
    };

    const auto pitchesOf = [&notesOf] (const phazite::engine::Plan& plan, phazite::TrackID track)
    {
        std::vector<int> pitches;

        if (const auto notes = notesOf (plan, track))
            for (const auto& note : notes->getNotes())
                pitches.push_back (note.pitch);

        return pitches;
    };

    const auto first = compile();
    CHECK (pitchesOf (*first, song.firstTrack) == std::vector { 60 });
    CHECK (pitchesOf (*first, other) == std::vector { 48 });

    // Nothing changed: both tracks' notes as they were; a note added to the first track: its notes again, the other's
    // the same.
    const auto again = compile();
    CHECK (notesOf (*again, song.firstTrack) == notesOf (*first, song.firstTrack));
    song.addNote (clip, 64, 1.0, 1.0);
    const auto added = compile();
    CHECK (pitchesOf (*added, song.firstTrack) == std::vector { 60, 64 });
    CHECK (notesOf (*added, other) == notesOf (*first, other));

    // The clip moved onto the other track: both again; undone, back where they were.
    {
        const phazite::UndoStep step (state, "Move Clip");
        phazite::moveClip (state, clip, phazite::BeatPosition::fromBeats (8.0), other);
    }

    const auto moved = compile();
    CHECK (pitchesOf (*moved, song.firstTrack).empty());
    CHECK (pitchesOf (*moved, other) == std::vector { 48, 60, 64 });
    phazite::undo (state);
    const auto undone = compile();
    CHECK (pitchesOf (*undone, song.firstTrack) == std::vector { 60, 64 });
    CHECK (pitchesOf (*undone, other) == std::vector { 48 });

    // The first track deleted and its deletion undone: its notes again.
    {
        const phazite::UndoStep step (state, "Delete Track");
        phazite::deleteTracks (state, { song.firstTrack }, nullptr);
    }

    compile();
    phazite::undo (state);
    CHECK (pitchesOf (*compile(), song.firstTrack) == std::vector { 60, 64 });
}

TEST_CASE ("A note sounding as its track's last clip goes ends at once, and one held as the loop moves before the playhead ends")
{
    // Test Tone plays the velocity of the note it holds; at 120 BPM a beat is half a second.
    TestSong song;
    song.setTempo (120.0);
    auto& state = *phazite::findSong (song.context);
    phazite::test::addTestDevice (state, song.firstTrack, phazite::test::TestTone::typeID);
    const auto quietAt = [] (const juce::AudioBuffer<float>& audio, double from, double to)
    {
        const auto first = (int) (from * EnginePlayback::sampleRate);
        return juce::exactlyEqual (audio.getMagnitude (0, first, (int) (to * EnginePlayback::sampleRate) - first), 0.0f);
    };

    {
        // A note held from the song's start; its clip deleted as it sounds (the track's only one): it ends, and doesn't
        // sound on until Stop.
        const auto clip = song.addMidiClip (song.firstTrack, 0.0, 16.0);
        song.addNote (clip, 60, 0.0, 16.0);
        EnginePlayback playback;
        song.run ("transport.playStop");
        REQUIRE_FALSE (quietAt (playback.play (0.5), 0.4, 0.5));

        {
            const phazite::UndoStep step (state, "Delete Clip");
            phazite::deleteClips (state, { clip }, false);
        }

        CHECK (quietAt (playback.play (0.5), 0.1, 0.5));
        song.run ("transport.playStop");
    }

    {
        // A note held from beat 8 and one at beat 1; the playhead at beat 10 when the loop becomes beats 0 to 4: playback
        // goes round to its start, the held note ending, and the loop's note plays the first time round.
        const auto clip = song.addMidiClip (song.firstTrack, 0.0, 16.0);
        song.addNote (clip, 62, 8.0, 8.0);
        song.addNote (clip, 64, 1.0, 1.0);
        EnginePlayback playback;
        song.run ("transport.playStop");
        REQUIRE_FALSE (quietAt (playback.play (5.0), 4.5, 5.0));
        phazite::loopBeatRange (state, 0.0, 4.0);
        const auto looped = playback.play (1.25);
        CHECK (quietAt (looped, 0.1, 0.45));
        CHECK_FALSE (quietAt (looped, 0.6, 0.9));
        song.run ("transport.playStop");
    }
}

TEST_CASE ("The piano roll's guide notes play through Phazite's engine while the song is stopped")
{
    TestSong song;
    song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 4.0);
    EnginePlayback playback;
    playback.play (0.05);

    const auto track = phazite::auditionNotes (song.context, clip, { 60 }, 100);
    REQUIRE (track == song.firstTrack);
    const auto sounding = playback.play (0.2);
    CHECK (sounding.getMagnitude (0, 0, sounding.getNumSamples()) > 0.01f);

    phazite::stopAuditioning (song.context, track);
    playback.play (2.0); // its release
    const auto after = playback.play (0.2);
    CHECK (after.getMagnitude (0, 0, after.getNumSamples()) < 0.001f);
}

TEST_CASE ("A lane shows its values as Phazite's engine plays them: straight, bent, held and jumping")
{
    TestSong song;
    song.setTempo (97.0);
    const auto ref = song.getVolumeParameter (song.firstTrack);
    juce::Random random (23);
    const float bends[] = { 0.0f, 0.2f, -0.5f, 0.5f, 0.75f, -0.9f, 1.0f, -1.0f };

    for (int round = 0; round < 25; ++round)
    {
        // Points along 16 beats, now and then two at one beat (a jump), with every kind of bend and random ones.
        std::vector<phazite::AutomationPoint> points;
        auto beat = 0.0;

        for (int i = 0; i < 8; ++i)
        {
            const auto bend = random.nextInt (3) == 0 ? random.nextFloat() * 2.0f - 1.0f : bends[random.nextInt (8)];
            points.push_back ({ phazite::BeatPosition::fromBeats (beat), random.nextFloat(), bend });
            beat += random.nextInt (5) == 0 ? 0.0 : 0.25 + random.nextDouble() * 2.0;
        }

        phazite::replaceAutomationPoints (song.context, ref, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (100.0) }, points);
        std::vector<phazite::engine::CurvePoint> enginePoints;

        for (const auto& point : phazite::getAutomationPoints (song.context, ref))
            enginePoints.push_back ({ point.beat.inBeats(), point.position, point.curve });

        const phazite::engine::Curve curve (std::move (enginePoints));
        const phazite::BeatRange shown { phazite::BeatPosition(), phazite::BeatDuration::fromBeats (beat + 2.0) };
        const auto values = phazite::getAutomationPositions (song.context, ref, shown, 400);
        REQUIRE (values.size() == 400);

        for (size_t i = 0; i < values.size(); ++i)
            CHECK (values[i] == doctest::Approx (curve.getPositionAt ((beat + 2.0) * (double) i / 399.0)).epsilon (1.0e-6));
    }
}

namespace
{
// A song at a tempo whose first track plays Test Frequency, its frequency automated: held at 20 Hz for a beat, then a line
// from the bottom of its travel to the top over eight beats (20 Hz to 20 kHz), and from beat 9 a line from 100 Hz to 10 kHz
// over eight more. Its frequency, ID and all.
phazite::ParameterRef addFrequencyRamps (TestSong& song, double tempo)
{
    song.setTempo (tempo);
    const auto device =
        phazite::test::addTestDevice (*phazite::findSong (song.context), song.firstTrack, phazite::test::TestFrequency::typeID);
    const phazite::ParameterRef frequency { device, "frequency" };
    const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };
    song.addAutomation (
        frequency, { { at (0.0), 20.0f }, { at (1.0), 20.0f }, { at (9.0), 20000.0f }, { at (9.0), 100.0f }, { at (17.0), 10000.0f } });
    return frequency;
}

// What Test Frequency played at a beat, in hertz, as what it plays there to what it played while held at 20 Hz.
float getFrequencyAt (const juce::AudioBuffer<float>& audio, double sampleRate, double tempo, double beat)
{
    const auto sampleAt = [&] (double b) { return audio.getSample (0, (int) std::lround (b * 60.0 / tempo * sampleRate)); };
    return 20.0f * sampleAt (beat) / sampleAt (0.5);
}

// That a render or a play of the ramps passes 20 * 1000^p Hz at p along the first line, and 1 kHz halfway along the second,
// within a fiftieth of an octave.
void checkFrequencyRamps (const juce::AudioBuffer<float>& audio, double sampleRate, double tempo)
{
    for (const auto& [beat, hertz] :
         { std::pair { 3.0, 112.468f }, std::pair { 5.0, 632.456f }, std::pair { 7.0, 3556.56f }, std::pair { 13.0, 1000.0f } })
    {
        CAPTURE (beat);
        const auto played = getFrequencyAt (audio, sampleRate, tempo, beat);
        CHECK_MESSAGE (std::abs (std::log2 (played / hertz)) < 0.02f, "played " << played << " Hz, " << hertz << " Hz expected");
    }
}
} // namespace

TEST_CASE ("A straight line on a frequency is an even sweep through the octaves, live at every condition and rendered")
{
    // Roadmap S4.7: a frequency's knob is logarithmic, and a line is worked out along its travel and turned into hertz where it
    // plays, so that it goes through as many octaves in each part of its length: from 20 Hz to 20 kHz it passes 112 Hz, 632 Hz
    // and 3.56 kHz at its quarters, and from 100 Hz to 10 kHz, 1 kHz halfway. A line straight in hertz played 10 kHz halfway,
    // and one along a knob skewed around 632 Hz spent a fifth of its travel below 26 Hz.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            TestSong song;
            addFrequencyRamps (song, condition.getTempo());
            EnginePlayback playback (condition);
            song.run ("transport.playStop");
            const auto audio = playback.play (18.0 * 60.0 / condition.getTempo());
            song.run ("transport.playStop");
            checkFrequencyRamps (audio, condition.getSampleRate(), condition.getTempo());
        });

    TestSong song;
    addFrequencyRamps (song, 120.0);
    checkFrequencyRamps (phazite::test::renderOffline (song.context, 9.0), phazite::test::renderSampleRate, 120.0);
}

TEST_CASE ("Write takes a stepped parameter's lane value on its steps as playback starts: no value off its steps, no step for nothing")
{
    // The reviewers of S4.7: a punch-in took the lane's value as the engine turns a position into a value, off a stepped
    // parameter's steps where the lane lies between two (the sampler's slices: 4.4), set it by hand there, and began a
    // "Record Automation" step at Play for a value that hadn't changed; and its pass, begun at the lane's own position, wrote
    // a point more as an Undo during it set the value by hand again, on its step.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    const auto sampler = song.setInstrument (track, phazite::devices::Sampler::typeID);
    const phazite::ParameterRef slices { sampler, "sliceCount" };
    auto& state = *phazite::findSong (song.context);
    const auto travel = phazite::getControlTravel (state, slices);
    REQUIRE (travel.has_value());

    for (const auto count : { 4.0f, 11.0f, 23.0f })
    {
        CAPTURE (count);
        phazite::deleteAutomation (phazite::getParameter (song.context, slices));
        song.setParameter (sampler, "sliceCount", count);
        song.addAutomation (slices,
                            { { phazite::BeatPosition(), count + 0.4f }, { phazite::BeatPosition::fromBeats (16.0), count + 0.4f } });
        phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
        song.setParameter (sampler, "attack", 5.0f); // a step to undo while the pass writes
        const auto before = phazite::describeUndo (&state);
        EnginePlayback playback;
        song.run ("transport.playStop");
        playback.play (0.25);
        CHECK (juce::exactlyEqual (phazite::getValueSetByHand (state, slices), count));
        CHECK (phazite::describeUndo (&state) == before);
        phazite::undo (state);
        playback.play (0.25);
        song.run ("transport.playStop");

        // What the pass wrote, from where it started up to its end, where the lane comes back: the value on its step all along.
        const auto points = phazite::getAutomationPoints (song.context, slices);
        const auto onStep = travel->toPosition (count);
        REQUIRE (points.size() >= 2);
        CHECK (points.front().position == doctest::Approx (onStep).epsilon (1.0e-5));
        auto lastOnStep = 0.0;

        for (const auto& point : points)
            if (std::abs (point.position - onStep) < 1.0e-5f)
                lastOnStep = point.beat.inBeats();

        for (const auto& point : points)
            if (point.beat.inBeats() < lastOnStep)
                CHECK (point.position == doctest::Approx (onStep).epsilon (1.0e-5));

        phazite::setAutomationMode (song.context, track, phazite::AutomationMode::read);
    }
}

TEST_CASE ("A modulation moves a frequency as many octaves wherever it is: an LFO's swing at 200 Hz as at 5 kHz")
{
    // Modulation adds along the knob's travel: on a logarithmic one, a depth is a number of octaves, whatever the frequency.
    // An LFO a tenth of the travel either way swings a frequency an octave each way (1000^0.1 is 2), so from a quarter of it
    // to four times it.
    for (const auto base : { 200.0f, 5000.0f })
    {
        CAPTURE (base);
        TestSong song;
        song.setTempo (120.0);
        const auto device =
            phazite::test::addTestDevice (*phazite::findSong (song.context), song.firstTrack, phazite::test::TestFrequency::typeID);
        song.setParameter (device, "frequency", base);
        const auto lfo = song.addModulator (song.firstTrack, phazite::ModulatorType::lfo); // a sine, both ways, once a second
        song.modulate (lfo, { device, "frequency" }, 0.1f);
        const auto audio = phazite::test::renderOffline (song.context, 2.0);
        const auto range =
            audio.findMinMax (0, phazite::test::renderSampleRate / 2, audio.getNumSamples() - phazite::test::renderSampleRate / 2);
        CHECK (range.getEnd() / range.getStart() == doctest::Approx (4.0f).epsilon (0.01));
    }
}

TEST_CASE ("Phazite's engine plays a fader's automation, which the fader shows; stopped and sent back, where the song stopped until "
           "the playhead is moved, then the lane's value at the playhead")
{
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto top = phazite::decibelsToFaderPosition (0.0f), bottom = phazite::decibelsToFaderPosition (-24.0f);
    song.addAutomation (song.getVolumeParameter (track),
                        { { phazite::BeatPosition(), top }, { phazite::BeatPosition::fromBeats (4.0), bottom } });
    EnginePlayback playback;
    const auto volume = phazite::getVolume (song.context, track);

    // Half way along the line at beat 2.
    song.run ("transport.playStop");
    playback.play (1.0);
    CHECK (volume.getValue() == doctest::Approx ((top + bottom) / 2.0f).epsilon (0.01));

    // Stopped and sent back to the start: where the song stopped, so that what rings on rings on as it was.
    song.run ("transport.playStop");
    playback.play (0.05);
    CHECK (volume.getValue() == doctest::Approx ((top + bottom) / 2.0f).epsilon (0.01));

    // The playhead put there by hand: the lane's value there.
    phazite::movePlayhead (song.context, phazite::BeatPosition());
    playback.play (0.05);
    CHECK (volume.getValue() == doctest::Approx (top).epsilon (0.001));
}

TEST_CASE ("Touch writes what a control does while it is held, from the lane's value, as one undo step")
{
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getVolumeParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    EnginePlayback playback;
    auto volume = phazite::getVolume (song.context, track);

    song.run ("transport.playStop");
    playback.play (0.5); // to beat 1
    volume.beginGesture();
    CHECK (volume.getValue() == doctest::Approx (0.5f)); // grabbed where the lane is
    volume.setValue (0.3f);
    playback.play (0.5);
    CHECK (volume.getValue() == doctest::Approx (0.3f)); // played as set by hand meanwhile
    volume.setValue (0.6f);
    playback.play (0.5);
    volume.endGesture();
    playback.play (0.5); // to beat 4, the lane playing again
    CHECK (volume.getValue() == doctest::Approx (0.5f));
    song.run ("transport.playStop");

    // From where it was grabbed: the lane's value, the moves, and at the end the lane's value again.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    REQUIRE (points.size() >= 5);
    CHECK (points[1].beat.inBeats() == doctest::Approx (1.0).epsilon (0.05));
    CHECK (points[1].position == doctest::Approx (0.5f));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - 0.3f) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - 0.6f) < 0.001f; }));
    CHECK (points[points.size() - 2].position == doctest::Approx (0.5f));
    CHECK (points.back().beat.inBeats() == doctest::Approx (16.0));

    // One step takes it back, the control's moves with it.
    phazite::undo (*phazite::findSong (song.context));
    CHECK (phazite::getAutomationPoints (song.context, lane).size() == 2);
}

TEST_CASE ("Touch over a curve of one point writes its pass, and the point stays where it was meanwhile")
{
    // A curve of one point plays its value everywhere, and a value set by hand moves that point; but not while a pass writes
    // the parameter, which writes its own points.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getVolumeParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition::fromBeats (12.0), 0.5f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    EnginePlayback playback;
    auto volume = phazite::getVolume (song.context, track);

    song.run ("transport.playStop");
    playback.play (0.5); // to beat 1
    volume.beginGesture();
    volume.setValue (0.3f);
    playback.play (0.5);
    volume.setValue (0.6f);
    playback.play (0.5);

    const auto meanwhile = phazite::getAutomationPoints (song.context, lane);
    REQUIRE (meanwhile.size() == 1);
    CHECK (meanwhile.front().position == doctest::Approx (0.5f));

    volume.endGesture();
    song.run ("transport.playStop");

    // The pass, from beat 1, and the point at beat 12 as it was.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (points.size() > 1);
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - 0.6f) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 12.0) < 0.001 && std::abs (p.position - 0.5f) < 0.001f; }));
}

TEST_CASE ("Latch writes from a control's first move until playback stops, and write puts its track in latch")
{
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    // A pan without automation: latched from its first move (a key press, not a drag) until the song stops.
    song.run ("transport.playStop");
    playback.play (0.5);
    pan.setValue (-0.5f);
    playback.play (1.0);
    song.run ("transport.playStop");
    auto points = phazite::getAutomationPoints (song.context, lane);
    REQUIRE (points.size() >= 2);
    CHECK (points.front().beat.inBeats() == doctest::Approx (1.0).epsilon (0.05));
    CHECK (points.back().beat.inBeats() == doctest::Approx (3.0).epsilon (0.05));
    CHECK (points.back().position == doctest::Approx (panPosition (-0.5f)));

    // Write: its automated parameters write from the start, the control's value; let go, the track goes on in latch.
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
    song.run ("transport.playStop");
    playback.play (0.5);
    pan.beginGesture();
    pan.setValue (0.25f);
    playback.play (0.5);
    pan.endGesture();
    CHECK (phazite::getAutomationMode (song.context, track) == phazite::AutomationMode::latch);
    song.run ("transport.playStop");
    points = phazite::getAutomationPoints (song.context, lane);
    CHECK (points.front().beat.inBeats() == doctest::Approx (0.0));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.25f)) < 0.001f; }));
}

TEST_CASE ("A control's drag taken back by Escape takes back what it recorded: touch as never grabbed, latch and write going on as "
           "before the grab")
{
    // Taken back as a drag ends, a latch pass kept the drag's moves, to write as playback stopped, and a write pass wrote all
    // it had recorded since playback started into the drag's step, which the rollback then removed (2026-11-14).
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    EnginePlayback playback;
    const auto hasValue = [] (const std::vector<phazite::AutomationPoint>& points, float value)
    { return std::any_of (points.begin(), points.end(), [value] (const auto& p) { return std::abs (p.position - value) < 0.001f; }); };

    SUBCASE ("Touch")
    {
        const auto lane = song.getVolumeParameter (track);
        song.addAutomation (lane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
        phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
        auto volume = phazite::getVolume (song.context, track);

        song.run ("transport.playStop");
        playback.play (0.5);
        volume.beginGesture();
        volume.setValue (0.3f);
        playback.play (0.5);
        volume.setValue (0.6f);
        playback.play (0.5);
        volume.cancelGesture();
        playback.play (0.5);
        CHECK (volume.getValue() == doctest::Approx (0.5f)); // the lane plays again
        song.run ("transport.playStop");

        const auto points = phazite::getAutomationPoints (song.context, lane);
        CHECK (points.size() == 2);
        CHECK_FALSE (hasValue (points, 0.3f));
        CHECK_FALSE (hasValue (points, 0.6f));
    }

    SUBCASE ("Latch")
    {
        const auto lane = song.getPanParameter (track);
        phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
        auto pan = phazite::getPan (song.context, track);

        song.run ("transport.playStop");
        playback.play (0.5);
        pan.setValue (-0.5f); // latched from here
        playback.play (0.5);
        pan.beginGesture();
        pan.setValue (0.8f);
        playback.play (0.5);
        pan.cancelGesture();
        CHECK (pan.getValue() == doctest::Approx (-0.5f));
        playback.play (0.5);
        song.run ("transport.playStop");

        const auto points = phazite::getAutomationPoints (song.context, lane);
        REQUIRE (points.size() >= 2);
        CHECK_FALSE (hasValue (points, panPosition (0.8f)));
        CHECK (points.back().position == doctest::Approx (panPosition (-0.5f)));
        CHECK (points.back().beat.inBeats() == doctest::Approx (4.0).epsilon (0.05)); // written to where playback stopped
    }

    SUBCASE ("Write")
    {
        const auto lane = song.getPanParameter (track);
        song.addAutomation (lane, { { phazite::BeatPosition(), -0.5f }, { phazite::BeatPosition::fromBeats (16.0), -0.5f } });
        phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
        auto pan = phazite::getPan (song.context, track);

        song.run ("transport.playStop");
        playback.play (0.5);
        pan.beginGesture();
        pan.setValue (0.25f);
        playback.play (0.5);
        pan.cancelGesture();
        CHECK (phazite::getAutomationMode (song.context, track) == phazite::AutomationMode::write); // not let go into latch
        playback.play (0.5);
        song.run ("transport.playStop");

        // The pass from playback's start, as before the drag, to where it stopped; without the drag's value.
        const auto points = phazite::getAutomationPoints (song.context, lane);
        REQUIRE (points.size() >= 2);
        CHECK (points.front().beat.inBeats() == doctest::Approx (0.0));
        CHECK_FALSE (hasValue (points, panPosition (0.25f)));
        CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                            { return std::abs (p.beat.inBeats() - 3.0) < 0.15 && std::abs (p.position - panPosition (-0.5f)) < 0.001f; }));
    }
}

TEST_CASE ("A control's moves are written where they were heard: earlier by the output's latency and the song's delay compensation")
{
    // Known bug 103: automation was written at the engine's playhead, ahead of what was heard by as much as the sound takes to
    // reach the speakers, so a move made to the music played back late. Here an output a tenth of a second late, 0.2 beats.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getVolumeParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    // The engine's clock held: the playhead read and the place heard add the same time since the last block.
    const phazite::test::HeldClock clock;
    EnginePlayback playback;
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    host.prepare (EnginePlayback::sampleRate, EnginePlayback::blockSize, 0, 0, 4800);
    auto volume = phazite::getVolume (song.context, track);

    song.run ("transport.playStop");
    playback.play (1.0);
    const auto grabbedAt = phazite::getPlayheadBeat (song.context).inBeats(); // the engine's, about beat 2
    volume.beginGesture();
    volume.setValue (0.3f);
    playback.play (0.5);
    const auto letGoAt = phazite::getPlayheadBeat (song.context).inBeats();
    volume.endGesture();
    song.run ("transport.playStop");

    // The pass from where the grab was heard, to where the control was let go, heard.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    REQUIRE (points.size() >= 4);
    CHECK (points[1].beat.inBeats() == doctest::Approx (grabbedAt - 0.2).epsilon (0.001));
    CHECK (points[1].position == doctest::Approx (0.5f));
    CHECK (std::any_of (points.begin(), points.end(), [&] (const auto& p)
                        { return std::abs (p.beat.inBeats() - (letGoAt - 0.2)) < 0.002 && std::abs (p.position - 0.3f) < 0.001f; }));
}

TEST_CASE ("A jump back while writing with the loop on ends the pass where the song was, and writes on from where it lands")
{
    // Known bug 103: the jump was taken for the loop going round, which wrote the value held through to the loop's end, and
    // on from the loop's start to where the playhead landed. Beats 0 to 8 are looped; latch writes from beat 4.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
    phazite::loopRange (song.context, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (8.0) });
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    song.run ("transport.playStop");
    playback.play (2.0); // to beat 4
    pan.setValue (-0.5f);
    playback.play (1.0); // to beat 6
    phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (2.0));
    playback.play (0.5); // to beat 3
    song.run ("transport.playStop");

    // From beat 4 to 6, then from 2 to 3: nothing written at the loop's end, nor from its start.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    REQUIRE (points.size() >= 2);
    CHECK (points.front().beat.inBeats() == doctest::Approx (2.0).epsilon (0.01));
    CHECK (std::none_of (points.begin(), points.end(), [] (const auto& p) { return p.beat.inBeats() > 6.05; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 6.0) < 0.05 && std::abs (p.position - panPosition (-0.5f)) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 3.0) < 0.05 && std::abs (p.position - panPosition (-0.5f)) < 0.001f; }));
}

TEST_CASE ("Escape in Write takes back the drag alone: what the pass wrote at the loop's end during it stays, up to the grab")
{
    // Known bug 163: the points the writer wrote as the loop went round during the drag joined the drag's step, and Escape took
    // them back with it, leaving the lane as it was before playback started. Write mode writes the lane's value where playback
    // starts, -1, over a ramp up to 1; beats 0 to 2 are looped.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), -1.0f }, { phazite::BeatPosition::fromBeats (2.0), 1.0f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
    phazite::loopRange (song.context, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (2.0) });
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    song.run ("transport.playStop");
    playback.play (0.5); // to beat 1
    pan.beginGesture();
    pan.setValue (0.25f);
    playback.play (0.75); // round the loop, to beat 0.5
    pan.setValue (0.75f);
    playback.play (0.1);
    pan.cancelGesture();
    playback.play (0.2);
    song.run ("transport.playStop");

    // Written from playback's start to the loop's end at -1, then on again from the loop's start; none of the drag's values.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (
        std::none_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.25f)) < 0.001f; }));
    CHECK (
        std::none_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.75f)) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 2.0) < 0.001 && std::abs (p.position - panPosition (-1.0f)) < 0.001f; }));
    CHECK (std::none_of (points.begin(), points.end(), [] (const auto& p)
                         { return p.beat.inBeats() > 0.9 && p.beat.inBeats() < 1.99 && p.position > panPosition (-0.99f); }));
}

TEST_CASE ("A control grabbed before playback starts, in Write, is taken back by Escape to the pass as playback started")
{
    // Known bug 163: the grab was noted with no pass (none wrote while stopped), so Escape took away the whole pass that write
    // mode began as playback started, as if the control had been grabbed in touch.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), -1.0f }, { phazite::BeatPosition::fromBeats (16.0), 1.0f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    pan.beginGesture(); // stopped
    song.run ("transport.playStop");
    playback.play (0.5);
    pan.setValue (0.75f);
    playback.play (0.25);
    pan.cancelGesture();
    playback.play (0.25); // to beat 2
    song.run ("transport.playStop");

    // Written from playback's start at the lane's value there, -1, as far as where playback stopped; without the drag's value.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (
        std::none_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.75f)) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 2.0) < 0.05 && std::abs (p.position - panPosition (-1.0f)) < 0.001f; }));
}

TEST_CASE ("A control held as Play is pressed, in Write, goes back by Escape to the value its pass writes, which is what plays")
{
    // Known bug 174: the punch-in as playback started set the value by hand to the lane's within the drag's step, so Escape
    // put it back to where it was before the drag, 0, while the pass went on writing the lane's value, -1: what played (the
    // value set by hand, while the pass writes) wasn't what was written. Escape takes back the drag alone.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), -1.0f }, { phazite::BeatPosition::fromBeats (16.0), -1.0f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);
    auto& player = phazite::getSession (song.context)->getPlayer();

    pan.beginGesture(); // stopped, at 0
    pan.setValue (0.25f);
    song.run ("transport.playStop");
    playback.play (0.25);
    pan.setValue (0.75f);
    playback.play (0.25);
    pan.cancelGesture();
    CHECK (phazite::getValueSetByHand (*phazite::findSong (song.context), lane) == doctest::Approx (-1.0f));
    playback.play (0.25);
    CHECK (pan.getValue() == doctest::Approx (-1.0f));
    CHECK (player.getPlayedValue (lane, false).value_or (0.0f) == doctest::Approx (-1.0f));
    playback.play (0.25); // to beat 2
    song.run ("transport.playStop");

    // What was written, the pass from playback's start at -1, is what played; the drag's values went with it.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (
        std::none_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.75f)) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 2.0) < 0.05 && std::abs (p.position - panPosition (-1.0f)) < 0.001f; }));
    CHECK (pan.getValue() == doctest::Approx (-1.0f));
}

TEST_CASE ("Undo straight after Escape on a control held as Play was pressed, in Write: what plays is still what is written")
{
    // A reviewer, of known bug 174's fix: the value Escape gives back is a step, "Record Automation", and an Undo of it
    // while playing set the value back to the rolled-back one while the pass went on writing the lane's. Now the pass writes
    // what an undo or a redo sets, from then. The whole history walked back and forth after it.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    song.addAutomation (lane, { { phazite::BeatPosition(), -1.0f }, { phazite::BeatPosition::fromBeats (16.0), -1.0f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
    EnginePlayback playback;
    auto& songState = *phazite::findSong (song.context);
    phazite::test::UndoCheck check (song.context);
    auto pan = phazite::getPan (song.context, track);
    auto& player = phazite::getSession (song.context)->getPlayer();

    pan.beginGesture(); // stopped, at 0
    pan.setValue (0.25f);
    song.run ("transport.playStop");
    playback.play (0.25);
    pan.setValue (0.75f);
    playback.play (0.25);
    pan.cancelGesture();
    REQUIRE (phazite::getValueSetByHand (songState, lane) == doctest::Approx (-1.0f));

    // Undo at once, still playing: the value set by hand goes back, and that is what plays and what the pass writes.
    phazite::undo (songState);
    const auto undone = phazite::getValueSetByHand (songState, lane);
    playback.play (0.25);
    CHECK (player.getPlayedValue (lane, false).value_or (99.0f) == doctest::Approx (undone));
    playback.play (0.25); // to beat 2
    song.run ("transport.playStop");

    // Written as it played: the lane's value up to the undo, and the value it set from then to where playback stopped.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (std::any_of (points.begin(), points.end(), [undone] (const auto& p)
                        { return std::abs (p.beat.inBeats() - 2.0) < 0.05 && std::abs (p.position - panPosition (undone)) < 0.001f; }));
    CHECK (
        std::none_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.75f)) < 0.001f; }));
    check.walkBackAndForth();
}

namespace
{
bool hasPanPoint (const std::vector<phazite::AutomationPoint>& points, double beat, float pan, double near = 0.05)
{
    return std::any_of (points.begin(), points.end(), [=] (const auto& p)
                        { return std::abs (p.beat.inBeats() - beat) < near && std::abs (p.position - panPosition (pan)) < 0.001f; });
}
} // namespace

TEST_CASE ("Undo or Redo with the loop gone round since a pass's last move: the round is kept, written after it as a step of its own")
{
    // A reviewer, of known bug 174's fix: the pass followed the value an undo set at once, and with the loop gone round since
    // its last move it wrote the round through the song's history inside the undo, which JUCE's UndoManager refuses (it
    // asserts, and the round was lost). Latch writes the pan; beats 0 to 2 are looped.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
    phazite::loopRange (song.context, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (2.0) });
    EnginePlayback playback;
    auto& songState = *phazite::findSong (song.context);
    auto& history = songState.getUndoManager();
    auto pan = phazite::getPan (song.context, track);

    song.run ("transport.playStop");
    playback.play (0.25);      // to beat 0.5
    pan.setValue (-0.5f);      // a step of its own, which latch punches in at
    playback.play (1.0);       // round the loop, to beat 0.5 again
    phazite::undo (songState); // the pan back to 0, which the pass follows: the round before is finished
    CHECK (phazite::getValueSetByHand (songState, lane) == doctest::Approx (0.0f));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return hasPanPoint (phazite::getAutomationPoints (song.context, lane), 2.0, -0.5f, 0.001); }));
    CHECK (history.getUndoDescription() == "Record Automation");

    // Redo, the loop gone round again since an undo that finished nothing: the same.
    pan.setValue (0.5f);
    playback.play (0.25); // to beat 1
    phazite::undo (songState);
    REQUIRE (history.canRedo());
    playback.play (0.75); // round the loop, to beat 0.5
    phazite::redo (songState);
    CHECK (phazite::getValueSetByHand (songState, lane) == doctest::Approx (0.5f));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return hasPanPoint (phazite::getAutomationPoints (song.context, lane), 2.0, 0.0f, 0.001); }));
    CHECK (history.getUndoDescription() == "Record Automation");
    playback.play (0.25);
    song.run ("transport.playStop");
}

TEST_CASE ("Stop during a control's drag, then Escape: what was written as playback stopped stays, a step of its own; the drag alone goes")
{
    // A reviewer, of known bug 163's fix: Stop works while the mouse is down, and what the writer wrote as playback stopped
    // joined the drag's step, which Escape took back, latched pass and all. Latch writes the pan from beat 1.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    song.run ("transport.playStop");
    playback.play (0.5);
    pan.setValue (-0.5f); // latched from here
    playback.play (0.5);
    pan.beginGesture();
    pan.setValue (0.8f);
    playback.play (0.5);             // to beat 3
    song.run ("transport.playStop"); // the key, with the mouse down
    pan.cancelGesture();
    pan.endGesture(); // the mouse goes up

    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (hasPanPoint (points, 1.0, -0.5f));
    CHECK (hasPanPoint (points, 3.0, -0.5f));
    CHECK_FALSE (hasPanPoint (points, 2.0, 0.8f, 2.0));
    CHECK (song.context.getUndoManager()->getUndoDescription() == juce::String ("Record Automation"));
    phazite::undo (*phazite::findSong (song.context));
    CHECK (phazite::getAutomationPoints (song.context, lane).empty());
}

TEST_CASE ("A plug-in's parameter let go during a control's drag writes its pass in a step of its own, which the drag's Escape keeps")
{
    // A reviewer, of known bug 163's fix: a plug-in's gesture that ended during a control's drag wrote into the drag's step,
    // and Escape took it back. Here the pan plays the plug-in's part, written as its own window's gesture.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto volumeLane = song.getVolumeParameter (track);
    const auto panLane = song.getPanParameter (track);
    song.addAutomation (volumeLane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    EnginePlayback playback;
    auto volume = phazite::getVolume (song.context, track);
    auto pan = phazite::getPan (song.context, track);
    auto& writer = phazite::getSession (song.context)->getWriter();

    song.run ("transport.playStop");
    playback.play (0.5);
    volume.beginGesture();
    volume.setValue (0.3f);
    writer.pluginGestureChanged (panLane, true);
    pan.setValue (-0.5f);
    playback.play (0.5);
    writer.pluginGestureChanged (panLane, false);
    playback.play (0.25);
    volume.cancelGesture();
    song.run ("transport.playStop");

    const auto points = phazite::getAutomationPoints (song.context, panLane);
    CHECK (hasPanPoint (points, 1.0, -0.5f));
    CHECK (hasPanPoint (points, 2.0, -0.5f));
    CHECK (phazite::getAutomationPoints (song.context, volumeLane).size() == 2); // the drag's own pass gone
}

TEST_CASE ("A jump after the loop went round without a move writes the pass to the loop's end and from its start first")
{
    // A reviewer, of known bug 103's fix: the jump ended the pass where it was heard, missing that the loop had gone round
    // since its last move. Beats 0 to 8 are looped; latch writes from beat 7; the loop goes round; Home at beat 1.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
    phazite::loopRange (song.context, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (8.0) });
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    song.run ("transport.playStop");
    playback.play (3.5); // to beat 7
    pan.setValue (-0.5f);
    playback.play (1.0); // round, to beat 1
    phazite::movePlayhead (song.context, phazite::BeatPosition());
    playback.play (0.25);
    song.run ("transport.playStop");

    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (hasPanPoint (points, 7.0, -0.5f));
    CHECK (hasPanPoint (points, 8.0, -0.5f));
    CHECK (std::none_of (points.begin(), points.end(), [] (const auto& p) { return p.beat.inBeats() > 1.05 && p.beat.inBeats() < 6.95; }));
}

TEST_CASE ("The loop going round between two moves is known from the engine's rounds, however little the playhead went back")
{
    // A reviewer, of known bug 103's fix: a wrap was guessed from the playhead going back by more than a block, so a move
    // nearly a whole loop after the last was taken for none. Beats 0 to 2 are looped, a second; the second move comes half a
    // block short of a whole loop after the first.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lane = song.getPanParameter (track);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::latch);
    phazite::loopRange (song.context, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (2.0) });
    EnginePlayback playback;
    auto pan = phazite::getPan (song.context, track);

    song.run ("transport.playStop");
    playback.play (0.25);
    pan.setValue (-0.5f);
    playback.playSamples (48000 - 128);
    pan.setValue (0.5f);
    playback.play (0.1);
    song.run ("transport.playStop");

    // The first move held to the loop's end, and the second from its start.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (hasPanPoint (points, 2.0, -0.5f, 0.001));
    CHECK (hasPanPoint (points, 0.0, -0.5f, 0.001));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - panPosition (0.5f)) < 0.001f; }));
}

TEST_CASE ("The loop's rounds are counted where the engine goes round: not at a count-in's end inside a block, nor as the tempo slows")
{
    // A reviewer, 2026-11-15: rounds were inferred from a block starting before where the one before ended, by the speed it
    // started at; so the count-in's last block (moving for the whole block though the song started in its middle) and every
    // block of a slowing ramp counted one, and the writer then wrote the pass's value on to the loop's end, where playback
    // never was. Beats 0 to 8 are looped; Write holds the pan's lane value from playback's start over a ramp up to 1.
    const auto writesNoFurtherThanPlayback = [] (bool countIn, bool slowing)
    {
        TestSong song;
        song.setTempo (120.0);
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        song.addMidiClip (track, 0.0, 16.0);
        const auto lane = song.getPanParameter (track);
        song.addAutomation (lane, { { phazite::BeatPosition(), -1.0f }, { phazite::BeatPosition::fromBeats (16.0), 1.0f } });
        auto& state = *phazite::findSong (song.context);

        if (slowing) // 120 to 60 BPM over 16 beats
        {
            phazite::addTempoChange (state, phazite::BeatPosition::fromBeats (16.0), 60.0);
            phazite::setTempoCurve (state, 0, phazite::tempoRamp);
        }

        phazite::setAutomationMode (song.context, track, phazite::AutomationMode::write);
        phazite::loopRange (song.context, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (8.0) });
        song.setSwitch ("transport.countIn", countIn);
        const juce::ScopeGuard countInOff { [&song] { song.setSwitch ("transport.countIn", false); } }; // the app's setting
        phazite::routeLiveMidiInput (state);
        EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::realTime, EnginePlayback::sampleRate,
                                 countIn ? 448 : 512); // a bar of count-in, 96,000 samples, ends inside a 448-sample block

        song.run ("transport.record");
        playback.play (countIn ? 2.0 : 0.0);
        playback.play (1.0); // to about beat 2
        const auto stoppedAt = phazite::getPlayheadBeat (song.context).inBeats();
        song.run ("transport.record");

        // The lane past where playback was is as it was: no point between there and the ramp's end.
        CAPTURE (stoppedAt);
        const auto points = phazite::getAutomationPoints (song.context, lane);
        return std::none_of (points.begin(), points.end(),
                             [stoppedAt] (const auto& p) { return p.beat.inBeats() > stoppedAt + 0.1 && p.beat.inBeats() < 15.9; });
    };

    CHECK (writesNoFurtherThanPlayback (true, false));
    CHECK (writesNoFurtherThanPlayback (false, true));
}
TEST_CASE ("Phazite's engine plays a track's LFO on its volume, which the LFO's editor and the fader's ring show, and a macro")
{
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 16.0);
    const auto lfo = song.addModulator (track, phazite::ModulatorType::lfo);
    song.setModulatorParameter (lfo, "rateType", 0.0f); // in hertz, at 1 Hz
    song.modulate (lfo, song.getVolumeParameter (track), 0.5f);
    EnginePlayback playback;
    const auto volume = phazite::getVolume (song.context, track);

    // Its editor's history, as it played, swinging both ways: a bipolar LFO, as Phazite makes one.
    song.run ("transport.playStop");
    playback.play (1.25); // to the top of its cycle
    const auto history = phazite::getModulatorOutput (song.context, lfo, 1.0);
    REQUIRE (history.size() >= 90);
    CHECK (*std::min_element (history.begin(), history.end()) < -0.8f);
    CHECK (*std::max_element (history.begin(), history.end()) > 0.8f);
    CHECK (phazite::getModulatorValue (song.context, lfo) == doctest::Approx (history.back()).epsilon (0.05));

    // The fader moves with it, from where it is set by hand: its ring shows where it is now.
    const auto range = phazite::getModulationRange (volume);
    REQUIRE (range.has_value());
    CHECK (range->now >= range->low - 0.001f);
    CHECK (range->now <= range->high + 0.001f);
    CHECK (std::abs (volume.getValue() - volume.getBaseValue()) > 0.01f);

    // A macro turned up turns the volume down at once, without a new plan.
    song.run ("transport.playStop");
    phazite::setModulatorOn (song.context, lfo, false);
    const auto macro = song.addModulator (track, phazite::ModulatorType::macro);
    song.modulate (macro, song.getVolumeParameter (track), -0.5f);
    playback.play (0.1);
    const auto before = volume.getValue();
    song.setMacroValue (macro, 1.0f);
    playback.play (0.1);
    CHECK (volume.getValue() == doctest::Approx (std::max (0.0f, before - 0.5f)).epsilon (0.01));
}

TEST_CASE ("In 6/8 an LFO a bar a cycle goes round once a bar, free or with the transport, and a 1/4 one each quarter note")
{
    // At 120 BPM a bar of 6/8 is three quarter notes, a second and a half. Saws, each read where it is at moments on the
    // control points: a cycle apart they are where they were, half a cycle apart elsewhere.
    TestSong song;
    song.setTempo (120.0);
    song.setTimeSignature (6, 8);
    const auto track = song.firstTrack;
    song.addMidiClip (track, 0.0, 24.0); // eight bars: playback stops at the song's end

    const auto addSaw = [&song, track] (float sync, float rateType)
    {
        const auto lfo = song.addModulator (track, phazite::ModulatorType::lfo);
        song.setModulatorParameter (lfo, "wave", 2.0f); // a saw, up
        song.setModulatorParameter (lfo, "syncType", sync);
        song.setModulatorParameter (lfo, "rateType", rateType);
        song.modulate (lfo, song.getPanParameter (track), 0.1f);
        return lfo;
    };

    const auto freeBar = addSaw (0.0f, 3.0f);      // free, a bar a cycle
    const auto transportBar = addSaw (1.0f, 3.0f); // with the transport, a bar a cycle
    const auto quarter = addSaw (1.0f, 8.0f);      // with the transport, a quarter note a cycle
    EnginePlayback playback;
    song.run ("transport.playStop");

    // Each saw's value at moments on the control points (a multiple of 32 samples): 0.3 and 1.8 s a bar apart, 0.75 and
    // 2.25 s too, half a bar after them; 0.55 and 1.05 s a quarter note apart, half a quarter note after 0.3 s.
    struct Values
    {
        float freeBar, transportBar, quarter;
    };

    std::map<int, Values> at; // by milliseconds
    auto played = 0.0;

    for (const auto milliseconds : { 300, 550, 750, 1050, 1800, 2250 })
    {
        const auto seconds = milliseconds / 1000.0;
        playback.play (seconds - played);
        played = seconds;
        at[milliseconds] = { phazite::getModulatorValue (song.context, freeBar), phazite::getModulatorValue (song.context, transportBar),
                             phazite::getModulatorValue (song.context, quarter) };
    }

    for (const auto& [lfo, valueOf] : { std::pair (std::string ("free, a bar"), &Values::freeBar),
                                        std::pair (std::string ("with the transport, a bar"), &Values::transportBar) })
    {
        CAPTURE (lfo);
        CHECK (at[300].*valueOf == doctest::Approx (at[1800].*valueOf).epsilon (0.001));
        CHECK (at[750].*valueOf == doctest::Approx (at[2250].*valueOf).epsilon (0.001));
        CHECK (std::abs (at[300].*valueOf - at[750].*valueOf) > 0.2f);
    }

    CHECK (at[550].quarter == doctest::Approx (at[1050].quarter).epsilon (0.001));
    CHECK (at[300].quarter == doctest::Approx (at[1800].quarter).epsilon (0.001));
    CHECK (std::abs (at[300].quarter - at[550].quarter) > 0.2f);
}

TEST_CASE ("In 6/8 the chorus's LFO in time with the song keeps to where the song is in quarter notes, block after block")
{
    // A cycle a quarter note (half a second at 120 BPM), on a tone so that the chorus plays rather than sleeps: after each
    // block, its LFO as far round as the song is, with nothing to put right (until 2026-11-10 the song's beats in 6/8 were
    // eighths, which the LFO, counting quarter notes, chased every block).
    TestSong song;
    song.setTempo (120.0);
    song.setTimeSignature (6, 8);
    const auto track = song.firstTrack;
    song.addAudioClip (track, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 4.0, 440.0, 0.25f), 0.0);
    const auto chorusID = song.addEffect (track, phazite::devices::Motion::typeID);
    song.setParameter (chorusID, "sync", 1.0f);
    song.setParameter (chorusID, "division", phazite::devices::dsp::quarterNoteValue);
    EnginePlayback playback;
    song.run ("transport.playStop");
    playback.play (0.1);

    auto* chorus = phazite::makeDeviceHandle (*phazite::findSong (song.context), chorusID).getProcessor<phazite::devices::Motion>();
    REQUIRE (chorus != nullptr);
    auto played = (int) std::lround (0.1 * EnginePlayback::sampleRate);

    for (int block = 0; block < 100; ++block)
    {
        playback.play (EnginePlayback::blockSize / EnginePlayback::sampleRate);
        played += EnginePlayback::blockSize;
        const auto expected = std::fmod (played / 24000.0, 1.0);
        auto off = (double) chorus->getChorus().getPhase().get() - expected;
        off -= std::round (off);
        CAPTURE (block);
        REQUIRE (std::abs (off) < 1.0e-4);
    }
}

TEST_CASE ("A chorus's LFO a bar a cycle follows the song's bars: three quarter notes in 6/8, then four after a change to 4/4")
{
    // In time with the song, a bar a cycle (its Division at first), on a tone so that the chorus plays rather than sleeps:
    // two bars of 6/8, three quarter notes (a second and a half at 120 BPM) each, then 4/4 from the sixth quarter note, as
    // the song's tempo map can hold it. After each block its LFO is as far round as the song is through its bar.
    TestSong song;
    song.setTempo (120.0);
    song.setTimeSignature (6, 8);
    namespace schema = phazite::schema;
    auto tempoMap = phazite::findSong (song.context)->getState().getChildWithName (schema::TEMPOMAP);
    tempoMap.appendChild (
        juce::ValueTree (schema::TIMESIGNATURE, { { schema::mapBeat, 6.0 }, { schema::numerator, 4 }, { schema::denominator, 4 } }),
        nullptr);
    const auto track = song.firstTrack;
    song.addAudioClip (track, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 8.0, 440.0, 0.25f), 0.0);
    const auto chorusID = song.addEffect (track, phazite::devices::Motion::typeID);
    song.setParameter (chorusID, "sync", 1.0f);
    song.setParameter (chorusID, "division", phazite::devices::dsp::barNoteValue);
    EnginePlayback playback;
    song.run ("transport.playStop");
    playback.play (0.1);

    auto* chorus = phazite::makeDeviceHandle (*phazite::findSong (song.context), chorusID).getProcessor<phazite::devices::Motion>();
    REQUIRE (chorus != nullptr);
    auto played = (int) std::lround (0.1 * EnginePlayback::sampleRate);

    while (played < 6.5 * EnginePlayback::sampleRate)
    {
        playback.play (EnginePlayback::blockSize / EnginePlayback::sampleRate);
        played += EnginePlayback::blockSize;
        const auto beat = played / 24000.0;
        const auto bars = beat < 6.0 ? beat / 3.0 : 2.0 + (beat - 6.0) / 4.0;

        // Where the time signature changes in a block, the LFO glides onto the new bar's place over 10 ms.
        if (std::abs (beat - 6.0) < 0.1)
            continue;

        auto off = (double) chorus->getChorus().getPhase().get() - (bars - std::floor (bars));
        off -= std::round (off);
        CAPTURE (beat);
        REQUIRE (std::abs (off) < 1.0e-4);
    }
}

namespace
{
// The song as Phazite's engine renders it from its start, and the level of a side of it over a stretch, in dBFS.
juce::AudioBuffer<float> renderOwn (TestSong& song, double seconds = 1.0)
{
    phazite::test::dispatchPendingMessages();
    return phazite::renderWithOwnEngine (*phazite::findSong (song.context), (int) (seconds * 48000.0), 48000.0, 256);
}

float levelOf (const juce::AudioBuffer<float>& audio, int side, double from = 0.25, double to = 0.75)
{
    const auto start = (int) (from * 48000.0);
    return juce::Decibels::gainToDecibels (audio.getRMSLevel (side, start, (int) (to * 48000.0) - start));
}

// A 440 Hz tone, at a quarter of full scale, on a track from the song's start.
phazite::TrackID addToneTrack (TestSong& song, phazite::TrackID track, const char* name)
{
    song.addAudioClip (
        track, phazite::test::writeSound (song.getSoundsFolder().getChildFile (juce::String (name) + ".wav"), 2.0, 440.0, 0.25f), 0.0);
    return track;
}

constexpr float toneLevel = -18.05f; // the tone's RMS through the master's -3 dB
} // namespace

TEST_CASE ("Phazite's engine sends to a return before or after the fader, at the send's level")
{
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    song.addReturn (track, phazite::devices::Equalizer::typeID, -6.0f); // a flat EQ: the return passes what it is sent
    const auto send = phazite::getSends (song.context, track).front().id;
    song.setVolume (track, -100.0f);

    // After the fader, down: nothing. Before it: the tone, on both sides, 6 dB down.
    CHECK (levelOf (renderOwn (song), 0) < -90.0f);
    phazite::setPreFader (song.context, send, true);
    auto audio = renderOwn (song);
    CHECK (levelOf (audio, 0) == doctest::Approx (toneLevel - 6.0f).epsilon (0.002));
    CHECK (levelOf (audio, 1) == doctest::Approx (toneLevel - 6.0f).epsilon (0.002));

    // Its level is taken in at once.
    phazite::getSendLevel (song.context, send).setValue (phazite::decibelsToFaderPosition (0.0f));
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel).epsilon (0.002));
}

namespace
{
// A song at a tempo whose first track holds a note on Test Tone (a steady level, its note's velocity) for 32 beats, and sends
// it at 0 dB to an empty return: before its fader, the fader down, so that the master hears the return alone, what the send
// sends; or after it, the fader at 0 dB, so that the master hears both. The send's level, its track and all.
struct SendToReturn
{
    phazite::TrackID track, returnTrack;
    phazite::ParameterRef level;
};

SendToReturn addSendToReturn (TestSong& song, double tempo, bool preFader = true)
{
    song.setTempo (tempo);
    auto& state = *phazite::findSong (song.context);
    const auto track = song.firstTrack;
    phazite::test::addTestDevice (state, track, phazite::test::TestTone::typeID);
    song.addNote (song.addMidiClip (track, 0.0, 32.0), 60, 0.0, 32.0);
    phazite::TrackID returnTrack;

    {
        const phazite::UndoStep step (state, "Add Return");
        returnTrack = phazite::addNewReturnTrack (state, nullptr);
    }

    song.addSend (track, returnTrack, 0.0f);
    const auto send = phazite::getSends (song.context, track).front().id;

    if (preFader)
    {
        phazite::setPreFader (song.context, send, true);
        song.setVolume (track, -100.0f);
    }

    return { track, returnTrack, { send, phazite::schema::sendLevelParameter } };
}

// What Test Tone's held note plays through the master's -3 dB, as the return plays it at a send's 0 dB; and a send's gain at a
// position along the fader's travel.
float heldTone()
{
    return juce::Decibels::decibelsToGain (phazite::defaultMasterVolume) * 100.0f / 127.0f;
}

float sendGainAt (float position)
{
    return juce::Decibels::decibelsToGain (phazite::faderPositionToDecibels (position), -100.0f);
}

// A send's lane held at silence for a beat, then a straight line up the fader's travel to 0 dB over eight beats.
void addSendRamp (TestSong& song, const phazite::ParameterRef& level)
{
    const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };
    song.addAutomation (level, { { at (0.0), 0.0f }, { at (1.0), 0.0f }, { at (9.0), phazite::decibelsToFaderPosition (0.0f) } });
}

// That what played (the return alone) follows the ramp: silent before it, and at each beat the tone at the send's gain on the
// line there, along the fader's travel.
void checkSendRamp (const juce::AudioBuffer<float>& audio, double sampleRate, double tempo)
{
    const auto top = phazite::decibelsToFaderPosition (0.0f);

    for (const auto beat : { 0.5, 2.0, 3.0, 5.0, 7.0, 8.5, 10.0 })
    {
        CAPTURE (beat);
        const auto position = top * (float) std::clamp ((beat - 1.0) / 8.0, 0.0, 1.0);
        const auto played = audio.getSample (0, (int) std::lround (beat * 60.0 / tempo * sampleRate));
        CHECK (played == doctest::Approx (heldTone() * sendGainAt (position)).epsilon (0.002));
    }
}
} // namespace

TEST_CASE ("A send's lane ramping from silence moves what its return plays, live at every condition and rendered alike")
{
    // Known bug 192: a send's lane was drawn, written and saved, but Phazite's engine played the send at its level set by hand,
    // so the return here played the tone at 0 dB from the start. The send follows its lane as a fader follows its volume's:
    // along the fader's travel, on the song's control points, whatever the rate, the block size and the tempo.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            TestSong song;
            const auto sent = addSendToReturn (song, condition.getTempo());
            addSendRamp (song, sent.level);
            EnginePlayback playback (condition);
            song.run ("transport.playStop");
            const auto audio = playback.play (11.0 * 60.0 / condition.getTempo());
            song.run ("transport.playStop");
            checkSendRamp (audio, condition.getSampleRate(), condition.getTempo());
        });

    TestSong song;
    const auto sent = addSendToReturn (song, 120.0);
    addSendRamp (song, sent.level);
    checkSendRamp (phazite::test::renderOffline (song.context, 5.5), phazite::test::renderSampleRate, 120.0);
}

namespace
{
// The value a parameter plays now (its control shows it), as Phazite's engine plays the song.
float getPlayedValue (TestSong& song, const phazite::ParameterRef& parameter)
{
    return phazite::getParameter (song.context, parameter).getValue();
}

// Plays a second of blocks, stopped, checking after each tenth of a second that a parameter plays a value, within a
// fraction of it, and that the playhead is at a beat.
void checkHeldWhileStopped (EnginePlayback& playback, TestSong& song, const phazite::ParameterRef& parameter, float value, double playhead)
{
    for (int tenth = 0; tenth < 10; ++tenth)
    {
        CAPTURE (tenth);
        playback.play (0.1);
        CHECK (getPlayedValue (song, parameter) == doctest::Approx (value).epsilon (0.01));
        CHECK (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (playhead));
    }
}
} // namespace

TEST_CASE ("Stop going back to where playback started leaves what plays where the song stopped until it plays again or the playhead is "
           "moved, at every condition")
{
    // The owner's report: a reverb's tail through a filter its automation had closed, stopped, blipped loud as the filter's
    // lane opened it again at the start, where Stop sent the playhead. Now a Stop that sends the playhead back leaves every
    // automated value where the song stopped (as FL Studio does, and as Ableton Live's song stays where it stopped); the
    // values go to the playhead when the song plays, or when the playhead is put somewhere by hand. Test Frequency's
    // frequency and its track's fader, automated from beat 0 to beat 8.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            using phazite::BeatPosition;
            TestSong song;
            song.setTempo (condition.getTempo());
            const auto track = song.firstTrack;
            const auto device =
                phazite::test::addTestDevice (*phazite::findSong (song.context), track, phazite::test::TestFrequency::typeID);
            const phazite::ParameterRef frequency { device, "frequency" };
            const auto volume = song.getVolumeParameter (track);
            const auto top = phazite::decibelsToFaderPosition (0.0f), bottom = phazite::decibelsToFaderPosition (-24.0f);
            song.addAutomation (frequency, { { BeatPosition(), 20.0f }, { BeatPosition::fromBeats (8.0), 20000.0f } });
            song.addAutomation (volume, { { BeatPosition(), top }, { BeatPosition::fromBeats (8.0), bottom } });
            EnginePlayback playback (condition);
            const auto secondsFor = [&condition] (double beats) { return beats * 60.0 / condition.getTempo(); };

            // Played to near beat 7.5 and stopped there, or by Stop, which sends the playhead back.
            const auto playToBeat = [&] (bool sendBack)
            {
                song.run ("transport.playStop");
                playback.play (secondsFor (7.5));

                if (sendBack)
                    song.run ("transport.playStop");
                else
                    song.run ("transport.playPause");
            };

            // Sent back to the start: the values stay where the song stopped.
            song.run ("transport.playStop");
            playback.play (secondsFor (7.5));
            const auto stoppedFrequency = getPlayedValue (song, frequency), stoppedVolume = getPlayedValue (song, volume);
            REQUIRE (stoppedFrequency > 5000.0f); // 12.9 kHz at beat 7.5
            song.run ("transport.playStop");
            checkHeldWhileStopped (playback, song, frequency, stoppedFrequency, 0.0);
            CHECK (getPlayedValue (song, volume) == doctest::Approx (stoppedVolume).epsilon (0.01));

            // Played again: the start's values from its first block.
            song.run ("transport.playStop");
            playback.playSamples (playback.getBlockSize());
            CHECK (getPlayedValue (song, frequency) < 25.0f);
            CHECK (getPlayedValue (song, volume) == doctest::Approx (top).epsilon (0.01));
            song.run ("transport.playStop");

            // Stopped where it is, and the playhead put back at the start by hand (a locate, not a return): the start's values,
            // as before.
            playToBeat (false);
            phazite::movePlayhead (song.context, BeatPosition());
            checkHeldWhileStopped (playback, song, frequency, 20.0f, 0.0);
            CHECK (getPlayedValue (song, volume) == doctest::Approx (top).epsilon (0.01));

            // Stopped and sent back, then the playhead put at beat 4: beat 4's values (632 Hz, halfway along its travel).
            playToBeat (true);
            playback.play (0.1);
            phazite::movePlayhead (song.context, BeatPosition::fromBeats (4.0));
            checkHeldWhileStopped (playback, song, frequency, 632.456f, 4.0);
            CHECK (getPlayedValue (song, volume) == doctest::Approx ((top + bottom) / 2.0f).epsilon (0.01));
        });
}

TEST_CASE ("Stop going back to where playback started leaves a send's lane and a chain's fader where the song stopped")
{
    // As a device's values and a track's fader (above): a send's level and a container's chain's volume, each automated from
    // the top of its fader at beat 0 to its bottom at beat 8, stay where the song stopped near beat 7.5, and go to the
    // start's as the playhead is put there.
    using phazite::BeatPosition;
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0);
    const auto container = phazite::addContainer (song.context, sent.track);
    REQUIRE (container.isValid());
    const phazite::ParameterRef chainVolume { phazite::getChains (song.context, container).front().id,
                                              phazite::schema::chainVolumeParameter };
    const auto top = phazite::decibelsToFaderPosition (0.0f), bottom = phazite::decibelsToFaderPosition (-24.0f);

    for (const auto& lane : { sent.level, chainVolume })
        song.addAutomation (lane, { { BeatPosition(), top }, { BeatPosition::fromBeats (8.0), bottom } });

    EnginePlayback playback;
    song.run ("transport.playStop");
    playback.play (3.75); // to beat 7.5
    const auto stoppedLevel = getPlayedValue (song, sent.level), stoppedChain = getPlayedValue (song, chainVolume);
    REQUIRE (stoppedLevel < top - 0.5f * (top - bottom));
    song.run ("transport.playStop");
    checkHeldWhileStopped (playback, song, sent.level, stoppedLevel, 0.0);
    CHECK (getPlayedValue (song, chainVolume) == doctest::Approx (stoppedChain).epsilon (0.01));

    phazite::movePlayhead (song.context, BeatPosition());
    checkHeldWhileStopped (playback, song, sent.level, top, 0.0);
    CHECK (getPlayedValue (song, chainVolume) == doctest::Approx (top).epsilon (0.01));
}

TEST_CASE ("Playing past the song's end goes back to where playback started, leaving what plays where the song stopped")
{
    // The song's end stops it and sends it back as Stop does: the same.
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 4.0); // a bar: the song ends at beat 4
    const auto device =
        phazite::test::addTestDevice (*phazite::findSong (song.context), song.firstTrack, phazite::test::TestFrequency::typeID);
    const phazite::ParameterRef frequency { device, "frequency" };
    song.addAutomation (frequency, { { phazite::BeatPosition(), 20.0f }, { phazite::BeatPosition::fromBeats (8.0), 20000.0f } });
    song.setSwitch ("transport.loop", false);
    EnginePlayback playback;

    song.run ("transport.playStop");
    playback.play (1.9);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 100); // the session's timer sees where the song is
    REQUIRE (phazite::isPlaying (song.context));
    playback.play (0.2);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! phazite::isPlaying (song.context); }));
    playback.play (0.05);
    const auto stopped = getPlayedValue (song, frequency);
    CHECK (stopped > 632.0f); // past beat 4, halfway along its line
    checkHeldWhileStopped (playback, song, frequency, stopped, 0.0);
}

TEST_CASE ("A reverb's tail through a filter that closed before Stop rings on as it was after the playhead goes back")
{
    // The owner's report itself: a tone into Phazite's reverb and on into its filter, whose cutoff's lane closes from 20 kHz
    // at the start to 200 Hz at beat 4. Played to beat 4 and stopped where it is, and played to beat 4 again and stopped by
    // Stop, which sends the playhead back to the start, where the lane is open: the two tails are the same, sample for sample
    // (the filter opened again, a loud blip).
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    song.addEffect (track, phazite::devices::Reverb::typeID);
    const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
    song.addAutomation ({ filter, "cutoff" },
                        { { phazite::BeatPosition(), 20000.0f }, { phazite::BeatPosition::fromBeats (4.0), 200.0f } });
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    EnginePlayback::waitForFiles();

    const auto tailAfter = [&] (void (*stop) (phazite::ProjectContext&))
    {
        phazite::movePlayhead (song.context, phazite::BeatPosition());
        playback.play (0.1); // stopped a while: each stop's blocks fall on the same control points
        song.run ("transport.playStop");
        playback.play (2.0); // to beat 4
        stop (song.context);
        return playback.play (1.0);
    };

    const auto paused = tailAfter ([] (phazite::ProjectContext& context) { phazite::stopPlaying (*phazite::findSong (context)); });
    const auto sentBack = tailAfter ([] (phazite::ProjectContext& context) { phazite::stopAndReturn (*phazite::findSong (context)); });
    REQUIRE (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (0.0));
    CHECK (paused.getMagnitude (0, 0, 4800) > 1.0e-4f);                                   // the tail rings
    CHECK (phazite::test::getDifference (sentBack, paused, 48000.0).peakDb <= -100.0f);   // none
    CHECK (sentBack.getMagnitude (0, 24000, 24000) <= paused.getMagnitude (0, 0, 24000)); // no rise
}

TEST_CASE ("Play, and a jump while the song plays, reset every device with little that is counted as cleared")
{
    // The audit of 2026-11-20: a start and a jump reset every device of every strip in the block that starts or lands
    // (Engine::flushStrip()), so a reset must cost little. The EQ's linear phase forgets its history and its convolution's
    // partitions by a count of what came in since and writes none of them (0.3 MiB before): what the devices' resets clear
    // through the count (devices/dsp/ClearedSamples.h: the linear phase's convolution, and whatever large comes to be
    // cleared so) stays small in that block. The count doesn't see a plain loop: a delay line and a comb are cleared
    // whole, as measured cheap (docs/realtime-audit.md, finding 25).
    // A synth's chord rather than an audio clip, so that a jump lands at once rather than when the clip's file is read there.
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::Synth::typeID);
    song.addChord (song.addMidiClip (track, 0.0, 16.0), { 60, 64, 67 }, 0.0, 16.0);

    for (int i = 0; i < 3; ++i)
    {
        song.addEffect (track, phazite::devices::Delay::typeID);
        const auto reverb = song.addEffect (track, phazite::devices::Reverb::typeID);
        song.setParameter (reverb, "mode", (float) i);
    }

    const auto linear = song.addEffect (track, phazite::devices::Equalizer::typeID);
    song.setParameter (linear, "phase", 1.0f);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);

    // Played a second, so that every line holds sound, then stopped a while.
    song.run ("transport.playStop");
    REQUIRE (playback.play (1.0).getMagnitude (0, 0, (int) EnginePlayback::sampleRate) > 0.01f);
    song.run ("transport.playStop");
    playback.play (0.25);

    const auto clearedIn = [&playback] (const std::function<void()>& asked)
    {
        asked();
        const auto before = phazite::devices::dsp::ClearedSamples::get();
        playback.playSamples (2 * EnginePlayback::blockSize); // a jump fades out before it lands
        return phazite::devices::dsp::ClearedSamples::get() - before;
    };

    // The linear phase's partitions in and out (2,048 values at 48 kHz) are what a reset still clears.
    CHECK (clearedIn ([&song] { song.run ("transport.playStop"); }) < 16384);
    playback.play (0.5);
    CHECK (clearedIn ([&song] { phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (0.5)); }) < 16384);
    CHECK (phazite::isPlaying (song.context));
}

TEST_CASE ("A send that follows its lane and an LFO plays the same in blocks of 441 samples as in blocks of 512")
{
    // The ramp from one control point to the next goes on where a block's end cuts it, from where it reached, so that the send
    // plays the same in any blocks, as a fader does (and the runner's mix: DeviceRunner::mixWithDry()).
    std::vector<juce::AudioBuffer<float>> played;

    for (const auto blockSize : { 441, 512 })
    {
        CAPTURE (blockSize);
        TestSong song;
        const auto sent = addSendToReturn (song, 120.0);
        addSendRamp (song, sent.level);
        const auto lfo = song.addModulator (sent.track, phazite::ModulatorType::lfo);
        song.setModulatorParameter (lfo, "syncType", 1.0f); // with the transport, so that the half second stopped first counts
        song.setModulatorParameter (lfo, "rateType", 8.0f); // a cycle a quarter note
        song.modulate (lfo, sent.level, 0.1f);
        EnginePlayback playback (phazite::test::PlaybackCondition { "44.1 kHz", 44100, blockSize, 120 });
        song.run ("transport.playStop");
        played.push_back (playback.play (5.0));
        song.run ("transport.playStop");
    }

    REQUIRE (played[0].getNumSamples() == played[1].getNumSamples());
    auto largest = 0.0f;

    for (int i = 0; i < played[0].getNumSamples(); ++i)
        largest = std::max (largest, std::abs (played[0].getSample (0, i) - played[1].getSample (0, i)));

    CHECK (played[0].getMagnitude (0, 0, played[0].getNumSamples()) > 0.1f);
    CHECK (largest < 1.0e-5f);

    // And what they play is the lane and the LFO, not the level set by hand (known bug 192): nearly silent at beat 0.5, where
    // the lane holds silence and the LFO moves it a tenth of the travel at most, and swinging after beat 9, where the lane is
    // at 0 dB, by about 5 dB over a cycle.
    for (const auto& audio : played)
    {
        CHECK (std::abs (audio.getSample (0, 11025)) < 0.02f * heldTone());
        const auto range = audio.findMinMax (0, 4 * 44100 + 22050, 22050); // from beat 9, a quarter note
        CHECK (range.getStart() > 0.0f);
        CHECK (range.getEnd() > 1.5f * range.getStart());
    }
}

TEST_CASE ("A send leaving its lane for the level set by hand eases there over 15 ms, as a fader does, not in a short block")
{
    // The review of known bug 192: a lane at -30 dB deleted under a send set to 0 dB while the song plays went up 30 dB within
    // a block, 1.3 ms at 64 samples, a click on whatever the return plays. A send that has followed nothing stays as it was.
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0);
    const auto low = phazite::decibelsToFaderPosition (-30.0f);
    song.addAutomation (sent.level, { { phazite::BeatPosition(), low }, { phazite::BeatPosition::fromBeats (32.0), low } });
    EnginePlayback playback (phazite::test::PlaybackCondition { "48 kHz in blocks of 64", 48000, 64, 120 });
    song.run ("transport.playStop");
    const auto before = playback.play (0.5);
    CHECK (before.getSample (0, before.getNumSamples() - 1) == doctest::Approx (heldTone() * sendGainAt (low)).epsilon (0.01));

    phazite::deleteAutomation (phazite::getParameter (song.context, sent.level));
    const auto after = playback.play (0.5);
    song.run ("transport.playStop");
    CHECK (after.getSample (0, after.getNumSamples() - 1) == doctest::Approx (heldTone()).epsilon (0.001));

    // The largest step from one sample to the next, as a part of the way from -30 dB to 0 dB: over 15 ms (720 samples), about
    // 1/720 of it; over a block, 1/64.
    auto largest = std::abs (after.getSample (0, 0) - before.getSample (0, before.getNumSamples() - 1));

    for (int i = 1; i < after.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (after.getSample (0, i) - after.getSample (0, i - 1)));

    const auto way = heldTone() * (1.0f - sendGainAt (low));
    INFO ("the largest step is " << largest / way << " of the way");
    CHECK (largest < way / 600.0f);
}

TEST_CASE ("Phazite's engine plays a send's automation, which its knob shows, the lane's value at the playhead put there while stopped, "
           "and Touch writes it")
{
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0);
    const auto top = phazite::decibelsToFaderPosition (0.0f), bottom = phazite::decibelsToFaderPosition (-24.0f);
    song.addAutomation (sent.level, { { phazite::BeatPosition(), top }, { phazite::BeatPosition::fromBeats (4.0), bottom } });
    EnginePlayback playback;
    auto knob = phazite::getSendLevel (song.context, sent.level.device);
    const auto lastSample = [] (const juce::AudioBuffer<float>& audio) { return audio.getSample (0, audio.getNumSamples() - 1); };

    // Half way along the line at beat 2: the knob there, and the return playing it.
    song.run ("transport.playStop");
    const auto halfway = playback.play (1.0);
    CHECK (knob.getValue() == doctest::Approx ((top + bottom) / 2.0f).epsilon (0.01));
    CHECK (lastSample (halfway) == doctest::Approx (heldTone() * sendGainAt ((top + bottom) / 2.0f)).epsilon (0.01));

    // Stopped and sent back to the start: where the song stopped; the playhead put there by hand: the lane's value there.
    song.run ("transport.playStop");
    playback.play (0.05);
    CHECK (knob.getValue() == doctest::Approx ((top + bottom) / 2.0f).epsilon (0.01));
    phazite::movePlayhead (song.context, phazite::BeatPosition());
    playback.play (0.05);
    CHECK (knob.getValue() == doctest::Approx (top).epsilon (0.001));

    // In Touch, held, it plays as set by hand, and the lane plays again once it is let go.
    phazite::setAutomationMode (song.context, sent.track, phazite::AutomationMode::touch);
    song.run ("transport.playStop");
    playback.play (0.25); // to beat 0.5
    knob.beginGesture();
    knob.setValue (0.3f);
    const auto held = playback.play (0.25);
    CHECK (knob.getValue() == doctest::Approx (0.3f));
    CHECK (lastSample (held) == doctest::Approx (heldTone() * sendGainAt (0.3f)).epsilon (0.01));
    knob.endGesture();
    playback.play (0.5); // to beat 2
    CHECK (knob.getValue() == doctest::Approx ((top + bottom) / 2.0f).epsilon (0.02));
    song.run ("transport.playStop");

    // What it wrote, as one undo step.
    const auto points = phazite::getAutomationPoints (song.context, sent.level);
    CHECK (points.size() > 2);
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.position - 0.3f) < 0.001f; }));
    phazite::undo (*phazite::findSong (song.context));
    CHECK (phazite::getAutomationPoints (song.context, sent.level).size() == 2);
}

TEST_CASE ("A track's macro turns its send down, which the return plays and the knob shows, and an LFO swings it")
{
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0);
    const auto top = phazite::decibelsToFaderPosition (0.0f);
    const auto macro = song.addModulator (sent.track, phazite::ModulatorType::macro);
    song.modulate (macro, sent.level, -0.5f);
    song.setMacroValue (macro, 1.0f);
    const auto audio = renderOwn (song);
    CHECK (audio.getSample (0, 24000) == doctest::Approx (heldTone() * sendGainAt (top - 0.5f)).epsilon (0.002));

    EnginePlayback playback;
    auto knob = phazite::getSendLevel (song.context, sent.level.device);
    song.run ("transport.playStop");
    playback.play (0.25);
    CHECK (knob.getValue() == doctest::Approx (top - 0.5f).epsilon (0.001));
    CHECK (knob.getBaseValue() == doctest::Approx (top).epsilon (0.001));
    song.run ("transport.playStop");

    // An LFO in its place: the return swings around the send's level.
    phazite::removeModulator (song.context, macro);
    const auto lfo = song.addModulator (sent.track, phazite::ModulatorType::lfo);
    song.setModulatorParameter (lfo, "rateType", 0.0f); // in hertz, at 1 Hz
    song.modulate (lfo, sent.level, 0.2f);
    const auto swung = renderOwn (song, 2.0);
    const auto range = swung.findMinMax (0, 0, swung.getNumSamples());
    CHECK (range.getEnd() > heldTone() * sendGainAt (top) * 1.5f);
    CHECK (range.getStart() < heldTone() * sendGainAt (top) * 0.5f);
}

TEST_CASE ("A send's lane stays in time with the sound it sends through a device with latency, and an Undo of its edit plays as before")
{
    // Delay compensation: what a send sends comes into its return as late as the latest path there, so its lane follows the
    // song as late, as a fader's follows its track's. Here the track's sound comes 1000 samples late through Test Latency; the
    // lane jumps from silence to 0 dB at beat 2, which a render plays at beat 2's sample (it plays ahead by the compensation).
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0);
    auto& state = *phazite::findSong (song.context);

    {
        const phazite::UndoStep step (state, "Add Latency");
        const auto latency = phazite::test::addTestDevice (state, sent.track, phazite::test::TestLatency::typeID);
        phazite::setValueSetByHand (state, phazite::ParameterRef { latency, "samples" }, 1000.0f);
    }

    const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };
    const auto top = phazite::decibelsToFaderPosition (0.0f);
    song.addAutomation (sent.level, { { at (0.0), 0.0f }, { at (2.0), 0.0f }, { at (2.0), top } });
    const auto jumped = renderOwn (song, 1.5);
    const auto beat2 = 48000; // at 120 BPM and 48 kHz
    CHECK (std::abs (jumped.getSample (0, beat2 - 64)) < 1.0e-6f);
    CHECK (jumped.getSample (0, beat2 + 64) == doctest::Approx (heldTone()).epsilon (0.002));

    // An edit of the lane and its Undo and Redo, exact in the song and in the plan compiled after each; and undone, the
    // send plays as it did.
    phazite::test::UndoCheck undo (song.context);
    undo.check ("Add Automation Point", [&] { phazite::addAutomationPoint (song.context, sent.level, { at (1.0), top, 0.0f }); });
    CHECK (renderOwn (song, 1.5).getSample (0, beat2 / 2) == doctest::Approx (heldTone()).epsilon (0.01));
    phazite::undo (state);
    CHECK (phazite::test::getDifference (renderOwn (song, 1.5), jumped, 48000.0).peakDb <= -100.0f); // none
}

TEST_CASE ("A device after one with latency is given the song's place of the sound it plays")
{
    // What a device counts from the song's place (an LFO in time with the song, its grid of control steps, a noise that
    // starts with the song) must meet the sound that belongs there, whatever latency stands before it: its place is the
    // song's less how late the sound comes into it, as its automation's is. Test Place plays the place it is given; behind a
    // device 1,000 samples late it plays, in a render, the same as behind none, where it played 1,000 samples ahead.
    const auto rendered = [] (float latencySamples)
    {
        TestSong song;
        auto& state = *phazite::findSong (song.context);
        const auto track = addToneTrack (song, song.firstTrack, "Tone");

        {
            const phazite::UndoStep step (state, "Add Devices");
            const auto latency = phazite::test::addTestDevice (state, track, phazite::test::TestLatency::typeID);
            phazite::setValueSetByHand (state, phazite::ParameterRef { latency, "samples" }, latencySamples);
            phazite::test::addTestDevice (state, track, phazite::test::TestPlace::typeID);
        }

        return renderOwn (song, 1.0);
    };

    const auto direct = rendered (0.0f), late = rendered (1000.0f);
    REQUIRE (direct.getNumSamples() > 40000);
    REQUIRE (std::abs (direct.getSample (0, 40000)) > 1.0e-3f); // it plays its place, which the fader passes on

    for (const auto at : { 100, 4800, 20000, 40000 })
    {
        CAPTURE (at);
        CHECK (late.getSample (0, at) == doctest::Approx (direct.getSample (0, at)).epsilon (1.0e-6));
        CHECK (direct.getSample (0, at + 64) > direct.getSample (0, at)); // and its place moves on with the song
    }
}

TEST_CASE ("A send's lane plays on as its track is frozen, and on the track that Bounce in Place makes")
{
    // A freeze renders the track before its fader: its fader and its sends play on live, and so do their lanes. Bounce in
    // Place copies the send and its lane onto the new track, and mutes the original (its send after the fader with it).
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0, false);
    addSendRamp (song, sent.level);
    const auto before = renderOwn (song, 5.0);
    CHECK (before.getSample (0, 8 * 24000) > before.getSample (0, 2 * 24000) * 1.5f); // the return comes up

    // The largest difference from that after the first tenth of a second (the file's first samples, at another rate than the
    // render's, ring as the tone starts there), as a part of the tone.
    const auto differenceFrom = [&before] (const juce::AudioBuffer<float>& after)
    {
        auto largest = 0.0f;

        for (int i = 4800; i < std::min (before.getNumSamples(), after.getNumSamples()); ++i)
            largest = std::max (largest, std::abs (after.getSample (0, i) - before.getSample (0, i)));

        return largest / heldTone();
    };

    song.context.getSongSelection().selectOnly (sent.track);
    song.run ("track.freeze");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }));
    REQUIRE (phazite::isFrozen (song.context, sent.track));
    CHECK (differenceFrom (renderOwn (song, 5.0)) < 1.0e-3f);

    song.run ("track.freeze"); // unfrozen
    REQUIRE_FALSE (phazite::isFrozen (song.context, sent.track));
    song.renders.finished.clear();
    song.run ("track.bounceInPlace");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }));
    REQUIRE (phazite::getAllPlaylistTracks (song.context).size() == 3); // the return, and the bounce
    CHECK (differenceFrom (renderOwn (song, 5.0)) < 1.0e-3f);
}

TEST_CASE ("Bounce in Place moves a send before the fader, its lane and its modulation to the new track: the return hears the "
           "sound once, and Undo puts the send back")
{
    // Known bug 194: the bounce copied every send onto the new track and muted the original, whose sends before its fader go
    // on while it is muted (Ableton Live's and most DAWs' mute), so the return played the bounced sound twice, 6 dB louder.
    TestSong song;
    const auto sent = addSendToReturn (song, 120.0, true);
    addSendRamp (song, sent.level);
    const auto macro = song.addModulator (sent.track, phazite::ModulatorType::macro);
    song.modulate (macro, sent.level, -0.2f);
    song.setMacroValue (macro, 0.5f);
    const auto before = renderOwn (song, 5.0);
    CHECK (before.getSample (0, 8 * 24000) > 0.1f);

    // The largest difference from that after the first tenth of a second (the file's first samples, at another rate than the
    // render's, ring as the tone starts there), as a part of the tone.
    const auto differenceFrom = [&before] (const juce::AudioBuffer<float>& after)
    {
        auto largest = 0.0f;

        for (int i = 4800; i < std::min (before.getNumSamples(), after.getNumSamples()); ++i)
            largest = std::max (largest, std::abs (after.getSample (0, i) - before.getSample (0, i)));

        return largest / heldTone();
    };

    // Exact in the song, undone and redone (the render's file stays for the redo).
    song.context.getSongSelection().selectOnly (sent.track);
    phazite::test::UndoCheck undo (song.context);
    undo.check ("Bounce in Place",
                [&song]
                {
                    song.run ("track.bounceInPlace");
                    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }));
                });

    phazite::TrackID bounced;

    for (const auto track : phazite::getAllPlaylistTracks (song.context))
        if (track != sent.track && track != sent.returnTrack)
            bounced = track;

    REQUIRE (bounced.isValid());
    CHECK (phazite::getSends (song.context, sent.track).empty());
    const auto moved = phazite::getSends (song.context, bounced);
    REQUIRE (moved.size() == 1);
    const phazite::ParameterRef movedLevel { moved.front().id, phazite::schema::sendLevelParameter };
    CHECK (phazite::getAutomationPoints (song.context, movedLevel).size() == 3);
    CHECK (phazite::getModulations (song.context, movedLevel).size() == 1);
    CHECK (differenceFrom (renderOwn (song, 5.0)) < 1.0e-3f);

    // Undone: the send back on the original, with its lane and its modulation, as it played.
    phazite::undo (*phazite::findSong (song.context));
    CHECK (phazite::getSends (song.context, sent.track).size() == 1);
    CHECK (phazite::getAutomationPoints (song.context, sent.level).size() == 3);
    CHECK (phazite::getModulations (song.context, sent.level).size() == 1);
    CHECK (phazite::test::getDifference (renderOwn (song, 5.0), before, 48000.0).peakDb <= -100.0f); // none
}

TEST_CASE (
    "A muted track's sends before its fader go on and those after it are silent while a soloed out one sends to a soloed return alone")
{
    // Mute silences a track's output only (the owner's decision; docs/design/decisions.md, roadmap S4.8): what listens to it
    // before its fader hears it on. Another's solo silences it all, but a send to a return that is soloed. The tone and the
    // return (a flat EQ) each at the tone's level; in a render, and live as the engine plays the song.
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto other = song.addTrack ("Other");
    const auto reverb = song.addReturn (track, phazite::devices::Equalizer::typeID, 0.0f);
    const auto send = phazite::getSends (song.context, track).front().id;

    // Heard: the tone, and the return in phase with it, 6 dB louder.
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel + 6.02f).epsilon (0.002));

    // Muted, its send after the fader: nothing.
    phazite::toggleMute (song.context, track);
    CHECK (levelOf (renderOwn (song), 0) < -90.0f);

    // Muted, its send before the fader: the return goes on hearing it, the tone itself silent.
    phazite::setPreFader (song.context, send, true);
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel).epsilon (0.002));

    {
        EnginePlayback playback;
        EnginePlayback::waitForFiles();
        song.run ("transport.playStop");
        CHECK (levelOf (playback.play (0.75), 0) == doctest::Approx (toneLevel).epsilon (0.002));
        song.run ("transport.playStop");
    }

    // Heard again, but soloed out by another track: nothing, before the fader or after.
    phazite::toggleMute (song.context, track);
    phazite::toggleSolo (song.context, other);
    CHECK (levelOf (renderOwn (song), 0) < -90.0f);
    phazite::setPreFader (song.context, send, false);
    CHECK (levelOf (renderOwn (song), 0) < -90.0f);

    // But to a return that is soloed it sends, itself unheard.
    phazite::toggleSolo (song.context, other);
    phazite::toggleSolo (song.context, reverb);
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("A track unmuted or no longer soloed out in the middle of a note plays it at once as its instrument played on meanwhile")
{
    // Mute and solo silence a track's output, as in Ableton Live and most DAWs: its instrument and effects play on, so that
    // the note held, a reverb's tail and a delay's echoes are heard at once as it is heard again (known bug 78: a muted track
    // threw its notes away, counted as sounding, and stayed silent until its next note). Test Tone plays the velocity of the
    // note it holds, here one held from the song's start to its end.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            TestSong song;
            song.setTempo (condition.getTempo());
            auto& state = *phazite::findSong (song.context);
            phazite::test::addTestDevice (state, song.firstTrack, phazite::test::TestTone::typeID);
            song.addNote (song.addMidiClip (song.firstTrack, 0.0, 64.0), 60, 0.0, 64.0);
            const auto other = song.addTrack ("Other");
            EnginePlayback playback (condition);

            // The end of a stretch played (after the channel's 5 ms fade), and how loud it is there.
            const auto levelAtEnd = [&playback] (double seconds)
            {
                const auto audio = playback.play (seconds);
                const auto last = (int) (0.05 * playback.getSampleRate());
                return audio.getMagnitude (0, audio.getNumSamples() - last, last);
            };

            const auto held = juce::Decibels::decibelsToGain (phazite::defaultMasterVolume) * 100.0f / 127.0f;
            song.run ("transport.playStop");
            CHECK (levelAtEnd (0.2) == doctest::Approx (held).epsilon (0.001));

            // Muted, and unmuted again: the note it holds is heard again at once.
            phazite::toggleMute (song.context, song.firstTrack);
            CHECK (levelAtEnd (0.2) == 0.0f);
            phazite::toggleMute (song.context, song.firstTrack);
            CHECK (levelAtEnd (0.2) == doctest::Approx (held).epsilon (0.001));

            // Silenced by another track's solo, and heard again as the solo ends.
            phazite::toggleSolo (song.context, other);
            CHECK (levelAtEnd (0.2) == 0.0f);
            phazite::toggleSolo (song.context, other);
            CHECK (levelAtEnd (0.2) == doctest::Approx (held).epsilon (0.001));
            song.run ("transport.playStop");
        });
}

TEST_CASE ("A jump into a long audio file where it isn't read yet is silent only while the reading thread reads it first")
{
    // A file longer than is read whole, from the song's start; a jump 40 s in, far past what is read ahead. Play from a stop
    // and the loop's start are read ahead; a jump can't be (known bug 36): the clip is silent until its file is read there,
    // and fades in then, which the jump asks for at once, ahead of everything else, waking the reading thread
    // (EngineTransportTests.cpp times that with the thread waiting). Here the file was just written, so it is in the system's
    // cache; a cold file adds a disk's seek and read to what the silence lasts (docs/design/engine.md).
    TestSong song;
    song.setTempo (120.0);
    song.addAudioClip (song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Long.wav"), 50.0, 440.0, 0.25f),
                       0.0);
    EnginePlayback playback;
    EnginePlayback::waitForFiles();
    song.run ("transport.playStop");
    REQUIRE (playback.play (0.5).getMagnitude (0, 12000, 12000) > 0.1f);

    // The jump read where it lands before the engine plays there: before, the reading thread learnt of it only as the engine
    // landed, so this waited for ever (here until a hang's limit; read at once, it comes in a fraction of a millisecond).
    const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
    REQUIRE (plan != nullptr);
    const auto file = plan->strips.front().audio.at (0).file;
    const auto landing = (std::int64_t) (40.0 * file->getSampleRate());
    REQUIRE_FALSE (file->isRead (landing));
    phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (80.0));
    CHECK (phazite::test::dispatchMessagesUntil ([&file, landing] { return file->isRead (landing); }));

    // Then it is silent for the jump's fades alone (3 ms out, and its clip's 40 samples in), and while the clip, a 44.1 kHz
    // file in a 48 kHz engine, makes its resampling ready where the jump lands (whole blocks, 20 ms at most: Engine.h,
    // Landing): no stretch of more than that and 400 samples near silence, where the tone crosses zero for a sample or two.
    const auto played = playback.play (0.2);
    auto quiet = 0, longestQuiet = 0;

    for (int i = 0; i < played.getNumSamples(); ++i)
    {
        quiet = std::abs (played.getSample (0, i)) < 1.0e-3f ? quiet + 1 : 0;
        longestQuiet = std::max (longestQuiet, quiet);
    }

    CAPTURE (longestQuiet);
    CHECK (longestQuiet < 400 + (int) (phazite::engine::Engine::maximumHoldSeconds * EnginePlayback::sampleRate));
    song.run ("transport.playStop");
}

TEST_CASE ("A sound file written again in its place by another program plays as it is now from the first plan a second on, made "
           "for another reason; one not written again keeps its track's audio (known bug 198)")
{
    // A track plays a file; another program writes it again in its place, with other samples, then with another length too,
    // each time with a modification time of its own. A plan made for another reason (a note on another track) asks the disk
    // about the file once a second has passed since it last did (trustFilesMilliseconds, model/engine/AudioFiles.h), and plays it as it is
    // then; before, it asks nothing. Phazite's clock held, so that a second doesn't depend on how busy the machine is.
    // Before, the track's audio was kept as first read until its clips changed, and the plan's check asserted that the file
    // differed from the disk.
    phazite::test::HeldClock clock;
    TestSong song;
    song.setTempo (120.0);
    song.addAudioClip (song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Loop.wav"), 2.0, 440.0, 0.25f),
                       0.0);
    phazite::test::waitForAudioWork (song.context);
    const auto copies = song.getProject().getAudioFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (copies.size() == 1);
    const auto played = copies[0]; // the project's copy, which the clip plays
    const auto notes = song.addMidiClip (song.addTrack ("Other"), 0.0, 4.0);
    EnginePlayback playback;

    // The file the engine's plan plays for the track, and the frequency the track plays from the song's start.
    const auto playedFile = [&song]
    {
        const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
        REQUIRE (plan != nullptr);

        for (const auto& strip : plan->strips)
            if (strip.id == (std::int64_t) song.firstTrack.getRaw() && ! strip.audio.empty())
                return strip.audio.front().file;

        FAIL ("the track plays nothing");
        return std::shared_ptr<phazite::engine::AudioFile>();
    };

    const auto frequency = [&song, &playback]
    {
        phazite::movePlayhead (song.context, phazite::BeatPosition());
        EnginePlayback::waitForFiles();
        song.run ("transport.playStop");
        const auto audio = playback.play (0.6);
        song.run ("transport.playStop");
        auto crossings = 0;

        for (auto i = (int) (0.2 * EnginePlayback::sampleRate) + 1; i < (int) (0.6 * EnginePlayback::sampleRate); ++i)
            if ((audio.getSample (0, i - 1) < 0.0f) != (audio.getSample (0, i) < 0.0f))
                ++crossings;

        return crossings / (2.0 * 0.4);
    };

    // A note on the other track, and how many times its plan asked the disk about a file.
    const auto editElsewhere = [&song, notes] (double beat)
    {
        const auto asked = phazite::SongPlayer::countFilesAsked();
        song.addNote (notes, 60, beat, 0.5);
        phazite::test::dispatchPendingMessages();
        return phazite::SongPlayer::countFilesAsked() - asked;
    };

    // The file as first written; a second on, a plan for another reason asks about it once, and keeps the track's audio.
    CHECK (frequency() == doctest::Approx (440.0).epsilon (0.02));
    const auto first = playedFile();
    clock.advance (1000.0);
    CHECK (editElsewhere (0.0) == 1);
    CHECK (playedFile() == first);

    // Written again, as long, so with as many bytes: within the second, a plan asks nothing (and plays it as it was).
    const auto written = played.getLastModificationTime();
    phazite::test::writeSound (played, 2.0, 660.0, 0.25f);
    REQUIRE (played.setLastModificationTime (written + juce::RelativeTime::hours (1.0)));
    CHECK (editElsewhere (0.5) == 0);
    CHECK (playedFile() == first);

    // A second on: asked about, and played as it is now.
    clock.advance (1000.0);
    CHECK (editElsewhere (1.0) == 1);
    CHECK (playedFile() != first);
    CHECK (frequency() == doctest::Approx (660.0).epsilon (0.02));

    // Written again shorter, half as long.
    phazite::test::writeSound (played, 1.0, 330.0, 0.25f);
    REQUIRE (played.setLastModificationTime (written + juce::RelativeTime::hours (2.0)));
    clock.advance (1000.0);
    CHECK (editElsewhere (1.5) == 1);
    REQUIRE (playedFile() != nullptr);
    CHECK (playedFile()->getLength() == 44100);
    CHECK (frequency() == doctest::Approx (330.0).epsilon (0.02));
}

namespace
{
// The path of the file a track's first audio region plays in the engine's plan (what its file was opened as: its identity on
// the disk, model/engine/AudioFiles.h), or nothing while it plays none.
juce::String getPathPlayed (phazite::TrackID track)
{
    const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
    REQUIRE (plan != nullptr);

    for (const auto& strip : plan->strips)
        if (strip.id == (std::int64_t) track.getRaw() && ! strip.audio.empty())
            return strip.audio.front().file->getOrigin().upToFirstOccurrenceOf ("|", false, false);

    return {};
}

// The frequency a song plays from its start, through the engine, counted from its zero crossings between 0.2 and 0.6 s.
double getFrequencyPlayed (TestSong& song, EnginePlayback& playback)
{
    phazite::movePlayhead (song.context, phazite::BeatPosition());
    EnginePlayback::waitForFiles();
    song.run ("transport.playStop");
    const auto audio = playback.play (0.6);
    song.run ("transport.playStop");
    auto crossings = 0;

    for (auto i = (int) (0.2 * EnginePlayback::sampleRate) + 1; i < (int) (0.6 * EnginePlayback::sampleRate); ++i)
        if ((audio.getSample (0, i - 1) < 0.0f) != (audio.getSample (0, i) < 0.0f))
            ++crossings;

    return crossings / (2.0 * 0.4);
}
} // namespace

TEST_CASE ("A clip's source written again in its place has a render of its own made, which plays from the first plan a second on, "
           "made for another reason (known bug 198)")
{
    // A reversed clip plays Phazite's render of its file, named for the file as it is on the disk (getRenderFile()). The file
    // written again in its place, with other samples and a modification time of its own: within the second, a plan plays the
    // render as it was; a plan a second on, made for another reason (a note on another track), finds the file changed and
    // asks for a render under another name, which is made and played; the old one is left in the cache, never asked for
    // again. Before, the render kept its name and was never made again, and the old sound played on. Phazite's clock held.
    phazite::test::HeldClock clock;
    TestSong song;
    song.setTempo (120.0);
    const auto clip = song.addAudioClip (
        song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 2.0, 440.0, 0.25f), 0.0);
    phazite::test::waitForAudioWork (song.context);
    const auto copies = song.getProject().getAudioFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (copies.size() == 1);
    const auto source = copies[0]; // the project's copy, which the clip's render is made from
    const auto notes = song.addMidiClip (song.addTrack ("Other"), 0.0, 4.0);
    EnginePlayback playback;
    const auto renderOf = [&song, clip]
    {
        const auto& played = *phazite::findSong (song.context);
        return phazite::getPlayedFile (played, phazite::findClipState (played, clip));
    };

    // Reversed: its render made, and played (a sine is the same backwards).
    phazite::setClipReversed (song.context, clip, true);
    const auto first = renderOf();
    REQUIRE (first.getFileName().startsWith (phazite::renderFilePrefix));
    song.waitForClipRenders();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getPathPlayed (song.firstTrack) == first.getFullPathName(); }));
    CHECK (getFrequencyPlayed (song, playback) == doctest::Approx (440.0).epsilon (0.02));

    // Written again, as long: within the second, the render is the same, and plays as it was.
    const auto written = source.getLastModificationTime();
    phazite::test::writeSound (source, 2.0, 660.0, 0.25f);
    REQUIRE (source.setLastModificationTime (written + juce::RelativeTime::hours (1.0)));
    song.addNote (notes, 60, 0.0, 0.5);
    phazite::test::dispatchPendingMessages();
    CHECK (renderOf() == first);
    CHECK (getPathPlayed (song.firstTrack) == first.getFullPathName());

    // A second on, a plan for another reason: a render of the file as it is now, made and played.
    clock.advance (1000.0);
    song.addNote (notes, 62, 1.0, 0.5);
    phazite::test::dispatchPendingMessages();
    const auto second = renderOf();
    CHECK (second != first);
    REQUIRE (second.getFileName().startsWith (phazite::renderFilePrefix));
    song.waitForClipRenders();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getPathPlayed (song.firstTrack) == second.getFullPathName(); }));
    CHECK (getFrequencyPlayed (song, playback) == doctest::Approx (660.0).epsilon (0.02));
    CHECK (first.existsAsFile()); // until the cache is purged, as the project closes
}

TEST_CASE ("A clip's source written again in its place has a render of its own made from a plan alone, nothing in the song changed "
           "(known bug 198)")
{
    // The second review of known bug 198: the case before wakes the project's clip renders through the song's change (a
    // note), so they would make the new render even if nothing told them of the file. Here nothing in the song changes: the
    // source is written again, a second passes, and a plan made for no change of the song's (PlaybackSession::update())
    // asks the disk about the source, finds it changed and tells the record's listeners, the clip renders among them
    // (ClipRenders::fileIdentityChanged()), which make the render it now names. Phazite's clock held.
    phazite::test::HeldClock clock;
    TestSong song;
    song.setTempo (120.0);
    const auto clip = song.addAudioClip (
        song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 2.0, 440.0, 0.25f), 0.0);
    phazite::test::waitForAudioWork (song.context);
    const auto copies = song.getProject().getAudioFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (copies.size() == 1);
    const auto source = copies[0];
    EnginePlayback playback;
    const auto renderOf = [&song, clip]
    {
        const auto& played = *phazite::findSong (song.context);
        return phazite::getPlayedFile (played, phazite::findClipState (played, clip));
    };

    phazite::setClipReversed (song.context, clip, true);
    const auto first = renderOf();
    song.waitForClipRenders();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getPathPlayed (song.firstTrack) == first.getFullPathName(); }));

    // Written again; a second on, a plan alone.
    const auto written = source.getLastModificationTime();
    phazite::test::writeSound (source, 2.0, 660.0, 0.25f);
    REQUIRE (source.setLastModificationTime (written + juce::RelativeTime::hours (1.0)));
    clock.advance (1000.0);
    auto* session = phazite::findSession (*phazite::findSong (song.context));
    REQUIRE (session != nullptr);
    session->update();

    const auto second = renderOf();
    REQUIRE (second != first);
    song.waitForClipRenders();
    CHECK (second.existsAsFile());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getPathPlayed (song.firstTrack) == second.getFullPathName(); }));
    CHECK (getFrequencyPlayed (song, playback) == doctest::Approx (660.0).epsilon (0.02));
}

TEST_CASE ("Two tracks that play one file play it alike after a plan finds it written again in its place (known bug 198)")
{
    // Two tracks play one file, the first track's audio kept from plan to plan. The file written again in its place, and the
    // second track's clip changed within the second (its gain): the plan asks the disk about the file for the second track,
    // finds it changed, and makes the first track's audio again too. Before, the first track was kept as it was (its file
    // asked about less than a second before) and played the file as first read, the second as it is now, until another
    // plan. Phazite's clock held.
    phazite::test::HeldClock clock;
    TestSong song;
    song.setTempo (120.0);
    const auto clip = song.addAudioClip (
        song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Loop.wav"), 2.0, 440.0, 0.25f), 0.0);
    phazite::test::waitForAudioWork (song.context);
    const auto copies = song.getProject().getAudioFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (copies.size() == 1);
    const auto played = copies[0];
    const auto second = song.addTrack ("Second");
    const auto copy = song.copyClip (clip, 0.0, false);
    phazite::moveClip (song.context, copy, phazite::BeatPosition::fromBeats (0.0), second);
    phazite::test::dispatchPendingMessages();

    const auto fileOf = [] (phazite::TrackID track)
    {
        const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
        REQUIRE (plan != nullptr);

        for (const auto& strip : plan->strips)
            if (strip.id == (std::int64_t) track.getRaw() && ! strip.audio.empty())
                return strip.audio.front().file;

        return std::shared_ptr<phazite::engine::AudioFile>();
    };

    const auto before = fileOf (song.firstTrack);
    REQUIRE (before != nullptr);
    REQUIRE (fileOf (second) == before);

    const auto written = played.getLastModificationTime();
    phazite::test::writeSound (played, 1.0, 660.0, 0.25f);
    REQUIRE (played.setLastModificationTime (written + juce::RelativeTime::hours (1.0)));
    phazite::setClipGain (song.context, copy, -3.0f);
    phazite::test::dispatchPendingMessages();

    const auto now = fileOf (second);
    REQUIRE (now != nullptr);
    CHECK (now != before);
    CHECK (now->getLength() == 44100);
    CHECK (fileOf (song.firstTrack) == now);

    // The other order: the first track's audio not kept but made afresh, from what was known of the file (the tempo changed),
    // in the plan in which the second track's clip changed and asked about it. The first track made again too.
    phazite::test::writeSound (played, 1.5, 330.0, 0.25f);
    REQUIRE (played.setLastModificationTime (written + juce::RelativeTime::hours (2.0)));
    song.setTempo (100.0);
    phazite::setClipGain (song.context, copy, -6.0f);
    phazite::test::dispatchPendingMessages();

    const auto third = fileOf (second);
    REQUIRE (third != nullptr);
    CHECK (third != now);
    CHECK (third->getLength() == 66150);
    CHECK (fileOf (song.firstTrack) == third);
}

TEST_CASE ("A device asleep on silence plays its modulation into the values it shows without playing a sound")
{
    // An EQ on a track that plays nothing falls asleep a quarter of a second in; an LFO at 1 Hz on its band's gain. What its
    // parameter played read as the value set by hand while it slept (known bug 171: a knob's ring and a graph's handles
    // stood still): the values played are worked out on its sleeping path too, as it would play them.
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 16.0);
    const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    const auto lfo = song.addModulator (song.firstTrack, phazite::ModulatorType::lfo);
    song.setModulatorParameter (lfo, "rateType", 0.0f); // in hertz, at 1 Hz
    const phazite::ParameterRef gain { eq, "band4Gain" };
    song.modulate (lfo, gain, 0.5f);
    const auto handle = phazite::getParameter (song.context, gain);
    EnginePlayback playback;

    song.run ("transport.playStop");
    const auto played = playback.play (0.5); // asleep from a quarter of a second in
    CHECK (played.getMagnitude (0, played.getNumSamples()) == 0.0f);
    auto farthest = 0.0f;

    for (int step = 0; step < 20; ++step)
    {
        playback.play (0.05);
        farthest = std::max (farthest, std::abs (handle.getValue() - handle.getBaseValue()));
    }

    CHECK (farthest > 3.0f); // in decibels: the LFO's swing is half of the gain's range, 48 dB
    song.run ("transport.playStop");
}

TEST_CASE ("An audio clip of a float file plays silence where the file holds what isn't a number while the rest of the song plays on")
{
    // A WAV file in 32-bit float can hold NaN and infinity, which the engine read into the mix as they were: the master's last
    // guard then silenced the whole song wherever the clip played them. A second of a 660 Hz sine at 48 kHz, NaN from 0.4 s
    // and infinities from 0.5 s to 0.6 s, beside another track's tone at 440 Hz.
    TestSong song;
    song.setTempo (120.0);
    const auto file = song.getSoundsFolder().getChildFile ("Not a number.wav");
    juce::AudioBuffer<float> sound (1, 48000);

    for (int i = 0; i < sound.getNumSamples(); ++i)
        sound.setSample (0, i,
                         i >= 19200 && i < 24000   ? std::numeric_limits<float>::quiet_NaN()
                         : i >= 24000 && i < 28800 ? (i % 2 == 0 ? 1.0f : -1.0f) * std::numeric_limits<float>::infinity()
                                                   : 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 660.0 * i / 48000.0));

    file.getParentDirectory().createDirectory();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (32).withSampleFormat (
                        juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (sound, 0, sound.getNumSamples());
    }

    song.addAudioClip (song.firstTrack, file, 0.0);
    addToneTrack (song, song.addTrack ("Tone"), "Tone");
    const auto audio = renderOwn (song, 1.0);

    // Every sample a number; where the file holds none, the other tone alone; and sound throughout.
    auto numbers = true;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            numbers = numbers && std::isfinite (audio.getSample (channel, i));

    CHECK (numbers);
    CHECK (levelOf (audio, 0, 0.41, 0.59) == doctest::Approx (toneLevel).epsilon (0.002));

    for (double from = 0.05; from < 0.95; from += 0.05)
    {
        CAPTURE (from);
        CHECK (levelOf (audio, 0, from, from + 0.05) > toneLevel - 1.0f);
    }
}

TEST_CASE ("A device with latency that joins a playing song fades the mix out and in rather than step the other tracks' delays")
{
    // A tone on each side; Test Delay, 64 samples late, added to the left one's track as the song plays: the right one is
    // then delayed 64 samples to line up with it, by a delay that held nothing. It went silent for 64 samples in a step and
    // came back in another, a click (known bug 167: a step of up to the tone's level, 0.25, at its peak); now what goes out
    // fades out over 3 ms on the plan before and in after it, and the right one's delay, until it holds 64 samples of the
    // past, plays nothing and then fades in. At every condition: a fade's parts fall where the blocks fall.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            TestSong song;
            song.setTempo (condition.getTempo());
            auto& state = *phazite::findSong (song.context);
            const auto left = addToneTrack (song, song.firstTrack, "Left");
            const auto right = addToneTrack (song, song.addTrack ("Right"), "Right");
            song.setPan (left, -1.0f);
            song.setPan (right, 1.0f);
            EnginePlayback playback (condition);
            EnginePlayback::waitForFiles();

            // The new plan near the tone's peak (half a second is a whole number of its cycles, where it crosses zero).
            const auto rate = condition.getSampleRate();
            song.run ("transport.playStop");
            const auto before = playback.playSamples ((int) std::lround (rate * (0.5 + 0.25 / 440.0)));
            phazite::test::addTestDevice (state, left, phazite::test::TestDelay::typeID);
            const auto after = playback.play (0.5);
            song.run ("transport.playStop");

            // On the right side, no sample further from the one before than twice the most the tone moves in one; and the
            // tone heard again after the fades.
            juce::AudioBuffer<float> whole (1, before.getNumSamples() + after.getNumSamples());
            whole.copyFrom (0, 0, before, 1, 0, before.getNumSamples());
            whole.copyFrom (0, before.getNumSamples(), after, 1, 0, after.getNumSamples());
            auto largest = 0.0f;

            for (int i = 1; i < whole.getNumSamples(); ++i)
                largest = std::max (largest, std::abs (whole.getSample (0, i) - whole.getSample (0, i - 1)));

            const auto mostTheToneMoves = (float) (0.25 * juce::MathConstants<double>::twoPi * 440.0 / rate);
            const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
            REQUIRE (plan != nullptr);
            REQUIRE (phazite::engine::Engine::getOutputLatency (*plan) == phazite::test::TestDelay::latency);
            CAPTURE (largest);
            CHECK (largest <= 2.0f * mostTheToneMoves);
            CHECK (after.getMagnitude (1, after.getNumSamples() / 2, after.getNumSamples() / 2) > 0.2f);
        });
}

namespace
{
// A file of one level throughout (a quarter of full scale), mono at 48 kHz in 32-bit floats: what plays of it shows any
// change of level, a dip, a gap or a step, to the sample.
juce::File writeLevel (const juce::File& file, double seconds)
{
    juce::AudioBuffer<float> sound (1, (int) (seconds * 48000.0));
    juce::FloatVectorOperations::fill (sound.getWritePointer (0), 0.25f, sound.getNumSamples());
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (32).withSampleFormat (
                    juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (sound, 0, sound.getNumSamples());
    return file;
}

// A song of two tracks of that level, the first hard left with Test Latency on it (as late as asked), the other hard right,
// which the engine delays to line up with the first: it plays at 0.25 on its side while nothing dips. Played at 48 kHz in
// blocks of 256, the files read as they play.
struct LatencySong
{
    LatencySong()
    {
        song.setTempo (120.0);
        song.addAudioClip (left, writeLevel (song.getSoundsFolder().getChildFile ("Left.wav"), 20.0), 0.0);
        song.addAudioClip (right, writeLevel (song.getSoundsFolder().getChildFile ("Right.wav"), 20.0), 0.0);
        song.setPan (left, -1.0f);
        song.setPan (right, 1.0f);
        latency = phazite::test::addTestDevice (state, left, phazite::test::TestLatency::typeID);
        EnginePlayback::waitForFiles();
    }

    void setLatency (phazite::DeviceID device, int samples) { song.setParameter (device, "samples", (float) samples); }

    // Plays so many seconds, and adds what the right side played to what it played before.
    void play (double seconds)
    {
        const auto audio = playback.play (seconds);
        const auto from = rightSide.getNumSamples();
        rightSide.setSize (1, from + audio.getNumSamples(), true);
        rightSide.copyFrom (0, from, audio, 1, 0, audio.getNumSamples());
    }

    // From some seconds in, what the right side played: how many samples it dipped below its level, in how many dips, the
    // largest step from one sample to the next, and the longest it was silent for.
    struct Dips
    {
        int samples = 0, count = 0, longestSilence = 0;
        float largestStep = 0.0f;
    };

    Dips findDips (double fromSeconds) const
    {
        Dips dips;
        auto silence = 0;
        auto inDip = false;

        for (int i = (int) (fromSeconds * EnginePlayback::sampleRate); i < rightSide.getNumSamples(); ++i)
        {
            const auto sample = rightSide.getSample (0, i);
            const auto dipped = sample < 0.25f - 1.0e-4f;
            dips.samples += dipped ? 1 : 0;
            dips.count += dipped && ! inDip ? 1 : 0;
            inDip = dipped;
            silence = std::abs (sample) < 1.0e-6f ? silence + 1 : 0;
            dips.longestSilence = std::max (dips.longestSilence, silence);

            if (i > 0)
                dips.largestStep = std::max (dips.largestStep, std::abs (sample - rightSide.getSample (0, i - 1)));
        }

        return dips;
    }

    TestSong song;
    phazite::Song& state = *phazite::findSong (song.context);
    const phazite::TrackID left = song.firstTrack, right = song.addTrack ("Right");
    phazite::DeviceID latency;
    EnginePlayback playback;
    juce::AudioBuffer<float> rightSide { 1, 0 };
};

// A fade's samples at the tests' rate (engine::Engine::fadeSeconds), and more than the largest step two make at once (the
// mix's and a path's own) at a quarter of full scale: a click is tens of times larger; and an audio clip's own fade in after
// a jump, which is shorter (40 samples: engine/audio/AudioRegion.h).
const int fadeSamples = (int) std::lround (phazite::engine::Engine::fadeSeconds * EnginePlayback::sampleRate);
constexpr float fadeStep = 2.0f * 0.25f / 100.0f, clipFadeStep = 1.01f * 0.25f / 40.0f;
} // namespace

TEST_CASE (
    "A delay compensation that changes while the song plays dips the mix for two fades and silences a path only for what it can't hold")
{
    // Known bug 167, as the review measured it: a path made a sample longer silenced the whole mix for as long as its delay
    // (513 samples and the fades), and one made 8192 long for 8192; now each path's delay plays on from what came into it
    // (its ring, kept from plan to plan: engine/nodes/Strip.h), so the mix dips for the two fades alone, and a path is
    // silent only for what its ring can't hold of the past yet.
    SUBCASE ("A sample longer: the fades alone")
    {
        LatencySong s;
        s.setLatency (s.latency, 512);
        s.song.run ("transport.playStop");
        s.play (1.0);
        s.setLatency (s.latency, 513);
        s.play (1.0);
        const auto dips = s.findDips (0.5);
        MESSAGE ("512 to 513 samples: the right side dips for " << dips.samples << " samples, silent for " << dips.longestSilence);
        CHECK (dips.count == 1);
        CHECK (dips.samples <= 2 * fadeSamples + 2);
        CHECK (dips.largestStep < fadeStep);
    }

    SUBCASE ("512 to 8192 samples: within its ring, which holds its past")
    {
        LatencySong s;
        s.setLatency (s.latency, 512);
        s.song.run ("transport.playStop");
        s.play (1.0);
        s.setLatency (s.latency, 8192);
        s.play (1.0);
        const auto dips = s.findDips (0.5);
        MESSAGE ("512 to 8192 samples: the right side dips for " << dips.samples << " samples, silent for " << dips.longestSilence);
        CHECK (dips.count == 1);
        CHECK (dips.longestSilence == 0);
        CHECK (dips.largestStep < fadeStep);
    }

    SUBCASE ("Ten plug-ins joining one after another and ten at once")
    {
        LatencySong s;
        s.song.run ("transport.playStop");
        s.play (0.5);

        for (int i = 1; i <= 10; ++i)
        {
            s.setLatency (s.latency, 64 * i);
            s.play (0.1);
        }

        auto dips = s.findDips (0.25);
        MESSAGE ("ten joining a tenth of a second apart: " << dips.count << " dips, " << dips.samples << " samples in all");
        CHECK (dips.count <= 10);
        CHECK (dips.samples <= 10 * (2 * fadeSamples + 2) + 64);
        CHECK (dips.largestStep < fadeStep);

        // Ten at once, as a project's plug-ins come in as it loads: one plan, one dip.
        const auto from = s.rightSide.getNumSamples();
        {
            const phazite::UndoStep step (s.state, "Add");

            for (int i = 0; i < 10; ++i)
            {
                const auto device = phazite::test::addTestDevice (s.state, s.left, phazite::test::TestLatency::typeID);
                phazite::setValueSetByHand (s.state, phazite::ParameterRef { device, "samples" }, (float) (10 + i));
            }
        }

        s.play (0.5);
        dips = s.findDips (from / EnginePlayback::sampleRate);
        MESSAGE ("ten joining at once: " << dips.count << " dips, " << dips.samples << " samples in all");
        CHECK (dips.count == 1);
        CHECK (dips.largestStep < fadeStep);
    }
}

TEST_CASE ("Plans that change the delay compensation one after another while it fills or fades and a stop or jump meanwhile never click")
{
    // The review's case for known bug 167: a path made 4096 samples longer fills; 100 samples on, another change. A silence
    // measured from the half-filled plan let the path cut in mid-sound once it filled. Now its delay knows what it holds,
    // whatever plans come; and a jump or a stop during the fade fades as they do.
    for (const juce::String then : { "another change", "a jump", "a stop" })
    {
        CAPTURE (then);
        LatencySong s;
        s.song.run ("transport.playStop");
        s.play (0.5);
        s.setLatency (s.latency, 4096);
        s.play (100.0 / EnginePlayback::sampleRate); // in the fade out
        s.setLatency (s.latency, 4160);
        s.play (400.0 / EnginePlayback::sampleRate); // in the fill
        s.setLatency (s.latency, 4200);

        if (then == "a jump")
            phazite::movePlayhead (s.song.context, phazite::BeatPosition::fromBeats (2.0));
        else if (then == "a stop")
            s.song.run ("transport.playStop");

        s.play (0.5);
        const auto dips = s.findDips (0.25);
        CAPTURE (dips.largestStep);
        CHECK (dips.largestStep < (then == "a jump" ? clipFadeStep : fadeStep));
    }
}

TEST_CASE ("A send added while the song plays fades in once its delay holds the past rather than cut in")
{
    // Known bug 167's new path (the review's 6): a send that needs a delay to line up with the return's others made it
    // empty, which cut in mid-sound once it had played its length. Left: the sources' own sound; right: the return (a flat
    // EQ), which hears the first, 64 samples late (Test Latency), and then the second too, delayed to line up with it, each
    // sent before its fader (before its pan to the left).
    LatencySong s;
    s.setLatency (s.latency, 64);
    const auto second = s.song.addTrack ("Second");
    s.song.addAudioClip (second, writeLevel (s.song.getSoundsFolder().getChildFile ("Second.wav"), 20.0), 0.0);
    s.song.setPan (second, -1.0f);
    s.song.setVolume (s.right, -100.0f);
    const auto reverb = s.song.addReturn (s.left, phazite::devices::Equalizer::typeID, 0.0f);
    phazite::setPreFader (s.song.context, phazite::getSends (s.song.context, s.left).front().id, true);
    s.song.setPan (reverb, 1.0f);
    EnginePlayback::waitForFiles();
    s.song.run ("transport.playStop");
    s.play (0.5);
    s.song.addSend (second, reverb, 0.0f);
    phazite::setPreFader (s.song.context, phazite::getSends (s.song.context, second).front().id, true);
    s.play (0.5);

    auto largest = 0.0f;

    for (int i = (int) (0.25 * EnginePlayback::sampleRate); i < s.rightSide.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (s.rightSide.getSample (0, i) - s.rightSide.getSample (0, i - 1)));

    CAPTURE (largest);
    CHECK (largest < 2.0f * fadeStep);
    CHECK (s.rightSide.getSample (0, s.rightSide.getNumSamples() - 1) > 0.3f); // both sent, the second faded in
}

TEST_CASE ("The first latency on a sum and its bypass back and forth never silence the other tracks while the song plays")
{
    // The review of known bug 167 (A): a path that needed no delay had no ring, so the first latency on a sum gave every
    // other path into it a ring that held nothing, silent for the whole latency (8192 samples at 48 kHz: 170 ms), each time
    // the only device with latency went back to none and came again. Every path keeps a ring now, delayed or not.
    LatencySong s;
    s.song.run ("transport.playStop");
    s.play (0.5);

    for (const auto samples : { 8192, 0, 8192 })
    {
        s.setLatency (s.latency, samples);
        s.play (0.5);
    }

    const auto dips = s.findDips (0.25);
    MESSAGE ("0 to 8192 samples, back and again: " << dips.count << " dips, " << dips.samples << " samples in all, silent for "
                                                   << dips.longestSilence);
    CHECK (dips.longestSilence == 0);
    CHECK (dips.count == 3);
    CHECK (dips.samples <= 3 * (2 * fadeSamples + 2));
    CHECK (dips.largestStep < fadeStep);
}

namespace
{
// A tone (440 Hz) on a track of its own, sent to a return hard right (a flat EQ) before its fader; a silent
// track with Test Latency sent to the return too, which lines the tone's send up with it: the return plays the tone on the
// right.
struct SentTone
{
    explicit SentTone (bool beforeFader = true)
    {
        song.setTempo (120.0);
        tone = addToneTrack (song, song.firstTrack, "Tone");

        reverb = song.addReturn (tone, phazite::devices::Equalizer::typeID, 0.0f);
        phazite::setPreFader (song.context, phazite::getSends (song.context, tone).front().id, beforeFader);
        song.setPan (reverb, 1.0f);
        late = song.addTrack ("Late");
        latency = phazite::test::addTestDevice (state, late, phazite::test::TestLatency::typeID);
        song.addSend (late, reverb, 0.0f);
        EnginePlayback::waitForFiles();
    }

    // Plays half a second, the latency changed to so many samples, and half a second more: the largest step on the right
    // from one sample to the next, which twice the most the tone moves in one is more than.
    float playThroughAChange (int samples)
    {
        song.run ("transport.playStop");
        const auto played = playback.playSamples ((int) std::lround (EnginePlayback::sampleRate * (0.5 + 0.25 / 440.0)));
        song.setParameter (latency, "samples", (float) samples);
        const auto after = playback.play (0.5);
        song.run ("transport.playStop");
        auto largest = 0.0f;
        auto last = played.getSample (1, (int) (0.25 * EnginePlayback::sampleRate));

        for (const auto* audio : { &played, &after })
            for (int i = (audio == &played ? (int) (0.25 * EnginePlayback::sampleRate) + 1 : 0); i < audio->getNumSamples(); ++i)
            {
                largest = std::max (largest, std::abs (audio->getSample (1, i) - last));
                last = audio->getSample (1, i);
            }

        return largest;
    }

    TestSong song;
    phazite::Song& state = *phazite::findSong (song.context);
    phazite::TrackID tone, reverb, late;
    phazite::DeviceID latency;
    EnginePlayback playback;
    static constexpr float mostTheToneMoves = (float) (0.25 * juce::MathConstants<double>::twoPi * 440.0 / 48000.0);
};
} // namespace

TEST_CASE ("A path that changes under a channel that isn't heard but goes on toward what is dips the mix rather than jump")
{
    // The review of known bug 167 (D): a changed path dipped the mix only when the channel its sound came from was heard,
    // and was judged as the plan was compiled; but a muted track's send before its fader, a soloed-out track's send to a
    // soloed return and a muted key that a heard device listens to are heard, and their read points jumped, repeating or
    // skipping a few milliseconds of the tone. Now every change dips the mix (a 6 ms dip for one nobody hears is the lesser
    // fault, and nothing can change between the compile and the audio thread taking the plan).
    SUBCASE ("A muted track's send before its fader")
    {
        SentTone s;
        phazite::toggleMute (s.song.context, s.tone);
        const auto largest = s.playThroughAChange (64);
        CAPTURE (largest);
        CHECK (largest < 2.0f * SentTone::mostTheToneMoves);
    }

    SUBCASE ("A soloed-out track's send to a soloed return")
    {
        SentTone s (false);
        phazite::toggleSolo (s.song.context, s.reverb);
        const auto largest = s.playThroughAChange (64);
        CAPTURE (largest);
        CHECK (largest < 2.0f * SentTone::mostTheToneMoves);
    }

    SUBCASE ("A muted key that a heard device listens to")
    {
        // The heard track, hard right: Test Latency, then Test Sidechain, which plays its key; the key the tone, muted, which
        // its sidechain hears as late as the heard track's latency lines it up.
        SentTone s;
        const auto heard = s.song.addTrack ("Heard");
        s.song.setPan (heard, 1.0f);
        s.song.setVolume (s.reverb, -100.0f);
        const auto late = phazite::test::addTestDevice (s.state, heard, phazite::test::TestLatency::typeID);
        s.latency = late;
        const auto listener = phazite::test::addTestDevice (s.state, heard, phazite::test::TestSidechain::typeID);
        phazite::setSidechainSource (s.song.context, listener, s.tone);
        phazite::toggleMute (s.song.context, s.tone);
        s.song.setParameter (late, "samples", 32.0f);
        const auto largest = s.playThroughAChange (96);
        CAPTURE (largest);
        CHECK (largest < 2.0f * SentTone::mostTheToneMoves);
    }
}

TEST_CASE ("A track moved into a bus while the song plays fades out and in rather than cut in")
{
    // The review of known bug 167 (E, finding 6's other case): a track that joins a bus gets a new path into it, lined up
    // with the bus's other tracks (one 64 samples late), and its path into the master goes. Its sound left its old path at
    // once, and its new one played nothing until it held its delay: now a path that goes dips the mix, and the new one
    // fades in once it holds the past.
    LatencySong s;
    s.setLatency (s.latency, 64);
    const auto bus = s.song.addBus ("Bus", { s.left });
    s.song.setPan (bus, 0.0f);
    s.song.run ("transport.playStop");
    s.play (0.5);
    {
        const phazite::UndoStep step (s.state, "Move");
        phazite::moveTrackIntoFolder (s.state, s.right, bus);
    }
    s.play (0.5);
    const auto dips = s.findDips (0.25);
    CAPTURE (dips.largestStep);
    CHECK (dips.largestStep < fadeStep);
}

namespace
{
// Everyday edits while the loop plays, each against what the song plays without it (the review of known bug 167, round 4):
// a track playing steadily throughout (a 440 Hz tone, hard right, whose every sample differs from the last, so that a
// sample played from the wrong place in a ring can't pass for the right one), and the edited track (a level, hard left)
// with a send to a return (a flat EQ, hard left), a second return it doesn't send to yet, and a bus (hard left).
enum class EveryDayEdit
{
    deleteTrack,
    removeSend,
    switchSend,
    moveIntoBus,
    addSend,
    addLatency
};

struct EditedSong
{
    explicit EditedSong (const phazite::test::PlaybackCondition& condition)
    {
        song.setTempo (condition.getTempo());
        addToneTrack (song, steady, "Steady");
        song.setPan (steady, 1.0f);
        phazite::test::addTestDevice (state, edited, phazite::test::TestTone::typeID);
        song.addNote (song.addMidiClip (edited, 0.0, 64.0), 62, 0.0, 64.0, 80);
        song.setPan (edited, -1.0f);
        reverb = phazite::addReturnTrack (state, "Reverb", nullptr);
        other = phazite::addReturnTrack (state, "Other", nullptr);
        send = phazite::addSend (state, edited, reverb, false);
        REQUIRE (send.isValid());
        phazite::setSendLevel (state, send, 0.0f);
        song.setPan (reverb, -1.0f);
        song.setPan (other, -1.0f);
        const auto member = song.addTrack ("Member");
        bus = song.addBus ("Bus", { member });
        song.setPan (bus, -1.0f);
        EnginePlayback::waitForFiles();
    }

    void make (EveryDayEdit edit)
    {
        const phazite::UndoStep step (state, "Edit");

        switch (edit)
        {
            case EveryDayEdit::deleteTrack:
                phazite::deleteTracks (state, { edited }, nullptr);
                break;
            case EveryDayEdit::removeSend:
                phazite::removeSend (state, send);
                break;
            case EveryDayEdit::switchSend:
                phazite::setPreFader (state, send, true);
                break;
            case EveryDayEdit::moveIntoBus:
                phazite::moveTrackIntoFolder (state, edited, bus);
                break;
            case EveryDayEdit::addSend:
                phazite::setSendLevel (state, phazite::addSend (state, edited, other, false), 0.0f);
                break;
            case EveryDayEdit::addLatency:
                phazite::setValueSetByHand (
                    state,
                    phazite::ParameterRef { phazite::test::addTestDevice (state, edited, phazite::test::TestLatency::typeID), "samples" },
                    64.0f);
                break;
        }
    }

    // What the engine plays: a while, the edit if one is given, and a while more.
    juce::AudioBuffer<float> play (const phazite::test::PlaybackCondition& condition, std::optional<EveryDayEdit> edit)
    {
        EnginePlayback playback (condition, EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
        song.run ("transport.playStop");
        const auto first = (int) std::lround (0.3 * condition.getSampleRate()) + 37; // not on a block's edge
        const auto before = playback.playSamples (first);

        if (edit.has_value())
            make (*edit);

        const auto after = playback.play (0.3);
        song.run ("transport.playStop");
        juce::AudioBuffer<float> whole (2, before.getNumSamples() + after.getNumSamples());

        for (int channel = 0; channel < 2; ++channel)
        {
            whole.copyFrom (channel, 0, before, channel, 0, before.getNumSamples());
            whole.copyFrom (channel, before.getNumSamples(), after, channel, 0, after.getNumSamples());
        }

        return whole;
    }

    TestSong song;
    phazite::Song& state = *phazite::findSong (song.context);
    const phazite::TrackID steady = song.firstTrack, edited = song.addTrack ("Edited");
    phazite::TrackID reverb, other, bus;
    phazite::DeviceID send;
};

// The largest change from one sample to the next on a side.
float findLargestStep (const juce::AudioBuffer<float>& audio, int channel)
{
    auto largest = 0.0f;

    for (int i = 1; i < audio.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (audio.getSample (channel, i) - audio.getSample (channel, i - 1)));

    return largest;
}
} // namespace

TEST_CASE ("An everyday edit while the song plays leaves the other tracks' sound as it was sample for sample")
{
    // Deleting a track, taking a send away or switching it before or after its fader, moving a track into a bus, adding a
    // send: none touches what the other tracks play (the steady track on the right is as it is without the edit, to the
    // last bit), and the edited track's own sound has no step larger than a 3 ms fade's (its old path fades out alone, its
    // new one fades in). A device with latency added moves the delay compensation: the whole mix dips for the two fades
    // then. At every condition: a fade's parts fall where the blocks fall.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            for (const auto edit : { EveryDayEdit::deleteTrack, EveryDayEdit::removeSend, EveryDayEdit::switchSend,
                                     EveryDayEdit::moveIntoBus, EveryDayEdit::addSend, EveryDayEdit::addLatency })
            {
                CAPTURE ((int) edit);
                EditedSong song (condition);
                const auto without = song.play (condition, std::nullopt);
                const auto with = song.play (condition, edit);
                REQUIRE (with.getNumSamples() == without.getNumSamples());
                auto differences = 0;

                for (int i = 0; i < with.getNumSamples(); ++i)
                    differences += juce::exactlyEqual (with.getSample (1, i), without.getSample (1, i)) ? 0 : 1;

                // The edited track's level, and a fade's step at it (the sends and the bus add up to twice it at most).
                const auto fade = std::lround (phazite::engine::Engine::fadeSeconds * condition.getSampleRate());
                const auto steadyLevel = without.getMagnitude (1, without.getNumSamples() / 2, without.getNumSamples() / 2);
                const auto editedLevel = without.getMagnitude (0, without.getNumSamples() / 2, without.getNumSamples() / 2);
                CAPTURE (differences);
                CAPTURE (findLargestStep (with, 0));

                // (The device added starts with what it delays empty, so the edited track's own sound there is its own.)
                // (The tone dipped steps by no more than it moves and a fade's step at its level.)
                if (edit == EveryDayEdit::addLatency)
                {
                    CHECK (differences > 0); // the mix dips
                    CHECK (findLargestStep (with, 1) <= findLargestStep (without, 1) + steadyLevel / (float) (fade + 1) * 1.01f);
                }
                else
                {
                    CHECK (differences == 0);
                    CHECK (findLargestStep (with, 0) <= 2.0f * editedLevel / (float) (fade + 1) * 1.01f);
                }
            }
        });
}

TEST_CASE ("A track deleted while the song plays stays silent through a delay change that comes during its fade or after")
{
    // The review of known bug 167, round 5: the plan played is kept while a leaving path fades out, and kept longer when a
    // plan that moves the delay compensation comes meanwhile (the mix dips across it). A leaving path whose fade had run out
    // played again at its full level until the new plan took over; a leave that outlasted the dip played the plan kept at
    // full level again; and the new plan, fading in, played again what a path whose delay grew had played (the return's
    // path to the master, 64 samples longer), with the deleted track's send in it as it faded. The deleted track (a level,
    // hard left) and its send only fall from the edit on: the latency changing 1 ms or 2.9 ms after the deletion, or 1 ms
    // before it, at every condition.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            for (const auto after : { 0.001, 0.0029, -0.001 })
            {
                CAPTURE (after);
                EditedSong song (condition);
                const auto latency = phazite::test::addTestDevice (song.state, song.steady, phazite::test::TestLatency::typeID);
                EnginePlayback playback (condition, EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
                song.song.run ("transport.playStop");
                const auto before = playback.playSamples ((int) std::lround (0.3 * condition.getSampleRate()) + 37);
                const auto changeLatency = [&] { song.song.setParameter (latency, "samples", 64.0f); };

                if (after < 0.0)
                    changeLatency();
                else
                    song.make (EveryDayEdit::deleteTrack);

                const auto between = playback.playSamples ((int) std::lround (std::abs (after) * condition.getSampleRate()));

                if (after < 0.0)
                    song.make (EveryDayEdit::deleteTrack);
                else
                    changeLatency();

                const auto rest = playback.play (0.1);
                song.song.run ("transport.playStop");
                REQUIRE (before.getMagnitude (0, before.getNumSamples() - 100, 100) > 0.1f);

                // (Before the deletion, the dip alone: from the deletion on, nothing rises.)
                auto last = std::abs (between.getSample (0, 0));
                auto rises = 0;

                for (const auto* audio : { &between, &rest })
                {
                    for (int i = 0; i < audio->getNumSamples(); ++i)
                    {
                        const auto now = std::abs (audio->getSample (0, i));
                        rises += now > last + 1.0e-5f ? 1 : 0;
                        last = now;
                    }
                }

                CHECK (rises == 0);
                CHECK (rest.getMagnitude (0, rest.getNumSamples() / 2, rest.getNumSamples() / 2) < 1.0e-5f);

                // And the mix comes back: the steady track (hard right) at its level, not held silent.
                const auto window = (int) (0.02 * condition.getSampleRate());
                const auto steadyLevel = before.getMagnitude (1, before.getNumSamples() - window, window);
                REQUIRE (steadyLevel > 0.1f);
                CHECK (rest.getMagnitude (1, rest.getNumSamples() / 2, rest.getNumSamples() / 2) >= 0.99f * steadyLevel);
            }
        });
}

namespace
{
// What happens while the mix is held silent across a leave (below).
enum class DuringTheHold
{
    nothing,
    stop,
    jump,
    secondPlan
};

// The largest change from one sample to the next on a side, from a sample on.
float findLargestStepFrom (const juce::AudioBuffer<float>& audio, int channel, int from)
{
    auto largest = 0.0f;

    for (int i = std::max (1, from); i < audio.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (audio.getSample (channel, i) - audio.getSample (channel, i - 1)));

    return largest;
}
} // namespace

TEST_CASE ("A path that grows by more than a block as another leaves holds the mix silent no longer than the leave, the growth and a fade")
{
    // The review of known bug 167, round 5: the hold across a leave (the mix kept silent until the paths whose delays grew
    // read past the leaving paths' fades) was worked out again in every part while the plan played was kept, from the
    // same growth, so that one larger than a block never ran out: the mix stayed silent and every later edit was ignored
    // until the audio device started again. A track deleted as another's latency becomes 2048 samples (every other path to
    // the master grows by 2048), and a track with 2048 samples of latency moved into a bus (the bus's other track's path
    // grows by 2048): the steady track (a tone, hard right) is back at its level within the leave's fade, the growth and
    // the fade in, and a later edit is heard (a send of it before its fader to a return hard left). The same with a stop,
    // a jump or a second plan (the latency made 3000) in the middle of the hold. At every condition: blocks of 64 to 1024.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            for (const auto moveIntoBus : { false, true })
            {
                for (const auto during : { DuringTheHold::nothing, DuringTheHold::stop, DuringTheHold::jump, DuringTheHold::secondPlan })
                {
                    CAPTURE (moveIntoBus);
                    CAPTURE ((int) during);
                    EditedSong song (condition);
                    const auto rate = condition.getSampleRate();
                    const auto late = song.song.addTrack ("Late");
                    const auto latency =
                        phazite::test::addTestDevice (song.state, moveIntoBus ? song.edited : late, phazite::test::TestLatency::typeID);
                    const auto setLatency = [&] (float samples)
                    { phazite::setValueSetByHand (song.state, phazite::ParameterRef { latency, "samples" }, samples); };

                    if (moveIntoBus)
                        setLatency (2048.0f);

                    EnginePlayback playback (condition, EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
                    song.song.run ("transport.playStop");
                    const auto window = (int) (0.02 * rate);
                    const auto before = playback.playSamples ((int) std::lround (0.3 * rate) + 37);
                    const auto steadyLevel = before.getMagnitude (1, before.getNumSamples() - window, window);
                    REQUIRE (steadyLevel > 0.1f);

                    if (moveIntoBus)
                    {
                        song.make (EveryDayEdit::moveIntoBus);
                    }
                    else
                    {
                        const phazite::UndoStep step (song.state, "Edit");
                        phazite::deleteTracks (song.state, { song.edited }, nullptr);
                        setLatency (2048.0f);
                    }

                    const auto fade = (int) std::lround (phazite::engine::Engine::fadeSeconds * rate);
                    auto growth = 2048;

                    // In the middle of the hold (the leave's fade done, the growth not yet).
                    if (during != DuringTheHold::nothing)
                    {
                        playback.playSamples (fade + 1024);

                        if (during == DuringTheHold::stop)
                        {
                            song.song.run ("transport.playStop");
                            playback.play (0.05);
                            song.song.run ("transport.playStop");
                        }
                        else if (during == DuringTheHold::jump)
                        {
                            phazite::movePlayhead (song.state, phazite::BeatPosition::fromBeats (1.0));
                        }
                        else
                        {
                            setLatency (3000.0f);
                            growth = 3000;
                        }
                    }

                    // Back at its level by then: the leave's fade, the growth and the fade in (after a stop or a jump, the
                    // transport's own fade in).
                    const auto back = 2 * fade + growth;
                    const auto after = playback.playSamples (back + window);
                    CHECK (after.getMagnitude (1, back, window) >= 0.99f * steadyLevel);

                    // And a later edit is taken: the tone sent to the return on the left, before its fader (it fades in).
                    {
                        const phazite::UndoStep step (song.state, "Add Send");
                        const auto send = phazite::addSend (song.state, song.steady, song.other, true);
                        REQUIRE (send.isValid());
                        phazite::setSendLevel (song.state, send, 0.0f);
                    }

                    const auto later = playback.play (0.1);
                    song.song.run ("transport.playStop");
                    const auto from = later.getNumSamples() - window;
                    CHECK (findLargestStepFrom (later, 0, from) >= 0.5f * findLargestStepFrom (later, 1, from));
                    CHECK (later.getMagnitude (1, from, window) >= 0.99f * steadyLevel);
                }
            }
        });
}

TEST_CASE ("Two tracks deleted one after the other while the song plays each fade out from where it was")
{
    // The review of known bug 167, round 5: a path that leaves during another's leave starts its own fade as the engine
    // takes its plan, rather than step to where the other's fade is. The edited track (a level, hard left) deleted, and 1 ms
    // later the steady one (a tone, hard right): neither side steps by more than its sound moves and a fade's step at its
    // level, at every condition.
    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            EditedSong song (condition);
            const auto without = song.play (condition, std::nullopt);
            EnginePlayback playback (condition, EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
            song.song.run ("transport.playStop");
            const auto before = playback.playSamples ((int) std::lround (0.3 * condition.getSampleRate()) + 37);
            song.make (EveryDayEdit::deleteTrack);
            const auto between = playback.playSamples ((int) std::lround (0.001 * condition.getSampleRate()));

            {
                const phazite::UndoStep step (song.state, "Delete");
                phazite::deleteTracks (song.state, { song.steady }, nullptr);
            }

            const auto rest = playback.play (0.1);
            song.song.run ("transport.playStop");
            juce::AudioBuffer<float> whole (2, before.getNumSamples() + between.getNumSamples() + rest.getNumSamples());
            auto at = 0;

            for (const auto* audio : { &before, &between, &rest })
            {
                for (int channel = 0; channel < 2; ++channel)
                    whole.copyFrom (channel, at, *audio, channel, 0, audio->getNumSamples());

                at += audio->getNumSamples();
            }

            const auto fade = (float) std::lround (phazite::engine::Engine::fadeSeconds * condition.getSampleRate());
            const auto steadyLevel = without.getMagnitude (1, without.getNumSamples() / 2, without.getNumSamples() / 2);
            const auto editedLevel = without.getMagnitude (0, without.getNumSamples() / 2, without.getNumSamples() / 2);
            CHECK (rest.getMagnitude (0, rest.getNumSamples() / 2, rest.getNumSamples() / 2) < 1.0e-5f);
            CHECK (rest.getMagnitude (1, rest.getNumSamples() / 2, rest.getNumSamples() / 2) < 1.0e-5f);
            CHECK (findLargestStep (whole, 0) <= 2.0f * editedLevel / (fade + 1.0f) * 1.01f);
            CHECK (findLargestStep (whole, 1) <= findLargestStep (without, 1) + steadyLevel / (fade + 1.0f) * 1.01f);
        });
}

TEST_CASE ("A render leaves out what it can't hear: a muted track that feeds nothing heard")
{
    // Nothing unmutes during a render, so a muted track's instrument and effects, which play on live (known bug 78), cost a
    // freeze, a bounce, an export and each stem nothing there: a strip is left out of a render's plan unless it is heard, a
    // send of it sends (a muted track's sends before its fader do), a sidechain that is heard listens to it, or a pad's track
    // adds it up. And a stem's render, of one channel, plays only what reaches it.
    TestSong song;
    auto& state = *phazite::findSong (song.context);
    const auto heard = addToneTrack (song, song.firstTrack, "Heard");
    const auto muted = addToneTrack (song, song.addTrack ("Muted"), "Muted");
    const auto sending = addToneTrack (song, song.addTrack ("Sending"), "Sending");
    const auto key = addToneTrack (song, song.addTrack ("Key"), "Key");
    const auto reverb = song.addReturn (sending, phazite::devices::Equalizer::typeID, -6.0f);
    phazite::setPreFader (song.context, phazite::getSends (song.context, sending).front().id, true);
    const auto ducker = phazite::test::addTestDevice (state, heard, phazite::test::TestSidechain::typeID);
    phazite::setSidechainSource (song.context, ducker, key);

    for (const auto track : { muted, sending, key })
        phazite::toggleMute (song.context, track);

    phazite::test::dispatchPendingMessages();
    const auto stripsOf = [] (const phazite::engine::Plan& plan)
    {
        std::set<std::int64_t> ids;

        for (const auto& strip : plan.strips)
            ids.insert (strip.id);

        return ids;
    };

    const auto id = [] (phazite::TrackID track) { return (std::int64_t) track.getRaw(); };
    phazite::SongPlayer whole (state);
    CHECK (stripsOf (*whole.compile (48000.0, 256, std::nullopt))
           == std::set<std::int64_t> { 0, id (heard), id (sending), id (key), id (reverb) });

    // The stem of the heard track: it, and the key its sidechain listens to.
    phazite::SongPlayer stem (state);
    CHECK (stripsOf (*stem.compile (48000.0, 256, std::nullopt, phazite::SongPlayer::RenderOf { id (heard) }))
           == std::set<std::int64_t> { 0, id (heard), id (key) });

    // Live, every one plays.
    phazite::SongPlayer live (state);
    CHECK (stripsOf (*live.compile (48000.0, 256, phazite::SongPlayer::Live {})).size() == 6);
}

TEST_CASE ("A stretched clip of a float file plays silence where the file holds what isn't a number while the rest of it plays")
{
    // The stretcher read the file itself, as it was, and spread a NaN over the whole render of the clip.
    TestSong song;
    song.setTempo (120.0);
    const auto file = song.getSoundsFolder().getChildFile ("Not a number.wav");
    juce::AudioBuffer<float> sound (1, 96000);

    for (int i = 0; i < sound.getNumSamples(); ++i)
        sound.setSample (0, i,
                         i >= 40000 && i < 41000 ? std::numeric_limits<float>::quiet_NaN()
                                                 : 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / 48000.0));

    file.getParentDirectory().createDirectory();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (32).withSampleFormat (
                        juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (sound, 0, sound.getNumSamples());
    }

    const auto clip = song.addAudioClip (song.firstTrack, file, 0.0);
    phazite::setClipPitch (song.context, clip, 12.0f);
    song.waitForClipRenders();
    const auto audio = renderOwn (song, 2.0);
    auto numbers = true;

    for (int i = 0; i < audio.getNumSamples(); ++i)
        numbers = numbers && std::isfinite (audio.getSample (0, i));

    CHECK (numbers);
    CHECK (levelOf (audio, 0, 0.1, 0.6) > -25.0f); // before the NaN
    CHECK (levelOf (audio, 0, 1.0, 1.9) > -25.0f); // and after it
}

TEST_CASE ("Phazite's engine plays a bus's tracks through its fader, a soloed track in it alone, and a plain folder turns its tracks")
{
    TestSong song;
    song.setTempo (120.0);
    const auto first = addToneTrack (song, song.firstTrack, "First");
    const auto second = addToneTrack (song, song.addTrack ("Second"), "Second");
    const auto bus = song.addBus ("Bus", { first, second });
    song.setVolume (bus, -6.0f);

    // Both tones, 6 dB louder together, through the bus's -6 dB.
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel).epsilon (0.002));

    // One soloed: the bus plays it alone.
    phazite::toggleSolo (song.context, first);
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel - 6.0f).epsilon (0.002));
    phazite::toggleSolo (song.context, first);

    // A plain folder (a VCA) turns its tracks by its volume, without a fader of its own.
    phazite::setMixedAsBus (song.context, bus, false);
    song.setVolume (bus, -12.0f);
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel + 6.0f - 12.0f).epsilon (0.002));
}

TEST_CASE ("Phazite's engine mixes a container's devices with what comes into it, by its mix")
{
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
    song.setParameter (filter, "cutoff", 40.0f); // a low-pass far below the tone: all but silent
    const auto container = phazite::groupIntoContainer (song.context, { filter });
    REQUIRE (container.isValid());
    CHECK (levelOf (renderOwn (song), 0) < toneLevel - 30.0f);

    // At no mix, the tone as it came in.
    auto mix = phazite::getMix (song.context, container);
    mix.setValue (0.0f);
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Phazite's engine plays a plug-in from its instance in its helper, as the plug-in is set")
{
    phazite::test::TestPlugin testPlugin;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    auto& state = *phazite::findSong (song.context);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::findPluginInstance (state, effect) != nullptr);

    // Its gain at 1: the tone as it is.
    song.run ("transport.playStop");
    auto audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.1, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));

    // Turned down as its editor turns it, at half: 6 dB quieter.
    phazite::test::TestPlugin::setGain (state, effect, 0.5f);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.1, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));

    // Switched off, it passes the tone as it came.
    phazite::setDeviceOn (song.context, effect, false);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.1, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Phazite's engine renders the Exodus example the same to the last bit on one thread or four")
{
    // Its intro: the drums, with four pads on tracks of their own, the rolling bass, the sub and the arpeggio's four layers
    // on two buses, whose compressors duck them under a muted track, and the master's filters in automation lanes.
    TestSong song (TestSong::Start::exodus);
    phazite::test::dispatchPendingMessages();
    auto& state = *phazite::findSong (song.context);
    const auto one = phazite::renderWithOwnEngine (state, 48000 * 9, 48000.0, 256, 1);
    const auto four = phazite::renderWithOwnEngine (state, 48000 * 9, 48000.0, 256, 4);
    REQUIRE (one.getMagnitude (0, 0, one.getNumSamples()) > 0.01f);

    auto same = true;

    for (int channel = 0; channel < 2; ++channel)
        same = same
               && std::memcmp (one.getReadPointer (channel), four.getReadPointer (channel), sizeof (float) * (size_t) one.getNumSamples())
                      == 0;

    CHECK (same);
}

TEST_CASE ("In 6/8 the delay's 1 bar echoes a bar of the song's later: three quarter notes, a second and a half at 120 BPM")
{
    // A click at the song's start through a delay of a bar, its echo alone, once: silence, then the echo a bar on.
    TestSong song;
    song.setTempo (120.0);
    song.setTimeSignature (6, 8);
    const auto track = song.firstTrack;
    song.addAudioClip (track, phazite::test::writeClicks (song.getSoundsFolder(), 1, 120.0), 0.0);
    song.addMidiClip (song.addTrack ("Long"), 0.0, 12.0); // the song goes on past the echo
    const auto delay = song.addEffect (track, phazite::devices::Delay::typeID);
    song.setParameter (delay, "note", 13.0f); // 1 bar
    song.setParameter (delay, "feedback", 0.0f);
    song.setParameter (delay, "mix", 1.0f);

    // After the click (whose first block the effect's mix ramps into from where it was made), silence until the echo.
    const auto audio = renderOwn (song, 2.5);
    const auto echo = findFirstSound (audio, 4800);
    CHECK (echo >= 72000 - 4);
    CHECK (echo <= 72000 + 48);
}

namespace
{
// The controllers' changes an instrument was given, in order, as (type, number, value in MIDI's steps).
std::vector<std::tuple<phazite::devices::NoteEvent::Type, int, int>> controlsGiven (const std::vector<phazite::devices::NoteEvent>& events)
{
    using Type = phazite::devices::NoteEvent::Type;
    std::vector<std::tuple<Type, int, int>> given;

    for (const auto& event : events)
    {
        if (event.type == Type::pitchBend)
            given.emplace_back (event.type, event.number, juce::roundToInt (event.value * 8192.0f) + 8192);
        else if (event.type == Type::controller || event.type == Type::channelPressure || event.type == Type::aftertouch
                 || event.type == Type::noteOn)
            given.emplace_back (event.type, event.number, juce::roundToInt (event.value * 127.0f));
    }

    return given;
}
} // namespace

TEST_CASE ("Where playback starts, jumps or goes round the loop, the clips' controllers, bend and pressure are chased before the notes")
{
    // Known bug 85's review: a plug-in with the mod wheel on its filter kept the last value it was given across a jump until
    // the next change, and a note started in the middle of a bend played unbent. As Cubase and Ableton Live chase them:
    // each value as the clip has it there, before the notes sounding start; where it has none yet, the bend's middle, no
    // pressure, and the mod wheel (a controller MIDI's Reset All Controllers resets) to none; the volume, which it changes
    // only later, as it is. A note held from beat 0 to 16 on the stand-in, which keeps what it is given.
    using Type = phazite::devices::NoteEvent::Type;

    phazite::test::atEveryCondition (
        [] (const phazite::test::PlaybackCondition& condition)
        {
            TestSong song;
            song.setTempo (condition.getTempo());
            auto& state = *phazite::findSong (song.context);
            const auto toneID = phazite::test::addTestDevice (state, song.firstTrack, phazite::test::TestTone::typeID);
            const auto clip = song.addMidiClip (song.firstTrack, 0.0, 16.0);
            song.addNote (clip, 60, 0.0, 16.0, 100);
            const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };
            phazite::addControl (state, clip, at (1.0), phazite::ControlKind::controller, 1, 100);
            phazite::addControl (state, clip, at (2.0), phazite::ControlKind::pitchWheel, 0, 12288);
            phazite::addControl (state, clip, at (3.0), phazite::ControlKind::channelPressure, 0, 80);
            phazite::addControl (state, clip, at (3.5), phazite::ControlKind::aftertouch, 60, 50);
            phazite::addControl (state, clip, at (6.0), phazite::ControlKind::controller, 7, 90);
            EnginePlayback playback (condition);
            playback.play (0.01);

            const auto tone = [&song, toneID]() -> phazite::test::TestTone&
            {
                auto* node = phazite::findPlayingSession (*phazite::findSong (song.context))
                                 ->getPlayer()
                                 .findDevice ((std::int64_t) toneID.getRaw());
                REQUIRE (node != nullptr);
                return dynamic_cast<phazite::test::TestTone&> (node->getRunner().getProcessor());
            };

            const std::vector<std::tuple<Type, int, int>> inTheMiddle { { Type::controller, 1, 100 },
                                                                        { Type::pitchBend, 0, 12288 },
                                                                        { Type::channelPressure, 0, 80 },
                                                                        { Type::noteOn, 60, 100 },
                                                                        { Type::aftertouch, 60, 50 } };
            const std::vector<std::tuple<Type, int, int>> atTheStart { { Type::controller, 1, 0 },
                                                                       { Type::pitchBend, 0, 8192 },
                                                                       { Type::channelPressure, 0, 0 },
                                                                       { Type::noteOn, 60, 100 } };
            const auto heardNext = [&] (double seconds)
            {
                tone().received.clear();
                playback.play (seconds);
                return controlsGiven (tone().received);
            };

            // Playback started at beat 4.
            phazite::movePlayhead (state, at (4.0));
            playback.play (0.01);
            tone().received.clear();
            phazite::playOrStop (state);
            CHECK (heardNext (0.05) == inTheMiddle);

            // A jump back to beat 0.5, where nothing is changed yet.
            phazite::movePlayhead (state, at (0.5));
            CHECK (heardNext (0.05) == atTheStart);

            // Round a loop from beat 4 to 5: at each time round, the values at its start again.
            phazite::loopBeats (state, { at (4.0), at (5.0) });
            phazite::movePlayhead (state, at (4.5));
            heardNext (0.05);
            CHECK (heardNext (60.0 / condition.getTempo() * 0.6) == inTheMiddle); // round the loop once
            phazite::stopAndReturn (state);
        });
}

TEST_CASE ("Phazite's synth plays a note started after a bend bent, whether playback reached it or jumped to it")
{
    // Known bug 85's review: after a jump, the synth (its bend reset with it) played the note sounding there unbent. A note
    // held from beat 0, bent up at beat 1: the same pitch half a beat after a jump to beat 2 as at that place played through.
    TestSong song;
    song.setTempo (120.0);
    auto& state = *phazite::findSong (song.context);
    song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 8.0);
    song.addNote (clip, 69, 0.0, 8.0, 100);
    phazite::addControl (state, clip, phazite::BeatPosition::fromBeats (1.0), phazite::ControlKind::pitchWheel, 0, 16383);
    EnginePlayback playback;

    // Zero crossings on the left over a quarter of a second, from so far into what was played.
    const auto crossings = [] (const juce::AudioBuffer<float>& audio, double from)
    {
        auto count = 0;
        const auto first = (int) (from * EnginePlayback::sampleRate);

        for (int i = first + 1; i < first + (int) (0.25 * EnginePlayback::sampleRate); ++i)
            count += (audio.getSample (0, i - 1) < 0.0f) != (audio.getSample (0, i) < 0.0f) ? 1 : 0;

        return count;
    };

    phazite::playOrStop (state);
    const auto through = crossings (playback.play (1.6), 1.25); // beats 2.5 to 3
    phazite::stopAndReturn (state);
    playback.play (0.2);

    phazite::movePlayhead (state, phazite::BeatPosition::fromBeats (2.0));
    phazite::playOrStop (state);
    const auto jumped = crossings (playback.play (0.6), 0.25); // beats 2.5 to 3 again
    phazite::stopAndReturn (state);

    CHECK (through > 230); // 440 Hz bent up: more than 220 crossings
    CHECK (std::abs (jumped - through) <= 2);
}
