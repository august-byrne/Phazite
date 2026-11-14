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
#include "model/arrangement/TimelineModel.h"
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
#include "model/plugins/internal/SandboxedProcessor.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/Keymap.h"
#include "ui/input/PluginKeys.h"
#include "ui/mixer/ChannelStrip.h"

#include <atomic>
#include <span>
#include <thread>

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
constexpr const char* crashNaN = "21";
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
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);

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
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
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
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
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

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
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
    auto playback = std::make_unique<EnginePlayback> (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
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
    playback.reset(); // before the next one, whose timing its going would undo
    playback = std::make_unique<EnginePlayback> (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
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

TEST_CASE ("What a plug-in in a helper tells of itself that changes nothing is no unsaved change: a VST3's restart, values again")
{
    // The owner, 2026-11-08: an idle project asked to save its changes as Phazite closed, with none in Source Control.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    REQUIRE (song.getProject().save().wasOk());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // Its gain and tone told again as they are, as many plug-ins' windows do as they open; then what JUCE's VST3 host tells
    // of a VST3's restartComponent(), for its latency say: another program, with new parameter info.
    auto& instance = getSandboxed (song, effect);
    const auto state = instance.getRemote().getState (10000);
    REQUIRE (state.has_value());
    juce::MemoryInputStream before (*state, false);
    const auto gainNow = before.readFloat(), toneNow = before.readFloat();
    enum Tells
    {
        nothingMore,
        restart, // what JUCE's VST3 host tells of a restartComponent()
        dirty    // that its settings changed, as a VST3's setDirty()
    };

    const auto tell = [&instance] (float gain, float tone, Tells tells)
    {
        juce::MemoryOutputStream told;
        told.writeFloat (gain);
        told.writeFloat (tone);
        told.writeFloat (1.0f); // as its own window would set them
        told.writeFloat ((float) tells);
        REQUIRE (instance.getRemote().setState (told.getMemoryBlock(), 10000));
    };

    tell (gainNow, toneNow, restart);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 1000); // what it told has come and gone
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
    CHECK (getSong (song).getChangesOutsideHistory() == 0);

    // Its settings said to have changed, though they didn't: closing asks it for them, and finds none to save.
    tell (gainNow, toneNow, dirty);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return song.getProject().hasUnsavedChanges(); }, 20000));
    CHECK_FALSE (song.getProject().checkUnsavedChanges());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // Its gain told as another value is one, which closing finds too.
    tell (gainNow * 0.5f, toneNow, nothingMore);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return song.getProject().hasUnsavedChanges(); }, 20000));
    CHECK (song.getProject().checkUnsavedChanges());
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
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");

    // Listening to nothing: silence from it, the tone alone.
    CHECK (levelOf (playback.play (0.6), 0, 0.2, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));

    // Listening to the tone's track: the tone from it too, twice as loud together.
    phazite::setSidechainSource (song.context, effect, heard);
    CHECK (levelOf (playback.play (0.6), 0, 0.2, 0.5) == doctest::Approx (toneLevel + 6.02f).epsilon (0.002));
}

