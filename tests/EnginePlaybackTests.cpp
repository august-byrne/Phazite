// SPDX-License-Identifier: AGPL-3.0-or-later
// The song as Phazite's own engine plays it (model/playback/; docs/tracktion-migration.md, T2): the plan the model compiles
// from the song, and the transport the model's commands drive through it while it plays the project.
#include "EnginePlayback.h"
#include "TestSong.h"
#include "devices/synth/Synth.h"
#include "engine/transport/Click.h"
#include "model/arrangement/TimelineModel.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/LevelSource.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"

#include <cmath>

using phazite::test::EnginePlayback;
using phazite::test::TestSong;

namespace
{
// The first sample something sounds at, or -1.
int findFirstSound (const juce::AudioBuffer<float>& audio, int from = 0)
{
    for (int i = from; i < audio.getNumSamples(); ++i)
        if (audio.getSample (0, i) != 0.0f)
            return i;

    return -1;
}
} // namespace

TEST_CASE ("The engine's tempo map places beats where Tracktion's tempo sequence does")
{
    TestSong song;
    auto& edit = *phazite::getEdit (song.context);
    juce::Random random (11);

    for (int round = 0; round < 8; ++round)
    {
        // Tempos and time signatures along the song, in its tempo map, which Tracktion's mirrors.
        auto map = phazite::getPhaziteState (edit).getChildWithName (phazite::schema::TEMPOMAP);
        REQUIRE (map.isValid());
        map.removeAllChildren (nullptr);
        auto beat = 0.0;

        for (int change = 0; change < 4; ++change)
        {
            juce::ValueTree tempo (phazite::schema::TEMPO);
            tempo.setProperty (phazite::schema::mapBeat, beat, nullptr);
            tempo.setProperty (phazite::schema::bpm, 40.0 + random.nextInt (200), nullptr);
            map.appendChild (tempo, nullptr);
            beat += 4.0 * (1 + random.nextInt (4));
        }

        const int denominators[] = { 2, 4, 8, 16 };
        juce::ValueTree signature (phazite::schema::TIMESIGNATURE);
        signature.setProperty (phazite::schema::mapBeat, 0.0, nullptr);
        signature.setProperty (phazite::schema::numerator, 2 + random.nextInt (10), nullptr);
        signature.setProperty (phazite::schema::denominator, denominators[random.nextInt (4)], nullptr);
        map.appendChild (signature, nullptr);
        phazite::mirror::reconcileTempoMap (edit);

        phazite::SongPlayer player (edit);
        const auto plan = player.compile (48000.0, 512, true);

        for (auto at = 0.0; at < beat + 8.0; at += 0.37)
        {
            CAPTURE (at);
            const auto seconds = edit.tempoSequence.toTime (phazite::toTracktion (phazite::BeatPosition::fromBeats (at))).inSeconds();
            CHECK (plan->tempo.toSamples (at) / 48000.0 == doctest::Approx (seconds).epsilon (1.0e-9));
        }
    }
}

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

    // Without the loop, playback stops at the song's end, and goes back: the song's follower sees the playhead cross it.
    const auto followTheSong = [] { phazite::test::dispatchMessagesUntil ([] { return false; }, 100); }; // its timer, at 30 Hz

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

TEST_CASE ("Phazite's engine clicks the metronome on each beat's sample, and Tracktion's doesn't play meanwhile")
{
    TestSong song;
    song.setTempo (120.0);
    song.addMidiClip (song.firstTrack, 0.0, 16.0);
    song.setSwitch ("transport.metronome", true);
    EnginePlayback playback;

    song.run ("transport.playStop");
    CHECK_FALSE (phazite::getEdit (song.context)->getTransport().isPlaying());
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
