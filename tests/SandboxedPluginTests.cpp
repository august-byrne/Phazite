// SPDX-License-Identifier: AGPL-3.0-or-later
// Plug-ins hosted in helper processes, as a song has them (docs/tracktion-migration.md, T3 step 2;
// model/plugins/internal/PluginHost.h): Tracktion's plug-in wraps an instance in a helper, one helper for each plug-in;
// Phazite's engine plays it through its helper, as it is set, with its mix; Tracktion's render of a frozen track plays it
// too; its settings survive a save; and one that crashes leaves Phazite playing, an effect letting its sound through.
#include "EnginePlayback.h"
#include "SandboxedPlugins.h"
#include "TestSong.h"
#include "model/arrangement/SongSelection.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixHandle.h"
#include "model/mixer/MixerModel.h"
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

// A 440 Hz tone, at a quarter of full scale, on a track from the song's start.
phazite::TrackID addToneTrack (TestSong& song, phazite::TrackID track, const char* name)
{
    song.addAudioClip (
        track, phazite::test::writeSound (song.getSoundsFolder().getChildFile (juce::String (name) + ".wav"), 4.0, 440.0, 0.25f), 0.0);
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
