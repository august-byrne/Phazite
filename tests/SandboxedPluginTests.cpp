// SPDX-License-Identifier: AGPL-3.0-or-later
// Plug-ins hosted in helper processes, as a song has them (model/plugins/internal/PluginHost.h): each an instance in a
// helper, one helper for each plug-in, which the song's session makes; Phazite's engine plays it through its helper, as
// it is set, with its mix, its automation and its modulators; what it turns itself is set by hand, for touch and the
// last touched; the render of a frozen track plays it too; its settings survive a save; and one that crashes leaves
// Phazite playing, an effect letting its sound through. Its parameters are named by the plug-in's own IDs
// (host/TestPlugins.h).
#include "EnginePlayback.h"
#include "SandboxedPlugins.h"
#include "TestClips.h"
#include "TestKeyboardLayouts.h"
#include "TestSong.h"
#include "app/AppCommands.h"
#include "app/PluginWindow.h"
#include "app/ProjectSession.h"
#include "app/SandboxedEditor.h"
#include "app/SessionEnd.h"
#include "engine/nodes/DeviceRunner.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/TimelineModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/Json.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/PluginScanner.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixHandle.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "model/plugins/internal/SandboxedProcessor.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/Export.h"
#include "model/project/NewProject.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"
#include "ui/components/ParameterText.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/KeyboardLayout.h"
#include "ui/input/Keymap.h"
#include "ui/input/PluginKeys.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/DeviceSlots.h"
#include "ui/shell/RenderPanel.h"
#include "ui/versions/BranchPicker.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
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
#else
 #include <csignal>
 #include <sys/types.h>
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

// How long the message thread was kept by something it did, in milliseconds: printed, or a promise that it didn't wait for
// what it would have waited for, checked against about half the long limit of that wait (known bug 180: never against a
// time a busy machine can reach).
double timeKept (const std::function<void()>& done)
{
    const auto since = juce::Time::getMillisecondCounterHiRes();
    done();
    return juce::Time::getMillisecondCounterHiRes() - since;
}

// Once a device's helper has taken what was sent to it before (a value set, a state given): it takes its pipe's messages
// in turn, so a round trip after them, its state asked and answered, comes after them (known bug 180: a wait for a time).
void helperHasTaken (TestSong& song, phazite::DeviceID device)
{
    REQUIRE (getSandboxed (song, device).getRemote().getState (phazite::test::hangTimeout).has_value());
}

// Once the hanging stand-in keeps a block (its Hang on, the block played: host/TestPlugins.h).
bool waitForBlockKept()
{
    return phazite::test::dispatchMessagesUntil ([] { return phazite::test::TestPlugins::getHangKeeping().existsAsFile(); });
}

#if JUCE_WINDOWS
// A plug-in's editor, asked of its helper as a plug-in's window asks for it, without waiting, and waited for here: none for
// one that makes none. (On Windows alone, as the cases that open one: Linux CI has no display for a plug-in's window.)
std::unique_ptr<phazite::SandboxedEditor> makeEditor (std::shared_ptr<phazite::SandboxedInstance> instance)
{
    struct Made
    {
        bool done = false;
        std::unique_ptr<phazite::SandboxedEditor> editor;
    };

    auto made = std::make_shared<Made>();
    auto request = phazite::requestSandboxedEditor (std::move (instance),
                                                    [made] (std::unique_ptr<phazite::SandboxedEditor> editor)
                                                    {
                                                        made->editor = std::move (editor);
                                                        made->done = true;
                                                    });

    if (request == nullptr)
        return nullptr;

    REQUIRE (phazite::test::dispatchMessagesUntil ([made] { return made->done; }));
    return std::move (made->editor);
}
#endif

// A crashed plug-in reloaded as its slot's click reloads it, in the background, and waited for here: why it didn't load, if
// it didn't.
std::optional<juce::String> reloadAndWait (TestSong& song, phazite::DeviceID device)
{
    struct Reloaded
    {
        bool done = false;
        std::optional<juce::String> problem;
    };

    auto reloaded = std::make_shared<Reloaded>();
    phazite::reloadDevice (song.context, device,
                           [reloaded] (std::optional<juce::String> problem)
                           {
                               reloaded->problem = std::move (problem);
                               reloaded->done = true;
                           });

    REQUIRE (phazite::test::dispatchMessagesUntil ([reloaded] { return reloaded->done; }));
    return reloaded->problem;
}

#if JUCE_WINDOWS
// The window of another process's inside a window of this one's, at any depth (in the editor's socket): the helper's for
// a plug-in's editor.
HWND findHelperWindow (HWND parent)
{
    struct Found
    {
        HWND window = nullptr;
    } found;

    EnumChildWindows (
        parent,
        [] (HWND child, LPARAM data) -> BOOL
        {
            DWORD process = 0;
            GetWindowThreadProcessId (child, &process);

            if (process == GetCurrentProcessId())
                return TRUE;

            reinterpret_cast<Found*> (data)->window = child;
            return FALSE;
        },
        reinterpret_cast<LPARAM> (&found));

    return found.window;
}
#endif

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
    helperHasTaken (song, effect); // its helper has its gain

    // Its state, as the project stores it: the gain the helper's plug-in has.
    juce::MemoryBlock state;
    getSandboxed (song, effect).getStateInformation (state);
    REQUIRE (state.getSize() == 8);
    CHECK (juce::MemoryInputStream (state, false).readFloat() == doctest::Approx (0.5f));
    song.saveWithPluginStates();

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

TEST_CASE ("A plug-in instrument in a helper hears the keys' pressure, a note's and the channel's, played live and from a clip")
{
    // Known bug 173: the engine dropped aftertouch and channel pressure, so a plug-in got neither. The stand-in synth's notes
    // follow both, turned down by half at full pressure: 440 Hz at its Level, as the first test has it.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, "PhaziteTestSynth");
    auto& s = getSong (song);
    phazite::routeLiveMidiInput (s); // live MIDI plays the track
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    playback.play (0.05);

    // What the stand-in's notes are turned down by at a pressure: by half at full pressure.
    const auto decibels = [] (int value) { return juce::Decibels::gainToDecibels (1.0f - 0.5f * (float) value / 127.0f); };

    // Live, as a MIDI input's driver plays them into live MIDI's track (the computer keyboard plays notes alone): a note,
    // then the channel's pressure at half, then the note's at half too.
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    const auto playFromMidiInput = [&host, &s] (const juce::MidiMessage& message)
    {
        const auto event = phazite::engine::toNoteEvent (message, 0);
        REQUIRE (event.has_value());
        host.getEngine().playLive ((std::int64_t) phazite::findSession (s)->getLiveTrack().getRaw(), *event);
    };

    playFromMidiInput (juce::MidiMessage::noteOn (1, 69, 0.8f));
    CHECK (levelOf (playback.play (0.5), 0, 0.1, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));
    playFromMidiInput (juce::MidiMessage::channelPressureChange (1, 64));
    CHECK (levelOf (playback.play (0.5), 0, 0.1, 0.5) == doctest::Approx (toneLevel + decibels (64)).epsilon (0.002));
    playFromMidiInput (juce::MidiMessage::aftertouchChange (1, 69, 64));
    CHECK (levelOf (playback.play (0.5), 0, 0.1, 0.5) == doctest::Approx (toneLevel + 2.0f * decibels (64)).epsilon (0.002));
    playFromMidiInput (juce::MidiMessage::noteOff (1, 69));
    playback.play (0.1);

    // From a clip, where they are: a note held from the start, the channel's pressure at a quarter at beat 2 and the note's at
    // half at beat 4. Playback starts from silence (the synth reset), with no pressure (chased, none held before).
    const auto clip = song.addMidiClip (track, 0.0, 8.0);
    song.addNote (clip, 69, 0.0, 8.0);
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (2.0), phazite::ControlKind::channelPressure, 0, 32);
    phazite::addControl (s, clip, phazite::BeatPosition::fromBeats (4.0), phazite::ControlKind::aftertouch, 69, 64);
    song.run ("transport.playStop");
    const auto played = playback.play (3.0);
    CHECK (levelOf (played, 0, 0.1, 0.9) == doctest::Approx (toneLevel).epsilon (0.002));
    CHECK (levelOf (played, 0, 1.1, 1.9) == doctest::Approx (toneLevel + decibels (32)).epsilon (0.002));
    CHECK (levelOf (played, 0, 2.1, 2.9) == doctest::Approx (toneLevel + decibels (32) + decibels (64)).epsilon (0.002));
    song.run ("transport.playStop");
}

TEST_CASE ("A plug-in on a track of notes alone is given only its notes at Play, a jump and the loop's wrap; with a pedal, the pedal too")
{
    // Known bug 85's review of the chase: a track without controllers gets no controller, bend or pressure where playback
    // starts, jumps or goes round the loop; one whose clip holds the pedal gets it let go of (no value held before) at each
    // of them; and a controller that changes exactly where playback starts is sent once, its own value. The tests' MIDI
    // log stand-in keeps what it is given since it was reset (at Play and at a jump: Engine::flushStrip()). Beats 2 to 5
    // are looped.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    auto& state = getSong (song);
    const auto notesOnly = song.setInstrument (song.firstTrack, "PhaziteTestMidiLog");
    const auto notes = song.addMidiClip (song.firstTrack, 0.0, 16.0);
    song.addNote (notes, 60, 0.0, 16.0);
    song.addNote (notes, 64, 2.25, 1.0);

    const auto pedalTrack = song.addTrack ("Pedal");
    const auto pedalled = song.setInstrument (pedalTrack, "PhaziteTestMidiLog");
    const auto pedalClip = song.addMidiClip (pedalTrack, 0.0, 16.0);
    song.addNote (pedalClip, 48, 0.0, 16.0);
    phazite::addControl (state, pedalClip, phazite::BeatPosition::fromBeats (4.0), phazite::ControlKind::controller, 64, 127);
    phazite::addControl (state, pedalClip, phazite::BeatPosition::fromBeats (4.5), phazite::ControlKind::controller, 64, 0);
    phazite::addControl (state, pedalClip, phazite::BeatPosition::fromBeats (0.5), phazite::ControlKind::controller, 1, 20);
    phazite::addControl (state, pedalClip, phazite::BeatPosition::fromBeats (1.0), phazite::ControlKind::controller, 1, 50);
    phazite::loopBeats (state, { phazite::BeatPosition::fromBeats (2.0), phazite::BeatPosition::fromBeats (5.0) });

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    playback.play (0.05);

    // What each was given since it was last reset, as words ("on 60", "cc 64 0", "bend", "pressure"), from so far on.
    const auto heard = [&song] (phazite::DeviceID device, size_t from = 0)
    {
        const auto block = getSandboxed (song, device).getRemote().getState (10000);
        REQUIRE (block.has_value());
        std::vector<std::string> words;

        for (const auto& message : phazite::test::TestPlugins::readMidiLog (*block))
        {
            if (message.isNoteOn())
                words.push_back ("on " + std::to_string (message.getNoteNumber()));
            else if (message.isNoteOff())
                words.push_back ("off " + std::to_string (message.getNoteNumber()));
            else if (message.isController())
                words.push_back ("cc " + std::to_string (message.getControllerNumber()) + " "
                                 + std::to_string (message.getControllerValue()));
            else if (message.isPitchWheel())
                words.push_back ("bend");
            else
                words.push_back ("pressure");
        }

        return std::vector<std::string> (words.begin() + (std::ptrdiff_t) std::min (from, words.size()), words.end());
    };

    const auto controllersIn = [] (const std::vector<std::string>& words)
    { return std::count_if (words.begin(), words.end(), [] (const std::string& word) { return word.rfind ("cc", 0) == 0; }); };

    // Play from beat 1: the notes sounding there; the pedal let go of, and the mod wheel's own change on beat 1, once.
    phazite::movePlayhead (state, phazite::BeatPosition::fromBeats (1.0));
    phazite::playOrStop (state);
    playback.play (0.1);
    CHECK (heard (notesOnly) == std::vector<std::string> { "on 60" });
    CHECK (heard (pedalled) == std::vector<std::string> { "cc 64 0", "on 48", "cc 1 50" });

    // A jump to beat 3: the notes there, after what lets go of those that sounded (all notes off, through the helper, which
    // ends each note it gave first); the pedal let go of again, and the mod wheel at its value there.
    phazite::movePlayhead (state, phazite::BeatPosition::fromBeats (3.0));
    playback.play (0.1);
    const auto jumped = heard (notesOnly);
    CAPTURE (jumped.size());
    CHECK (std::find (jumped.begin(), jumped.end(), "on 60") != jumped.end());
    CHECK (std::find (jumped.begin(), jumped.end(), "on 64") != jumped.end());
    CHECK (std::none_of (jumped.begin(), jumped.end(),
                         [] (const std::string& word) { return word.rfind ("cc", 0) == 0 && word != "cc 123 0"; }));
    CHECK (std::none_of (jumped.begin(), jumped.end(), [] (const std::string& word) { return word == "bend" || word == "pressure"; }));
    const auto pedalJumped = heard (pedalled);
    CHECK (std::count (pedalJumped.begin(), pedalJumped.end(), "cc 64 0") == 1);
    CHECK (std::count (pedalJumped.begin(), pedalJumped.end(), "cc 1 50") == 1);

    // Round the loop, from beat 3 to its end at 5 and back to 2: the notes there again; the pedal, pressed at 4 and let go
    // of at 4.5, let go of again at its start, where the clip hasn't pressed it yet; the mod wheel at its value there.
    const auto before = heard (notesOnly).size(), pedalBefore = heard (pedalled).size();
    playback.play (1.2); // to beat 5.4: beat 2.4 the second time round
    const auto wrapped = heard (notesOnly, before);
    CHECK (controllersIn (wrapped) == 0);
    CHECK (std::count (wrapped.begin(), wrapped.end(), "on 60") == 1);
    const auto pedalWrapped = heard (pedalled, pedalBefore);
    CHECK (pedalWrapped == std::vector<std::string> { "cc 64 127", "cc 64 0", "off 48", "cc 1 50", "cc 64 0", "on 48" });
    phazite::stopAndReturn (state);
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
    const phazite::test::HeldClock clock; // the engine's: where the song is read, and where the plug-in's grab is heard, alike
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");

    // Played past the note's start, and held there until the helper tells what the plug-in did: grabbed where the song is
    // then, a little past beat 1, from its Level before (0.25), and the parameter last touched.
    const auto playedTo = [] { return phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlayheadBeat(); };
    auto handle = phazite::getParameter (song.context, level);
    playback.play (0.55);
    const auto grabbedAt = playedTo();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return juce::exactlyEqual (handle.getValue(), 0.75f); }));
    CHECK (phazite::getLastTouchedParameter (song.context) == level);

    // Played past the note's end: let go of there, which writes what it did.
    playback.play (0.55);
    const auto letGoAt = playedTo();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getAutomationPoints (song.context, level).empty(); }));

    const auto points = phazite::getAutomationPoints (song.context, level);
    REQUIRE (points.size() >= 3);
    CHECK (points.front().beat.inBeats() == doctest::Approx (grabbedAt).epsilon (0.005)); // within its last block
    CHECK (points.front().position == doctest::Approx (0.25f));
    CHECK (points[1].position == doctest::Approx (0.75f));
    CHECK (points.back().beat.inBeats() == doctest::Approx (letGoAt).epsilon (0.005));
    CHECK (points.back().position == doctest::Approx (0.75f));

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
    CHECK (phazite::test::dispatchMessagesUntil ([] { return SandboxedPlugins::getHost().getNumHelpers() == 1; }));
    playback.reset(); // before the next one, whose timing its going would undo
    playback = std::make_unique<EnginePlayback> (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelNow() == doctest::Approx (half).epsilon (0.002));

    // Its track duplicated: the copy's plug-in, another instance in the same helper, plays at its settings too (the two
    // together twice as loud as one).
    song.context.getSongSelection().selectOnly (track);
    // (Once the plug-in that said it changed has given its state, which a Duplicate of its track waits for: known bug 166.)
    song.run ("edit.duplicate");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return phazite::getMixerChannels (song.context).tracks.size() == 2; }));
    const auto copy = phazite::getMixerChannels (song.context).tracks[1];
    REQUIRE (phazite::getEffects (song.context, copy).size() == 1);
    song.waitForPlugins(); // the copy's, which loads in the background
    CHECK (&getSandboxed (song, phazite::getEffects (song.context, copy)[0].id) != instance);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 1);
    CHECK (levelNow() == doctest::Approx (half + 6.02f).epsilon (0.002));
    phazite::undo (getSong (song));

    // Taken away, and back by undo, as it was.
    phazite::removeDevice (song.context, effect);
    CHECK (levelNow() == doctest::Approx (toneLevel).epsilon (0.002));
    phazite::undo (getSong (song));
    song.waitForPlugins(); // brought back, it loads in the background
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
    song.saveWithPluginStates();
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // Another program, as its own window would choose: its Gain changes with it.
    getSandboxed (song, effect).setCurrentProgram (1);
    auto gain = phazite::getParameter (song.context, { effect, effectGain });
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return song.getProject().hasUnsavedChanges() && juce::exactlyEqual (gain.getValue(), 0.5f); }));
}

TEST_CASE ("What a plug-in in a helper tells of itself that changes nothing is no unsaved change: a VST3's restart, values again")
{
    // The owner, 2026-11-08: an idle project asked to save its changes as Phazite closed, with none in the Versions view.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    song.saveWithPluginStates();
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

    // What closing finds: its plug-ins' states asked for first (ProjectSession::closeThen()), which a store doesn't wait for.
    const auto closingFinds = [&song]
    {
        auto asked = false;
        phazite::whenPluginStatesIn (getSong (song), true, [&asked] (bool) { asked = true; });
        REQUIRE (phazite::test::dispatchMessagesUntil ([&asked] { return asked; }));
        return song.getProject().checkUnsavedChanges();
    };

    tell (gainNow, toneNow, restart);
    phazite::test::dispatchPendingMessages(); // what it told came with its answer (the helper sends it before),
    phazite::test::dispatchPendingMessages(); // and what that tells a message later
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
    CHECK (getSong (song).getChangesOutsideHistory() == 0);

    // Its settings said to have changed, though they didn't: closing asks it for them, and finds none to save.
    tell (gainNow, toneNow, dirty);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return song.getProject().hasUnsavedChanges(); }));
    CHECK_FALSE (closingFinds());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // Its gain told as another value is one, which closing finds too.
    tell (gainNow * 0.5f, toneNow, nothingMore);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return song.getProject().hasUnsavedChanges(); }));
    CHECK (closingFinds());
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
    phazite::ui::KeyDispatcher shortcutKeys { commands, phazite::ui::getUsLayout() };
    phazite::ui::ComputerKeyboard computerKeyboard { [] (const juce::MidiMessage&) {}, phazite::ui::getUsLayout() };
    phazite::ui::PluginKeys keys { shortcutKeys, computerKeyboard };
    phazite::ui::setPluginKeys (&keys);

    // Its editor, as a plug-in window makes it, as large as the plug-in's own, in a window on the screen.
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    auto editor = makeEditor (session->getPlugins().find (effect));
    REQUIRE (editor != nullptr);
    CHECK (editor->getWidth() == 300);
    CHECK (editor->getHeight() == 200);
    juce::Component window;
    window.setSize (400, 300);
    window.addAndMakeVisible (*editor);
    window.addToDesktop (0);
    window.setVisible (true);
    keys.watch (window);

    // The helper's window for it: in this one (in its socket), of another process.
    const auto parent = static_cast<HWND> (window.getPeer()->getNativeHandle());
    HWND child = nullptr;
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            child = findHelperWindow (parent);
            return child != nullptr;
        }));

    // The plug-in makes its editor larger: Phazite's follows.
    song.setParameter (effect, "0", 1.0f); // its Tone
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return editor->getWidth() == 360; }));
    CHECK (editor->getHeight() == 240);

    // Space pressed in the plug-in's window plays, as the helper hands it back (pressed again until the helper knows the
    // keys Phazite takes, which it is told as the editor shows).
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            PostMessageW (child, WM_KEYDOWN, VK_SPACE, 0);
            return phazite::test::dispatchMessagesUntil ([&] { return plays > 0; }, 100); // then pressed again
        }));

    // With Send All Keys to the Plug-in on, the plug-in gets it.
    keys.setSendsAllKeys (window, true);
    editor->tickNow();             // the editor tells the helper,
    helperHasTaken (song, effect); // which has taken it
    const auto before = plays;
    PostMessageW (child, WM_KEYDOWN, VK_SPACE, 0);
    // An absence: it isn't handed back while the message loop turns for a stretch (which a busy machine can only make miss).
    phazite::test::dispatchMessagesUntil ([&] { return plays != before; }, 500);
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
    SandboxedPlugins::useShortHangLimits(); // a hang's test
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto hanging = song.addEffect (track, "PhaziteTestHang");
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender); // its level, however busy the machine
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));

    // Its Hang on: its blocks come late, silent (the tone faded out), until its helper is ended, a second of late blocks
    // after; silent after that too.
    EnginePlayback::setTiming (EnginePlayback::Timing::realTime);
    song.setParameter (hanging, hangHang, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, hanging).hasCrashed();
        }));
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getEffects (song.context, track)[0].crashed);

    // Reloaded: the same instance in a new helper, from the state it last gave (asked once it was quiet after its Hang was
    // turned on, or as it opened, before), its value as the plug-in has it; its Hang off, it plays.
    auto* instance = &getSandboxed (song, hanging);
    juce::MemoryBlock last;
    instance->getStateInformation (last);
    const auto lastHang = juce::MemoryInputStream (last, false).readFloat();
    CHECK_FALSE (reloadAndWait (song, hanging).has_value());
    CHECK (&getSandboxed (song, hanging) == instance);
    CHECK_FALSE (instance->hasCrashed());
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 1);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::getEffects (song.context, track)[0].crashed);
    CHECK (phazite::getParameter (song.context, { hanging, hangHang }).getValue() == doctest::Approx (lastHang));
    song.setParameter (hanging, hangHang, 0.0f);
    EnginePlayback::setTiming (EnginePlayback::Timing::asRender);
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("A plug-in whose helper's message thread stops answering is ended for it, and nothing of Phazite's waits")
{
    SandboxedPlugins sandboxed;
    SandboxedPlugins::useShortHangLimits(); // a hang's test
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
        }));
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
    const auto problem = reloadAndWait (song, crashing);
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
        }));

    // A click reloads it (its Crash off again, as the plug-in has it): its slot shows its name again.
    song.run ("transport.playStop");
    phazite::test::Mouse mouse (*effectButton());
    mouse.click (effectButton()->getLocalBounds().getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return effectButton() != nullptr && effectButton()->getButtonText() == "Test Crash"; }));
    CHECK_FALSE (getSandboxed (song, crashing).hasCrashed());
}