TEST_CASE ("A plug-in in a helper hears where the song is in quarter notes, samples and bars, and its time signature, in 6/8")
{
    using Playhead = phazite::test::TestPlugins::Playhead;
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    song.setTimeSignature (6, 8);
    song.addMidiClip (song.firstTrack, 0.0, 24.0); // eight bars: playback stops at the song's end

    // From bar 2, three quarter notes in, the tempo halves; and from bar 3, six quarter notes in, the song is in 4/4, as its
    // tempo map can hold it.
    phazite::addTempoChange (getSong (song), phazite::BeatPosition::fromBeats (3.0), 60.0);
    namespace schema = phazite::schema;
    auto tempoMap = getSong (song).getState().getChildWithName (schema::TEMPOMAP);
    tempoMap.appendChild (
        juce::ValueTree (schema::TIMESIGNATURE, { { schema::mapBeat, 6.0 }, { schema::numerator, 4 }, { schema::denominator, 4 } }),
        nullptr);
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestPlayhead");
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);

    // Where its playhead said the song was as its last block started.
    const auto heard = [&song, effect]
    {
        juce::MemoryBlock state;
        getSandboxed (song, effect).getStateInformation (state);
        return Playhead::read (state);
    };

    // A second in, its last block starting at sample 47872: at 120 BPM a quarter note is half a second (24000 samples), in
    // the first bar.
    song.run ("transport.playStop");
    playback.play (1.0);
    auto playhead = heard();
    CHECK (playhead.ppq == doctest::Approx (47872.0 / 24000.0));
    CHECK (playhead.sample == 47872);
    CHECK (playhead.bpm == doctest::Approx (120.0));
    CHECK (playhead.numerator == 6);
    CHECK (playhead.denominator == 8);
    CHECK (playhead.playing);
    CHECK (playhead.barStart == doctest::Approx (0.0));
    CHECK (playhead.barCount == 0);

    // A second later, its last block starting at sample 95872, past the tempo change a bar (three quarter notes, 1.5 s) in:
    // a quarter note a second since, and its time in samples where the song is, however its quarter notes went.
    playback.play (1.0);
    playhead = heard();
    CHECK (playhead.ppq == doctest::Approx (3.0 + (95872.0 - 72000.0) / 48000.0));
    CHECK (playhead.sample == 95872);
    CHECK (playhead.bpm == doctest::Approx (60.0));
    CHECK (playhead.numerator == 6);
    CHECK (playhead.denominator == 8);
    CHECK (playhead.barStart == doctest::Approx (3.0));
    CHECK (playhead.barCount == 1);

    // Three seconds later, its last block starting at sample 239872, in the first bar of 4/4, which starts six quarter notes
    // in (4.5 s), its third.
    playback.play (3.0);
    playhead = heard();
    CHECK (playhead.ppq == doctest::Approx (6.0 + (239872.0 - 216000.0) / 48000.0));
    CHECK (playhead.sample == 239872);
    CHECK (playhead.numerator == 4);
    CHECK (playhead.denominator == 4);
    CHECK (playhead.barStart == doctest::Approx (6.0));
    CHECK (playhead.barCount == 2);
    song.run ("transport.playStop");
}

TEST_CASE ("Stopped, a plug-in in a helper hears where the playhead is in every piece of a block its parameters' changes cut")
{
    using Playhead = phazite::test::TestPlugins::Playhead;
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    song.setTimeSignature (6, 8);
    song.addMidiClip (song.firstTrack, 0.0, 24.0);
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestPlayhead");

    // An LFO on its Watch moves it on every control point, stopped too, which cuts each block into pieces in its helper.
    const auto lfo = song.addModulator (song.firstTrack, phazite::ModulatorType::lfo);
    song.setModulatorParameter (lfo, "rateType", 0.0f); // in hertz, at 1 Hz
    song.modulate (lfo, phazite::ParameterRef { effect, "50" }, 0.5f);

    // The playhead in bar 2 of 6/8, a quarter note and a half into it: 2.25 s in at 120 BPM.
    phazite::movePlayhead (song.context, phazite::BeatPosition::fromBeats (4.5));
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    playback.play (0.2);

    juce::MemoryBlock state;
    getSandboxed (song, effect).getStateInformation (state);
    const auto playhead = Playhead::read (state);
    CHECK_FALSE (playhead.playing);
    CHECK (playhead.ppq == doctest::Approx (4.5));
    CHECK (playhead.sample == 108000);
    CHECK (playhead.barStart == doctest::Approx (3.0));
    CHECK (playhead.barCount == 1);
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

    // Its Hang on: its blocks come late, silent (the tone faded out), until its helper is ended, a second of late blocks
    // after; silent after that too.
    song.setParameter (hanging, hangHang, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, hanging).hasCrashed();
        },
        20000));
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
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
    // seconds after, the song playing on meanwhile; the plug-in silent from then on.
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
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
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

TEST_CASE ("A plug-in that crashes in its helper leaves Phazite playing: an effect falls silent, as a gap")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto crashing = song.addEffect (track, "PhaziteTestCrash");
    EnginePlayback playback;
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));

    // It crashes as it plays (its Crash on), blocks coming meanwhile: Phazite plays on, the effect silent rather than dry
    // (a limiter or a fully wet reverb passed dry is worse than a gap).
    song.setParameter (crashing, crashCrash, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, crashing).hasCrashed();
        },
        20000));
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0); // its helper has gone

    // What it keeps: the state it last had, at once (none taken before); and the project saves as it would.
    juce::MemoryBlock state;
    getSandboxed (song, crashing).getStateInformation (state);
    CHECK (state.isEmpty());
    const auto savedAt = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (song.getProject().save().wasOk());
    CHECK (juce::Time::getMillisecondCounterHiRes() - savedAt < 2000.0);
}

