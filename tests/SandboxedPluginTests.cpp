// SPDX-License-Identifier: AGPL-3.0-or-later
// Plug-ins hosted in helper processes, as a song has them (docs/tracktion-migration.md, T3 steps 2 and 3;
// model/plugins/internal/PluginHost.h): Tracktion's plug-in wraps an instance in a helper, one helper for each plug-in;
// Phazite's engine plays it through its helper, as it is set, with its mix, its automation and its modulators; what it
// turns itself is set by hand, for touch and the last touched; Tracktion's render of a frozen track plays it too; its
// settings survive a save; and one that crashes leaves Phazite playing, an effect letting its sound through.
#include "EnginePlayback.h"
#include "SandboxedPlugins.h"
#include "TestSong.h"
#include "model/arrangement/SongSelection.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixHandle.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

using phazite::test::EnginePlayback;
using phazite::test::SandboxedPlugins;
using phazite::test::TestSong;

namespace
{
float levelOf (const juce::AudioBuffer<float>& audio, int side, double from, double to)
{
    const auto start = (int) (from * 48000.0);
    return juce::Decibels::gainToDecibels (audio.getRMSLevel (side, start, (int) (to * 48000.0) - start));
}

// A 440 Hz tone, at a quarter of full scale, on a track from the song's start, for so many seconds.
phazite::TrackID addToneTrack (TestSong& song, phazite::TrackID track, const char* name, double seconds = 4.0)
{
    song.addAudioClip (
        track, phazite::test::writeSound (song.getSoundsFolder().getChildFile (juce::String (name) + ".wav"), seconds, 440.0, 0.25f), 0.0);
    return track;
}

constexpr float toneLevel = -18.05f; // the tone's RMS through the master's -3 dB

phazite::SandboxedInstance& getSandboxed (TestSong& song, phazite::DeviceID device)
{
    auto* sandboxed = phazite::findSandboxedInstance (*phazite::getEdit (song.context), device);
    REQUIRE (sandboxed != nullptr);
    return *sandboxed;
}
} // namespace

TEST_CASE ("A plug-in in a helper plays in Phazite's engine as it is set, with its mix; its instances share their plug-in's helper")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    CHECK (getSandboxed (song, effect).getRemote().getInfo().parameters.size() == 2);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 1);
    EnginePlayback playback;

    // Its gain at 1: the tone as it is.
    song.run ("transport.playStop");
    auto audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.1, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));

    // Its Gain (Tracktion's first parameter of it) at half, as a control sets it: 6 dB quieter.
    song.setParameter (effect, "0", 0.5f);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));

    // At half its mix with what came in: three quarters of the tone.
    phazite::getMix (song.context, effect).setValue (0.5f);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel + juce::Decibels::gainToDecibels (0.75f)).epsilon (0.002));

    // Switched off, it passes the tone as it came.
    phazite::setDeviceOn (song.context, effect, false);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));

    // A second instance of it goes into the same helper; another plug-in into its own.
    song.addEffect (track, "PhaziteTestEffect");
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 1);
    song.addEffect (track, "PhaziteTestLatency");
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 2);
}

TEST_CASE ("A plug-in in a helper keeps its settings as the project saves, and Tracktion's render of a frozen track plays it")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    song.setParameter (effect, "0", 0.5f);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 200);

    // Its state, as the project stores it: the gain the helper's plug-in has.
    juce::MemoryBlock state;
    getSandboxed (song, effect).getStateInformation (state);
    REQUIRE (state.getSize() == 8);
    CHECK (juce::MemoryInputStream (state, false).readFloat() == doctest::Approx (0.5f));
    REQUIRE (song.getProject().save().wasOk());

    // Frozen: Tracktion renders the track through the helper, at that gain.
    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    REQUIRE (host.finished.size() == 1);
    CHECK (host.finished[0].endsWith (": ok"));
    EnginePlayback playback;
    song.run ("transport.playStop");
    const auto audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.1, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.01));
}