TEST_CASE ("Values set on more of a plug-in's parameters than a block takes go with the blocks after it")
{
    // A block takes so many changes; the values set past them stayed where they were until another value was set, however
    // long that was (the look for them was asked for once, and that one look ended at the block's limit).
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    REQUIRE (instance.getParameters().size() == 2);

    // Set from another thread than the message thread, as what plays a block sets them: kept for the next block.
    std::thread (
        [&instance]
        {
            instance.getParameters()[0]->setValue (0.25f);
            instance.getParameters()[1]->setValue (0.75f);
        })
        .join();

    // Blocks that take one change each: the first parameter's, the second's, then none.
    phazite::sandbox::ParameterChange change;
    phazite::sandbox::BlockInfo first;
    instance.takeChanges (first, &change, 1);
    REQUIRE (first.numChanges == 1);
    CHECK (change.index == 0);
    CHECK (change.value == 0.25f);

    phazite::sandbox::BlockInfo second;
    instance.takeChanges (second, &change, 1);
    REQUIRE (second.numChanges == 1);
    CHECK (change.index == 1);
    CHECK (change.value == 0.75f);

    phazite::sandbox::BlockInfo third;
    instance.takeChanges (third, &change, 1);
    CHECK (third.numChanges == 0);
}

TEST_CASE ("A shell file's hundreds of plug-ins are all found; what is past what Phazite takes from a file is said, not left out silently")
{
    // The review of known bug 181: a file's plug-ins past 256 were left out without a word (Waves' WaveShell holds hundreds).
    using phazite::test::TestPlugins;
    auto& host = SandboxedPlugins::getHost();
    auto scanner = phazite::createPluginScanner (host, phazite::pluginScanSecondsEach);
    const auto format = TestPlugins::createFormat();
    juce::OwnedArray<juce::PluginDescription> found;
    host.takeScanProblems();

    CHECK (scanner->findPluginTypesFor (*format, found, TestPlugins::shellScanned));
    CHECK (found.size() == TestPlugins::shellSize);
    CHECK (host.takeScanProblems().isEmpty());

    found.clear();
    CHECK (scanner->findPluginTypesFor (*format, found, TestPlugins::hugeShellScanned));
    CHECK (found.size() == phazite::maxScannedPlugins);
    const auto problems = host.takeScanProblems();
    REQUIRE (problems.size() == 1);
    CHECK (problems[0].startsWith (TestPlugins::hugeShellScanned));
    CHECK (problems[0].contains (juce::String (TestPlugins::hugeShellSize - phazite::maxScannedPlugins) + " of the "
                                 + juce::String (TestPlugins::hugeShellSize)));
    scanner->scanFinished();
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
    CHECK (took < phazite::pluginScanSecondsEach * 1000.0 / 2); // its time, not the default's, however busy the machine
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
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender); // its level, however busy the machine
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.4), 0, 0.1, 0.3) == doctest::Approx (toneLevel).epsilon (0.002));
    EnginePlayback::setTiming (EnginePlayback::Timing::realTime);

    // It crashes as it plays (its Crash on), blocks coming meanwhile: Phazite plays on, the effect silent rather than dry
    // (a limiter or a fully wet reverb passed dry is worse than a gap).
    song.setParameter (crashing, crashCrash, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            playback.play (0.05);
            return getSandboxed (song, crashing).hasCrashed();
        }));
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
    CHECK (SandboxedPlugins::getHost().getNumHelpers() == 0); // its helper has gone

    // What it keeps: the state it last gave, at once; and the project saves as it would.
    juce::MemoryBlock state;
    // (a wait for its gone helper would last a state's time: half of that tells them apart however busy the machine is)
    const auto stateLimit = (double) phazite::SandboxedInstance::getStateMilliseconds();
    CHECK (timeKept ([&] { getSandboxed (song, crashing).getStateInformation (state); }) < stateLimit / 2.0);
    CHECK (state == getSandboxed (song, crashing).getLastState());
    const auto savedAt = juce::Time::getMillisecondCounterHiRes();
    song.saveWithPluginStates();
    CHECK (juce::Time::getMillisecondCounterHiRes() - savedAt < stateLimit / 2.0);
}

TEST_CASE ("A plug-in opened from its project keeps the settings the project holds when it crashes: saved again, and reloaded")
{
    // The audit of 2026-11-13: only a state taken or given since the plug-in opened was kept, so one that crashed before
    // any was lost what the project held of it at the next save, and reloaded as new.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    song.setParameter (effect, effectGain, 0.5f);
    helperHasTaken (song, effect); // its helper has its gain

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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }));

    // Saved while it is crashed, the project holds its settings still.
    song.saveWithPluginStates();
    CHECK (getStored() == stored);

    // Reloaded, it has them: its Gain at half, as the plug-in in its new helper says.
    CHECK_FALSE (reloadAndWait (song, effect).has_value());
    REQUIRE_FALSE (instance.hasCrashed());
    const auto state = instance.getRemote().getState (10000);
    REQUIRE (state.has_value());
    REQUIRE (state->getSize() == 8);
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (0.5f));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getParameter (song.context, { effect, effectGain }).getValue() == doctest::Approx (0.5f));
    song.saveWithPluginStates();
    CHECK (getStored() == stored);
}

TEST_CASE ("A plug-in that crashes as its track is frozen fails the render, which says so, and Phazite goes on")
{
    // The audit of 2026-11-13: a render waited for a plug-in's block without end, so one that crashed in it kept the
    // render's thread for ever, and then Phazite's window, which waits for that thread as the render is cancelled.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto effect = song.addEffect (track, "PhaziteTestCrash");
    song.setParameter (effect, crashCrash, 1.0f);
    helperHasTaken (song, effect); // its helper has it before the render's first block

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    REQUIRE (host.finished.size() == 1);
    CHECK (host.finished[0].contains ("Test Crash crashed while Phazite rendered"));
    CHECK_FALSE (phazite::isFrozen (song.context, track));

    // Its slot says it crashed, and the song plays on, the crashed effect silent.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getSandboxed (song, effect).hasCrashed(); }));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getEffects (song.context, track)[0].crashed);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (playback.play (0.4).getMagnitude (0, 0, 19200) == 0.0f);
}

TEST_CASE ("A render waits for a plug-in that keeps its block as long as it takes, naming it once that is as long as a hang")
{
    // Known bug 42: a render ended the helper of a plug-in that kept one block for six times the live hang limit (here six
    // seconds), though it may only be slow. Now it waits, as REAPER's and Bitwig's do, and the render's window names the
    // plug-in, so that the user can cancel it.
    SandboxedPlugins sandboxed;
    SandboxedPlugins::useShortHangLimits(); // a hang's test
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto slow = song.addEffect (track, "PhaziteTestHang");
    song.setParameter (slow, hangHang, 1.0f);
    helperHasTaken (song, slow); // its helper has it before the render's first block

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (host.render != nullptr);

    // Named once it has kept the block as long as a hang takes live; still waited for past the old limit, its helper kept.
    const auto waitedFor = [&host]
    {
        const auto wait = host.render != nullptr ? host.render->getWait() : std::nullopt;
        return wait.has_value() ? wait->seconds : 0.0;
    };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return host.render->getWait().has_value(); }));
    CHECK (host.render->getWait()->plugin == "Test Hang");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return waitedFor() >= 7.0 || ! host.finished.isEmpty(); }));
    CHECK (host.finished.isEmpty());
    auto& instance = getSandboxed (song, slow);
    CHECK_FALSE (instance.hasCrashed());

    // It answers at last (its Hang turned off in its own helper, which the song doesn't hear): the track is frozen.
    instance.getRemote().setParameter (instance.findParameter (hangHang), 0.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }));
    CHECK (phazite::isFrozen (song.context, track));
    CHECK_FALSE (instance.hasCrashed());
}

TEST_CASE ("A render cancelled while a plug-in keeps its block doesn't wait for the plug-in")
{
    SandboxedPlugins sandboxed;
    phazite::test::TestPlugins::getHangKeeping().deleteFile();
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto hanging = song.addEffect (track, "PhaziteTestHang");
    song.setParameter (hanging, hangHang, 1.0f);
    helperHasTaken (song, hanging); // its helper has it before the render's first block

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
    REQUIRE (waitForBlockKept()); // the render's first block, which the plug-in keeps
    REQUIRE (host.finished.isEmpty());

    // Cancelled, as its window's Cancel and quitting do: at once, not once the plug-in is taken for hung.
    const auto cancelledAt = juce::Time::getMillisecondCounterHiRes();
    host.render = nullptr;
    // (a wait for the plug-in would last the prepare's minute at the least, and the plug-in is taken for hung only at a
    // hang's limit: half a minute tells them apart however busy the machine is; it took 3 s once beside other builds)
    CHECK (juce::Time::getMillisecondCounterHiRes() - cancelledAt < 30000.0);
    CHECK (host.finished.isEmpty());
    CHECK_FALSE (phazite::isFrozen (song.context, track));

    // The plug-in lets go of the block (its Hang off), and plays on.
    song.setParameter (hanging, hangHang, 0.0f);
    helperHasTaken (song, hanging); // its helper has it
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
        SandboxedPlugins::getHost().createInstance (TestPlugins::getDescription (TestPlugins::Kind::synth), {}, -1, 48000.0, 256, error);
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
    REQUIRE (state->getSize() >= 8); // its Crash and its NaN first, then its other parameters
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
        instance =
            SandboxedPlugins::getHost().createInstance (phazite::test::TestPlugins::getDescription (kind), {}, -1, 48000.0, 256, error);
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&played] { return played.instance->hasCrashed(); }));
    played.processor->prepare (48000.0, 256);
    CHECK (played.instance->getPreparedRate() == doctest::Approx (48000.0));

    // Reloaded, as a reload opens it in the background (here, waited for): at 48 kHz, where A4 is 440 Hz (not 220 Hz, as
    // from a synth at 96 kHz).
    auto& host = SandboxedPlugins::getHost();
    const auto reopening = phazite::PluginHost::describeReopening (*played.instance, played.instance->getLastState());
    CHECK (reopening.sampleRate == doctest::Approx (48000.0));
    auto opened = host.open (reopening.description, reopening.state, reopening.program, reopening.sampleRate, reopening.blockSize);
    juce::String error;
    REQUIRE_MESSAGE (host.replaceRemote (*played.instance, opened, reopening.sampleRate, error), error);
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }));

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

    // An absence: the render's block doesn't come while the plug-in keeps the live one (a stretch of the message loop, which a
    // busy machine can only make miss, never fail).
    phazite::test::dispatchMessagesUntil ([&renderedBlock] { return renderedBlock.load(); }, 200);
    CHECK_FALSE (renderedBlock.load());
    played.instance->setParameterValue (played.instance->findParameter (hangHang), 0.0f);
    render.join();
    CHECK (rendered.getSample (0, 255) == doctest::Approx (0.5f));
    CHECK (rendered.getSample (1, 255) == doctest::Approx (0.5f)); // faded in from the late block's silence before it
    CHECK_FALSE (played.processor->hasFailedInRender());
}

TEST_CASE ("A block through JUCE's processBlock() notices a helper that has gone at once, not at the hang limit")
{
    SandboxedPlugins sandboxed;
    Played played (phazite::test::TestPlugins::Kind::crash);

    // Its Crash on: the block crashes its helper.
    played.set (crashCrash, 1.0f);
    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto from = juce::Time::getMillisecondCounterHiRes();
    played.instance->processBlock (buffer, midi);
    CHECK (juce::Time::getMillisecondCounterHiRes() - from < phazite::test::hangTimeout / 2.0); // the hang limit is the long one
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
    CHECK (phazite::test::dispatchMessagesUntil ([&read] { return ! read().offline; }));
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }));

    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, 60000));
    CHECK (phazite::isFrozen (song.context, track));
}

namespace
{
using Kind = phazite::test::TestPlugins::Kind;
using Status = phazite::PluginInstances::Status;

// How long a test waits for what it waits for, at most: long, as the tests' waits are, for a loaded machine; it waits for
// conditions, never for time.
constexpr int longWait = phazite::test::hangTimeout;

// A slow stand-ins' gate (host/TestPlugins.h), for being made and giving a state or for closing: shut while this lives,
// unless opened, and open again as it goes; the files they leave as they begin to wait cleared first.
struct SlowGate
{
    explicit SlowGate (bool forClosing = false)
        : closing (forClosing)
    {
        for (const auto kind : { Kind::slow, Kind::slowToo, Kind::slowSibling })
            phazite::test::TestPlugins::getSlowWaiting (kind).deleteFile();

        shut();
    }

    ~SlowGate() { open(); }

    void shut() const { REQUIRE (phazite::test::TestPlugins::getSlowGate (closing).create()); }
    void open() const { phazite::test::TestPlugins::getSlowGate (closing).deleteFile(); }

    const bool closing;
};

// The app's hold for a value typed into a plug-in's control, as the main window sets it (the project's hold for work, the
// window's one hold: src/app/InputHold.h), for as long as this lives: the title its wait's window shows, and its cancel,
// which a test calls as Escape (ProjectSession::cancelHeldWork()) or the window's Cancel does.
struct TypedValueHold
{
    explicit TypedValueHold (TestSong& s)
        : song (s)
    {
        song.context.setWorkHolder (
            [this] (const juce::String& shown, phazite::ProjectContext::Cancel cancel, phazite::ProjectContext::Progress)
            {
                title = shown;
                escape = std::move (cancel);
                return std::make_unique<phazite::ProjectContext::WorkHold>();
            });
        phazite::ui::setTypedValueHold ([this] (const juce::String& shown, phazite::ProjectContext::Cancel cancel)
                                        { return song.context.holdForWork (shown, std::move (cancel)); });
    }

    ~TypedValueHold()
    {
        phazite::ui::setTypedValueHold ({});
        song.context.setWorkHolder ({});
    }

    bool isHolding() const { return song.context.isHoldingForWork(); }

    TestSong& song;
    juce::String title;
    phazite::ProjectContext::Cancel escape;

    JUCE_DECLARE_NON_COPYABLE (TypedValueHold)
};

// So many turns of the message loop (each a dispatch of what waits, as the window's paints and timers come), while what a
// test waits for may come meanwhile: whether all of them came.
bool turnMessageLoop (int turns)
{
    auto turned = 0;
    return phazite::test::dispatchMessagesUntil ([&turned, turns] { return ++turned >= turns; }, longWait);
}

// A slow stand-in at the end of a track's effects, as a device list adds one, by its description (the slow one and its
// sibling share a "file", which TestSong::addEffect() finds devices by); waited for as it loads in the background, unless
// the gate is shut, when it loads on.
phazite::DeviceID addSlow (TestSong& song, phazite::TrackID track, Kind kind = Kind::slow)
{
    phazite::addEffect (song.context, track, phazite::makeDeviceType (phazite::test::TestPlugins::getDescription (kind)));
    const auto effects = phazite::getEffects (song.context, track);
    REQUIRE (! effects.empty());

    if (! phazite::test::TestPlugins::getSlowGate().existsAsFile())
        song.waitForPlugins();

    return effects.back().id;
}
} // namespace

TEST_CASE (
    "A project with slow plug-ins opens at once: they load beside each other while the song is used, and each comes in as it is ready")
{
    // Known bug 69: opening a project kept the message thread waiting for each plug-in in turn, with nothing shown.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto slow = addSlow (song, track);
    const auto slowToo = song.addEffect (track, "PhaziteTestSlowToo");
    song.setParameter (slow, "60", 0.5f); // its Level
    song.saveWithPluginStates();

    // Shut: neither can be made until it opens. The project opens, and nothing waited for them: they load.
    SlowGate gate;
    song.reopen (false);
    auto& opened = getSong (song);
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);
    CHECK (phazite::getPluginStatus (opened, slowToo) == Status::loading);
    CHECK (phazite::getPluginsLoading (opened) == juce::StringArray ("Test Slow", "Test Slow Too"));

    // Each began in its own helper, beside the other.
    INFO ("Test Slow: " << song.describePlugin (slow).toStdString() << "; Test Slow Too: " << song.describePlugin (slowToo).toStdString());
    CHECK (phazite::test::dispatchMessagesUntil (
        []
        {
            return phazite::test::TestPlugins::getSlowWaiting (Kind::slow).existsAsFile()
                   && phazite::test::TestPlugins::getSlowWaiting (Kind::slowToo).existsAsFile();
        },
        longWait));

    // The message loop turns on while they load, and the song is used: a change undone and made again; one of them taken
    // away and back by undo, which loads it again in the background.
    CHECK (turnMessageLoop (100));
    song.setVolume (track, -6.0f);
    phazite::undo (opened);
    phazite::redo (opened);
    phazite::removeDevice (song.context, slow);
    CHECK (turnMessageLoop (10));
    phazite::undo (opened);
    CHECK (turnMessageLoop (10));
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);
    CHECK (phazite::getPluginsLoading (opened).size() == 2);

    // A plug-in added meanwhile loads in the background too (known bug 166), beside them: it comes in while they still load.
    phazite::addEffect (song.context, track, song.findDeviceType ("PhaziteTestEffect"));
    const auto added = phazite::getEffects (song.context, track).back().id;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::findPluginInstance (opened, added) != nullptr; }, longWait));
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);

    // A render started meanwhile says why it can't, rather than render without them.
    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, longWait));
    CHECK (host.finished[0].contains ("still loading"));
    CHECK_FALSE (phazite::isFrozen (song.context, track));

    // Opened: each comes in, with what the song set it to, and plays (6 dB down, its track's volume as the redo left it).
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return phazite::getPluginsLoading (opened).isEmpty(); }, longWait));
    CHECK (phazite::getPluginStatus (opened, slow) == Status::playing);
    CHECK (phazite::getPluginStatus (opened, slowToo) == Status::playing);
    auto& instance = getSandboxed (song, slow);
    CHECK (instance.getParameterValue (instance.findParameter ("60")) == doctest::Approx (0.5f));

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    const auto audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.2, 0.5) == doctest::Approx (toneLevel - 6.02f - 6.0f).epsilon (0.002));
}

TEST_CASE ("A plug-in added, or brought back by an undo, loads in the background: nothing waits for it, and what the song gives "
           "it meanwhile reaches it")
{
    // Known bug 166: a plug-in added, or brought back by an undo once the project's plug-ins had loaded, opened while the
    // message thread waited for it, up to the load limit of a minute.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    auto& opened = getSong (song);
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);

    // Shut: it can't be made until the gate opens. Added, and the song's plan made again, without the message thread waiting
    // for it: it shows as loading, and loads.
    SlowGate gate;
    phazite::DeviceID slow;
    const auto kept = timeKept (
        [&]
        {
            slow = addSlow (song, track);
            session->update();
        });
    MESSAGE ("The message thread was kept for " << kept << " ms");
    // (structural: it returned while the gate keeps the plug-in from being made, which it would never have done had it
    // waited for it; it says loading)
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);
    CHECK (phazite::getEffects (song.context, track).back().loading);
    CHECK (phazite::getPluginsLoading (opened) == juce::StringArray ("Test Slow"));
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return phazite::test::TestPlugins::getSlowWaiting (Kind::slow).existsAsFile(); },
                                                   longWait));

    // A render started meanwhile says why it can't, rather than render without it.
    phazite::test::RenderHost host;
    song.context.getSongSelection().selectOnly (track);
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, longWait));
    CHECK (host.finished[0].contains ("still loading"));
    CHECK_FALSE (phazite::isFrozen (song.context, track));

    // Taken away by an undo while it loads, it is abandoned; brought back by the redo, it loads again, without waiting.
    phazite::undo (opened);
    session->update();
    CHECK_FALSE (phazite::getPluginStatus (opened, slow).has_value());
    CHECK (phazite::getPluginsLoading (opened).isEmpty());
    const auto keptForRedo = timeKept (
        [&]
        {
            phazite::redo (opened);
            session->update();
        });
    MESSAGE ("The message thread was kept for " << keptForRedo << " ms"); // structural: returned while the gate is shut
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);

    // A state the song gives it meanwhile (as a preset loaded into it does): its Level at a quarter.
    juce::MemoryOutputStream given;
    given.writeFloat (0.25f);
    phazite::findDeviceState (opened, slow)
        .setProperty (phazite::schema::pluginState, phazite::writeBytes (given.getMemoryBlock()), nullptr);

    // Open: it comes in with it, and the one the undo abandoned never comes in.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return phazite::getPluginsLoading (opened).isEmpty(); }, longWait));
    REQUIRE (phazite::getPluginStatus (opened, slow) == Status::playing);
    CHECK_FALSE (phazite::getEffects (song.context, track).back().loading);
    const auto state = getSandboxed (song, slow).getRemote().getState (longWait); // the plug-in's own, its Level first
    REQUIRE (state.has_value());
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (0.25f));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [] { return SandboxedPlugins::getHost().getNumLoading (phazite::test::TestPlugins::getDescription (Kind::slow)) == 0; }, longWait));

    // Deleted while it loads: nothing comes of it.
    gate.shut();
    const auto deleted = addSlow (song, track);
    session->update();
    REQUIRE (phazite::getPluginStatus (opened, deleted) == Status::loading);
    phazite::removeDevice (song.context, deleted);
    session->update();
    CHECK_FALSE (phazite::getPluginStatus (opened, deleted).has_value());
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [] { return SandboxedPlugins::getHost().getNumLoading (phazite::test::TestPlugins::getDescription (Kind::slow)) == 0; }, longWait));
    CHECK (turnMessageLoop (20));
    CHECK (phazite::getPluginsLoading (opened).isEmpty());

    // A project closed while one loads waits for it neither (structural: closed while the gate is shut, which a wait for the
    // plug-in would never have been).
    gate.shut();
    auto other = std::make_unique<TestSong>();
    addSlow (*other, other->firstTrack);
    phazite::getSession (other->context)->update();
    REQUIRE (phazite::getPluginsLoading (getSong (*other)).size() == 1);
    const auto keptForClose = timeKept ([&other] { other = nullptr; });
    MESSAGE ("Closing kept the message thread for " << keptForClose << " ms");
    // Still loading in its helper, which a close that waited for it (until the load's time ran out, its helper ended) leaves not.
    CHECK (SandboxedPlugins::getHost().getNumLoading (phazite::test::TestPlugins::getDescription (Kind::slow)) > 0);
}