TEST_CASE ("A plug-in opened from its project keeps the settings the project holds when it crashes: saved again, and reloaded")
{
    // The audit of 2026-11-13: only a state taken or given since the plug-in opened was kept, so one that crashed before
    // any was lost what the project held of it at the next save, and reloaded as new.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    song.setParameter (effect, effectGain, 0.5f);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 200);

    // Saved and opened again: the plug-in opens from the settings the project holds, its Gain at half.
    song.reopen();
    const auto getStored = [&song, effect]
    { return phazite::findDeviceState (getSong (song), effect)[phazite::schema::pluginState].toString(); };
    const auto stored = getStored();
    REQUIRE (stored.isNotEmpty());
    auto& instance = getSandboxed (song, effect);
    CHECK (instance.getParameterValue (instance.findParameter (effectGain)) == doctest::Approx (0.5f));

    // Its helper goes, as one that crashes or is ended for a hang.
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }, 20000));

    // Saved while it is crashed, the project holds its settings still.
    REQUIRE (song.getProject().save().wasOk());
    CHECK (getStored() == stored);

    // Reloaded, it has them: its Gain at half, as the plug-in in its new helper says.
    CHECK_FALSE (phazite::reloadDevice (song.context, effect).has_value());
    REQUIRE_FALSE (instance.hasCrashed());
    const auto state = instance.getRemote().getState (10000);
    REQUIRE (state.has_value());
    REQUIRE (state->getSize() == 8);
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (0.5f));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getParameter (song.context, { effect, effectGain }).getValue() == doctest::Approx (0.5f));
    REQUIRE (song.getProject().save().wasOk());
    CHECK (getStored() == stored);
}

TEST_CASE ("A plug-in that crashes or hangs as its track is frozen fails the render, which says so, and Phazite goes on")
{
    // The audit of 2026-11-13: a render waited for a plug-in's block without end, so one that crashed or hung in it kept
    // the render's thread for ever, and then Phazite's window, which waits for that thread as the render is cancelled.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    auto crashes = false;

    SUBCASE ("one that crashes")
    {
        crashes = true;
    }

    SUBCASE ("one that hangs, which is ended for it")
    {
    }

    const auto effect = song.addEffect (track, crashes ? "PhaziteTestCrash" : "PhaziteTestHang");
    song.setParameter (effect, crashes ? crashCrash : hangHang, 1.0f);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 200); // its helper has it before the render's first block

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    REQUIRE (host.finished.size() == 1);
    CHECK (host.finished[0].contains (juce::String (crashes ? "Test Crash" : "Test Hang") + " crashed while Phazite rendered"));
    CHECK_FALSE (phazite::isFrozen (song.context, track));

    // Its slot says it crashed, and the song plays on, the crashed effect silent.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getSandboxed (song, effect).hasCrashed(); }, 20000));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getEffects (song.context, track)[0].crashed);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
}

