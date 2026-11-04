// SPDX-License-Identifier: AGPL-3.0-or-later
// Plug-ins hosted in helper processes, as a song has them (model/plugins/internal/PluginHost.h): each an instance in a
// helper, one helper for each plug-in, which the song's session makes; Phazite's engine plays it through its helper, as
// it is set, with its mix, its automation and its modulators; what it turns itself is set by hand, for touch and the
// last touched; the render of a frozen track plays it too; its settings survive a save; and one that crashes leaves
// Phazite playing, an effect letting its sound through. Its parameters are named by the plug-in's own IDs
// (host/TestPlugins.h).
#include "EnginePlayback.h"
#include "SandboxedPlugins.h"
#include "TestSong.h"
#include "app/SandboxedEditor.h"
#include "model/arrangement/SongSelection.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/PluginScanner.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixHandle.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/Keymap.h"
#include "ui/input/PluginKeys.h"
#include "ui/mixer/ChannelStrip.h"

// Last, so that its macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

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

phazite::Song& getSong (TestSong& song)
{
    auto* found = phazite::findSong (song.context);
    REQUIRE (found != nullptr);
    return *found;
}

phazite::SandboxedInstance& getSandboxed (TestSong& song, phazite::DeviceID device)
{
    auto* sandboxed = phazite::findPluginInstance (getSong (song), device);
    REQUIRE (sandboxed != nullptr);
    return *sandboxed;
}

// The stand-ins' parameters, by their own IDs.
constexpr const char* effectGain = "1";
constexpr const char* synthLevel = "10";
constexpr const char* crashCrash = "20";
constexpr const char* hangHang = "30";
constexpr const char* hangStall = "31";
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

    // Its Gain at half, as a control sets it: 6 dB quieter.
    song.setParameter (effect, effectGain, 0.5f);
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

TEST_CASE ("A plug-in in a helper keeps its settings as the project saves, and the render of a frozen track plays it")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    song.setParameter (effect, effectGain, 0.5f);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 200);

    // Its state, as the project stores it: the gain the helper's plug-in has.
    juce::MemoryBlock state;
    getSandboxed (song, effect).getStateInformation (state);
    REQUIRE (state.getSize() == 8);
    CHECK (juce::MemoryInputStream (state, false).readFloat() == doctest::Approx (0.5f));
    REQUIRE (song.getProject().save().wasOk());

    // Frozen: Phazite's engine renders the track offline through the helper, at that gain.
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
    const phazite::ParameterRef gain { effect, effectGain };
    song.setParameter (effect, effectGain, 0.5f);

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
    song.setParameter (effect, effectGain, 1.0f);
    audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel + juce::Decibels::gainToDecibels (0.75f)).epsilon (0.002));

    // The macro taken off: at its value set by hand.
    phazite::removeModulation (song.context, macro, gain);
    audio = playback.play (0.6);

    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Plug-in instruments in helpers, handed their blocks as the engine's block starts, play their notes and automation")
{
    // Three tracks of the stand-in synth, which the engine plays side by side, each handed its block before the tracks
    // play: each plays 440 Hz in turn for a second, at its Level (a quarter), the third's automated to half.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);

    for (int i = 0; i < 3; ++i)
    {
        const auto track = i == 0 ? song.firstTrack : song.addTrack ("Synth " + juce::String (i + 1));
        const auto synth = song.setInstrument (track, "PhaziteTestSynth");
        song.addNote (song.addMidiClip (track, 0.0, 8.0), 69, 2.0 * i, 2.0);

        if (i == 2)
            song.addAutomation ({ synth, synthLevel },
                                { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (8.0), 0.5f } });
    }

    EnginePlayback playback;
    song.run ("transport.playStop");
    const auto audio = playback.play (3.2);
    CHECK (levelOf (audio, 0, 0.1, 0.9) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (levelOf (audio, 0, 1.1, 1.9) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (levelOf (audio, 0, 2.1, 2.9) == doctest::Approx (toneLevel + 6.02f).epsilon (0.002));
}

TEST_CASE ("What a plug-in in a helper turns itself is set by hand: the parameter last touched, which touch writes while it is grabbed")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    const auto synth = song.setInstrument (track, "PhaziteTestSynth");
    const phazite::ParameterRef level { synth, synthLevel };

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
    phazite::undo (getSong (song));
    CHECK (phazite::getAutomationPoints (song.context, level).empty());
}