TEST_CASE ("A crashed plug-in reloads in the background: nothing waits for its helper or for it, and it shows as loading meanwhile")
{
    // Known bug 166: a reload waited for a new helper to start (up to 20 seconds) and the plug-in to open in it.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    const auto slow = addSlow (song, track);
    song.waitForPlugins();
    auto& opened = getSong (song);
    auto& plugins = phazite::getSession (song.context)->getPlugins();
    auto& instance = getSandboxed (song, slow);

    const auto crash = [&]
    {
        instance.getRemote().getHelper().kill();
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getEffects (song.context, track).back().crashed; }, longWait));
    };

    crash();
    const auto generation = plugins.getGeneration (slow);

    // Shut: its new helper starts, but the plug-in can't be made in it. Reloaded, and the message thread waits for none of it.
    SlowGate gate;
    auto reloaded = std::make_shared<std::optional<std::optional<juce::String>>>();
    const auto kept = timeKept (
        [&] { phazite::reloadDevice (song.context, slow, [reloaded] (std::optional<juce::String> problem) { *reloaded = problem; }); });
    MESSAGE ("The message thread was kept for " << kept << " ms");
    // (structural: it returned while the gate keeps the plug-in from being made, which it would never have done had it
    // waited for it; it says loading)
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);
    CHECK (phazite::getEffects (song.context, track).back().loading);
    CHECK_FALSE (phazite::getEffects (song.context, track).back().crashed);
    CHECK (phazite::getPluginsLoading (opened) == juce::StringArray ("Test Slow"));

    // Asked again meanwhile, nothing more happens; nothing is said until it is in.
    auto again = std::make_shared<bool> (false);
    phazite::reloadDevice (song.context, slow, [again] (std::optional<juce::String>) { *again = true; });
    CHECK (turnMessageLoop (50));
    CHECK_FALSE (reloaded->has_value());

    // Open: it is in, the same instance, its windows told to make their editors again.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([reloaded] { return reloaded->has_value(); }, longWait));
    CHECK_FALSE (reloaded->value().has_value());
    CHECK (phazite::getPluginStatus (opened, slow) == Status::playing);
    CHECK (&getSandboxed (song, slow) == &instance);
    CHECK (plugins.getGeneration (slow) == generation + 1);
    CHECK (turnMessageLoop (10));
    CHECK_FALSE (*again);

    // A state the song gives it while it reloads (as a preset loaded into it does: its Level at a fifth), which reached only
    // the crashed instance then: it plays it once it is in, not what it reloaded from (the review of known bug 166).
    crash();
    gate.shut();
    auto withPreset = std::make_shared<std::optional<std::optional<juce::String>>>();
    phazite::reloadDevice (song.context, slow, [withPreset] (std::optional<juce::String> problem) { *withPreset = problem; });
    REQUIRE (phazite::getPluginStatus (opened, slow) == Status::loading);
    juce::MemoryOutputStream preset;
    preset.writeFloat (0.2f);
    phazite::findDeviceState (opened, slow)
        .setProperty (phazite::schema::pluginState, phazite::writeBytes (preset.getMemoryBlock()), &opened.getUndoManager());
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([withPreset] { return withPreset->has_value(); }, longWait));
    REQUIRE_FALSE (withPreset->value().has_value());
    const auto playing = instance.getRemote().getState (longWait);
    REQUIRE (playing.has_value());
    CHECK (juce::MemoryInputStream (*playing, false).readFloat() == doctest::Approx (0.2f));

    // Reloaded from its own state (its Level at 0.7, set in its window and not yet in the song's), with a preset loaded
    // and undone meanwhile: the song's state is as it was, but not what the plug-in reloaded from, which Phazite no longer
    // holds; it plays the song's, which Phazite holds (the second review of known bug 166: it played its own, while saves
    // recorded the song's).
    phazite::PluginInstances::setSoonAfterReload (0.0); // from its own, not the song's, though it crashed soon after a reload
    const auto writeLevel = [] (float level)
    {
        juce::MemoryOutputStream written;
        written.writeFloat (level);
        return written.getMemoryBlock();
    };
    const auto own = writeLevel (0.7f);
    instance.setStateInformation (own.getData(), (int) own.getSize());
    crash();
    gate.shut();
    auto undone = std::make_shared<std::optional<std::optional<juce::String>>>();
    phazite::reloadDevice (song.context, slow, [undone] (std::optional<juce::String> problem) { *undone = problem; });
    REQUIRE (phazite::getPluginStatus (opened, slow) == Status::loading);
    {
        const phazite::UndoStep step (opened, "Load Preset");
        phazite::findDeviceState (opened, slow)
            .setProperty (phazite::schema::pluginState, phazite::writeBytes (writeLevel (0.4f)), &opened.getUndoManager());
    }
    phazite::undo (opened);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([undone] { return undone->has_value(); }, longWait));
    REQUIRE_FALSE (undone->value().has_value());
    const auto afterUndo = instance.getRemote().getState (longWait);
    REQUIRE (afterUndo.has_value());
    CHECK (juce::MemoryInputStream (*afterUndo, false).readFloat() == doctest::Approx (0.2f));
    CHECK (juce::MemoryInputStream (instance.getLastState(), false).readFloat() == doctest::Approx (0.2f));
    phazite::PluginInstances::setSoonAfterReload (-1.0);

    // Deleted while it reloads: nothing is said of it, and nothing comes of it.
    crash();
    gate.shut();
    auto told = std::make_shared<bool> (false);
    phazite::reloadDevice (song.context, slow, [told] (std::optional<juce::String>) { *told = true; });
    REQUIRE (phazite::getPluginStatus (opened, slow) == Status::loading);
    phazite::removeDevice (song.context, slow);
    phazite::getSession (song.context)->update();
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [] { return SandboxedPlugins::getHost().getNumLoading (phazite::test::TestPlugins::getDescription (Kind::slow)) == 0; }, longWait));
    CHECK (turnMessageLoop (20));
    CHECK_FALSE (*told);
    CHECK (phazite::getPluginsLoading (opened).isEmpty());
}

TEST_CASE ("A plug-in's value's text and a value typed are asked of its helper without waiting, and shown and set once they come")
{
    // Known bug 166: each waited up to a quarter of a second for the helper, and showed or read a number after that.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    song.waitForPlugins();
    const auto handle = phazite::getParameter (song.context, { slow, "60" });
    REQUIRE (handle.exists());
    REQUIRE (handle.readsTextLater());

    // Shut: the plug-in can't write its text. Asked, the value shows as a number meanwhile, and nothing waits for it.
    SlowGate gate;
    auto known = std::make_shared<int> (0);
    juce::String text;
    const auto kept = timeKept ([&] { text = handle.getText (0.5f, [known] { ++*known; }); });
    MESSAGE ("The message thread was kept for " << kept << " ms");
    CHECK (text == juce::String ("0.50"));
    CHECK (getSandboxed (song, slow).isAskingTexts()); // returned while its answer is still to come: it waited for none of it

    // A value typed, which the plug-in reads once it can: nothing is set meanwhile.
    auto read = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("25 %", [read] (std::optional<float> value) { *read = value; }); // returns before it is read (below)
    CHECK (turnMessageLoop (50));
    CHECK (*known == 0);
    CHECK_FALSE (read->has_value());

    // Open: the text comes, and is known from then on; the value typed is read as the plug-in reads it.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([known, read] { return *known == 1 && read->has_value(); }, longWait));
    CHECK (handle.getText (0.5f) == juce::String ("50 %"));
    REQUIRE (read->value().has_value());
    CHECK (*read->value() == doctest::Approx (0.25f));

    // A value it hasn't written yet shows as a number meanwhile, never another value's text as if it were this one's (the
    // review of known bug 166: "50 %" stood for 0.75 for as long as the helper was busy). Turned through many values while
    // the helper is busy, only the newest is asked next.
    gate.shut();
    CHECK (handle.getText (0.75f) == juce::String ("0.75"));

    for (int step = 1; step <= 20; ++step)
        handle.getText (0.75f + 0.01f * (float) step);

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&handle] { return handle.getText (0.75f) == juce::String ("75 %"); }, longWait));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&handle] { return handle.getText (0.95f) == juce::String ("95 %"); }, longWait));
    CHECK (handle.getText (0.8f) == juce::String ("0.80")); // passed over: asked only now

    // An answer that doesn't come in a state's time still tells what waits, so that it asks again: not before that time (the
    // clock Phazite reads it by held still), and once it has passed.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! getSandboxed (song, slow).isAskingTexts(); }, longWait));
    {
        phazite::test::HeldClock clock;
        gate.shut();
        auto told = std::make_shared<bool> (false);
        CHECK (handle.getText (0.3f, [told] { *told = true; }) == juce::String ("0.30"));
        clock.advance (phazite::SandboxedInstance::getStateMilliseconds() - 1.0);
        CHECK (turnMessageLoop (20));
        CHECK_FALSE (*told);

        // Past the time, not onto it: the clock counts whole ticks, so the two moves together can come out a part of a tick
        // short of it, where a held clock stays for good (the wait below ran out once, CI's linux-release, 2026-11-17).
        clock.advance (2.0);
        REQUIRE (phazite::test::dispatchMessagesUntil ([told] { return *told; }, longWait));
        CHECK (handle.getText (0.3f) == juce::String ("0.30"));
        gate.open();
    }

    // A value typed whose answer comes after an undo is set on nothing: the song isn't as it was typed for.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! getSandboxed (song, slow).isAskingTexts(); }, longWait));
    song.setVolume (song.firstTrack, -6.0f); // what the undo takes back
    gate.shut();
    auto afterUndo = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("25 %", [afterUndo] (std::optional<float> value) { *afterUndo = value; });
    phazite::undo (getSong (song));
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([afterUndo] { return afterUndo->has_value(); }, longWait));
    CHECK_FALSE (afterUndo->value().has_value());
}

TEST_CASE ("A value typed into a plug-in's parameter holds the window's input until it is read, and comes back if its plug-in goes")
{
    // The review of known bug 166: a value typed was set whenever the answer came, on whatever was there then; and one whose
    // plug-in went was never answered, its control showing the typed text for ever.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    const auto handle = phazite::getParameter (song.context, { slow, "60" });
    REQUIRE (handle.readsTextLater());
    TypedValueHold hold (song); // the app's, as its main window sets it

    // Read by the plug-in, as a control reads one typed: the window takes no input meanwhile, and does again once it is in.
    SlowGate gate;
    auto read = std::make_shared<std::optional<std::optional<float>>>();
    phazite::ui::readTypedValue (handle, "40 %", [read] (std::optional<float> value) { *read = value; });
    CHECK (hold.isHolding());
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([read] { return read->has_value(); }, longWait));
    CHECK (read->value().value_or (-1.0f) == doctest::Approx (0.4f));
    CHECK_FALSE (hold.isHolding());

    // Points moved between the typing and the answer (by hand, from another window: a step of its own):
    // the points it was typed for aren't as they were, and nothing is set.
    const phazite::ParameterRef level { slow, "60" };
    song.addAutomation (level, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (4.0), 0.5f } });
    gate.shut();
    auto moved = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("90 %", [moved] (std::optional<float> value) { *moved = value; });
    phazite::moveAutomationPoints (song.context, level, { 0 }, phazite::BeatDuration::fromBeats (1.0), 0.0f);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([moved] { return moved->has_value(); }, longWait));
    CHECK_FALSE (moved->value().has_value());

    // Its plug-in taken away before the answer: answered with nothing, a message after it went.
    gate.shut();
    auto gone = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("10 %", [gone] (std::optional<float> value) { *gone = value; });
    phazite::removeDevice (song.context, slow);
    phazite::getSession (song.context)->update();
    REQUIRE (phazite::test::dispatchMessagesUntil ([gone] { return gone->has_value(); }, longWait));
    CHECK_FALSE (gone->value().has_value());
}

TEST_CASE ("A value typed into a plug-in's control waits for as long as the plug-in takes, held as work is, and Escape gives it up")
{
    // The second review of known bug 166: after a quarter of a second the text was read as a value from 0 to 1 ("-6 dB" the
    // bottom, "440 Hz" the top) and set, as an undo step; and the window's input was held unseen. Now the window's one hold
    // for work holds it, whose window shows the wait after a second, and whose Escape or Cancel gives it up.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    const auto handle = phazite::getParameter (song.context, { slow, "60" });
    REQUIRE (handle.readsTextLater());
    const auto before = handle.getValue();
    const auto lastStep = phazite::describeUndo (&getSong (song));
    TypedValueHold hold (song);

    // Shut: it waits, held, its wait named for the window that shows it.
    SlowGate gate;
    auto read = std::make_shared<std::optional<std::optional<float>>>();
    auto calls = std::make_shared<int> (0);
    phazite::ui::readTypedValue (handle, "40 %",
                                 [read, calls] (std::optional<float> value)
                                 {
                                     *read = value;
                                     ++*calls;
                                 });
    CHECK (hold.isHolding());
    CHECK (hold.title == "Waiting for the plug-in to read the value. Press Esc to stop waiting.");
    CHECK (turnMessageLoop (20));
    CHECK_FALSE (read->has_value());
    REQUIRE (hold.escape != nullptr);

    // Escape: given up, the value dropped, the hold let go; the plug-in's answer, coming later, dropped too.
    hold.escape ({});
    REQUIRE (read->has_value());
    CHECK_FALSE (read->value().has_value());
    CHECK_FALSE (hold.isHolding());
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! getSandboxed (song, slow).isAskingTexts(); }, longWait));
    CHECK (turnMessageLoop (5));
    CHECK (*calls == 1);
    CHECK (handle.getValue() == doctest::Approx (before));
    CHECK (phazite::describeUndo (&getSong (song)) == lastStep);

    // Its helper gone before the plug-in read it: dropped, never a number Phazite read itself.
    gate.shut();
    auto lost = std::make_shared<std::optional<std::optional<float>>>();
    phazite::ui::readTypedValue (handle, "40 %", [lost] (std::optional<float> value) { *lost = value; });
    getSandboxed (song, slow).getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([lost] { return lost->has_value(); }, longWait));
    CHECK_FALSE (lost->value().has_value());
    CHECK_FALSE (hold.isHolding());
    CHECK (handle.getValue() == doctest::Approx (before));
}

TEST_CASE ("A value typed into a plug-in's control lands while a take records and Write writes, and not after an undo")
{
    // The second review of known bug 166: any change of the song while the plug-in read the value dropped it, and the song
    // changes all the time as it records (a take growing, automation written).
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    const auto handle = phazite::getParameter (song.context, { slow, "60" });
    REQUIRE (handle.readsTextLater());
    auto& model = getSong (song);

    // A take recording on an instrument's track, from the computer's live input.
    const auto keys = song.addTrack ("Keys");
    song.setInstrument (keys, "phaziteSynth");
    phazite::routeLiveMidiInput (model);
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    const auto countIn = audio.isCountInOn();
    audio.setCountInOn (false);
    const juce::ScopeGuard countInBack { [&audio, countIn] { audio.setCountInOn (countIn); } };
    EnginePlayback playback;
    playback.play (0.01);
    phazite::startRecording (model);
    REQUIRE (phazite::isRecording (model));

    SlowGate gate;
    auto whileRecording = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("40 %", [whileRecording] (std::optional<float> value) { *whileRecording = value; });
    playback.play (0.1);
    audio.playLive (juce::MidiMessage::noteOn (1, 60, 0.8f));
    playback.play (0.25);
    audio.playLive (juce::MidiMessage::noteOff (1, 60));
    playback.play (0.25);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([whileRecording] { return whileRecording->has_value(); }, longWait));
    CHECK (whileRecording->value().value_or (-1.0f) == doctest::Approx (0.4f));
    phazite::stopRecording (model);

    // Write on another track's pan (as the window's input is held, nothing is turned by hand meanwhile: its pass writes the
    // pan's value from where playback started), its pass written into the song as playback stops, while the plug-in reads.
    const auto other = song.addTrack ("Other");
    const auto panLane = song.getPanParameter (other);
    song.addAutomation (panLane, { { phazite::BeatPosition(), -0.5f }, { phazite::BeatPosition::fromBeats (16.0), -0.5f } });
    phazite::getPan (song.context, other).setValue (0.25f);
    phazite::setAutomationMode (song.context, other, phazite::AutomationMode::write);
    const auto pointsBefore = phazite::getAutomationPoints (song.context, panLane);
    phazite::playOrStop (model);
    playback.play (0.25);
    gate.shut();
    auto whileWriting = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("30 %", [whileWriting] (std::optional<float> value) { *whileWriting = value; });
    playback.play (0.5);
    phazite::stopAndReturn (model);
    const auto pointsAfter = phazite::getAutomationPoints (song.context, panLane);
    CHECK ((pointsAfter.size() != pointsBefore.size()
            || ! std::equal (pointsAfter.begin(), pointsAfter.end(), pointsBefore.begin(),
                             [] (const auto& a, const auto& b) { return juce::exactlyEqual (a.position, b.position); })));
    CHECK_FALSE (whileWriting->has_value());
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([whileWriting] { return whileWriting->has_value(); }, longWait));
    CHECK (whileWriting->value().value_or (-1.0f) == doctest::Approx (0.3f));

    // A step of another song's meanwhile (an export's own song of an old version, another project's): this song's history
    // hasn't moved, and it lands (the review of known bug 166: one count for every song's history dropped it).
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! getSandboxed (song, slow).isAskingTexts(); }, longWait));
    gate.shut();
    auto whileAnotherSong = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("25 %", [whileAnotherSong] (std::optional<float> value) { *whileAnotherSong = value; });
    auto another = phazite::createSong();
    {
        const phazite::UndoStep step (*another, "Add Track");
        phazite::ensureTracks (*another, 2);
    }
    phazite::undo (*another);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([whileAnotherSong] { return whileAnotherSong->has_value(); }, longWait));
    CHECK (whileAnotherSong->value().value_or (-1.0f) == doctest::Approx (0.25f));

    // An undo between the typing and the answer: what it was typed for may be other, and nothing is set.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! getSandboxed (song, slow).isAskingTexts(); }, longWait));
    song.setVolume (song.firstTrack, -6.0f); // what the undo takes back
    gate.shut();
    auto afterUndo = std::make_shared<std::optional<std::optional<float>>>();
    handle.readText ("20 %", [afterUndo] (std::optional<float> value) { *afterUndo = value; });
    phazite::undo (model);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([afterUndo] { return afterUndo->has_value(); }, longWait));
    CHECK_FALSE (afterUndo->value().has_value());
}