TEST_CASE ("A render cancelled while a plug-in keeps its block doesn't wait for the plug-in")
{
    SandboxedPlugins sandboxed;

    // For the helper it starts: no block is taken for a hang within the case, however busy the machine.
    SandboxedPlugins::getHost().setHangLimits (SandboxedPlugins::messageHangMilliseconds, 20000);
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto hanging = song.addEffect (track, "PhaziteTestHang");
    song.setParameter (hanging, hangHang, 1.0f);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 200);

    // The engine at 48 kHz, and the freeze at 44.1 kHz (the tests have no audio device, whose rate a freeze would take).
    // The freeze waits for the plug-in, which keeps the render's first block. Cancelled, the plug-in is prepared for the
    // engine's rate again, which its helper can't do while it keeps the block: its channel waits for the block instead
    // of Phazite for the helper (the audit of 2026-11-13: for the prepare's ten seconds).
    auto& engineHost = phazite::getPlaybackHost (phazite::test::getEngineContext());
    engineHost.prepare (48000.0, EnginePlayback::blockSize);
    const juce::ScopeGuard rateAsBefore { [&engineHost] { engineHost.prepare (44100.0, EnginePlayback::blockSize); } };
    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (host.render != nullptr);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 500);
    REQUIRE (host.finished.isEmpty());

    // Cancelled, as its window's Cancel and quitting do: at once, not once the plug-in is taken for hung.
    const auto cancelledAt = juce::Time::getMillisecondCounterHiRes();
    host.render = nullptr;
    // (the old wait was the prepare's ten seconds, and the plug-in is taken for hung only twenty seconds on: three tell
    // them apart on a busy machine too)
    CHECK (juce::Time::getMillisecondCounterHiRes() - cancelledAt < 3000.0);
    CHECK (host.finished.isEmpty());
    CHECK_FALSE (phazite::isFrozen (song.context, track));

    // The plug-in lets go of the block (its Hang off), and plays on.
    song.setParameter (hanging, hangHang, 0.0f);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 200);
    CHECK_FALSE (getSandboxed (song, hanging).hasCrashed());
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("A plug-in instrument with an input bus is its track's instrument: it plays its notes")
{
    // The audit of 2026-11-13: a plug-in with an input was played as an effect, whatever it was, and got no notes.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto synth = song.setInstrument (song.firstTrack, "PhaziteTestSynthWithInput");
    REQUIRE (getSandboxed (song, synth).getRemote().getInfo().numInputChannels == 2);
    song.addNote (song.addMidiClip (song.firstTrack, 0.0, 8.0), 69, 0.0, 2.0);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (1.0), 0, 0.1, 0.9) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("Stop ends the notes a plug-in instrument holds, though the plug-in takes no all notes off")
{
    // The audit of 2026-11-13: Stop sent a plug-in nothing but controller 123, which JUCE's VST3 host hands on only to a
    // plug-in that maps it to a parameter. The stand-in takes no controller.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    song.setInstrument (song.firstTrack, "PhaziteTestSynth");
    song.addNote (song.addMidiClip (song.firstTrack, 0.0, 16.0), 69, 0.0, 16.0);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.5), 0, 0.1, 0.4) == doctest::Approx (toneLevel).epsilon (0.002));

    // Stopped while the note sounds: it ends with the first block after.
    song.run ("transport.playStop");
    const auto audio = playback.play (0.4);
    CHECK (audio.getMagnitude (0, 4800, audio.getNumSamples() - 4800) == 0.0f);
}

TEST_CASE ("The notes of a block a plug-in couldn't be handed go with the next: a note that ended then doesn't sound on")
{
    // The audit of 2026-11-13: a block that found the plug-in's channel held or still on the block before was skipped, its
    // notes with it; a note that ended in it sounded on.
    using phazite::test::TestPlugins;
    using Type = phazite::devices::NoteEvent::Type;
    SandboxedPlugins sandboxed;
    juce::String error;
    const std::shared_ptr<phazite::SandboxedInstance> instance =
        SandboxedPlugins::getHost().createInstance (TestPlugins::getDescription (TestPlugins::Kind::synth), {}, 48000.0, 256, error);
    REQUIRE_MESSAGE (instance != nullptr, error);
    phazite::SandboxedProcessor processor (instance);
    processor.prepare (48000.0, 256);
    juce::AudioBuffer<float> buffer (2, 256);

    // A block with its events, as a render plays it (waiting for the plug-in, however busy the machine is) or as the audio
    // device does (never waiting for its channel); how loud it came back.
    const auto play = [&] (std::vector<phazite::devices::NoteEvent> events, bool asRender = true)
    {
        buffer.clear();
        phazite::devices::ProcessContext context;
        context.channels = buffer.getArrayOfWritePointers();
        context.numChannels = 2;
        context.numSamples = buffer.getNumSamples();
        context.events = events;
        context.isRendering = asRender;
        processor.process (context);
        return buffer.getMagnitude (0, 0, buffer.getNumSamples());
    };

    // A note begins, and sounds on.
    CHECK (play ({ { Type::noteOn, 0, 69, 0.8f } }) > 0.1f);
    CHECK (play ({}) > 0.1f);

    // It ends in a block that finds the channel held (as a render, or the plug-in being prepared, holds it): nothing plays.
    REQUIRE (instance->tryHold());
    CHECK (play ({ { Type::noteOff, 10, 69, 0.0f } }, false) == 0.0f);
    instance->let();

    // The next block takes the note's end with it, at its start.
    CHECK (play ({}) == 0.0f);
    CHECK (play ({}) == 0.0f);

    // Of two notes that begin in such blocks, the one that ends in them too isn't played, and the one still held is, from
    // the next block on, until it ends.
    REQUIRE (instance->tryHold());
    play ({ { Type::noteOn, 0, 60, 0.8f }, { Type::noteOn, 0, 64, 0.8f } }, false);
    play ({ { Type::noteOff, 0, 64, 0.0f } }, false);
    instance->let();
    CHECK (play ({}) > 0.1f);
    play ({ { Type::noteOff, 0, 60, 0.0f } });
    CHECK (play ({}) == 0.0f);

    // Playback starting again (a reset) forgets what waited: a note from before it doesn't begin after it.
    REQUIRE (instance->tryHold());
    play ({ { Type::noteOn, 0, 72, 0.8f } }, false);
    instance->let();
    processor.reset();
    CHECK (play ({}) == 0.0f);
}