TEST_CASE ("A plug-in in a helper keeps its settings and program through copies, undo, presets and its project opened again")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    auto playback = std::make_unique<EnginePlayback>();
    song.run ("transport.playStop");
    const auto levelNow = [&playback] { return levelOf (playback->play (0.6), 0, 0.2, 0.5); };
    const auto half = toneLevel - 6.02f;

    // Its programs, as the helper told them, the one it has, and another chosen: Half sets its Gain to half.
    auto* instance = &getSandboxed (song, effect);
    REQUIRE (instance->getNumPrograms() == 2);
    CHECK (instance->getProgramName (1) == "Half");
    CHECK (instance->getCurrentProgram() == 0);
    instance->setCurrentProgram (1);
    CHECK (instance->getCurrentProgram() == 1);
    CHECK (levelNow() == doctest::Approx (half).epsilon (0.002));

    // The project opened again: the plug-in in a new helper, the old one gone, with its program and its settings.
    playback = nullptr;
    song.reopen();
    CHECK (phazite::findDeviceState (getSong (song), effect)[phazite::schema::pluginProgram] == juce::var (1));
    instance = &getSandboxed (song, effect);
    CHECK (instance->getCurrentProgram() == 1);

    // The old helper goes with the old instance, once nothing that played the old project holds it.
    CHECK (phazite::test::dispatchMessagesUntil ([] { return SandboxedPlugins::getHost().getNumHelpers() == 1; }, 10000));
    playback = std::make_unique<EnginePlayback>();
    song.run ("transport.playStop");
    CHECK (levelNow() == doctest::Approx (half).epsilon (0.002));

    // Its track duplicated: the copy's plug-in, another instance in the same helper, plays at its settings too (the two
    // together twice as loud as one).
    song.context.getSongSelection().selectOnly (track);
    song.run ("edit.duplicate");
    REQUIRE (phazite::getMixerChannels (song.context).tracks.size() == 2);
    const auto copy = phazite::getMixerChannels (song.context).tracks[1];
    REQUIRE (phazite::getEffects (song.context, copy).size() == 1);
    CHECK (&getSandboxed (song, phazite::getEffects (song.context, copy)[0].id) != instance);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 1);
    CHECK (levelNow() == doctest::Approx (half + 6.02f).epsilon (0.002));
    phazite::undo (getSong (song));

    // Taken away, and back by undo, as it was.
    phazite::removeDevice (song.context, effect);
    CHECK (levelNow() == doctest::Approx (toneLevel).epsilon (0.002));
    phazite::undo (getSong (song));
    CHECK (levelNow() == doctest::Approx (half).epsilon (0.002));

    // A preset of it, saved at half and loaded into it again at full.
    const auto preset = song.getSoundsFolder().getChildFile (juce::String ("Half") + phazite::presetExtension);
    REQUIRE (phazite::saveDevicePreset (song.context, effect, preset));
    song.setParameter (effect, effectGain, 1.0f);
    CHECK (levelNow() == doctest::Approx (toneLevel).epsilon (0.002));
    REQUIRE (phazite::loadPresetInto (song.context, effect, preset));
    CHECK (levelNow() == doctest::Approx (half).epsilon (0.002));
}

TEST_CASE ("What a plug-in in a helper changes itself is an unsaved change of its project")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    REQUIRE (song.getProject().save().wasOk());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // Another program, as its own window would choose: its Gain changes with it.
    getSandboxed (song, effect).setCurrentProgram (1);
    auto gain = phazite::getParameter (song.context, { effect, effectGain });
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return song.getProject().hasUnsavedChanges() && juce::exactlyEqual (gain.getValue(), 0.5f); }, 20000));
}