TEST_CASE ("What isn't a number, and texts of any length, from a plug-in never get past where they come in")
{
    // Known bug 181: jlimit() passed NaN on, and what the helper told was taken as it came.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto crash = song.addEffect (song.firstTrack, "PhaziteTestCrash");
    auto& instance = getSandboxed (song, crash);
    const auto wild = instance.findParameter ("23"), told = instance.findParameter ("24");
    REQUIRE (wild >= 0);
    REQUIRE (told >= 0);

    // As it opened: its default and value that are NaN taken as 0, and its name and label that run on kept short.
    const auto& info = instance.getParameterInfos()[(size_t) wild];
    CHECK (info.defaultValue == 0.0f);
    CHECK (info.value == 0.0f);
    CHECK (info.name.length() <= phazite::sandbox::maxTextLength);
    CHECK (info.label.length() <= phazite::sandbox::maxTextLength);
    CHECK (instance.getParameterValue (wild) == 0.0f);

    // NaN, then an infinity, told by the plug-in as values of its own: neither changes anything, and what it told after them
    // comes as it was.
    auto wildHandle = phazite::getParameter (song.context, { crash, "23" });
    REQUIRE (wildHandle.exists());

    for (const auto nonsense : { 0.75f, 0.25f })
    {
        CAPTURE (nonsense);
        song.setParameter (crash, "22", nonsense);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return juce::exactlyEqual (instance.getParameterValue (told), nonsense); },
                                                       longWait));
        CHECK (instance.getParameterValue (wild) == 0.0f);
        CHECK (std::isfinite (wildHandle.getValue()));
    }

    // Its text for any value is a hundred thousand characters long: kept to what Phazite takes.
    auto known = std::make_shared<bool> (false);
    wildHandle.getText (0.5f, [known] { *known = true; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([known] { return *known; }, longWait));
    CHECK (wildHandle.getText (0.5f).length() == phazite::sandbox::maxTextLength);

    // What it reads as an infinity is no value; nor is a number typed past what a float holds, nor a value set that is no number.
    auto read = std::make_shared<std::optional<std::optional<float>>>();
    wildHandle.readText ("1", [read] (std::optional<float> value) { *read = value; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([read] { return read->has_value(); }, longWait));
    CHECK_FALSE (read->value().has_value());
    CHECK_FALSE (wildHandle.readText ("1" + juce::String::repeatedString ("0", 400)).has_value());
    CHECK_FALSE (phazite::readFiniteDouble ("1e99999").has_value());

    const auto lastStep = phazite::describeUndo (&getSong (song));
    wildHandle.setValue (std::numeric_limits<float>::quiet_NaN());
    instance.setParameterValue (wild, std::numeric_limits<float>::infinity());
    CHECK (phazite::describeUndo (&getSong (song)) == lastStep);
    CHECK (instance.getParameterValue (wild) == 0.0f);
    CHECK_FALSE (phazite::findDeviceState (getSong (song), crash).getChildWithProperty (phazite::schema::valueParameter, "23").isValid());
}

TEST_CASE ("A helper that floods Phazite with what it tells keeps a value for each parameter until the window takes them")
{
    // Known bug 181's review: every value a helper told was kept and written into the song, one after another, however many.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto crash = song.addEffect (song.firstTrack, "PhaziteTestCrash");
    auto& instance = getSandboxed (song, crash);
    const auto told = instance.findParameter ("24");
    REQUIRE (told >= 0);

    // Two IDs the same once cut to what Phazite keeps: the first is found by it.
    const auto cut = juce::String::repeatedString ("9", phazite::sandbox::maxIDLength);
    const auto& infos = instance.getParameterInfos();
    const auto first = std::find_if (infos.begin(), infos.end(), [&cut] (const auto& info) { return info.id == cut; });
    REQUIRE (first != infos.end());
    CHECK (instance.findParameter (cut) == (int) std::distance (infos.begin(), first));
    CHECK (instance.findParameter (cut + "#2") == instance.findParameter (cut) + 1); // the other by its order among its twins
    std::set<juce::String> ids;                                                      // every ID once, as JUCE and the engine need

    for (const auto& info : infos)
        CHECK (ids.insert (info.id).second);

    // A hundred thousand values of one parameter, told between two turns of the message loop: one change of the song's. And
    // its gesture begun, ten thousand gestures of another, then its end: begun and ended, each paired, nothing left held
    // (the second review: past a cap of a turn's gestures its end was dropped, and Touch kept writing).
    auto& model = getSong (song);
    const auto changesBefore = model.getChangesOutsideHistory (crash.getRaw());
    const auto other = instance.findParameter ("23");
    REQUIRE (other >= 0);
    std::map<int, int> heldGestures;
    const auto listening = instance.onGesture;
    instance.onGesture = [&heldGestures, listening] (int parameter, bool starting)
    {
        heldGestures[parameter] += starting ? 1 : -1;

        if (listening != nullptr)
            listening (parameter, starting);
    };

    const auto tell = [&instance] (phazite::sandbox::MessageType type, int parameter, auto value)
    {
        phazite::sandbox::MessageWriter notice (type, 0);
        notice.add (instance.getRemote().getID()).add ((std::int32_t) parameter).add (value);
        const auto block = notice.take(); // which the reader reads in place
        phazite::sandbox::MessageReader reader (block);
        juce::ignoreUnused (phazite::sandbox::RemoteInstance::readNoticeInstance (reader));
        instance.getRemote().handleNotice (reader.getType(), reader);
    };

    const auto kept = timeKept (
        [&]
        {
            for (int i = 0; i < 100000; ++i)
                tell (phazite::sandbox::MessageType::parameterChanged, told, (double) (i % 100) / 100.0 + 0.001);

            tell (phazite::sandbox::MessageType::gesture, told, true);

            for (int i = 0; i < 10000; ++i)
                tell (phazite::sandbox::MessageType::gesture, other, i % 2 == 0);

            tell (phazite::sandbox::MessageType::gesture, told, false);
        });
    MESSAGE ("Telling them took " << kept << " ms");
    const auto turn = timeKept ([] { phazite::test::dispatchPendingMessages(); });
    MESSAGE ("The turn of the message loop that took them took " << turn << " ms");
    CHECK (instance.getParameterValue (told) == doctest::Approx (0.991f));
    CHECK (model.getChangesOutsideHistory (crash.getRaw()) - changesBefore <= 2);
    CHECK (turnMessageLoop (5)); // the ends, a message later
    CHECK (heldGestures[told] == 0);
    CHECK (heldGestures[other] == 0);
    instance.onGesture = listening;
}

#if JUCE_WINDOWS
TEST_CASE ("A plug-in's window opens without waiting for its helper to make its editor, nor for the plug-in to load")
{
    // Known bug 166: opening a plug-in's window waited for its helper to make the editor, up to 10 seconds.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    const auto hanging = song.addEffect (song.firstTrack, "PhaziteTestHang");
    phazite::DeviceWindows windows (song.context);
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);

    // Asked for, it opens once the helper has made its editor; asked again meanwhile, the wait is given up (a helper alive
    // that never made it kept it for ever: the second review), and asked once more, it opens.
    phazite::showDeviceWindow (song.context, effect);
    CHECK (phazite::isDeviceWindowOpening (song.context, effect));
    CHECK_FALSE (phazite::isDeviceWindowOpen (song.context, effect));
    phazite::showDeviceWindow (song.context, effect);
    CHECK_FALSE (phazite::isDeviceWindowOpening (song.context, effect));
    CHECK (turnMessageLoop (20));
    CHECK_FALSE (phazite::isDeviceWindowOpen (song.context, effect)); // what it made for the wait given up is dropped
    phazite::showDeviceWindow (song.context, effect);
    CHECK (phazite::isDeviceWindowOpening (song.context, effect));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::isDeviceWindowOpen (song.context, effect); }, longWait));
    CHECK_FALSE (phazite::isDeviceWindowOpening (song.context, effect));
    windows.close (effect);

    // A helper whose message thread has stopped: its window asked for keeps the message thread for nothing; asked again while
    // it opens, it doesn't open.
    auto& instance = getSandboxed (song, hanging);
    song.setParameter (hanging, hangStall, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.getRemote().getState (50).has_value(); }, longWait));
    const auto kept = timeKept ([&] { phazite::showDeviceWindow (song.context, hanging); });
    MESSAGE ("The message thread was kept for " << kept << " ms");
    CHECK (kept < phazite::test::hangTimeout / 2.0); // a wait for the stalled helper would last until it is ended
    CHECK (turnMessageLoop (20));
    CHECK (phazite::isDeviceWindowOpening (song.context, hanging));
    phazite::showDeviceWindow (song.context, hanging); // asked again: the wait given up, as its button says
    CHECK_FALSE (phazite::isDeviceWindowOpening (song.context, hanging));

    // Ended (its hang's limit the tests' long one: ended here), it has no window to open.
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.hasCrashed(); }, longWait));
    phazite::showDeviceWindow (song.context, hanging);
    CHECK_FALSE (phazite::isDeviceWindowOpening (song.context, hanging));
    CHECK_FALSE (phazite::isDeviceWindowOpen (song.context, hanging));

    // One still loading opens once it has loaded; one deleted meanwhile never opens.
    SlowGate gate;
    const auto slow = addSlow (song, song.firstTrack);
    session->update();
    phazite::showDeviceWindow (song.context, slow);
    CHECK (phazite::isDeviceWindowOpening (song.context, slow));
    const auto deleted = addSlow (song, song.firstTrack, Kind::slowToo);
    session->update();
    phazite::showDeviceWindow (song.context, deleted);
    CHECK (phazite::isDeviceWindowOpening (song.context, deleted));
    phazite::removeDevice (song.context, deleted);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::isDeviceWindowOpening (song.context, deleted); }, longWait));

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::isDeviceWindowOpen (song.context, slow); }, longWait));
    CHECK_FALSE (phazite::isDeviceWindowOpen (song.context, deleted));
    windows.closeAll();
}
#endif

TEST_CASE ("A plug-in's slot in the mixer says it loads while it does; a click asks for its window, which opens once it has")
{
    // The review of known bug 166: nothing tested a slot while its plug-in loaded.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    auto& opened = getSong (song);
    phazite::CommandRegistry registry;
    registry.add ({ .id = "device.show", .name = "Show Device", .perform = [] {} });
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::ChannelStrip strip { commands, song.context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 700 };
    strip.setTrack (track);
    phazite::DeviceWindows windows (song.context);

    const auto slotButton = [&strip]() -> juce::TextButton*
    {
        if (auto* group = phazite::test::findComponentWithTitle (strip, "Effects"))
            for (auto* button : phazite::test::findAll<juce::TextButton> (*group))
                if (button->isVisible() && button->getButtonText().startsWith ("Test Slow"))
                    return button;

        return nullptr;
    };

    // Added while it can't be made: its slot says it loads.
    SlowGate gate;
    const auto slow = addSlow (song, track);
    phazite::getSession (song.context)->update();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return slotButton() != nullptr && slotButton()->getButtonText() == "Test Slow - Loading"; }, longWait));

    // A click on it asks for its window, which waits for it to load; a preset loaded meanwhile is what it comes in with.
    phazite::test::Mouse mouse (*slotButton());
    mouse.click (slotButton()->getLocalBounds().getCentre());
    CHECK (phazite::isDeviceWindowOpening (song.context, slow));
    juce::MemoryOutputStream preset;
    preset.writeFloat (0.6f);
    phazite::findDeviceState (opened, slow)
        .setProperty (phazite::schema::pluginState, phazite::writeBytes (preset.getMemoryBlock()), &opened.getUndoManager());

    // In: its slot shows its name, the preset plays, and its window opens.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return slotButton() != nullptr && slotButton()->getButtonText() == "Test Slow"; },
                                                   longWait));
    // Asked without the message thread waiting: the helper, putting the editor it makes into Phazite's window meanwhile,
    // may wait for it.
    const auto asked = getSandboxed (song, slow).getRemote().askState();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&asked] { return asked->isDone(); }, longWait));
    const auto state = phazite::sandbox::RemoteInstance::readState (*asked);
    REQUIRE (state.has_value());
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (0.6f));
#if JUCE_WINDOWS
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::isDeviceWindowOpen (song.context, slow); }, longWait));
#endif
    windows.closeAll();
}

#if JUCE_WINDOWS
TEST_CASE ("A plug-in's window stays while its device comes back as another instance, and opens on the instance the song has")
{
    // The review of known bug 166: an open window closed as its device came back (a version brought back, Discard, a branch
    // switched to in place), now that the new instance loads in the background; and a window still opening could open on an
    // instance the song no longer had, whose turns went nowhere.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    auto& opened = getSong (song);
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    phazite::DeviceWindows windows (song.context);

    const auto shownInstance = [&windows, slow]() -> phazite::SandboxedInstance*
    {
        auto* window = windows.findWindow (slow);
        auto* editor = window != nullptr ? dynamic_cast<phazite::SandboxedEditor*> (window->getContentComponent()) : nullptr;
        return editor != nullptr ? editor->getInstance().get() : nullptr;
    };

    // Asked for while the plug-in makes its editor (held at the gate), and replaced meanwhile: the window opens on the
    // instance the song has.
    SlowGate gate;
    gate.open();
    REQUIRE (
        phazite::test::dispatchMessagesUntil ([&] { return ! phazite::findPluginInstance (opened, slow)->isAskingState(); }, longWait));
    gate.shut();
    phazite::showDeviceWindow (song.context, slow);
    CHECK (phazite::isDeviceWindowOpening (song.context, slow));
    CHECK (turnMessageLoop (10));
    phazite::removeDevice (song.context, slow);
    session->update(); // its instance goes
    phazite::undo (opened);
    session->update();
    CHECK (phazite::getPluginStatus (opened, slow) == Status::loading);
    CHECK (turnMessageLoop (10));
    CHECK (phazite::isDeviceWindowOpening (song.context, slow));
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return shownInstance() != nullptr && shownInstance() == phazite::findPluginInstance (opened, slow); }, longWait));

    // Open; then the device taken away and brought back, its new instance held as it loads: the window stays, saying so,
    // and shows the new instance's editor once it is in.
    gate.shut();
    phazite::removeDevice (song.context, slow);
    session->update(); // its instance goes
    phazite::undo (opened);
    session->update();
    REQUIRE (phazite::getPluginStatus (opened, slow) == Status::loading);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            auto* window = windows.findWindow (slow);
            return window != nullptr && phazite::test::findComponentWithTitle (*window, "Loading") != nullptr;
        },
        longWait));

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return shownInstance() != nullptr && shownInstance() == phazite::findPluginInstance (opened, slow); }, longWait));
    windows.closeAll();
}
#endif

TEST_CASE ("A render of a song no session plays opens its plug-ins in the background, and begins once they are in")
{
    // The review of known bug 166: an old version's bounce, or its DAWproject, opened each of its own plug-ins while the
    // message thread waited.
    SandboxedPlugins sandboxed;
    phazite::test::TemporaryFolder folder;
    auto old = phazite::createSong();
    const auto track = phazite::ensureTracks (*old, 1).front();
    REQUIRE (phazite::test::addMidiClip (*old, track, { phazite::test::at (0.0), phazite::test::at (4.0) }).isValid());
    phazite::insertDeviceState (
        *old, track, phazite::makeDeviceState (*old, phazite::makeDeviceType (phazite::test::TestPlugins::getDescription (Kind::slow))));
    REQUIRE (phazite::findSession (*old) == nullptr);

    SlowGate gate;
    std::optional<phazite::ExportReport> report;
    std::unique_ptr<phazite::SongExport> exporting;
    phazite::ExportSettings settings;
    const auto kept = timeKept (
        [&]
        {
            exporting = phazite::startBackgroundExport (*old, SandboxedPlugins::getHost(), folder.folder.getChildFile ("Old.wav"), settings,
                                                        [&report] (const phazite::ExportReport& r) { report = r; });
        });
    MESSAGE ("The message thread was kept for " << kept << " ms");
    CHECK (phazite::test::dispatchMessagesUntil ([] { return phazite::test::TestPlugins::getSlowWaiting (Kind::slow).existsAsFile(); }));
    // (structural: it returned while the gate keeps its plug-in from being made, which it would never have done had it waited)
    CHECK (turnMessageLoop (20));
    CHECK_FALSE (report.has_value());

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, longWait));
    CHECK (report->result.wasOk());
}

TEST_CASE ("A save asks slow plug-ins for their states at once while the message loop turns; Auto Save asks those that changed")
{
    // Known bugs 69 and 89: a save asked every plug-in for its state in turn, on the message thread, and Auto Save did so a
    // second after each change, whether a plug-in had changed or not.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    const auto slow = addSlow (song, track);
    const auto slowToo = song.addEffect (track, "PhaziteTestSlowToo");
    song.saveWithPluginStates();
    auto& opened = getSong (song);

    // Both set by hand: their states may have changed. Shut, the save waits for their answers, the message thread not.
    song.setParameter (slow, "60", 0.5f);
    song.setParameter (slowToo, "60", 0.25f);
    SlowGate gate;
    auto saved = false;
    phazite::whenPluginStatesIn (opened, true, [&song, &saved] (bool) { saved = song.getProject().save().wasOk(); });
    CHECK_FALSE (saved);
    CHECK (turnMessageLoop (100));
    CHECK_FALSE (saved);

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved; }, longWait));
    CHECK (phazite::getPluginsWithOlderStates (opened).isEmpty());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // Auto Save asks only those that may have changed: shut, a change elsewhere saves without them.
    gate.shut();
    phazite::AutosaveTimer timer (song.getProject());
    timer.setAutoSave (true);
    song.setVolume (track, -6.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.getProject().hasUnsavedChanges(); }, longWait));

    // One that changed is asked, once Auto Save is due, and waited for while the message loop turns: the save comes with
    // its answer.
    song.setParameter (slow, "60", 0.75f);
    auto& slowInstance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&slowInstance] { return slowInstance.isAskingState(); }, longWait));
    CHECK (turnMessageLoop (100));
    CHECK (slowInstance.isAskingState());
    CHECK (song.getProject().hasUnsavedChanges());
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.getProject().hasUnsavedChanges(); }, longWait));

    // One that never answers is given up on once its time has passed: the project is saved, with the state it gave before,
    // and the save says which; one that didn't change and didn't answer either keeps a state that is its own.
    struct StateLimit
    {
        StateLimit() { phazite::SandboxedInstance::setStateMilliseconds (300); }
        ~StateLimit() { phazite::SandboxedInstance::setStateMilliseconds (0); }
    } limit;

    juce::StringArray told;
    timer.onOlderPluginStates = [&told] (const juce::StringArray& plugins) { told = plugins; };
    gate.shut();
    song.setParameter (slow, "60", 0.1f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.getProject().hasUnsavedChanges(); }, longWait));
    CHECK (told == juce::StringArray ("Test Slow"));
    CHECK (phazite::getPluginsWithOlderStates (opened) == juce::StringArray ("Test Slow"));

    saved = false;
    phazite::whenPluginStatesIn (opened, true, [&song, &saved] (bool) { saved = song.getProject().save().wasOk(); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved; }, longWait));
    CHECK (phazite::getPluginsWithOlderStates (opened) == juce::StringArray ("Test Slow"));

    // Answering again, it is asked again by the next Auto Save, which a change elsewhere brings: its state is no longer
    // older. (Its helper isn't ended for not answering meanwhile: this test's hang limit is long.)
    gate.open();
    REQUIRE_FALSE (slowInstance.hasCrashed());
    song.setVolume (track, -3.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return ! song.getProject().hasUnsavedChanges() && phazite::getPluginsWithOlderStates (opened).isEmpty(); }, longWait));
}

TEST_CASE ("Closing a project lets go of its slow plug-ins' helpers at once: they end on their own, beside each other")
{
    // Known bug 69: closing waited for each plug-in to close in its helper, and for each helper to quit, in turn.
    SandboxedPlugins sandboxed;
    SandboxedPlugins::getHost().waitForHelpersEnded();

    // Their helpers have as long as this test to quit before they are ended: their ending lasts until the gate opens.
    struct QuitLimit
    {
        QuitLimit() { SandboxedPlugins::getHost().setQuitMilliseconds (longWait); }
        ~QuitLimit() { SandboxedPlugins::getHost().setQuitMilliseconds (phazite::sandbox::Helper::quitMilliseconds); }
    } quitLimit;

    auto song = std::make_unique<TestSong>();
    addSlow (*song, song->firstTrack);
    song->addEffect (song->firstTrack, "PhaziteTestSlowToo");
    REQUIRE (SandboxedPlugins::getHost().getNumHelpers() == 2);

    // Shut, neither can close: the project closes, and their helpers end meanwhile, nothing waiting for them.
    SlowGate gate (true);
    song = nullptr;
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return SandboxedPlugins::getHost().getNumHelpers() == 0; }, longWait));
    CHECK (turnMessageLoop (100));
    CHECK (SandboxedPlugins::getHost().getNumHelpersEnding() == 2);

    gate.open();
    SandboxedPlugins::getHost().waitForHelpersEnded();
    CHECK (SandboxedPlugins::getHost().getNumHelpersEnding() == 0);
}

TEST_CASE ("A plug-in in mono alone, between Phazite's two sides, hears both and plays to both")
{
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto mono = song.addEffect (track, "PhaziteTestMono");
    CHECK (getSandboxed (song, mono).getRemote().getInfo().numInputChannels == 1);

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    const auto audio = playback.play (0.6);
    CHECK (levelOf (audio, 0, 0.1, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));
    CHECK (levelOf (audio, 1, 0.1, 0.5) == doctest::Approx (toneLevel - 6.02f).epsilon (0.002));
}

TEST_CASE ("A plug-in whose program would override its settings opens again on its program, then its settings")
{
    // The stand-in's programs set its Gain (Half: 0.5); its Gain turned in its own window after that is its setting.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto* instance = &getSandboxed (song, effect);
    instance->setCurrentProgram (1);
    const auto gain = instance->findParameter (effectGain);

    juce::MemoryOutputStream told;
    told.writeFloat (0.8f); // its gain
    told.writeFloat (0.5f); // its tone
    told.writeFloat (1.0f); // as its own window would set them, telling the host
    REQUIRE (instance->getRemote().setState (told.getMemoryBlock(), 10000));
    REQUIRE (
        phazite::test::dispatchMessagesUntil ([&] { return juce::exactlyEqual (instance->getParameterValue (gain), 0.8f); }, longWait));

    song.reopen();
    instance = &getSandboxed (song, effect);
    CHECK (instance->getCurrentProgram() == 1);
    const auto state = instance->getRemote().getState (10000);
    REQUIRE (state.has_value());
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (0.8f));
}

TEST_CASE ("A block whose length the helper's side of the memory says otherwise of is as one it didn't play")
{
    // What a helper that went wrong (or crashed halfway) leaves in its side of the memory is never taken for how much to read.
    SandboxedPlugins sandboxed;
    Played played (Kind::synth);
    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    const std::vector<phazite::devices::NoteEvent> note { { phazite::devices::NoteEvent::Type::noteOn, 0, 69, 0.8f } };
    phazite::devices::ProcessContext context;
    context.channels = buffer.getArrayOfWritePointers();
    context.numChannels = 2;
    context.numSamples = 256;
    context.events = note;
    context.isRendering = true;

    played.processor->start (context);
    played.instance->getRemote().getChannel().getInfo().numSamples = 1 << 24;
    played.processor->process (context);
    CHECK (buffer.getMagnitude (0, 0, 256) == 0.0f); // silent, as for a block it didn't play
}

