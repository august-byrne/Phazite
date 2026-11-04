// SPDX-License-Identifier: AGPL-3.0-or-later
// The song as Phazite's own engine plays it (model/playback/): the plan the model compiles from the song, and the transport
// the model's commands drive through it while it plays the project.
#include "EnginePlayback.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/synth/Synth.h"
#include "engine/transport/Click.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/LevelSource.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"

#include <cmath>
#include <cstring>

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
    followTheSong();
    CHECK_FALSE (phazite::isPlaying (song.context));
    CHECK (phazite::getPlayheadBeat (song.context).inBeats() == doctest::Approx (0.0));
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
            enginePoints.push_back ({ point.beat.inBeats(), point.value, point.curve });

        const phazite::engine::Curve curve (std::move (enginePoints));
        const phazite::BeatRange shown { phazite::BeatPosition(), phazite::BeatDuration::fromBeats (beat + 2.0) };
        const auto values = phazite::getAutomationValues (song.context, ref, shown, 400);
        REQUIRE (values.size() == 400);

        for (size_t i = 0; i < values.size(); ++i)
            CHECK (values[i] == doctest::Approx (curve.getValueAt ((beat + 2.0) * (double) i / 399.0)).epsilon (1.0e-6));
    }
}

TEST_CASE ("Phazite's engine plays a fader's automation, which the fader shows, and stopped, the lane's value at the playhead")
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

    // Stopped, back at the start: the lane's value there.
    song.run ("transport.playStop");
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
    CHECK (points[1].value == doctest::Approx (0.5f));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.value - 0.3f) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.value - 0.6f) < 0.001f; }));
    CHECK (points[points.size() - 2].value == doctest::Approx (0.5f));
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
    CHECK (meanwhile.front().value == doctest::Approx (0.5f));

    volume.endGesture();
    song.run ("transport.playStop");

    // The pass, from beat 1, and the point at beat 12 as it was.
    const auto points = phazite::getAutomationPoints (song.context, lane);
    CHECK (points.size() > 1);
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.value - 0.6f) < 0.001f; }));
    CHECK (std::any_of (points.begin(), points.end(),
                        [] (const auto& p) { return std::abs (p.beat.inBeats() - 12.0) < 0.001 && std::abs (p.value - 0.5f) < 0.001f; }));
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
    CHECK (points.back().value == doctest::Approx (-0.5f));

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
    CHECK (std::any_of (points.begin(), points.end(), [] (const auto& p) { return std::abs (p.value - 0.25f) < 0.001f; }));
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

TEST_CASE ("Phazite's engine sends to a return before or after the fader, at the send's level, and a muted track sends nothing")
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

    // Its level is taken in at once; a muted track sends nothing.
    phazite::getSendLevel (song.context, send).setValue (phazite::decibelsToFaderPosition (0.0f));
    CHECK (levelOf (renderOwn (song), 0) == doctest::Approx (toneLevel).epsilon (0.002));
    phazite::toggleMute (song.context, track);
    CHECK (levelOf (renderOwn (song), 0) < -90.0f);
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
    EnginePlayback playback;
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

TEST_CASE ("Phazite's engine renders the Trap Beat example the same to the last bit on one thread or four")
{
    // Its intro and the first hook's start: the drums, the 808's glides, the bells, the pad and the returns.
    TestSong song (TestSong::Start::trapBeat);
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