#if JUCE_WINDOWS
TEST_CASE ("A plug-in in a helper shows its editor in Phazite's window, as large as it makes it, and hands back play over it")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");

    // The app's keys over plug-ins: Space plays.
    int plays = 0;
    phazite::CommandRegistry registry;
    registry.add ({ .id = "transport.playStop", .name = "Play / Stop", .perform = [&plays] { ++plays; }, .overPlugins = true });
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["transport.playStop"] = { "spacebar" };
    commands.setKeymap (keymap);
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::ComputerKeyboard computerKeyboard { [] (const juce::MidiMessage&) {} };
    phazite::ui::PluginKeys keys { shortcutKeys, computerKeyboard };
    phazite::ui::setPluginKeys (&keys);

    // Its editor, as a plug-in window makes it, as large as the plug-in's own, in a window on the screen.
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    auto editor = phazite::createSandboxedEditor (session->getPlugins().find (effect));
    REQUIRE (editor != nullptr);
    CHECK (editor->getWidth() == 300);
    CHECK (editor->getHeight() == 200);
    juce::Component window;
    window.setSize (400, 300);
    window.addAndMakeVisible (*editor);
    window.addToDesktop (0);
    window.setVisible (true);
    keys.watch (window);

    // The helper's window for it: a child of this one, of another process.
    const auto parent = static_cast<HWND> (window.getPeer()->getNativeHandle());
    HWND child = nullptr;
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            child = FindWindowExW (parent, nullptr, nullptr, nullptr);
            return child != nullptr;
        },
        20000));
    DWORD process = 0;
    GetWindowThreadProcessId (child, &process);
    CHECK (process != GetCurrentProcessId());

    // The plug-in makes its editor larger: Phazite's follows.
    song.setParameter (effect, "0", 1.0f); // its Tone
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return editor->getWidth() == 360; }, 20000));
    CHECK (editor->getHeight() == 240);

    // Space pressed in the plug-in's window plays, as the helper hands it back (pressed again until the helper knows the
    // keys Phazite takes, which it is told as the editor shows).
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            PostMessageW (child, WM_KEYDOWN, VK_SPACE, 0);
            return phazite::test::dispatchMessagesUntil ([&] { return plays > 0; }, 100);
        },
        20000));

    // With Send All Keys to the Plug-in on, the plug-in gets it.
    keys.setSendsAllKeys (window, true);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 600); // the editor tells the helper
    const auto before = plays;
    PostMessageW (child, WM_KEYDOWN, VK_SPACE, 0);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 500);
    CHECK (plays == before);

    keys.unwatch (window);
    editor = nullptr;
    phazite::ui::setPluginKeys (nullptr);
}
#endif

TEST_CASE ("A plug-in in a helper that listens to another track hears it on its sidechain bus")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto heard = addToneTrack (song, song.firstTrack, "Tone");
    const auto listening = song.addTrack ("Listens");
    const auto effect = song.addEffect (listening, "PhaziteTestSidechain"); // plays its sidechain in place of its sound
    REQUIRE (phazite::hasSidechain (song.context, effect));
    EnginePlayback playback;
    song.run ("transport.playStop");

    // Listening to nothing: silence from it, the tone alone.
    CHECK (levelOf (playback.play (0.6), 0, 0.2, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));

    // Listening to the tone's track: the tone from it too, twice as loud together.
    phazite::setSidechainSource (song.context, effect, heard);
    CHECK (levelOf (playback.play (0.6), 0, 0.2, 0.5) == doctest::Approx (toneLevel + 6.02f).epsilon (0.002));
}

TEST_CASE ("A plug-in whose audio thread hangs in its helper is ended for it, says it crashed, and reloads as it was")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto hanging = song.addEffect (track, "PhaziteTestHang");
    EnginePlayback playback;
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));

    // Its Hang on: its blocks come late, the tone through, until its helper is ended, a second of late blocks after.
    song.setParameter (hanging, hangHang, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, hanging).hasCrashed();
        },
        20000));
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getEffects (song.context, track)[0].crashed);

    // Reloaded: the same instance in a new helper, from the state it last had (none taken: the plug-in's own, its Hang
    // off), its value as the plug-in has it.
    auto* instance = &getSandboxed (song, hanging);
    CHECK_FALSE (phazite::reloadDevice (song.context, hanging).has_value());
    CHECK (&getSandboxed (song, hanging) == instance);
    CHECK_FALSE (instance->hasCrashed());
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 1);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::getEffects (song.context, track)[0].crashed);
    CHECK (phazite::getParameter (song.context, { hanging, hangHang }).getValue() == doctest::Approx (0.0f));
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("A plug-in whose helper's message thread stops answering is ended for it, and nothing of Phazite's waits")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto stalling = song.addEffect (track, "PhaziteTestHang");
    EnginePlayback playback;
    song.run ("transport.playStop");

    // Its Stall on, which stops the helper's message thread: the heartbeat goes unanswered, and the helper is ended two
    // seconds after, the song playing on meanwhile.
    const auto stalledAt = juce::Time::getMillisecondCounterHiRes();
    song.setParameter (stalling, hangStall, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, stalling).hasCrashed();
        },
        20000));
    CHECK (juce::Time::getMillisecondCounterHiRes() - stalledAt >= SandboxedPlugins::messageHangMilliseconds);
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("A plug-in that crashes its helper as it loads stays off, another of it too, until it is reloaded")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto identity =
        phazite::test::TestPlugins::getDescription (phazite::test::TestPlugins::Kind::crashLoading).createIdentifierString();

    // Added, it crashes its helper as it loads: its slot says so, and the plug-in host keeps it off from then on (the app
    // keeps the list in its settings, for the next start).
    const auto crashing = song.addEffect (song.firstTrack, "PhaziteTestCrashLoading");
    CHECK (phazite::getEffects (song.context, song.firstTrack)[0].crashed);
    CHECK (SandboxedPlugins::getHost().getCrashedWhileLoading().contains (identity));
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0);

    // Another of it isn't loaded at all: its helper isn't started.
    song.addEffect (song.firstTrack, "PhaziteTestCrashLoading");
    CHECK (phazite::getEffects (song.context, song.firstTrack)[1].crashed);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0);

    // Reloaded, it loads again, crashes again, and says so.
    const auto problem = phazite::reloadDevice (song.context, crashing);
    REQUIRE (problem.has_value());
    CHECK (problem->contains ("crashed"));
    CHECK (SandboxedPlugins::getHost().getCrashedWhileLoading().contains (identity));
    CHECK (phazite::getEffects (song.context, song.firstTrack)[0].crashed);
}