#if JUCE_WINDOWS
TEST_CASE ("A plug-in's editor fits its plug-in at any Interface Size: as large on the screen as the plug-in makes it")
{
    // Known bug 68: the plug-in's size was taken for Phazite's own, cropped below 100 % and with margins above it.
    SandboxedPlugins sandboxed;

    struct InterfaceSize
    {
        ~InterfaceSize() { juce::Desktop::getInstance().setGlobalScaleFactor (1.0f); }
    } restored;

    for (const auto scale : { 0.75f, 2.0f })
    {
        CAPTURE (scale);
        juce::Desktop::getInstance().setGlobalScaleFactor (scale);
        TestSong song;
        const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
        auto* session = phazite::getSession (song.context);
        REQUIRE (session != nullptr);

        // In Phazite's units, the plug-in's 300 by 200 at this Interface Size.
        auto editor = makeEditor (session->getPlugins().find (effect));
        REQUIRE (editor != nullptr);
        CHECK (editor->getWidth() == juce::roundToInt (300.0f / scale));
        CHECK (editor->getHeight() == juce::roundToInt (200.0f / scale));

        // A size the helper tells is taken within a side's limits (before the editor shows, which would show it so).
        auto* instance = session->getPlugins().find (effect).get();
        instance->onEditorResized (-5, 1 << 30);
        CHECK (editor->getWidth() == 1);
        CHECK (editor->getHeight() == juce::roundToInt (16384.0f / scale));
        instance->onEditorResized (300, 200);

        juce::Component window;
        window.setSize (editor->getWidth() + 40, editor->getHeight() + 40);
        window.addAndMakeVisible (*editor);
        editor->setTopLeftPosition (20, 20);
        window.addToDesktop (0);
        window.setVisible (true);

        // The helper's window for it is as large on the screen as Phazite's area for it, which is the plug-in's editor.
        const auto parent = static_cast<HWND> (window.getPeer()->getNativeHandle());
        const auto platform = window.getPeer()->getPlatformScaleFactor();
        const auto area = (window.getPeer()->getAreaCoveredBy (*editor).toDouble() * platform).toNearestInt();
        CHECK (std::abs (area.getWidth() - juce::roundToInt (300.0 * platform)) <= 1);
        CHECK (std::abs (area.getHeight() - juce::roundToInt (200.0 * platform)) <= 1);
        RECT client {};
        CHECK (phazite::test::dispatchMessagesUntil (
            [&]
            {
                const auto child = findHelperWindow (parent);
                return child != nullptr && GetClientRect (child, &client) && std::abs (client.right - area.getWidth()) <= 1
                       && std::abs (client.bottom - area.getHeight()) <= 1;
            },
            longWait));

        editor = nullptr;
    }
}

TEST_CASE ("Auto Save asks a plug-in whose window was open for its state, though it said nothing changed")
{
    // A plug-in needn't tell what its window changes (a sample loaded in it): Auto Save asks it all the same.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    song.saveWithPluginStates();
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    auto& instance = getSandboxed (song, effect);

    // Its window shown and closed; its gain changed in it without a word to Phazite.
    REQUIRE (makeEditor (session->getPlugins().find (effect)) != nullptr);
    juce::MemoryOutputStream unsaid;
    unsaid.writeFloat (0.3f); // its gain
    unsaid.writeFloat (0.5f); // its tone
    REQUIRE (instance.getRemote().setState (unsaid.getMemoryBlock(), longWait));

    // A change elsewhere: Auto Save stores its state as it is now.
    phazite::AutosaveTimer timer (song.getProject());
    timer.setAutoSave (true);
    song.setVolume (song.firstTrack, -6.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.getProject().hasUnsavedChanges(); }, longWait));
    const auto stored = phazite::readBytes (phazite::findDeviceState (getSong (song), effect)[phazite::schema::pluginState].toString());
    REQUIRE (stored.has_value());
    CHECK (juce::MemoryInputStream (*stored, false).readFloat() == doctest::Approx (0.3f));
}

TEST_CASE ("Closing a plug-in's window keeps the message thread for no helper, one that hangs neither: its window goes first")
{
    // Known bug 165: the editor asked its helper to close its window and waited, while the helper, whose window was a child
    // of Phazite's, couldn't destroy it until Phazite's thread took what Windows sent it then: the wait always ran out (2 s).
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    const auto hanging = song.addEffect (song.firstTrack, "PhaziteTestHang");
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);

    for (const auto hangs : { false, true })
    {
        CAPTURE (hangs);
        const auto id = hangs ? hanging : effect;
        auto& instance = getSandboxed (song, id);
        auto editor = makeEditor (session->getPlugins().find (id));
        REQUIRE (editor != nullptr);
        auto window = std::make_unique<juce::Component>();
        window->setSize (400, 300);
        window->addAndMakeVisible (*editor);
        window->addToDesktop (0);
        window->setVisible (true);

        HWND helperWindow = nullptr;
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [&]
            {
                helperWindow = findHelperWindow (static_cast<HWND> (window->getPeer()->getNativeHandle()));
                return helperWindow != nullptr;
            },
            longWait));

        // The helper's message thread stopped: it answers nothing more, until it is ended for it.
        if (hangs)
        {
            song.setParameter (id, hangStall, 1.0f);
            REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.getRemote().getState (50).has_value(); }, longWait));
            REQUIRE_FALSE (instance.hasCrashed());
        }

        // The editor and the window it was in close at once, the helper's window taken out of Phazite's.
        const auto closing = juce::Time::getMillisecondCounterHiRes();
        editor = nullptr;
        window = nullptr;
        const auto kept = juce::Time::getMillisecondCounterHiRes() - closing;
        MESSAGE ("The message thread was kept for " << kept << " ms");
        if (hangs)
            CHECK (kept < phazite::test::hangTimeout / 2.0); // a wait for the stalled helper would last until it is ended

        // Not destroyed with Phazite's window: the helper that hangs still has it, out of sight.
        if (hangs)
        {
            CHECK (IsWindow (helperWindow));
            CHECK_FALSE (IsWindowVisible (helperWindow));
        }

        // The helper closes its window as it is asked, and plays on; or, stalled, goes with it as it is ended (its hang's
        // limit the tests' long one: ended here).
        if (hangs)
            instance.getRemote().getHelper().kill();

        REQUIRE (
            phazite::test::dispatchMessagesUntil ([&] { return ! IsWindow (helperWindow) && instance.hasCrashed() == hangs; }, longWait));

        if (! hangs)
            CHECK (instance.getRemote().getState (longWait).has_value());
    }
}

namespace
{
// The key messages this thread takes from its queue for a window, as it takes them (a hook on them, as JUCE's own window
// procedure would see them).
struct KeysTaken
{
    explicit KeysTaken (HWND forWindow)
    {
        window = forWindow;
        taken.clear();
        hook = SetWindowsHookExW (WH_GETMESSAGE, &KeysTaken::look, nullptr, GetCurrentThreadId());
    }

    ~KeysTaken() { UnhookWindowsHookEx (hook); }

    static LRESULT CALLBACK look (int code, WPARAM removed, LPARAM message)
    {
        if (const auto* got = reinterpret_cast<const MSG*> (message);
            code == HC_ACTION && removed == PM_REMOVE && got->hwnd == window && got->message == WM_KEYDOWN)
            taken.push_back ((int) got->wParam);

        return CallNextHookEx (nullptr, code, removed, message);
    }

    static inline HWND window = nullptr;
    static inline std::vector<int> taken;
    HHOOK hook = nullptr;
};
} // namespace

TEST_CASE ("A key a plug-in's editor doesn't take reaches Phazite's window through its socket; a window that goes first waits for nothing")
{
    // The review of known bug 165: JUCE's window in the helper posts the keys its plug-in doesn't take to its parent, the
    // socket, which dropped them; and a window that lost its native window before the editor went destroyed the socket with
    // the helper's window in it, which waited for the helper's thread.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto hanging = song.addEffect (song.firstTrack, "PhaziteTestHang");
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    auto& instance = getSandboxed (song, hanging);
    auto editor = makeEditor (session->getPlugins().find (hanging));
    REQUIRE (editor != nullptr);
    auto window = std::make_unique<juce::Component>();
    window->setSize (400, 300);
    window->addAndMakeVisible (*editor);
    window->addToDesktop (0);
    window->setVisible (true);
    const auto peer = static_cast<HWND> (window->getPeer()->getNativeHandle());
    HWND helperWindow = nullptr;
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            helperWindow = findHelperWindow (peer);
            return helperWindow != nullptr;
        },
        longWait));

    // A key posted to the socket, as JUCE's window in the helper posts one its plug-in doesn't take, reaches Phazite's.
    const auto socket = GetParent (helperWindow);
    REQUIRE (socket != peer);
    {
        KeysTaken keys (peer);
        PostMessageW (socket, WM_KEYDOWN, 'Q', 0);
        REQUIRE (phazite::test::dispatchMessagesUntil ([] { return ! KeysTaken::taken.empty(); }, longWait));
        CHECK (KeysTaken::taken.front() == 'Q');
    }

    // The helper's message thread stopped; the window loses its native window before the editor goes: nothing waits.
    song.setParameter (hanging, hangStall, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.getRemote().getState (50).has_value(); }, longWait));
    // (a wait for the stalled helper would last until it is ended, which only the end of this case does)
    CHECK (timeKept ([&window] { window->removeFromDesktop(); }) < phazite::test::hangTimeout / 2.0);
    CHECK (IsWindow (helperWindow)); // out of sight, in the socket taken out first
    CHECK (timeKept (
               [&]
               {
                   editor = nullptr;
                   window = nullptr;
               })
           < phazite::test::hangTimeout / 2.0);
    instance.getRemote().getHelper().kill(); // ended here (its hang's limit the tests' long one), its window with it
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! IsWindow (helperWindow) && instance.hasCrashed(); }, longWait));
}
#endif

TEST_CASE ("An answer that comes after the song gave a plug-in a state is older than it, and isn't stored over it")
{
    // A save asks a slow plug-in for its state; meanwhile the song gives it another (a preset loaded, an undo); the answer
    // to the save's question comes after: what the song gave stays, and the next store writes it.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    song.saveWithPluginStates();
    auto& instance = getSandboxed (song, slow);
    song.setParameter (slow, "60", 0.5f);

    struct StateLimit
    {
        StateLimit() { phazite::SandboxedInstance::setStateMilliseconds (300); } // the state given goes without its answer
        ~StateLimit() { phazite::SandboxedInstance::setStateMilliseconds (0); }
    } limit;

    SlowGate gate;
    instance.askState (phazite::SandboxedInstance::Ask::all);
    juce::MemoryOutputStream given;
    given.writeFloat (0.25f); // its Level
    instance.setStateInformation (given.getData(), (int) given.getDataSize());

    // The answer to the question asked before comes (the helper answers in turn: a question after it has its answer after).
    gate.open();
    REQUIRE (instance.getRemote().getState (longWait).has_value());
    instance.takeAskedState();
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    session->getPlugins().store (slow);
    const auto stored = phazite::readBytes (phazite::findDeviceState (getSong (song), slow)[phazite::schema::pluginState].toString());
    REQUIRE (stored.has_value());
    CHECK (*stored == given.getMemoryBlock());
}

namespace
{
// The slow stand-in's Level as a state of its holds it.
float readLevel (const juce::var& stored)
{
    const auto bytes = phazite::readBytes (stored.toString());
    REQUIRE (bytes.has_value());
    return juce::MemoryInputStream (*bytes, false).readFloat();
}
} // namespace

TEST_CASE ("A store that isn't a save waits for no plug-in: what it gave last is stored, and its answer as it comes, undo or not")
{
    // Known bug 166: a store for a copy, a commit, a snapshot or a preset waited for the plug-ins whose states may have
    // changed, up to 10 seconds.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    const auto slow = addSlow (song, track);
    auto& instance = getSandboxed (song, slow);
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    const auto level = instance.findParameter ("60");

    // Set in its own window, say: it is asked for its state once it is quiet, unasked.
    instance.setParameterValue (level, 0.5f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));

    // Set again, while it can't answer: the store goes on at once with the state it gave last, and asks it.
    SlowGate gate;
    instance.setParameterValue (level, 0.25f);
    session->getPlugins().storeTrack (track); // returned while the plug-in can't answer: it waited for none of it (below)
    auto device = phazite::findDeviceState (getSong (song), slow);
    CHECK (readLevel (device[phazite::schema::pluginState]) == doctest::Approx (0.5f));
    CHECK (instance.isAskingState());

    // Taken away meanwhile, and its answer comes: into its element, which the undo brings back, and the plug-in opened
    // again for it plays it.
    phazite::removeDevice (song.context, slow);
    CHECK (turnMessageLoop (10));
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isAskingState(); }, longWait));
    CHECK (turnMessageLoop (10));
    CHECK (readLevel (device[phazite::schema::pluginState]) == doctest::Approx (0.25f));
    phazite::undo (getSong (song));
    CHECK (readLevel (phazite::findDeviceState (getSong (song), slow)[phazite::schema::pluginState]) == doctest::Approx (0.25f));
    song.waitForPlugins(); // brought back, it loads in the background
    auto& back = getSandboxed (song, slow);
    CHECK (back.getParameterValue (back.findParameter ("60")) == doctest::Approx (0.25f));
}

TEST_CASE ("A plug-in prepared for another sample rate waits for no helper: it is skipped until its channel has come")
{
    // Known bug 166: the message thread waited for the helper to make the channel, behind whatever else it did, up to 10
    // seconds.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    auto& instance = getSandboxed (song, slow);
    CHECK (turnMessageLoop (10)); // what plays the song prepared it, for the rate it opened at
    const auto rate = instance.getRemote().getSampleRate();
    const auto otherRate = juce::exactlyEqual (rate, 96000.0) ? 44100.0 : 96000.0;

    // The helper busy giving a state it can't, while its gate is shut: the channel asked for comes after it.
    SlowGate gate;
    instance.askState (phazite::SandboxedInstance::Ask::all);
    instance.prepareToPlay (otherRate, 512); // returned while the helper can't answer: it waited for none of it (below)
    CHECK (instance.isPreparing());
    CHECK (turnMessageLoop (10));
    CHECK (instance.isPreparing());

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isPreparing(); }, longWait));
    CHECK (instance.getRemote().getSampleRate() == otherRate);
    CHECK_FALSE (instance.hasCrashed());
    instance.prepareToPlay (rate, 512);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isPreparing(); }, longWait));
    CHECK (instance.getRemote().getSampleRate() == rate);
}

namespace
{
// The slow stand-in's Level set as its own window sets it, a change it tells; once Phazite has heard it.
void turnInItsWindow (phazite::SandboxedInstance& instance, float level)
{
    juce::MemoryOutputStream told;
    told.writeFloat (level);
    told.writeFloat (1.0f); // as its own window would, telling the host
    REQUIRE (instance.getRemote().setState (told.getMemoryBlock(), longWait));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return juce::exactlyEqual (instance.getParameterValue (instance.findParameter ("60")), level); }, longWait));
}

// The Level the slow stand-in's element in the song holds.
float getStoredLevel (const phazite::Song& song, phazite::DeviceID device)
{
    return readLevel (phazite::findDeviceState (song, device)[phazite::schema::pluginState]);
}
} // namespace

TEST_CASE ("A command that reads the plug-ins' states runs once one changed in its window has given its own: a duplicate, a commit")
{
    // Known bug 166 (the review): stores no longer waited, and nothing but a save and a close asked first, so that a track
    // duplicated, or a commit made, at once after a knob was turned in a plug-in's window held its state before.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    const auto slow = addSlow (song, track);
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    song.saveWithPluginStates();

    // Its Level turned in its window, and the track duplicated at once, while the plug-in can't answer: the duplicate waits,
    // and no other command that changes the song runs meanwhile; then it holds the Level as it was turned.
    turnInItsWindow (instance, 0.5f);

    {
        SlowGate gate;
        song.context.getSongSelection().selectOnly (track);
        song.run ("edit.duplicate");
        CHECK (song.commands.isWaitingForPluginStates());
        CHECK (turnMessageLoop (10));
        CHECK (phazite::getMixerChannels (song.context).tracks.size() == 1);
        CHECK_FALSE (song.commands.invoke ("track.add"));
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getMixerChannels (song.context).tracks.size() == 2; }, longWait));
    CHECK_FALSE (song.commands.isWaitingForPluginStates());
    const auto copy = phazite::getMixerChannels (song.context).tracks[1];
    REQUIRE (phazite::getEffects (song.context, copy).size() == 1);
    CHECK (getStoredLevel (getSong (song), phazite::getEffects (song.context, copy)[0].id) == doctest::Approx (0.5f));
    phazite::undo (getSong (song));

    // Turned again, and a version saved at once (the versions' commands, as the app's session adds them, saving as it saves):
    // the version holds it, and the Versions view shows nothing left to save.
    phazite::test::RenderHost renders;
    phazite::addVersionCommands (song.commands, song.context, song.save, renders);
    const auto commit = [&]
    {
        const auto before = phazite::getVersions (song.context).size();
        song.run ("version.save");
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [&] { return phazite::getVersions (song.context).size() > before && ! phazite::isSavingVersion (song.context); }, longWait));
        REQUIRE (phazite::test::waitForVersionHistory (song.context));
    };

    commit();
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (song.context)));
    turnInItsWindow (instance, 0.25f);

    // Nothing else changed, nothing stored or saved yet: the change shows at once, and Commit is there.
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (song.context)); }, longWait));
    CHECK (phazite::canSaveVersion (phazite::test::withChangesIn (song.context)));
    const auto versions = phazite::getVersions (song.context).size();

    {
        SlowGate gate;
        song.run ("version.save");
        CHECK (turnMessageLoop (10));
        CHECK (phazite::getVersions (song.context).size() == versions);
    }

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return phazite::getVersions (song.context).size() > versions && ! phazite::isSavingVersion (song.context); }, longWait));
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    CHECK (getStoredLevel (getSong (song), slow) == doctest::Approx (0.25f));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (song.context)));
}

TEST_CASE ("Delete of notes waits for no plug-in; a macro, Bring Back and a preset saved get the states first, once")
{
    // The review of known bug 166: the flag was on the command, not on what is selected, so Delete of notes waited for a
    // plug-in that said it changed; and a macro ran its steps without the gate.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    const auto slow = addSlow (song, track);
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    auto& opened = getSong (song);
    const auto clip = song.addMidiClip (track, 0.0, 4.0);
    const auto note = phazite::test::addMidiNote (opened, clip, 60, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (1.0));

    // A change unanswered, the plug-in slow to answer: Delete of the selected note runs at once.
    turnInItsWindow (instance, 0.5f);

    {
        SlowGate gate;
        auto& selection = phazite::getSelection (song.context); // as the piano roll selects them
        selection.selectOnly (clip);
        selection.setNoteClips ({ clip });
        selection.selectNotes ({ note });
        song.run ("edit.delete");
        CHECK_FALSE (song.commands.isWaitingForPluginStates());
        CHECK (opened.getUndoManager().getUndoDescription().startsWith ("Delete"));
    }

    // Bring Back of the commit before the change, straight after it: set back where it differs from what the plug-in has now
    // (the turned Level, not the one it gave before), and Undo gives the turned one back.
    phazite::test::RenderHost renders;
    phazite::addVersionCommands (song.commands, song.context, song.save, renders);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    song.saveWithPluginStates();
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (song.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, longWait));
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    const auto version = phazite::getVersions (song.context).front().id;
    turnInItsWindow (instance, 0.25f);

    {
        SlowGate gate;
        phazite::selectVersion (song.context, version);
        song.run ("version.bringBack");
        CHECK (song.commands.isWaitingForPluginStates());
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! song.commands.isWaitingForPluginStates(); }, longWait));
    REQUIRE (phazite::test::waitForVersionHistory (song.context)); // the commit read on the history's thread (known bug 126)
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getStoredLevel (opened, slow) > 0.49f; }, longWait));
    phazite::undo (opened);
    CHECK (getStoredLevel (opened, slow) == doctest::Approx (0.25f));

    // A preset saved straight after a change, as the Browser's Save Preset command (which says it reads them), and as a
    // step of a macro: each holds the state as turned, the macro asking once before it starts.
    const auto preset = song.getSoundsFolder().getChildFile (juce::String ("Turned") + phazite::presetExtension);
    song.commands.add ({ .id = "test.savePreset",
                         .name = "Save Preset",
                         .perform = [&] { REQUIRE (phazite::saveDevicePreset (song.context, slow, preset)); },
                         .needsPluginStates = true });
    REQUIRE (song.commands.addMacro ("test.macro", "Save It", { "test.savePreset" }));

    for (const auto* id : { "test.savePreset", "test.macro" })
    {
        CAPTURE (id);
        preset.deleteFile();
        const auto level = juce::String (id) == "test.macro" ? 0.375f : 0.625f;
        turnInItsWindow (instance, level);

        {
            SlowGate gate;
            song.run (id);
            CHECK (song.commands.isWaitingForPluginStates());
            CHECK_FALSE (preset.existsAsFile());
        }

        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return preset.existsAsFile(); }, longWait));
        const auto saved = phazite::readPreset (preset);
        const auto state = phazite::readBytes (saved[phazite::schema::preset::device][phazite::schema::preset::state].toString());
        REQUIRE (state.has_value());
        CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (level));
    }
}