TEST_CASE ("What a plug-in plays that isn't a number never reaches the mix: its block is as one it didn't play")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestCrash");
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));

    // Its NaN on (its helper has it before the next block, as its state, its Crash then its NaN, says): every block it plays
    // is NaN, and is as one it didn't play: silent.
    song.setParameter (effect, crashNaN, 1.0f);
    phazite::test::dispatchPendingMessages();
    const auto state = getSandboxed (song, effect).getRemote().getState (10000);
    REQUIRE (state.has_value());
    REQUIRE (state->getSize() == 8);
    juce::MemoryInputStream values (*state, false);
    CHECK (values.readFloat() == 0.0f);
    REQUIRE (values.readFloat() == 1.0f);
    const auto audio = playback.play (0.4);
    const auto* samples = audio.getReadPointer (0);
    CHECK (std::all_of (samples, samples + audio.getNumSamples(), [] (float sample) { return std::isfinite (sample); }));
    CHECK (audio.getMagnitude (0, 4800, audio.getNumSamples() - 4800) == 0.0f);
}

namespace
{
// A stand-in's instance opened through the plug-in host, as a song's is, and a processor of Phazite's engine playing it,
// prepared at 48 kHz for blocks of 256.
struct Played
{
    explicit Played (phazite::test::TestPlugins::Kind kind)
    {
        juce::String error;
        instance = SandboxedPlugins::getHost().createInstance (phazite::test::TestPlugins::getDescription (kind), {}, 48000.0, 256, error);
        REQUIRE_MESSAGE (instance != nullptr, error);
        processor = std::make_unique<phazite::SandboxedProcessor> (instance);
        processor->prepare (48000.0, 256);
    }

    // A block of the buffer's, from a sample of it: as a render plays it (waiting for the plug-in), or as the audio device
    // does (never waiting for its channel), with its events.
    void play (juce::AudioBuffer<float>& audio, int from, bool asRender, std::span<const phazite::devices::NoteEvent> events = {})
    {
        float* channels[] = { audio.getWritePointer (0, from), audio.getWritePointer (1, from) };
        phazite::devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = 256;
        context.events = events;
        context.isRendering = asRender;
        processor->process (context);
    }

    // What the playhead stand-in says it heard last.
    phazite::test::TestPlugins::Playhead getHeard() const
    {
        const auto state = instance->getRemote().getState (10000);
        REQUIRE (state.has_value());
        return phazite::test::TestPlugins::Playhead::read (*state);
    }

    // A value set by hand, which the helper has once a request after it is answered.
    void set (const char* ownID, float value) const
    {
        instance->setParameterValue (instance->findParameter (ownID), value);
        REQUIRE (instance->getRemote().getState (10000).has_value());
    }

    std::shared_ptr<phazite::SandboxedInstance> instance;
    std::unique_ptr<phazite::SandboxedProcessor> processor;
};

// A sine on both sides, at a quarter of full scale, for so many samples.
juce::AudioBuffer<float> makeSine (int numSamples, double hertz = 440.0)
{
    juce::AudioBuffer<float> sine (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
        for (int side = 0; side < 2; ++side)
            sine.setSample (side, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * i / 48000.0));

    return sine;
}

// The largest step from one sample to the next on a side, from a sample on.
float getLargestStep (const juce::AudioBuffer<float>& audio, int side, int from)
{
    auto largest = 0.0f;

    for (int i = std::max (1, from); i < audio.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (audio.getSample (side, i) - audio.getSample (side, i - 1)));

    return largest;
}
} // namespace