TEST_CASE ("A crashed plug-in's slot in the mixer says so, and reloads at a click")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto crashing = song.addEffect (track, "PhaziteTestCrash");
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::ChannelStrip strip { commands, song.context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 700 };
    strip.setTrack (track);

    const auto effectButton = [&strip]() -> juce::TextButton*
    {
        if (auto* group = phazite::test::findComponentWithTitle (strip, "Effects"))
            for (auto* button : phazite::test::findAll<juce::TextButton> (*group))
                if (button->isVisible() && button->getButtonText().startsWith ("Test Crash"))
                    return button;

        return nullptr;
    };

    REQUIRE (effectButton() != nullptr);
    CHECK (effectButton()->getButtonText() == "Test Crash");

    // It crashes as it plays: its slot says so.
    EnginePlayback playback;
    song.run ("transport.playStop");
    song.setParameter (crashing, crashCrash, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return effectButton() != nullptr && effectButton()->getButtonText() == "Test Crash - Crashed";
        },
        20000));

    // A click reloads it (its Crash off again, as the plug-in has it): its slot shows its name again.
    song.run ("transport.playStop");
    phazite::test::Mouse mouse (*effectButton());
    mouse.click (effectButton()->getLocalBounds().getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return effectButton() != nullptr && effectButton()->getButtonText() == "Test Crash"; }));
    CHECK_FALSE (getSandboxed (song, crashing).hasCrashed());
}

TEST_CASE ("The plug-in scan finds what a file holds in a helper, and leaves out one that hangs or crashes it, each in its time")
{
    using phazite::test::TestPlugins;
    constexpr int secondsEach = 2;
    auto scanner = phazite::createPluginScanner (SandboxedPlugins::getHost(), secondsEach);
    const auto format = TestPlugins::createFormat(); // its name, which the helper scans with
    juce::OwnedArray<juce::PluginDescription> found;

    // A file's plug-ins, found in the helper.
    CHECK (scanner->findPluginTypesFor (*format, found, "PhaziteTestEffect"));
    REQUIRE (found.size() == 1);
    CHECK (found[0]->name == "Test Effect");

    // One that hangs its scan is left out (blacklisted) once its time is up, the helper ended with it.
    found.clear();
    const auto hungAt = juce::Time::getMillisecondCounterHiRes();
    CHECK_FALSE (scanner->findPluginTypesFor (*format, found, TestPlugins::hangsScanned));
    const auto took = juce::Time::getMillisecondCounterHiRes() - hungAt;
    CHECK (took >= secondsEach * 1000.0);
    CHECK (took < secondsEach * 4000.0);
    CHECK (found.isEmpty());

    // One that crashes its helper, twice (once more in a fresh helper), is left out too.
    CHECK_FALSE (scanner->findPluginTypesFor (*format, found, TestPlugins::crashesScanned));
    CHECK (found.isEmpty());

    // The next file scans as ever, in another helper.
    CHECK (scanner->findPluginTypesFor (*format, found, "PhaziteTestSynth"));
    REQUIRE (found.size() == 1);
    CHECK (found[0]->name == "Test Synth");
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

    // It crashes as it plays (its Crash on), blocks coming meanwhile: Phazite plays on, the tone through.
    song.setParameter (crashing, crashCrash, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, crashing).hasCrashed();
        },
        20000));
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0); // its helper has gone

    // What it keeps: the state it last had, at once (none taken before); and the project saves as it would.
    juce::MemoryBlock state;
    getSandboxed (song, crashing).getStateInformation (state);
    CHECK (state.isEmpty());
    const auto savedAt = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (song.getProject().save().wasOk());
    CHECK (juce::Time::getMillisecondCounterHiRes() - savedAt < 2000.0);
}