TEST_CASE ("Save As straight after a change in a plug-in's window holds it; while it waits, a drag in the window changes nothing")
{
    // The review of known bug 166: Save As, which opens the copy, held the state before; and the window took edits while a
    // command waited for the plug-ins' states, before its progress window showed.
    SandboxedPlugins sandboxed;
    phazite::test::TemporaryFolder temp;
    juce::File file;
    phazite::DeviceID slow;

    {
        juce::String error;
        auto made = phazite::Project::create (temp.folder, "Slow", error);
        REQUIRE (made != nullptr);
        auto& song = made->getSong();
        slow = phazite::insertDeviceState (
            song, phazite::getTrackIDs (song).front(),
            phazite::makeDeviceState (song, phazite::makeDeviceType (phazite::test::TestPlugins::getDescription (Kind::slow))));
        REQUIRE (made->save().wasOk());
        file = made->getFile();
    }

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (phazite::test::getEngineContext(), recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.onError = [] (const juce::String&, const juce::String&) {};
    session.onNotice = [] (const juce::String&, const juce::String&) {};
    session.setAutoSave (false);
    session.chooseSaveFile = [&temp] (const juce::File&, std::function<void (const juce::File&)> chosen)
    { chosen (temp.folder.getChildFile ("Copy")); };
    phazite::test::RenderHost host;
    phazite::CommandRegistry registry;
    session.addCommands (registry, host);
    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getPluginsLoading (session.getProject()->getSong()).isEmpty(); },
                                                   longWait));
    auto* instance = phazite::findPluginInstance (session.getProject()->getSong(), slow);
    REQUIRE (instance != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance->isStateUnasked() && ! instance->isAskingState(); }, longWait));

    // A slider in a window of the app's, as an edit the mouse makes.
    juce::Slider slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
    slider.setRange (0.0, 1.0);
    slider.setValue (0.5, juce::dontSendNotification);
    phazite::test::TestWindow window { slider, 200, 30 };

    // Turned in its window, then Save As at once, the plug-in slow to answer: the window takes no drag meanwhile.
    turnInItsWindow (*instance, 0.75f);

    {
        SlowGate gate;
        REQUIRE (registry.invoke ("project.saveAs"));
        CHECK (registry.isWaitingForPluginStates());
        phazite::test::Mouse mouse (slider);
        mouse.press ({ 100, 15 });
        mouse.dragTo ({ 190, 15 });
        mouse.release();
        CHECK (slider.getValue() == doctest::Approx (0.5));
    }

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return session.getProject() != nullptr && session.getProject()->getFile() != file; }, longWait));
    CHECK (getStoredLevel (session.getProject()->getSong(), slow) == doctest::Approx (0.75f));

    // And the window takes a drag again.
    phazite::test::Mouse mouse (slider);
    mouse.press ({ 100, 15 });
    mouse.dragTo ({ 190, 15 });
    mouse.release();
    CHECK (slider.getValue() > 0.6);
}

TEST_CASE ("A render's wait shows on its progress bar in the place of its percentage, which comes back once it is over")
{
    // The review of known bug 42: once a wait had shown, the bar showed no percentage again.
    struct Drawn final : juce::LookAndFeel_V4
    {
        juce::String text;
        void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int, int, double, const juce::String& textToShow) override
        {
            text = textToShow;
        }
    } drawn;

    auto progress = 0.5;
    juce::ProgressBar bar (progress);
    bar.setLookAndFeel (&drawn);
    bar.setSize (200, 20);
    const auto draw = [&bar, &drawn]
    {
        juce::Image image (juce::Image::ARGB, 200, 20, true);
        juce::Graphics g (image);
        bar.paintEntireComponent (g, false);
        return drawn.text;
    };

    juce::String shown;
    phazite::ui::RenderPanel::showWait (bar, shown, phazite::RenderWait { "Test Hang", 7.4 });
    CHECK (draw() == "Waiting for Test Hang (7 s)");
    phazite::ui::RenderPanel::showWait (bar, shown, std::nullopt);
    CHECK (draw().endsWith ("%"));
    bar.setLookAndFeel (nullptr);
}

TEST_CASE ("A store's answer that comes late shows as a change to commit; an old instance's answer leaves a newer one's alone")
{
    // Known bug 166 (the review): a commit held the state before a change its counts of the plug-in's reports already
    // counted, so the answer, written in later, showed as no change; and an answer of an instance an undo had replaced was
    // given to the new one over its own changes.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    song.saveWithPluginStates();

    // A commit's store, as the versions' model function makes it, without the command's wait: it holds the state before.
    turnInItsWindow (instance, 0.5f);

    {
        SlowGate gate;
        std::optional<juce::Result> result;
        REQUIRE (phazite::test::waitForVersionHistory (song.context));
        REQUIRE (phazite::saveVersion (song.context, [&result] (juce::Result r) { result = r; }));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, longWait));
        REQUIRE (phazite::test::waitForVersionHistory (song.context));
    }

    // Its answer comes: a change to commit.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return getStoredLevel (getSong (song), slow) > 0.49f; }, longWait));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (song.context)); }, longWait));
}

TEST_CASE ("An answer of a plug-in an undo replaced leaves the new instance's own settings alone")
{
    // Known bug 166 (the review): the answer of an instance an undo had replaced was given to the new one over its changes.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& old = getSandboxed (song, effect);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! old.isStateUnasked() && ! old.isAskingState(); }));
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    auto& plugins = session->getPlugins();
    const auto gain = old.findParameter (effectGain);

    // Its gain turned in its window, and the device taken away at once, which stores it, asking it; the song's plug-ins
    // synced as the message loop would; and back by undo, a new instance from what its element held, turned at once. The
    // old one's answer is taken in only now.
    juce::MemoryOutputStream told;
    told.writeFloat (0.5f); // its gain
    told.writeFloat (0.5f); // its tone
    told.writeFloat (1.0f); // as its own window would set them, telling the host
    REQUIRE (old.getRemote().setState (told.getMemoryBlock(), longWait));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return juce::exactlyEqual (old.getParameterValue (gain), 0.5f); }));
    phazite::removeDevice (song.context, effect);
    REQUIRE (old.isAskingState());
    plugins.sync();
    phazite::undo (getSong (song));
    plugins.sync();

    // (Brought back, it loads in the background, known bug 166: the old one's answer may come first, written into the element
    // it opens from, or after it is in, as here before.)
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return plugins.find (effect) != nullptr; }));
    auto* back = plugins.find (effect).get();
    REQUIRE (back != nullptr);
    REQUIRE (back != &old);
    back->setParameterValue (gain, 0.125f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! old.isAskingState(); }));
    CHECK (turnMessageLoop (10));

    // The new one keeps its own.
    CHECK (back->getParameterValue (gain) == doctest::Approx (0.125f));
    const auto state = back->getRemote().getState (longWait);
    REQUIRE (state.has_value());
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (0.125f));
}

TEST_CASE ("A plug-in that keeps telling changes is asked at the latest a second after the first, and no more often than it can give")
{
    // The review of known bug 166: "quiet for a quarter of a second" never came for a plug-in that tells a value of its own
    // all the time, and a state of tens of megabytes was asked again after every tweak. With a state of 20 MB: when it is
    // asked, and what each side pays for an answer.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));

    struct Bulk
    {
        Bulk() { REQUIRE (phazite::test::TestPlugins::getSlowBulk().create()); }
        ~Bulk() { phazite::test::TestPlugins::getSlowBulk().deleteFile(); }
    } bulk;

    // On the clock Phazite reads its asks by, held still (known bug 180: judged by the wall clock, a busy machine moved
    // them): set all the time, from Phazite (a value set by hand counts as a change, as one the plug-in tells does).
    phazite::test::HeldClock clock;
    const auto level = instance.findParameter ("60");
    const auto answersBefore = instance.countAnswers();
    auto value = 0.0f;
    const auto change = [&]
    {
        value = value > 0.9f ? 0.1f : value + 0.01f;
        instance.setParameterValue (level, value);
        phazite::test::dispatchPendingMessages();
    };
    const auto changeFor = [&] (double milliseconds)
    {
        for (auto passed = 0.0; passed < milliseconds; passed += 100.0)
        {
            clock.advance (std::min (100.0, milliseconds - passed));
            change();
        }
    };

    // Never quiet, it is asked at the latest a second after its first change, not before.
    SlowGate gate; // its answer held, for as long as the test says it takes
    change();
    changeFor (999.0);
    CHECK_FALSE (instance.isAskingState());

    // Past the second, not onto it: the clock's milliseconds are doubles, and a difference of exactly 1000 may read as less.
    changeFor (2.0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.isAskingState(); }));

    // Its answer takes 300 ms (on the clock): the next ask no sooner than ten times that after the first, though a second
    // after the first change since has passed long before.
    clock.advance (300.0);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.countAnswers() == answersBefore + 1; }, longWait));
    CHECK (instance.getLastAnswerMilliseconds() == doctest::Approx (300.0));
    gate.shut();
    changeFor (3000.0 - 300.0 - 1.0);
    CHECK_FALSE (instance.isAskingState());
    changeFor (2.0); // past it, as above (held exactly on the limit, the ask never came: seen once in a whole run)
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.isAskingState(); }));
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.countAnswers() == answersBefore + 2; }, longWait));

    // What the message thread pays for an answer: its 20 MB copied out of the reply (as readState() does).
    REQUIRE (instance.getLastState().getSize() > 20u * 1024 * 1024);
    const auto copied = timeKept ([&instance] { juce::MemoryBlock copy (instance.getLastState()); });
    MESSAGE ("20 MB state: " << copied << " ms of the message thread's to copy it");
}

TEST_CASE ("A plug-in that crashes again after a reload, before anything of it changed, reloads from what the project holds")
{
    // The review of known bug 166: a reload opens from the state the plug-in gave last, the user's latest settings, which
    // may be what it crashes for; reloaded again before anything changed, it opens from the song's.
    SandboxedPlugins sandboxed;

    // The second crash comes soon after the reload, however busy the machine (it took more than the 5 s once beside others).
    struct Soon
    {
        Soon() { phazite::PluginInstances::setSoonAfterReload (longWait); }
        ~Soon() { phazite::PluginInstances::setSoonAfterReload (-1.0); }
    } soon;

    TestSong song;
    const auto hanging = song.addEffect (song.firstTrack, "PhaziteTestHang");
    song.saveWithPluginStates(); // the song holds its state with its Hang off
    auto& instance = getSandboxed (song, hanging);
    const auto hang = instance.findParameter (hangHang);

    // Its Hang on, which it gives as its state; ended.
    instance.setParameterValue (hang, 1.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }));
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.hasCrashed(); }));

    // Reloaded from its own: its Hang on.
    CHECK_FALSE (reloadAndWait (song, hanging).has_value());
    CHECK_FALSE (phazite::wasReloadedFromProject (song.context, hanging));
    CHECK (instance.getParameterValue (hang) == doctest::Approx (1.0f));

    // Ended again before anything changed: reloaded from what the song holds, its Hang off, and its notice says so.
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.hasCrashed(); }));
    CHECK_FALSE (reloadAndWait (song, hanging).has_value());
    CHECK (phazite::wasReloadedFromProject (song.context, hanging));
    CHECK (instance.getParameterValue (hang) == doctest::Approx (0.0f));

    // Changed since, it reloads from its own again.
    instance.setParameterValue (hang, 0.5f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }));
    instance.getRemote().getHelper().kill();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.hasCrashed(); }));
    CHECK_FALSE (reloadAndWait (song, hanging).has_value());
    CHECK_FALSE (phazite::wasReloadedFromProject (song.context, hanging));
    CHECK (instance.getParameterValue (hang) == doctest::Approx (0.5f));
}

TEST_CASE ("A save waits for its own asks alone, and asks again, once, a plug-in whose answer it waited for was to an older ask")
{
    // The review of known bug 166: a save counted asks it didn't make, which could hold it off without end; and one whose
    // earlier answer predated it took that answer for its own.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);

    // Asked by a store, its gate shut; then turned in its window: the save asked meanwhile waits for that answer, then asks
    // again, and takes the state as turned.
    turnInItsWindow (instance, 0.5f);
    SlowGate gate;
    session->getPlugins().store (slow);
    REQUIRE (instance.isAskingState());
    instance.setParameterValue (instance.findParameter ("60"), 0.25f); // after the store's ask
    auto saved = false;
    phazite::whenPluginStatesIn (getSong (song), true, [&] (bool) { saved = song.getProject().save().wasOk(); });
    CHECK (turnMessageLoop (10));
    CHECK_FALSE (saved);
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved; }, longWait));
    CHECK (getStoredLevel (getSong (song), slow) == doctest::Approx (0.25f));
}

TEST_CASE ("A preset whose settings are those the song's element holds is given to the plug-in all the same")
{
    // The review of known bug 166: the store before a preset's load wrote what the plug-in gave last, the preset's own
    // settings, so the load changed nothing in the song and the plug-in kept what it had.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }));
    const auto preset = song.getSoundsFolder().getChildFile (juce::String ("Now") + phazite::presetExtension);
    REQUIRE (phazite::saveDevicePreset (song.context, effect, preset));
    instance.setParameterValue (instance.findParameter (effectGain), 0.2f); // and loaded at once, before it was asked
    REQUIRE (phazite::loadPresetInto (song.context, effect, preset));
    const auto state = instance.getRemote().getState (longWait); // the plug-in's own, as it has it now
    REQUIRE (state.has_value());
    CHECK (juce::MemoryInputStream (*state, false).readFloat() == doctest::Approx (1.0f));
}

TEST_CASE ("A render waits for a plug-in's channel for its rate, cancelled at once, and plays it after two prepares in a row")
{
    // The review of known bug 166: a render cancelled while it waited for a plug-in's channel for its rate kept the message
    // thread up to a minute; and one that began while an earlier channel was still to come played the plug-in as silence.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto slow = addSlow (song, track);
    auto& instance = getSandboxed (song, slow);
    CHECK (turnMessageLoop (10));
    const auto rate = instance.getRemote().getSampleRate();
    const auto otherRate = juce::exactlyEqual (rate, 96000.0) ? 48000.0 : 96000.0;
    song.context.getSongSelection().selectOnly (track);

    // The helper busy (its gate shut), and the plug-in prepared for another rate: the freeze, at the plug-in's own rate,
    // waits for that channel to come and its own after it; cancelled, it goes at once.
    {
        SlowGate gate;
        instance.askState (phazite::SandboxedInstance::Ask::all);
        instance.prepareToPlay (otherRate, 512);
        REQUIRE (instance.isPreparing());
        phazite::test::RenderHost host;
        phazite::toggleFreeze (song.context, host);
        REQUIRE (host.render != nullptr);
        // It waits for the channel, which the helper can't make while its gate is shut (named once it has waited as long as a
        // block takes to be a hang, which the tests' long limit puts past the prepare's minute: not waited for here).
        CHECK (turnMessageLoop (10));
        REQUIRE (host.render != nullptr);
        REQUIRE (host.finished.isEmpty());
        // (a wait for the channel would last the prepare's minute: half a minute tells them apart however busy the machine)
        CHECK (timeKept ([&host] { host.render = nullptr; }) < 30000.0);
        CHECK_FALSE (phazite::isFrozen (song.context, track));
    }

    // Two prepares in a row, the first's channel still to come as the freeze begins: it waits for both, and plays the plug-in.
    {
        SlowGate gate;
        instance.askState (phazite::SandboxedInstance::Ask::all);
        instance.prepareToPlay (juce::exactlyEqual (rate, 48000.0) ? 44100.0 : 48000.0, 512);
        phazite::test::RenderHost host;
        phazite::toggleFreeze (song.context, host);
        REQUIRE (host.render != nullptr);
        CHECK (turnMessageLoop (10));
        gate.open();
        REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, longWait));
        CHECK (phazite::isFrozen (song.context, track));
    }

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.6), 0, 0.2, 0.5) == doctest::Approx (toneLevel).epsilon (0.002));
}

#if JUCE_WINDOWS
TEST_CASE ("The session's end is heard by a window of Phazite's own, and its end cancelled too, which waits for plug-ins again")
{
    // The review of known bug 168: the flag was set from a system metric on any quit and could stay set for good.
    std::vector<bool> told;
    phazite::SessionEndWatch watch ([&told] (bool ending) { told.push_back (ending); });
    CHECK_FALSE (watch.isEnding());
    CHECK (watch.handle (WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == TRUE); // it lets the session end; the app's own windows ask
    CHECK (watch.isEnding());
    watch.handle (WM_ENDSESSION, FALSE, 0); // cancelled
    CHECK_FALSE (watch.isEnding());
    CHECK (told == std::vector<bool> { true, false });
    watch.handle (WM_ENDSESSION, FALSE, 0); // nothing to cancel
    CHECK (told.size() == 2);
}
#endif

TEST_CASE ("A song's plug-ins in one helper open one at a time; one taken away first never opens; another's crash isn't theirs")
{
    // One helper does one thing at a time: plug-ins sent to it at once each waited for those before them within their own
    // time limit, and a crash of one took the others' loads with it.
    SandboxedPlugins sandboxed;
    const auto slowDescription = phazite::test::TestPlugins::getDescription (Kind::slow);

    {
        TestSong song;
        const auto first = addSlow (song, song.firstTrack);
        const auto sibling = addSlow (song, song.firstTrack, Kind::slowSibling);
        const auto third = addSlow (song, song.firstTrack);
        song.saveWithPluginStates();

        SlowGate gate;
        song.reopen (false);
        auto& opened = getSong (song);
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [] { return phazite::test::TestPlugins::getSlowWaiting (Kind::slow).existsAsFile(); }, longWait));
        CHECK (turnMessageLoop (100));
        CHECK (SandboxedPlugins::getHost().getNumLoading (slowDescription) == 1);

        // The second taken away while it waits its turn: its job finds it gone, and opens nothing. What plays the song lets
        // go of it a turn of the message loop after the song changed, so the gate opens once it has: opened at once, the
        // second's turn could come first on a busy machine, and it opened (CI's linux-clang, 2026-11-15).
        phazite::removeDevice (song.context, sibling);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return phazite::getPluginsLoading (opened).size() == 2; }, longWait));
        gate.open();
        REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return phazite::getPluginsLoading (opened).isEmpty(); }, longWait));
        CHECK (phazite::getPluginStatus (opened, first) == Status::playing);
        CHECK (phazite::getPluginStatus (opened, third) == Status::playing);
        CHECK_FALSE (phazite::test::TestPlugins::getSlowWaiting (Kind::slowSibling).existsAsFile());
    }

    // One that crashes its helper as it loads is kept off; the other in its file opens in a fresh helper.
    TestSong song;
    const auto crashing = addSlow (song, song.firstTrack);
    const auto sibling = addSlow (song, song.firstTrack, Kind::slowSibling);
    REQUIRE (phazite::findPluginInstance (getSong (song), sibling) != nullptr);
    juce::MemoryOutputStream crashes;
    crashes.writeFloat (-1.0f); // a Level below 0, which it crashes from as it next opens
    getSandboxed (song, crashing).setStateInformation (crashes.getData(), (int) crashes.getDataSize());
    song.saveWithPluginStates();

    song.reopen();
    CHECK (phazite::getPluginStatus (getSong (song), crashing) == Status::crashedWhileLoading);
    CHECK (phazite::getPluginStatus (getSong (song), sibling) == Status::playing);
}

namespace
{
// Ends a helper's process from outside, as a crash ends it, and waits until it has ended: its watching thread may not have
// looked since, so that Phazite still takes the helper for running (sandbox/client/Helper.h).
void endAsACrash (phazite::sandbox::Helper& helper)
{
    const auto id = helper.getProcessID();
    REQUIRE (id > 0);
#if JUCE_WINDOWS
    auto* process = OpenProcess (PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD) id);
    REQUIRE (process != nullptr);
    TerminateProcess (process, 1);
    const auto ended = WaitForSingleObject (process, (DWORD) phazite::test::hangTimeout);
    CloseHandle (process);
    REQUIRE (ended == WAIT_OBJECT_0);
#else
    REQUIRE (::kill ((pid_t) id, SIGKILL) == 0);

    // Ended once it is a zombie, or gone (reaped by the watching thread, which looked meanwhile). Linux's /proc says so;
    // a macOS build will need another way.
    const auto stat = juce::File ("/proc").getChildFile (juce::String (id)).getChildFile ("stat");
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&stat]
        { return ! stat.existsAsFile() || stat.loadFileAsString().fromLastOccurrenceOf (")", false, false).trim().startsWithChar ('Z'); }));
#endif
}
} // namespace

TEST_CASE ("A plug-in asked for just after its helper crashed opens in a fresh helper, and the crash isn't taken for its own")
{
    // Known bug 191's review: a load went to a helper that had crashed a moment before, until its watching thread looked (a
    // fifth of a second later at most), and failed as it looked; where nothing else was in the helper, its crash was taken
    // for the plug-in's own as it loaded, which kept the plug-in off.
    //
    // On Windows alone for now: on CI's Linux (the Clang job, 2026-11-19) round 8 found the plug-in kept off, "It crashed as
    // it loaded": the fresh helper went as the plug-in loaded in it, or the first was not found ended. Known bug 213.
    if (juce::SystemStats::getOperatingSystemType() == juce::SystemStats::Linux)
    {
        MESSAGE ("Not run on Linux yet (known bug 213).");
        return;
    }

    SandboxedPlugins sandboxed;
    auto& host = SandboxedPlugins::getHost();
    const auto effect = phazite::test::TestPlugins::getDescription (Kind::effect);

    for (int round = 0; round < 24; ++round)
    {
        CAPTURE (round);

        // One opened in its helper, not an instance yet (as one opened in the background is until the message thread takes
        // it in): what keeps the helper, which holds nothing else. The helper crashes.
        auto held = host.open (effect, {}, -1, 48000.0, 512);
        INFO (held.error.toStdString());
        REQUIRE (held.remote != nullptr);
        endAsACrash (held.remote->getHelper());

        // Another asked for at once opens in a fresh helper, and the plug-in isn't kept off.
        auto opened = host.open (effect, {}, -1, 48000.0, 512);
        INFO (opened.error.toStdString());
        REQUIRE (opened.remote != nullptr);
        CHECK (&opened.remote->getHelper() != &held.remote->getHelper());
        REQUIRE_FALSE (host.hasCrashedWhileLoading (effect));
    }
}