TEST_CASE ("A block's deadline is a millisecond at least, so that a part of a block cut short isn't late for a round trip")
{
    // The audit of 2026-11-13: a part of a few samples, as the engine cuts at a loop's end and after a count-in, had as
    // many microseconds, less than a round trip to the helper takes.
    using phazite::SandboxedInstance;
    const auto lasting = [] (int samples) { return (std::int64_t) (1.0e9 * samples / 48000.0); };
    CHECK (SandboxedInstance::getDeadlineNanoseconds (512, 512, 48000.0) == lasting (512));
    CHECK (SandboxedInstance::getDeadlineNanoseconds (1, 512, 48000.0) == 1'000'000);
    CHECK (SandboxedInstance::getDeadlineNanoseconds (5, 512, 48000.0) == 1'000'000);
    CHECK (SandboxedInstance::getDeadlineNanoseconds (96, 512, 48000.0) == lasting (96)); // two milliseconds: as long as it lasts

    // Blocks shorter than a millisecond: never longer than one of the device's lasts.
    CHECK (SandboxedInstance::getDeadlineNanoseconds (1, 16, 48000.0) == lasting (16));
}

TEST_CASE ("A late, busy or crashed effect fades to silence as late as it plays, and fades in again: no step at either edge")
{
    // The audit of 2026-11-13: such an effect passed its input through dry, as early as its latency, with a step at both
    // edges; a limiter or a fully wet reverb passed dry is worse than a gap. The latency stand-in delays its sound 64
    // samples.
    SandboxedPlugins sandboxed;
    const auto sine = makeSine (26 * 256);

    // A sine through it: so many blocks it plays, so many it can't be handed (its channel held, as a render holding it or a
    // block still the helper's leaves it), and so many it plays again.
    const auto playWithGap = [&sine] (Played& played, int before, int gap, int after)
    {
        auto audio = sine;

        for (int block = 0; block < before + gap + after; ++block)
        {
            const auto handed = block < before || block >= before + gap;

            if (! handed)
                REQUIRE (played.instance->tryHold());

            played.play (audio, block * 256, handed);

            if (! handed)
                played.instance->let();
        }

        audio.setSize (2, (before + gap + after) * 256, true);
        return audio;
    };

    // The latency stand-in, ten blocks, then six it can't be handed: as it fades out, the sine as late as the plug-in plays
    // it, fading, with no step (none larger than the sine's own, 0.0144 a sample at 440 Hz); silent from a few milliseconds
    // into the gap on.
    Played latency (phazite::test::TestPlugins::Kind::latency);
    REQUIRE (latency.processor->getLatencySamples() == 64);
    const auto fadedOut = playWithGap (latency, 10, 6, 0);

    for (int side = 0; side < 2; ++side)
        CHECK (getLargestStep (fadedOut, side, 128) < 0.016f);

    for (int i = 10 * 256; i < 10 * 256 + 200; i += 7)
        CHECK (std::abs (fadedOut.getSample (0, i)) <= std::abs (sine.getSample (0, i - 64)) + 1.0e-6f);

    CHECK (fadedOut.getMagnitude (0, 11 * 256, 5 * 256) == 0.0f);

    // The effect stand-in (which keeps nothing of the blocks it played, as the latency's delay does), four blocks, four it
    // can't be handed, then four it plays again: silent in the gap, faded in from it with no step, then as it plays.
    Played effect (phazite::test::TestPlugins::Kind::effect);
    const auto fadedIn = playWithGap (effect, 4, 4, 4);

    for (int side = 0; side < 2; ++side)
        CHECK (getLargestStep (fadedIn, side, 1) < 0.016f);

    CHECK (fadedIn.getMagnitude (0, 5 * 256, 3 * 256) == 0.0f);

    for (int i = 9 * 256; i < 12 * 256; i += 37)
        CHECK (fadedIn.getSample (0, i) == doctest::Approx (sine.getSample (0, i)).epsilon (1.0e-4));
}