TEST_CASE ("A plug-in in a helper follows its automation and its modulators on Phazite's engine, from its value set by hand")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 8.0);
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    const phazite::ParameterRef gain { effect, "0" }; // its Gain, Tracktion's first parameter of it
    song.setParameter (effect, "0", 0.5f);

    // Automated from 1 down to 0 over two seconds (four beats at 120): on its curve as it plays.
    song.addAutomation (gain, { { phazite::BeatPosition(), 1.0f }, { phazite::BeatPosition::fromBeats (4.0), 0.0f } });
    EnginePlayback playback;
    song.run ("transport.playStop");
    auto audio = playback.play (1.8);

    for (const auto& [second, value] : { std::pair (0.5, 0.75f), std::pair (1.0, 0.5f), std::pair (1.5, 0.25f) })
        CHECK (levelOf (audio, 0, second - 0.05, second + 0.05)
               == doctest::Approx (toneLevel + juce::Decibels::gainToDecibels (value)).epsilon (0.005));

    // Without its automation, back at its value set by hand.
    phazite::deleteAutomationPoints (song.context, gain, { 0, 1 });
    REQUIRE (phazite::getAutomationPoints (song.context, gain).empty());
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));

    // A macro turned up moves it from there: a quarter down, from half.
    const auto macro = song.addModulator (track, phazite::ModulatorType::macro);
    song.modulate (macro, gain, -0.25f);
    song.setMacroValue (macro, 1.0f);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel + juce::Decibels::gainToDecibels (0.25f)).epsilon (0.002));

    // Set by hand while it is modulated: from the new value.
    song.setParameter (effect, "0", 1.0f);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel + juce::Decibels::gainToDecibels (0.75f)).epsilon (0.002));

    // The macro taken off: at its value set by hand.
    phazite::removeModulation (song.context, macro, gain);
    audio = playback.play (0.6);

    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("What a plug-in in a helper turns itself is set by hand: the parameter last touched, which touch writes while it is grabbed")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    const auto synth = song.setInstrument (track, "PhaziteTestSynth");
    const phazite::ParameterRef level { synth, "0" }; // its Level, Tracktion's first parameter of it

    // Note 127 at beat 1 for a beat: the stand-in turns its Level to 0.75 itself, grabbed for as long.
    const auto clip = song.addMidiClip (track, 0.0, 8.0);
    song.addNote (clip, 127, 1.0, 1.0);
    phazite::setAutomationMode (song.context, track, phazite::AutomationMode::touch);
    song.setVolume (track, -6.0f); // the parameter last touched until then
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::getLastTouchedParameter (song.context) == song.getVolumeParameter (track));
    EnginePlayback playback;
    song.run ("transport.playStop");

    // Played past the note's start, and held there until the helper tells what the plug-in did: grabbed where the song is
    // then, a little past beat 1, from its Level before (0.25), and the parameter last touched.
    const auto playedTo = [] { return phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlayheadBeat(); };
    auto handle = phazite::getParameter (song.context, level);
    playback.play (0.55);
    const auto grabbedAt = playedTo();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return juce::exactlyEqual (handle.getValue(), 0.75f); }, 20000));
    CHECK (phazite::getLastTouchedParameter (song.context) == level);

    // Played past the note's end: let go of there, which writes what it did.
    playback.play (0.55);
    const auto letGoAt = playedTo();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getAutomationPoints (song.context, level).empty(); }, 20000));

    const auto points = phazite::getAutomationPoints (song.context, level);
    REQUIRE (points.size() >= 3);
    CHECK (points.front().beat.inBeats() == doctest::Approx (grabbedAt).epsilon (0.005)); // within its last block
    CHECK (points.front().value == doctest::Approx (0.25f));
    CHECK (points[1].value == doctest::Approx (0.75f));
    CHECK (points.back().beat.inBeats() == doctest::Approx (letGoAt).epsilon (0.005));
    CHECK (points.back().value == doctest::Approx (0.75f));

    // One step takes it back.
    phazite::undo (*phazite::getEdit (song.context));
    CHECK (phazite::getAutomationPoints (song.context, level).empty());
}

TEST_CASE ("A plug-in that crashes in its helper leaves Phazite playing: an effect lets its sound through")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto crashing = song.addEffect (track, "PhaziteTestCrash");
    EnginePlayback playback;
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));

    // It crashes as it plays (its Crash, Tracktion's first parameter of it, on), blocks coming meanwhile: Phazite plays on,
    // the tone through.
    song.setParameter (crashing, "0", 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, crashing).hasCrashed();
        },
        20000));
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0); // its helper has gone

    // What it keeps: the state it last had, at once (none taken before).
    juce::MemoryBlock state;
    getSandboxed (song, crashing).getStateInformation (state);
    CHECK (state.isEmpty());
}