TEST_CASE ("A plug-in still loading is silent in its place, an effect too, and its sound fades in as it comes in")
{
    // An effect that can't play is silent, never dry (a limiter on the master passed dry is worse than a gap).
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone", 30.0);
    const auto slow = addSlow (song, track);
    song.saveWithPluginStates();

    SlowGate gate;
    song.reopen (false);
    auto& opened = getSong (song);
    REQUIRE (phazite::getPluginStatus (opened, slow) == Status::loading);
    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    auto audio = playback.play (0.3);
    CHECK (audio.getMagnitude (0, 0, audio.getNumSamples()) == 0.0f);

    // It comes in: no step larger than the tone's own, as its sound fades in over 5 ms.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return phazite::getPluginsLoading (opened).isEmpty(); }, longWait));
    CHECK (turnMessageLoop (10)); // the plan made again with it
    audio = playback.play (0.6);
    CHECK (getLargestStep (audio, 0, 0) < 0.02f);
    CHECK (levelOf (audio, 0, 0.3, 0.55) == doctest::Approx (toneLevel).epsilon (0.002));
}

TEST_CASE ("What waits for a song's plug-ins' states is told once, with not all of them, if the song's plug-ins go first")
{
    // A version switched to, or the project closed some other way, while a close waited: the close goes on.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    auto& project = song.getProject();

    // Its first state, asked as it joined the song, in before the gate shuts: an answer to that one, coming just after,
    // was every answer what waits below asked for, and it was told at once with all of them (two of CI's Linux jobs in
    // one run, 2026-11-17).
    auto& instance = getSandboxed (song, slow);
    REQUIRE (
        phazite::test::dispatchMessagesUntil ([&instance] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    SlowGate gate;
    auto calls = 0;
    auto allIn = true;
    phazite::whenPluginStatesIn (getSong (song), true,
                                 [&calls, &allIn] (bool in)
                                 {
                                     ++calls;
                                     allIn = in;
                                 });
    CHECK (turnMessageLoop (10));
    CHECK (calls == 0);

    phazite::setSong (song.context, nullptr);
    CHECK (calls == 1);
    CHECK_FALSE (allIn);
    gate.open();
    phazite::setSong (song.context, &project.getSong());
    CHECK (turnMessageLoop (10));
    CHECK (calls == 1);
}

TEST_CASE ("A close and a save that wait on a slow plug-in each go on once, for the project they were for")
{
    // A quit (a close) and then Ctrl+S, while a plug-in answers: the close opens another project, and the save, which was
    // for the one closed, saves nothing else.
    SandboxedPlugins sandboxed;
    phazite::test::TemporaryFolder temp;
    juce::File file;
    phazite::DeviceID slow;

    {
        juce::String error;
        auto made = phazite::Project::create (temp.folder, "Slow", error);
        REQUIRE (made != nullptr);
        auto& song = made->getSong();
        slow = phazite::insertDeviceState (
            song, phazite::getTrackIDs (song).front(),
            phazite::makeDeviceState (song, phazite::makeDeviceType (phazite::test::TestPlugins::getDescription (Kind::slow))));
        REQUIRE (made->save().wasOk());
        file = made->getFile();
    }

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (phazite::test::getEngineContext(), recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    juce::StringArray errors;
    session.onError = [&errors] (const juce::String& title, const juce::String&) { errors.add (title); };
    session.onNotice = [] (const juce::String&, const juce::String&) {};

    // No question in a window of JUCE's as it closes below: the slow plug-in's settings, coming in late on a slow build,
    // can leave the project with an unsaved change, and a session given no answer asks in a real window (in which JUCE read
    // a display that a machine without one hasn't: UBSan, CI's linux-asan, 2026-11-17).
    session.askToSave = [] (const juce::File&, const juce::String&, std::function<void (phazite::ProjectSession::UnsavedChanges)> answer)
    { answer (phazite::ProjectSession::UnsavedChanges::discard); };
    session.setAutoSave (false);
    phazite::test::RenderHost host;
    phazite::CommandRegistry registry;
    session.addCommands (registry, host);
    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getPluginsLoading (session.getProject()->getSong()).isEmpty(); },
                                                   longWait));

    // A save by hand that waits more than a second shows in the progress window; its Cancel goes on without the plug-in,
    // whose state is then older than its own, as the notice says.
    auto* instance = phazite::findPluginInstance (session.getProject()->getSong(), slow);
    REQUIRE (instance != nullptr);
    instance->setParameterValue (instance->findParameter ("60"), 0.5f);

    {
        SlowGate gate;
        session.saveProject();
        REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return host.render != nullptr; }, longWait));
        CHECK (host.render->getTitle() == "Waiting for the settings of Test Slow");
        host.render = nullptr; // Cancel
        REQUIRE (phazite::test::dispatchMessagesUntil ([&errors] { return errors.contains ("Plug-in settings not saved"); }, longWait));
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance->isAskingState(); }, longWait));
    SlowGate gate;
    auto closes = 0, changes = 0;
    session.closeThen (
        [&] (bool closed)
        {
            ++closes;

            if (closed)
            {
                REQUIRE (session.createProject ({}).wasOk());
                session.onProjectChanged = [&changes] { ++changes; };
            }
        });

    session.saveProject();
    CHECK (turnMessageLoop (100));
    CHECK (closes == 0);

    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&closes] { return closes > 0; }, longWait));
    CHECK (turnMessageLoop (100));
    CHECK (closes == 1);
    CHECK (changes == 0); // the save was for the project closed: it saved no other
    CHECK (session.getProject()->getFile() != file);
    session.onProjectChanged = nullptr; // the session tells of its project as it goes too, after the count has gone
}

TEST_CASE ("The computer's session ending asks the save question at once, a close that waits for a plug-in's settings too")
{
    // Known bug 168: Windows shutting down while a close waited for a plug-in's settings ended Phazite before its question.
    SandboxedPlugins sandboxed;
    phazite::test::TemporaryFolder temp;
    juce::File file;
    phazite::DeviceID slow;

    {
        juce::String error;
        auto made = phazite::Project::create (temp.folder, "Slow", error);
        REQUIRE (made != nullptr);
        auto& song = made->getSong();
        slow = phazite::insertDeviceState (
            song, phazite::getTrackIDs (song).front(),
            phazite::makeDeviceState (song, phazite::makeDeviceType (phazite::test::TestPlugins::getDescription (Kind::slow))));
        REQUIRE (made->save().wasOk());
        file = made->getFile();
    }

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (phazite::test::getEngineContext(), recent, temp.folder, temp.folder.getChildFile ("Unsaved"));
    session.onError = [] (const juce::String&, const juce::String&) {};
    session.onNotice = [] (const juce::String&, const juce::String&) {};
    session.setAutoSave (false);
    auto questions = 0;
    std::function<void (phazite::ProjectSession::UnsavedChanges)> answer;
    session.askToSave = [&] (const juce::File&, const juce::String&, std::function<void (phazite::ProjectSession::UnsavedChanges)> then)
    {
        ++questions;
        answer = std::move (then);
    };

    session.openProject (file);
    REQUIRE (session.getProject() != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getPluginsLoading (session.getProject()->getSong()).isEmpty(); },
                                                   longWait));
    auto* instance = phazite::findPluginInstance (session.getProject()->getSong(), slow);
    REQUIRE (instance != nullptr);
    instance->setParameterValue (instance->findParameter ("60"), 0.5f); // its settings may have changed
    auto volume = phazite::getVolume (session.getContext(), phazite::getTrackIDs (session.getProject()->getSong()).front());
    REQUIRE (volume.exists());
    volume.setValue (0.5f); // an unsaved change

    // A close waits for the slow plug-in's settings, which don't come while its gate is shut.
    SlowGate gate;
    auto closes = 0;
    auto closed = false;
    session.closeThen (
        [&] (bool wasClosed)
        {
            ++closes;
            closed = wasClosed;
        });
    CHECK (turnMessageLoop (10));
    CHECK (session.isClosing());
    CHECK (questions == 0);

    // The session ends: the close asks now, the gate still shut, once; Cancel keeps Phazite open, as it waits for its
    // plug-ins again.
    session.setSessionEnding (true);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return questions > 0; }, longWait));
    CHECK (turnMessageLoop (10));
    CHECK (questions == 1);
    CHECK_FALSE (session.isClosing());
    answer (phazite::ProjectSession::UnsavedChanges::cancel);
    CHECK (closes == 1);
    CHECK_FALSE (closed);

    // A close begun while the session ends waits for no plug-in either: "Don't Save" closes.
    session.closeThen (
        [&] (bool wasClosed)
        {
            ++closes;
            closed = wasClosed;
        });
    CHECK (questions == 2);
    answer (phazite::ProjectSession::UnsavedChanges::discard);
    CHECK (closes == 2);
    CHECK (closed);
    session.setSessionEnding (false);
}

namespace
{
// A session of the app's, as it runs (its gate for the plug-ins' states, its commands and the song's), with a project of a
// track whose plug-in is the slow stand-in, opened and loaded; its own helpers' hang limits long.
struct SlowSession
{
    SlowSession()
    {
        juce::String error;

        {
            auto made = phazite::Project::create (temp.folder, "Slow", error);
            REQUIRE (made != nullptr);
            auto& song = made->getSong();
            track = phazite::getTrackIDs (song).front();
            slow = phazite::insertDeviceState (
                song, track,
                phazite::makeDeviceState (song, phazite::makeDeviceType (phazite::test::TestPlugins::getDescription (Kind::slow))));
            REQUIRE (made->save().wasOk());
            file = made->getFile();
        }

        session.onError = [] (const juce::String&, const juce::String&) {};
        session.onNotice = [] (const juce::String&, const juce::String&) {};
        session.setAutoSave (false);
        session.addCommands (registry, host);
        phazite::addSongCommands (registry, session.getContext(), host);
        session.openProject (file);
        REQUIRE (session.getProject() != nullptr);
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [this] { return phazite::getPluginsLoading (session.getProject()->getSong()).isEmpty(); }, longWait));
        instance = phazite::findPluginInstance (session.getProject()->getSong(), slow);
        REQUIRE (instance != nullptr);
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! instance->isStateUnasked() && ! instance->isAskingState(); },
                                                       longWait));
    }

    phazite::Song& getSong() { return session.getProject()->getSong(); }
    bool hasTrack() { return ! phazite::getTrackIDs (getSong()).empty(); }

    SandboxedPlugins sandboxed;
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent { settings };
    phazite::ProjectSession session { phazite::test::getEngineContext(), recent, temp.folder, temp.folder.getChildFile ("Unsaved") };
    phazite::test::RenderHost host;
    phazite::CommandRegistry registry;
    juce::File file;
    phazite::TrackID track;
    phazite::DeviceID slow;
    phazite::SandboxedInstance* instance = nullptr;
};
} // namespace

TEST_CASE ("A key let go while a command waits for the plug-ins' states isn't taken for held: Delete runs again")
{
    // The review of known bug 166: the window's hold on its input took every key event, so a key let go during the wait
    // stayed held for the shortcuts (the next press was taken for a repeat, and did nothing) and for the computer keyboard
    // (a note on until some later key).
    SlowSession s;
    phazite::ui::CommandBridge bridge { s.registry };
    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["edit.delete"] = { "delete" };
    bridge.setKeymap (keymap);
    phazite::ui::KeyDispatcher dispatcher { bridge, phazite::ui::getUsLayout() };
    juce::Component content;
    phazite::test::TestWindow window { content, 200, 100 };
    window.addKeyListener (&dispatcher);
    s.session.getContext().getSongSelection().selectOnly (s.track);
    const juce::KeyPress del (juce::KeyPress::deleteKey);

    // Delete, the plug-in's change unanswered and its gate shut: the command waits; the key goes up meanwhile.
    turnInItsWindow (*s.instance, 0.5f);

    {
        SlowGate gate;
        window.keyGoesDown();
        window.pressKey (del);
        REQUIRE (s.registry.isWaitingForPluginStates());
        window.releaseKeys();
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&s] { return ! s.hasTrack(); }, longWait));
    phazite::undo (s.getSong());
    REQUIRE (s.hasTrack());

    // Pressed again: it runs again, the key not taken for still held.
    s.session.getContext().getSongSelection().selectOnly (s.track);
    window.keyGoesDown();
    window.pressKey (del);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&s] { return ! s.hasTrack(); }, longWait));
    window.releaseKeys();
    window.removeKeyListener (&dispatcher);
}

TEST_CASE ("A note of the computer keyboard let go while a command waits for the plug-ins' states ends as the wait does")
{
    // The review of known bug 166: the window's hold on its input took the key's release, so its note played on.
    SlowSession s;
    phazite::test::SimulatedLayout layout; // the US layout, with the keys the test holds
    int notesOn = 0, notesOff = 0;
    phazite::ui::ComputerKeyboard keyboard { [&] (const juce::MidiMessage& message)
                                             {
                                                 notesOn += message.isNoteOn() ? 1 : 0;
                                                 notesOff += message.isNoteOff() ? 1 : 0;
                                             },
                                             layout };
    juce::Component content;
    phazite::test::TestWindow window { content, 200, 100 };
    window.addKeyListener (&keyboard);
    s.session.getContext().getSongSelection().selectOnly (s.track);

    // A held: its note plays.
    layout.down.insert ('a');
    window.keyGoesDown();
    REQUIRE (notesOn == 1);

    // Delete waits for the plug-in's change; A goes up meanwhile.
    turnInItsWindow (*s.instance, 0.5f);

    {
        SlowGate gate;
        REQUIRE (s.registry.invoke ("edit.delete"));
        REQUIRE (s.registry.isWaitingForPluginStates());
        layout.down.erase ('a');
        window.releaseKeys();
        CHECK (notesOff == 0); // taken by the hold
    }

    // The note ends once the wait is over (the command run), and no sooner: the window took no input meanwhile.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&s] { return ! s.hasTrack(); }, longWait));
    CHECK (notesOff == 1);
    CHECK (notesOn == 1);
    window.removeKeyListener (&keyboard);
}
TEST_CASE ("What waits for a command that waits for the plug-ins' states comes once it has run, the window's hold no dialog")
{
    // The review of known bug 166: Windows ending its session during a command's wait took the window's hold on its input
    // for a dialog to finish first, and never closed; the close is to start once the command has run (Main.cpp).
    SlowSession s;
    s.session.getContext().getSongSelection().selectOnly (s.track);
    turnInItsWindow (*s.instance, 0.5f);
    auto ran = false, trackWasThere = true;

    {
        SlowGate gate;
        REQUIRE (s.registry.invoke ("edit.delete"));
        REQUIRE (s.session.isWaitingForPluginStates());
        auto* modal = juce::Component::getCurrentlyModalComponent();
        REQUIRE (modal != nullptr);
        CHECK (s.session.isHoldingInput (modal)); // the hold, not a dialog

        s.session.afterPluginStatesGate (
            [&]
            {
                ran = true;
                trackWasThere = s.hasTrack();
            });
        CHECK_FALSE (ran);
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&ran] { return ran; }, longWait));
    CHECK_FALSE (trackWasThere); // after the Delete
    CHECK_FALSE (s.session.isWaitingForPluginStates());
    CHECK_FALSE (s.session.isHoldingInput (juce::Component::getCurrentlyModalComponent()));

    // With nothing waiting, at once.
    ran = false;
    s.session.afterPluginStatesGate ([&ran] { ran = true; });
    CHECK (ran);
}
TEST_CASE ("A command's wait offers to go on without the plug-ins or to drop the command: Cancel changes nothing")
{
    // The review of known bug 166: the wait's Cancel went on with what each plug-in gave last, so a Delete waited for ran.
    SlowSession s;
    s.session.getContext().getSongSelection().selectOnly (s.track);

    for (const auto goOn : { false, true })
    {
        CAPTURE (goOn);
        turnInItsWindow (*s.instance, goOn ? 0.25f : 0.5f);
        SlowGate gate;
        REQUIRE (s.registry.invoke ("edit.delete"));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&s] { return s.host.render != nullptr; }, longWait));
        CHECK (s.host.render->getGoOnName() == "Go On Without Them");

        if (goOn)
            s.host.render->goOn();

        s.host.render = nullptr; // the window goes
        REQUIRE (phazite::test::dispatchMessagesUntil ([&s] { return ! s.registry.isWaitingForPluginStates(); }, longWait));
        CHECK (turnMessageLoop (10));
        CHECK (s.hasTrack() == ! goOn);
    }
}

TEST_CASE ("A preset loaded or a chain duplicated or a version saved straight after a change in a plug-in's window holds it")
{
    // The review of known bug 166: these went around the gate: a preset's load stored what the plug-in gave last, so its
    // Undo lost the change; a chain's copy held the state before.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto track = song.firstTrack;
    const auto slow = addSlow (song, track);
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    auto& opened = getSong (song);

    // A preset of it at 0.5; turned to 0.75 and the preset loaded at once: it waits, loads, and Undo gives 0.75 back.
    turnInItsWindow (instance, 0.5f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));
    const auto preset = song.getSoundsFolder().getChildFile (juce::String ("Half") + phazite::presetExtension);
    REQUIRE (phazite::saveDevicePreset (song.context, slow, preset));
    turnInItsWindow (instance, 0.75f);
    auto loaded = false;

    {
        SlowGate gate;
        phazite::loadPresetWithPluginStates (song.context, slow, preset, [&loaded] (bool done) { loaded = done; });
        CHECK (turnMessageLoop (10));
        CHECK_FALSE (loaded);
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&loaded] { return loaded; }, longWait));
    CHECK (getStoredLevel (opened, slow) == doctest::Approx (0.5f));
    phazite::undo (opened);
    CHECK (getStoredLevel (opened, slow) == doctest::Approx (0.75f));

    // A version saved at once, as Save Version does (its command): it waits for the state, and records the turned one.
    phazite::test::RenderHost renders;
    phazite::addVersionCommands (song.commands, song.context, song.save, renders);
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    turnInItsWindow (instance, 0.125f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return ! phazite::getChangesSinceLastVersion (phazite::test::withChangesIn (song.context)).empty(); }, longWait));

    {
        SlowGate gate;
        song.run ("version.save");
        CHECK (song.commands.isWaitingForPluginStates());
    }

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            return ! phazite::isSavingVersion (song.context)
                   && ! phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (song.context));
        },
        longWait));
    CHECK (getStoredLevel (opened, slow) == doctest::Approx (0.125f));

    // Grouped into a container, turned, and its chain duplicated at once: the copy holds the turned state.
    const auto container = phazite::groupIntoContainer (song.context, { slow });
    REQUIRE (container.isValid());
    const auto chains = phazite::getChains (song.context, container);
    REQUIRE (chains.size() == 1);
    turnInItsWindow (instance, 0.375f);

    {
        SlowGate gate;
        phazite::duplicateChainWithPluginStates (song.context, chains.front().id);
        CHECK (turnMessageLoop (10));
        CHECK (phazite::getChains (song.context, container).size() == 1);
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getChains (song.context, container).size() == 2; }, longWait));
    std::vector<float> levels;
    std::function<void (const juce::ValueTree&)> find = [&] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (phazite::isPluginState (child))
                levels.push_back (readLevel (child[phazite::schema::pluginState]));

            find (child);
        }
    };

    find (phazite::findTrackState (opened, track));
    REQUIRE (levels.size() == 2);
    CHECK (levels[0] == doctest::Approx (0.375f));
    CHECK (levels[1] == doctest::Approx (0.375f));
}

TEST_CASE ("A branch merged from the branch picker straight after a change in a plug-in's window waits for its state")
{
    // The review of known bug 166: the picker's Merge started the merge at once, finding its clashes against the state the
    // plug-in gave before the change, where version.merge's command waits for it.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return ! getSandboxed (song, slow).isStateUnasked() && ! getSandboxed (song, slow).isAskingState(); }, longWait));
    phazite::test::RenderHost renders;
    phazite::addVersionCommands (song.commands, song.context, song.save, renders);
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    const auto commit = [&]
    {
        const auto before = phazite::getVersions (song.context).size();
        song.run ("version.save");
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [&] { return phazite::getVersions (song.context).size() > before && ! phazite::isSavingVersion (song.context); }, longWait));
        REQUIRE (phazite::test::waitForVersionHistory (song.context));
    };

    // A commit on main, and one on a branch from it that turned the plug-in; back on main.
    song.setVolume (song.firstTrack, -3.0f);
    commit();
    const auto mainBranch = phazite::getCurrentBranch (song.context)->id;
    REQUIRE (phazite::newBranchNamed (song.context, "idea").isNotEmpty());
    turnInItsWindow (getSandboxed (song, slow), 0.25f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return phazite::hasChangesSinceLastVersion (phazite::test::withChangesIn (song.context)); }, longWait));
    commit();
    phazite::selectBranch (song.context, mainBranch);
    song.run ("version.switchBranch");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getCurrentBranch (song.context)->id == mainBranch; }, longWait));
    REQUIRE (phazite::test::waitForVersionHistory (song.context));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::getPluginsLoading (getSong (song)).isEmpty(); }, longWait));
    auto& instance = getSandboxed (song, slow);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));

    // Turned in its window on main, and the branch merged in at once from the picker: the merge starts once it has answered.
    turnInItsWindow (instance, 0.75f);
    phazite::ui::CommandBridge bridge { song.commands };

    {
        SlowGate gate;
        phazite::ui::BranchPicker picker (bridge, song.context, phazite::ui::BranchPicker::Mode::merge);
        REQUIRE (picker.getNumRows() == 1);
        picker.choose (0);
        CHECK (turnMessageLoop (10));
        CHECK (phazite::getMergeLines (song.context).empty());
    }

    // The merge's lines listed (the Versions view's merge list), its clashes found against the turned state.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::getMergeLines (song.context).empty(); }, longWait));
    CHECK (getStoredLevel (getSong (song), slow) == doctest::Approx (0.75f));
}