TEST_CASE ("A block whose channels the helper's side of the memory says otherwise of is as one it didn't play, read no further")
{
    // The audit of 2026-11-13: Phazite read as many channels back as the helper's side of the shared memory said, which a
    // helper gone wrong could leave anything in: reading and writing out of bounds on the audio thread.
    SandboxedPlugins sandboxed;
    Played played (phazite::test::TestPlugins::Kind::synth);
    REQUIRE (played.processor->startsEarly());

    // Two channels handed over, and a third beside them that isn't the block's.
    juce::AudioBuffer<float> buffer (3, 256);
    buffer.clear();
    juce::FloatVectorOperations::fill (buffer.getWritePointer (2), 0.5f, 256);
    const std::vector<phazite::devices::NoteEvent> note { { phazite::devices::NoteEvent::Type::noteOn, 0, 69, 0.8f } };
    phazite::devices::ProcessContext context;
    context.channels = buffer.getArrayOfWritePointers();
    context.numChannels = 2;
    context.numSamples = 256;
    context.events = note;
    context.isRendering = true;

    // Its side of the memory says three came back.
    played.processor->start (context);
    played.instance->getRemote().getChannel().getInfo().numChannels = 3;
    played.processor->process (context);
    CHECK (buffer.getMagnitude (2, 0, 256) == doctest::Approx (0.5f)); // untouched
    CHECK (buffer.getMagnitude (0, 0, 256) == 0.0f);                   // the instrument silent, as for a block it didn't play
}

TEST_CASE ("A plug-in that crashed in a render at another rate is reloaded at the rate it plays at")
{
    // The audit of 2026-11-13: a crashed plug-in kept the rate it was prepared at before it crashed, which its reload
    // opened it at.
    using Note = phazite::devices::NoteEvent;
    SandboxedPlugins sandboxed;
    Played played (phazite::test::TestPlugins::Kind::synth);

    // A render at 96 kHz prepares it so, and it crashes in it; the engine prepares it at 48 kHz again, crashed.
    played.processor->prepare (96000.0, 256);
    played.instance->getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&played] { return played.instance->hasCrashed(); }, 20000));
    played.processor->prepare (48000.0, 256);
    CHECK (played.instance->getPreparedRate() == doctest::Approx (48000.0));

    // Reloaded: at 48 kHz, where A4 is 440 Hz (not 220 Hz, as from a synth at 96 kHz).
    juce::String error;
    REQUIRE_MESSAGE (SandboxedPlugins::getHost().reload (*played.instance, error), error);
    juce::AudioBuffer<float> audio (2, 48 * 256);
    audio.clear();
    const std::vector<Note> note { { Note::Type::noteOn, 0, 69, 0.8f } };

    for (int block = 0; block < 48; ++block)
        played.play (audio, block * 256, true, block == 0 ? std::span<const Note> (note) : std::span<const Note>());

    auto crossings = 0;

    for (int i = 1; i < audio.getNumSamples(); ++i)
        crossings += (audio.getSample (0, i - 1) < 0.0f) != (audio.getSample (0, i) < 0.0f) ? 1 : 0;

    // 0.256 seconds of 440 Hz: 225 crossings.
    CHECK (std::abs (crossings - 225) <= 4);
}

TEST_CASE ("A render of a plug-in that had crashed before it began fails at once, naming the plug-in")
{
    // The audit of 2026-11-13: only a crash or a hang during the render failed it; one before left the plug-in silent in
    // it, or dry, without a word.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }, 20000));

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    REQUIRE (host.finished.size() == 1);
    CHECK (host.finished[0].contains ("Test Effect has crashed"));
    CHECK_FALSE (phazite::isFrozen (song.context, track));
}

TEST_CASE ("A render's first block waits for a block of live playback the plug-in is still on, rather than skip it")
{
    // The audit of 2026-11-13: a render's first block found the channel busy with a late block of live playback and was
    // skipped, the plug-in's sound missing from the render's start.
    SandboxedPlugins sandboxed;
    Played played (phazite::test::TestPlugins::Kind::hang);
    juce::AudioBuffer<float> live (2, 256), rendered (2, 256);
    live.clear();

    // Its Hang on: a live block goes late, and the plug-in is still on it.
    played.set (hangHang, 1.0f);
    played.play (live, 0, false);
    REQUIRE (played.instance->getRemote().getChannel().isBusy());

    // A render's first block, on a thread of its own as a render's is: it waits while the plug-in keeps the live block,
    // which it gives back as its Hang goes off; then its own plays (the hang stand-in passes its sound as it came).
    for (int side = 0; side < 2; ++side)
        juce::FloatVectorOperations::fill (rendered.getWritePointer (side), 0.5f, 256);

    std::atomic<bool> renderedBlock { false };
    std::thread render (
        [&]
        {
            played.play (rendered, 0, true);
            renderedBlock = true;
        });

    phazite::test::dispatchMessagesUntil ([&renderedBlock] { return renderedBlock.load(); }, 200);
    played.instance->setParameterValue (played.instance->findParameter (hangHang), 0.0f);
    render.join();
    CHECK (rendered.getSample (0, 255) == doctest::Approx (0.5f));
    CHECK (rendered.getSample (1, 255) == doctest::Approx (0.5f)); // faded in from the late block's silence before it
    CHECK_FALSE (played.processor->hasFailedInRender());
}