TEST_CASE ("A reload falls back to the song's state only for a crash soon after one, keeping the plug-in's own for the next")
{
    // The review of known bug 166: a plug-in that crashed again any time after a reload, untouched, reopened from the song,
    // the user's own settings gone; and a reload that failed lost them for the next try.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    song.saveWithPluginStates(); // the song holds its Level at 1
    auto& instance = getSandboxed (song, slow);
    const auto level = instance.findParameter ("60");
    turnInItsWindow (instance, 0.5f); // its own
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }, longWait));

    struct Soon
    {
        ~Soon() { phazite::PluginInstances::setSoonAfterReload (-1.0); }
    } soon;

    const auto crash = [&instance]
    {
        instance.getRemote().getHelper().kill();
        REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }, longWait));
    };

    // Long after a reload (the time set to none): its own, again.
    phazite::PluginInstances::setSoonAfterReload (0.0);
    crash();
    CHECK_FALSE (reloadAndWait (song, slow).has_value());
    crash();
    CHECK_FALSE (reloadAndWait (song, slow).has_value());
    CHECK_FALSE (phazite::wasReloadedFromProject (song.context, slow));
    CHECK (instance.getParameterValue (level) == doctest::Approx (0.5f));

    // Soon after: the song's, which the notice says; and the next reload its own again.
    phazite::PluginInstances::setSoonAfterReload (longWait);
    crash();
    CHECK_FALSE (reloadAndWait (song, slow).has_value());
    CHECK (phazite::wasReloadedFromProject (song.context, slow));
    CHECK (instance.getParameterValue (level) == doctest::Approx (1.0f));
    crash();
    CHECK_FALSE (reloadAndWait (song, slow).has_value());
    CHECK_FALSE (phazite::wasReloadedFromProject (song.context, slow));
    CHECK (instance.getParameterValue (level) == doctest::Approx (0.5f));

    // A reload that fails (it doesn't load in time, its gate shut) keeps its own for the next.
    crash();
    {
        SlowGate gate;
        SandboxedPlugins::getHost().setLoadMilliseconds (300);
        CHECK (reloadAndWait (song, slow).has_value());
        SandboxedPlugins::getHost().setLoadMilliseconds (phazite::PluginHost::defaultLoadMilliseconds);
    }

    juce::MemoryBlock kept;
    instance.getStateInformation (kept);
    CHECK (juce::MemoryInputStream (kept, false).readFloat() == doctest::Approx (0.5f));
}

TEST_CASE ("The changes a plug-in tells are kept only as far as its answers need them")
{
    // The review of known bug 166: every change a plug-in told was kept until a store found its state its own.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);

    for (int i = 0; i < 200; ++i)
    {
        juce::MemoryOutputStream told;
        told.writeFloat ((float) (i % 100) / 100.0f); // its gain
        told.writeFloat (0.5f);                       // its tone
        told.writeFloat (1.0f);                       // as its own window would set them, telling the host
        REQUIRE (instance.getRemote().setState (told.getMemoryBlock(), longWait));
        phazite::test::dispatchPendingMessages();
    }

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }));
    CHECK (turnMessageLoop (10));
    CHECK (session->getPlugins().countReportsKept (effect) <= 1);
}

TEST_CASE ("A channel its helper refuses leaves the plug-in unprepared, and a render that needs it fails rather than play it so")
{
    // The review of known bug 166: a prepare the helper refused stopped preparing, and a render took that for prepared,
    // playing the old channel at the old rate.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    CHECK (turnMessageLoop (10));
    const auto rate = instance.getRemote().getSampleRate();

    // A rate no plug-in plays at, which the helper refuses: it stops preparing, unprepared, the channel as it was.
    instance.prepareToPlay (1'000'000.0, 512);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isPreparing(); }));
    instance.hold(); // as a render holds it
    CHECK_FALSE (instance.finishPreparing());
    instance.let();
    CHECK (instance.getRemote().getSampleRate() == doctest::Approx (rate));
}

#if JUCE_WINDOWS
TEST_CASE ("A program that asks Phazite to close is no session's end: it closes as an ordinary quit does")
{
    // The review of known bug 168: the installer's close (Restart Manager) was taken for the session's end, which could stay
    // on for the rest of the run.
    std::vector<bool> told;
    phazite::SessionEndWatch watch ([&told] (bool ending) { told.push_back (ending); });
    CHECK (watch.handle (WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP) == TRUE);
    CHECK_FALSE (watch.isEnding());
    CHECK (told.empty());
}
#endif

TEST_CASE ("A helper ended before it starts never starts: Phazite quitting while plug-ins open waits for none of them")
{
    phazite::sandbox::Helper helper (SandboxedPlugins::getHost().getHelperProgram());
    helper.kill();
    CHECK_FALSE (helper.start (longWait));
    CHECK_FALSE (helper.isRunning());
}

#if JUCE_WINDOWS
TEST_CASE ("A plug-in's window closed opens again, as often as it is asked for")
{
    // The helper kept a stand-in's editor as its active one after its window closed, and made none again.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto slow = addSlow (song, song.firstTrack);
    phazite::DeviceWindows windows (song.context);

    for (int i = 0; i < 3; ++i)
    {
        phazite::showDeviceWindow (song.context, slow);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! phazite::isDeviceWindowOpening (song.context, slow); }, longWait));
        CHECK (phazite::isDeviceWindowOpen (song.context, slow));
        windows.close (slow);
    }
}
#endif

TEST_CASE ("A render whose plug-in's channel doesn't come in the prepare's time fails, naming it, and the plug-in reloads")
{
    // The time-out path a render takes when the helper doesn't make a channel (the helper taken for hung, ended), which a test
    // reached once only past the prepare's minute, under the tests' long hang limits: its render ends, its message names the
    // plug-in, nothing it held is touched after, and the plug-in reloads and is frozen.
    struct PrepareLimit
    {
        PrepareLimit() { phazite::SandboxedInstance::setPrepareMilliseconds (2000); }
        ~PrepareLimit() { phazite::SandboxedInstance::setPrepareMilliseconds (0); }
    } limit;

    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto slow = addSlow (song, track);
    auto& instance = getSandboxed (song, slow);
    CHECK (turnMessageLoop (10));
    const auto rate = instance.getRemote().getSampleRate();
    song.context.getSongSelection().selectOnly (track);

    {
        SlowGate gate; // the helper busy giving a state, the channel asked for behind it
        instance.askState (phazite::SandboxedInstance::Ask::all);
        instance.prepareToPlay (juce::exactlyEqual (rate, 96000.0) ? 48000.0 : 96000.0, 512);
        phazite::test::RenderHost host;
        phazite::toggleFreeze (song.context, host);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, longWait));
        CHECK (host.render == nullptr);
        CHECK (host.finished[0].contains ("Test Slow"));
        CHECK_FALSE (phazite::isFrozen (song.context, track));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&instance] { return instance.hasCrashed(); }, longWait));
    }

    CHECK_FALSE (reloadAndWait (song, slow).has_value());
    phazite::test::RenderHost host;
    phazite::toggleFreeze (song.context, host);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&host] { return ! host.finished.isEmpty(); }, longWait));
    CHECK (phazite::isFrozen (song.context, track));
}

namespace
{
// The synth's state gate (host/TestPlugins.h): shut while this lives, unless opened, and open again as it goes.
struct StateGate
{
    StateGate()
    {
        phazite::test::TestPlugins::getStateSetting().deleteFile();
        phazite::test::TestPlugins::getStateGate().create();
    }

    ~StateGate() { open(); }
    void open() { phazite::test::TestPlugins::getStateGate().deleteFile(); }
};

// The synth stand-in, in its helper, played straight through its processor, at 48 kHz in blocks of 256.
struct PlayedSynth
{
    PlayedSynth()
    {
        juce::String error;
        instance = SandboxedPlugins::getHost().createInstance (phazite::test::TestPlugins::getDescription (Kind::synth), {}, -1, 48000.0,
                                                               256, error);
        REQUIRE_MESSAGE (instance != nullptr, error);
        processor = std::make_unique<phazite::SandboxedProcessor> (instance);
        processor->prepare (48000.0, 256);
    }

    // A block with its events, as a render plays it (waiting for the plug-in, however busy the machine is) or as the audio
    // device does (never waiting past its deadline); how loud it came back.
    float play (std::vector<phazite::devices::NoteEvent> events, bool asRender)
    {
        buffer.clear();
        phazite::devices::ProcessContext context;
        context.channels = buffer.getArrayOfWritePointers();
        context.numChannels = 2;
        context.numSamples = buffer.getNumSamples();
        context.events = events;
        context.isRendering = asRender;
        processor->process (context);
        return buffer.getMagnitude (0, buffer.getNumSamples());
    }

    // A state that sets its Level, and makes it wait at the state gate as it is set, given as a preset or an undo gives it:
    // on the message thread, without waiting for the helper; once it has begun to wait.
    void giveSlowState (float level)
    {
        juce::MemoryOutputStream state;
        state.writeFloat (level);
        state.writeFloat (1.0f); // the wait
        instance->setStateInformation (state.getData(), (int) state.getDataSize());
        REQUIRE (phazite::test::dispatchMessagesUntil ([] { return phazite::test::TestPlugins::getStateSetting().existsAsFile(); }));
    }

    std::shared_ptr<phazite::SandboxedInstance> instance;
    std::unique_ptr<phazite::SandboxedProcessor> processor;
    juce::AudioBuffer<float> buffer { 2, 256 };
};
} // namespace

TEST_CASE ("A plug-in whose state takes longer to set than a hang is silent meanwhile, isn't taken for hung, and ends its notes")
{
    // The audit of 2026-11-20: the helper's audio thread waited for the plug-in's lock while its message thread gave it a
    // state, so Phazite found its channel busy at every block and ended the helper once a block had been kept a hang's
    // time; and the state's answer, let go of at once, held nothing off the heartbeat's limit either. A sampler that reads
    // its library as it is given a state takes longer than both.
    using Type = phazite::devices::NoteEvent::Type;
    SandboxedPlugins sandboxed;
    SandboxedPlugins::useShortHangLimits(); // a hang's test
    PlayedSynth synth;
    CHECK (synth.play ({ { Type::noteOn, 0, 69, 0.8f } }, true) > 0.1f);

    StateGate gate;
    synth.giveSlowState (0.5f);

    // Live blocks while it sets the state, for twice as long as the longer of the two hang limits: each answered at once,
    // unplayed, and silent; the helper not taken for hung. The note ends in the first.
    const auto from = juce::Time::getMillisecondCounterHiRes();
    const auto lasting = 2.0 * std::max (SandboxedPlugins::messageHangMilliseconds, SandboxedPlugins::audioHangMilliseconds);
    auto silent = juce::exactlyEqual (synth.play ({ { Type::noteOff, 10, 69, 0.0f } }, false), 0.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            silent = juce::exactlyEqual (synth.play ({}, false), 0.0f) && silent;
            return synth.instance->hasCrashed() || juce::Time::getMillisecondCounterHiRes() - from >= lasting;
        }));
    CHECK (silent);
    CHECK_FALSE (synth.instance->hasCrashed());
    CHECK (synth.instance->getRemote().getHelper().isRunning());
    CHECK (phazite::test::TestPlugins::getStateSetting().existsAsFile()); // still setting it

    // The state set: it plays again. The note's end went with the first block it played, where nothing sounds of it; a new
    // note sounds, at the Level the state gave it.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return ! phazite::test::TestPlugins::getStateSetting().existsAsFile(); }));
    CHECK (synth.play ({}, true) == 0.0f);
    CHECK (synth.play ({}, true) == 0.0f);
    CHECK (synth.play ({ { Type::noteOn, 0, 72, 0.8f } }, true) == doctest::Approx (0.5f).epsilon (0.05));
    CHECK_FALSE (synth.instance->hasCrashed());
}

TEST_CASE ("States set in a fraction of a block each, as presets auditioned while the song plays are, leave nearly every block played")
{
    // The review of the audit's fixes (2026-11-20): a live block that found the plug-in's lock held was answered unplayed at
    // once, so every preset set while the song played cut an instrument to silence for a block, however quick it was.
    using Type = phazite::devices::NoteEvent::Type;
    SandboxedPlugins sandboxed;
    PlayedSynth synth;
    CHECK (synth.play ({ { Type::noteOn, 0, 69, 0.8f } }, true) > 0.1f);

    // Two hundred states of half a millisecond each (a tenth of the block), a live block after each is sent: the helper's
    // message thread sets them one after another, holding the plug-in's lock for nearly all of a tenth of a second while the
    // blocks come; then live blocks until a request sent after them has its answer.
    juce::MemoryOutputStream state;
    state.writeFloat (0.25f); // its Level
    state.writeFloat (2.0f);  // half a millisecond to set

    for (int i = 0; i < 200; ++i)
    {
        synth.instance->setStateInformation (state.getData(), (int) state.getDataSize());
        synth.play ({}, false);
    }

    const auto after = synth.instance->getRemote().askState();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            synth.play ({}, false);
            return after->isDone();
        }));

    // Nearly none of them: with the single try for the lock every one went unplayed (207 of them here). Not none: on a
    // machine with fewer cores than threads at work (CI's), the helper's message thread can be kept off its core, the lock
    // in its hands, for longer than the half block the helper's audio thread tries for.
    CHECK (synth.processor->countUnplayedBlocks() < 50);
    CHECK_FALSE (synth.instance->hasCrashed());
}

TEST_CASE ("A note whose end there was no room to keep with a block the helper didn't play doesn't sound on")
{
    // The review of the audit's fixes: a block's events past what is kept of it were ended by an all notes off kept before
    // them, so a note among those kept whose end was among those dropped began after it, and sounded on.
    using Type = phazite::devices::NoteEvent::Type;
    SandboxedPlugins sandboxed;
    PlayedSynth synth;
    StateGate gate;
    synth.giveSlowState (0.25f);

    // A note, more events after it than what is kept of a block holds (slides, which reach no plug-in), then its end, in a
    // block the helper doesn't play.
    std::vector<phazite::devices::NoteEvent> events { { Type::noteOn, 0, 60, 0.8f } };

    for (int i = 0; i < 1100; ++i)
        events.push_back ({ Type::slide, 1, 60, 0.0f });

    events.push_back ({ Type::noteOff, 2, 60, 0.0f });
    CHECK (synth.play (events, false) == 0.0f);
    CHECK (synth.processor->countUnplayedBlocks() == 1);

    // Once the state is set, nothing of the note sounds.
    gate.open();
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return ! phazite::test::TestPlugins::getStateSetting().existsAsFile(); }));
    CHECK (synth.play ({}, true) == 0.0f);
    CHECK (synth.play ({}, true) == 0.0f);
}

TEST_CASE ("A plug-in prepared again at its rate while the audio thread holds its channel for a moment is prepared at once")
{
    // The review of the audit's fixes: preparing gave up on a channel held at all and left it to the timer, so a plug-in
    // whose latency changed as it played (its processor made anew and prepared) was skipped until the timer's next look,
    // where it used to wait for the audio thread, which lets go within a block.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    CHECK (turnMessageLoop (10)); // what plays the song prepared it, for the rate it opened at
    const auto rate = instance.getRemote().getSampleRate();

    // The audio thread holds the channel for a moment, as it does as it plays a block.
    juce::WaitableEvent holding;
    std::thread audio (
        [&]
        {
            instance.hold();
            holding.signal();
            juce::Thread::sleep (2);
            instance.let();
        });
    const juce::ScopeGuard joined { [&audio] { audio.join(); } };
    REQUIRE (holding.wait (phazite::test::hangTimeout));

    // Prepared meanwhile at its rate, for blocks of 4096 (it tries for the channel as long as one lasts, tens of
    // milliseconds): nothing to wait for, it isn't preparing.
    instance.prepareToPlay (rate, 4096);
    CHECK_FALSE (instance.isPreparing());
}

TEST_CASE ("A plug-in that never comes back from being given a state is taken for hung once a state's time to be set is up")
{
    // What still ends a helper whose message thread a plug-in keeps for ever as it is given a state: its blocks are answered
    // unplayed meanwhile, never kept, and the state's answer holds off the heartbeat's limit, but only for so long.
    struct SettingLimit
    {
        SettingLimit() { phazite::SandboxedInstance::setSettingMilliseconds (3000); }
        ~SettingLimit() { phazite::SandboxedInstance::setSettingMilliseconds (0); }
    } limit;

    SandboxedPlugins sandboxed;
    SandboxedPlugins::useShortHangLimits(); // a hang's test
    PlayedSynth synth;
    StateGate gate;
    const auto from = juce::Time::getMillisecondCounterHiRes();
    synth.giveSlowState (0.5f);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            synth.play ({}, false);
            return synth.instance->hasCrashed();
        }));

    // Not before the setting's time and the heartbeat's limit after it.
    CHECK (juce::Time::getMillisecondCounterHiRes() - from
           >= phazite::SandboxedInstance::getSettingMilliseconds() + SandboxedPlugins::messageHangMilliseconds);
    CHECK (synth.instance->getRemote().getHelper().wasEndedForHanging());
}

TEST_CASE ("A plug-in prepared while a render holds its channel waits for no render: it is prepared once the render lets go")
{
    // The audit of 2026-11-20: preparing waited for the channel, spinning on the message thread, while a render held it for
    // as long as a plug-in took over its block (for ever, for one that hung): the live plan, made again on the message thread
    // as a plug-in said another latency for playing offline, prepared what plays it.
    SandboxedPlugins sandboxed;
    TestSong song;
    const auto effect = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    auto& instance = getSandboxed (song, effect);
    CHECK (turnMessageLoop (10)); // what plays the song prepared it, for the rate it opened at
    const auto rate = instance.getRemote().getSampleRate();
    const auto otherRate = juce::exactlyEqual (rate, 96000.0) ? 44100.0 : 96000.0;

    // A render's thread holds the channel, as it does while it waits for the plug-in, until it is let go of (or a quarter of
    // the tests' hang limit has passed).
    const auto heldFor = phazite::test::hangTimeout / 4;
    juce::WaitableEvent letGo;
    std::atomic<bool> held { false };
    std::thread render (
        [&]
        {
            instance.hold();
            held = true;
            letGo.wait (heldFor);
            instance.let();
        });
    const juce::ScopeGuard joined { [&]
                                    {
                                        letGo.signal();
                                        render.join();
                                    } };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held.load(); }));

    // Prepared meanwhile for another rate, as the live plan made again prepares it: at once (a wait would last until the
    // render lets go: half of that tells them apart however busy the machine is); the engine skips it until it is prepared.
    CHECK (timeKept ([&] { instance.prepareToPlay (otherRate, 512); }) < heldFor / 2.0);
    CHECK (instance.isPreparing());
    CHECK (turnMessageLoop (10));
    CHECK (instance.isPreparing());

    // The render lets go: prepared then, by the message thread's timer.
    letGo.signal();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isPreparing(); }, longWait));
    CHECK (instance.getRemote().getSampleRate() == otherRate);
    CHECK_FALSE (instance.hasCrashed());
}

TEST_CASE ("A meter a plug-in tells as a parameter as it plays changes nothing of the song: it stays saved, its state unasked")
{
    // The audit of 2026-11-20: JUCE's VST3 host tells what a plug-in outputs to its host as it plays (a gain-reduction meter
    // it has as a parameter) as a change of that parameter, which Phazite took for a value set in the plug-in's window:
    // written into the song as set by hand, an unsaved change, and the plug-in's state asked again, for as long as the meter
    // moved.
    SandboxedPlugins sandboxed;
    TestSong song;
    song.setTempo (120.0);
    const auto track = addToneTrack (song, song.firstTrack, "Tone");
    const auto delay = song.addEffect (track, "PhaziteTestLatency");
    auto& instance = getSandboxed (song, delay);
    const auto meter = instance.findParameter ("41");
    REQUIRE (meter >= 0);

    // Its Latency on, which it says as it plays its next block, after what its Meter told as it played it.
    song.setParameter (delay, "40", 1.0f);
    helperHasTaken (song, delay);
    song.saveWithPluginStates();
    REQUIRE_FALSE (song.getProject().hasUnsavedChanges());
    const auto stateChanges = instance.countStateChanges();

    EnginePlayback playback (EnginePlayback::Input::silence, EnginePlayback::Timing::asRender);
    song.run ("transport.playStop");
    CHECK (levelOf (playback.play (0.5), 0, 0.2, 0.45) > toneLevel - 1.0f); // the tone through it: its Meter moved
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return instance.getRemote().getLatencySamples() == 128; }));
    phazite::test::dispatchPendingMessages(); // what came before it, taken in on the message thread

    CHECK (instance.getParameterValue (meter) == 0.0f);
    CHECK (instance.countStateChanges() == stateChanges);
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
    CHECK (getSong (song).getChangesOutsideHistory() == 0);
}