TEST_CASE ("A block through JUCE's processBlock() notices a helper that has gone at once, not at the hang limit")
{
    SandboxedPlugins sandboxed;
    SandboxedPlugins::getHost().setHangLimits (SandboxedPlugins::messageHangMilliseconds, 20000); // for the helper it starts
    Played played (phazite::test::TestPlugins::Kind::crash);

    // Its Crash on: the block crashes its helper.
    played.set (crashCrash, 1.0f);
    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto from = juce::Time::getMillisecondCounterHiRes();
    played.instance->processBlock (buffer, midi);
    CHECK (juce::Time::getMillisecondCounterHiRes() - from < 10000.0);
    CHECK (played.instance->hasCrashed());
}

TEST_CASE ("A plug-in is told that it plays offline for a render's blocks, and that it plays live again as the render gives it back")
{
    // The audit of 2026-11-13: a plug-in that streams from disk, loads in the background or has a mode for rendering was
    // never told that an export, a freeze or a bounce played it faster than real time.
    SandboxedPlugins sandboxed;
    Played played (phazite::test::TestPlugins::Kind::playhead);
    juce::AudioBuffer<float> audio (2, 256);
    audio.clear();

    // A render's block: told before it plays it.
    played.play (audio, 0, true);
    CHECK (played.getHeard().playedOffline);
    CHECK (played.getHeard().offline);

    // Given back by the render (as OfflineRender does, finished or cancelled): told at once that it plays live again (the
    // helper has the notice before the state's request after it), so that the first live block is played live, whole (the
    // stand-in passes its sound), not after a switch that would make it late.
    played.instance->playLive();
    CHECK_FALSE (played.getHeard().offline);

    for (int side = 0; side < 2; ++side)
        juce::FloatVectorOperations::fill (audio.getWritePointer (side), 0.5f, 256);

    played.play (audio, 0, false);
    CHECK_FALSE (played.getHeard().playedOffline);

    for (int side = 0; side < 2; ++side)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (side), 256);
        CHECK (range.getStart() == doctest::Approx (0.5f));
        CHECK (range.getEnd() == doctest::Approx (0.5f));
    }

    // Not given back (as a render that plays the song's plug-ins without lending them may leave it): told so as live
    // blocks come, within a few of them.
    played.play (audio, 0, true);
    REQUIRE (played.getHeard().offline);
    CHECK (phazite::test::dispatchMessagesUntil (
        [&]
        {
            played.play (audio, 0, false);
            return ! played.getHeard().offline;
        },
        5000));
}

TEST_CASE ("A plug-in is told that it plays offline as its track is frozen, and live again once it is")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 1.0);
    const auto effect = song.addEffect (track, "PhaziteTestPlayhead");

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    CHECK (phazite::isFrozen (song.context, track));

    // The last block it played was the freeze's, which it was told it played offline; given back, it is told it plays live
    // again, though it plays no more (its track plays its render now).
    const auto read = [&song, effect]
    {
        const auto state = getSandboxed (song, effect).getRemote().getState (10000);
        REQUIRE (state.has_value());
        return phazite::test::TestPlugins::Playhead::read (*state);
    };

    CHECK (read().playedOffline);
    CHECK (phazite::test::dispatchMessagesUntil ([&read] { return ! read().offline; }, 5000));
}

TEST_CASE ("A crashed plug-in that is switched off fails no render: it isn't heard")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 1.0);
    const auto effect = song.addEffect (track, "PhaziteTestEffect");
    phazite::setDeviceOn (song.context, effect, false);
    auto& instance = getSandboxed (song, effect);
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }, 20000));

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    CHECK (phazite::isFrozen (song.context, track));
}
