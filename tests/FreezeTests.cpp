// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing a track and bouncing it in place (roadmap step 2.11; src/model/mixer/FreezeModel.h): rendered in the
// background up to the fader (stopping before a sidechained device), played from a file in the project's Cache folder,
// kept frozen while only its mixer changes and unfrozen when what it froze changes, opened frozen, and cancelled without
// a trace; and a bounce onto a new audio track under the original, as one undo step.
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/dynamics/Compressor.h"
#include "devices/stereo/Stereo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/DevicesView.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TrackHeader.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace
{
using phazite::DeviceID;
using phazite::TrackID;

// A saved project with Bass, a sine from the start to 1.5 s through a Stereo that turns it up 6 dB, its fader at -6 dB
// and a send after it to the return Reverb; Bass is selected.
struct Song
{
    Song()
    {
        juce::String error;
        project = phazite::Project::create (temp.folder, "Song", error);
        REQUIRE (project != nullptr);
        auto& song = getSong();
        bass = phazite::ensureTracks (song, 1)[0];
        phazite::setTrackName (song, bass, "Bass");

        const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Sine.wav"), 2.0);
        clip = phazite::test::addAudioClip (song, bass, "Sine", sound, 0.0, 1.5);
        REQUIRE (clip.isValid());

        stereo = phazite::test::addOwnDevice (song, bass, phazite::devices::Stereo::typeID, { { "gain", 6.0f } });
        REQUIRE (stereo.isValid());
        phazite::setChannelVolume (song, bass, -6.0f);

        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        REQUIRE (reverb.isValid());
        REQUIRE (phazite::addSend (song, bass, reverb, false).isValid());

        REQUIRE (project->save().wasOk());
        song.getUndoManager().clearUndoHistory();
        open();
    }

    ~Song() { close(); }

    void open()
    {
        setSong (context, &getSong());
        getSelection (context).selectOnly (bass);
    }

    void close()
    {
        host.render = nullptr;
        setSong (context, nullptr);
    }

    // Closes the project, and opens it again from its file: its tracks, clips and devices keep their IDs.
    void reopen()
    {
        close();
        const auto file = project->getFile();
        project = nullptr;
        juce::String error;
        project = phazite::Project::open (file, false, error);
        REQUIRE (project != nullptr);
        open();
    }

    phazite::Song& getSong() const { return project->getSong(); }
    juce::File frozenFile() const { return phazite::getFrozenFile (getSong(), bass); }

    // Freezes Bass with its command's function, as the app does, and waits for it.
    void freeze()
    {
        host.finished.clear();
        phazite::toggleFreeze (context, host);
        REQUIRE (host.render != nullptr);
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! host.finished.isEmpty(); }));
        REQUIRE (host.finished == juce::StringArray { "Freezing Bass: ok" });
        REQUIRE (phazite::isFrozen (context, bass));
    }

    // Stereo's gain, set by hand as its control sets it.
    void setStereoGain (float decibels)
    {
        const phazite::UndoStep step (getSong(), "Change Gain");
        phazite::setValueSetByHand (getSong(), phazite::ParameterRef { stereo, "gain" }, decibels);
    }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::test::RenderHost host;
    TrackID bass, reverb;
    phazite::ClipID clip;
    DeviceID stereo;
};

// A file's audio, which must be one.
juce::AudioBuffer<float> readAudio (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    sampleRate = reader->sampleRate;
    return audio;
}

// How long a file lasts, in seconds.
double getLength (const juce::File& file)
{
    double sampleRate = 0.0;
    const auto audio = readAudio (file, sampleRate);
    return audio.getNumSamples() / sampleRate;
}

// The RMS level of a stretch of a file's left channel, in seconds.
double getLevel (const juce::File& file, double start, double end)
{
    double sampleRate = 0.0;
    const auto audio = readAudio (file, sampleRate);
    const auto first = juce::roundToInt (start * sampleRate);
    return audio.getRMSLevel (0, first, juce::roundToInt (end * sampleRate) - first);
}
} // namespace

TEST_CASE ("Freezing renders a track up to its fader in the background, and plays that file until it is unfrozen")
{
    Song song;
    auto& model = song.getSong();
    CHECK (phazite::getWhyNotFreeze (song.context, song.bass).isEmpty());
    CHECK (phazite::getFrozenDevices (model, song.bass) == std::vector { song.stereo });
    CHECK (phazite::getFreezeCommandName (song.context) == "Freeze Track");
    CHECK (phazite::canToggleFreeze (song.context));

    song.freeze();
    CHECK (phazite::isFrozen (song.context, song.bass));
    CHECK (phazite::isSelectedTrackFrozen (song.context));
    CHECK (phazite::getFreezeCommandName (song.context) == "Unfreeze Track");
    CHECK (phazite::getWhyNotFreeze (song.context, song.bass) == "Bass is frozen already.");
    CHECK (phazite::getFrozenTracks (model) == std::vector { song.bass });

    // Stereo is in the file, and stops playing; the fader and the send after it stay live.
    CHECK (phazite::isFrozenDevice (song.context, song.stereo));
    CHECK (phazite::isFrozenDevice (model, song.stereo));
    CHECK_FALSE (phazite::isFrozenDevice (model, phazite::getFader (model, song.bass)));
    CHECK_FALSE (phazite::isFrozenDevice (model, phazite::getSends (model, song.bass)[0]));
    CHECK (phazite::getDevices (model, song.bass).size() == 1);

    // The file is in the project's Cache folder: the clip and its tail.
    REQUIRE (song.frozenFile().existsAsFile());
    CHECK (song.frozenFile().getParentDirectory() == song.project->getCacheFolder());
    CHECK (getLength (song.frozenFile()) >= 1.5);

    // Not an undo step, but a change to save; the render's switches are as they were.
    CHECK_FALSE (phazite::canUndo (model));
    CHECK (song.project->hasUnsavedChanges());
    CHECK (phazite::isDeviceOn (model, song.stereo));
    CHECK_FALSE (phazite::isTrackMuted (model, song.bass, false));

    // Unfreezing plays it live again, and leaves the file until the project closes.
    phazite::toggleFreeze (song.context, song.host);
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::test::copyTrack (model, song.bass).hasProperty (phazite::schema::trackFrozen));
    CHECK_FALSE (phazite::isFrozenDevice (model, song.stereo));
    CHECK_FALSE (phazite::canUndo (model));
    const auto frozenFile = song.frozenFile();
    CHECK (frozenFile.existsAsFile());

    song.close();
    song.project = nullptr; // closing purges the files nothing uses
    CHECK_FALSE (frozenFile.existsAsFile());
}

TEST_CASE ("A frozen track stays frozen while only its mixer changes, and unfreezes when what it froze does")
{
    Song song;
    auto& model = song.getSong();
    song.freeze();

    // Its fader, name, mute switch and send: still frozen.
    phazite::setChannelVolume (model, song.bass, -3.0f);
    phazite::renameTrack (model, song.bass, "Bass Line");
    phazite::setTrackMuted (model, song.bass, true);
    phazite::setTrackMuted (model, song.bass, false);
    phazite::setSendLevel (model, phazite::getSends (model, song.bass)[0], -12.0f);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isFrozen (song.context, song.bass));

    // Its clip: unfrozen at the next turn of the message loop, in the clip's own undo step (the unfreezing is none).
    {
        const phazite::UndoStep step (model, "Move Clip");
        const auto halfASecond = phazite::BeatPosition::fromBeats (1.0); // at 120 BPM
        phazite::trimClipStart (model, song.clip, halfASecond);
    }

    CHECK (phazite::test::dispatchMessagesUntil ([&song] { return ! phazite::isFrozen (song.context, song.bass); }));
    CHECK_FALSE (phazite::isFrozenDevice (model, song.stereo));
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Move Clip"));

    // Undo puts the clip back, and leaves it unfrozen: the file may not hold what plays.
    phazite::undo (model);
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));

    // A frozen device: unfrozen too.
    phazite::setTrackName (model, song.bass, "Bass"); // the render's title
    song.freeze();
    song.setStereoGain (3.0f);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));

    // Undo of a change made before the freeze: unfrozen.
    {
        const phazite::UndoStep step (model, "Clip Gain");
        phazite::setClipGain (model, song.clip, -3.0f);
    }

    song.freeze();
    phazite::undo (model);
    CHECK (phazite::test::dispatchMessagesUntil ([&song] { return ! phazite::isFrozen (song.context, song.bass); }));
}

TEST_CASE ("A frozen track stays frozen as a send is added to it, its level turned and the send removed, and as each is undone "
           "and redone")
{
    // A frozen track's file is the track before its fader, and its sends come after it: what keeps it frozen follows no send
    // (SongValues.h: FreezeKeeper).
    Song song;
    auto& model = song.getSong();
    TrackID delay;

    {
        const phazite::UndoStep step (model, "Add Return");
        delay = phazite::addReturnTrack (model, "Delay", nullptr);
    }

    song.freeze();
    DeviceID send;

    {
        const phazite::UndoStep step (model, "Add Send");
        send = phazite::addSend (model, song.bass, delay, false);
    }

    REQUIRE (send.isValid());

    {
        const phazite::UndoStep step (model, "Change Send Level");
        phazite::setSendLevel (model, send, -12.0f);
    }

    {
        const phazite::UndoStep step (model, "Remove Send");
        phazite::removeSend (model, send);
    }

    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isFrozen (song.context, song.bass));

    for (int i = 0; i < 3; ++i)
        phazite::undo (model);

    REQUIRE (phazite::getSends (model, song.bass).size() == 1); // its own, which the fixture made
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isFrozen (song.context, song.bass));

    for (int i = 0; i < 3; ++i)
        phazite::redo (model);

    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isFrozen (song.context, song.bass));
}

TEST_CASE ("A frozen track unfreezes at the next turn of the message loop when its clip moves, a clip is added to it or an undo "
           "puts one back, and stays frozen through an undo and a redo of something else")
{
    // The clips are heard from the song's values (SongValues.h: Listener::clipsChanged()), compared once the step is whole:
    // during an undo, the rest of the step may not be back yet.
    Song song;
    auto& model = song.getSong();
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };
    const auto unfreezes = [&isFrozen]
    {
        CHECK (isFrozen()); // not within the edit
        CHECK (phazite::test::dispatchMessagesUntil ([&isFrozen] { return ! isFrozen(); }));
    };

    // An undo and a redo of something else: still frozen.
    TrackID other;

    {
        const phazite::UndoStep step (model, "Add Track");
        other = phazite::insertTrack (model, song.bass, nullptr);
    }

    REQUIRE (other.isValid());
    song.freeze();

    {
        const phazite::UndoStep step (model, "Add Clip");
        REQUIRE (phazite::test::addMidiClip (model, other, { phazite::test::at (0.0), phazite::test::at (4.0) }).isValid());
    }

    phazite::undo (model);
    phazite::test::dispatchPendingMessages();
    phazite::redo (model);
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());

    // Its clip moved.
    {
        const phazite::UndoStep step (model, "Move Clip");
        phazite::moveClip (model, song.clip, phazite::test::at (2.0), song.bass);
    }

    unfreezes();

    // A clip added to it.
    phazite::undo (model);
    phazite::test::dispatchPendingMessages();
    song.freeze();
    const auto sound = phazite::test::writeSound (song.temp.folder.getChildFile ("Second.wav"), 0.5);

    {
        const phazite::UndoStep step (model, "Add Clip");
        REQUIRE (phazite::test::addAudioClip (model, song.bass, "Second", sound, 4.0, 0.5).isValid());
    }

    unfreezes();

    // A clip put back by an undo: its delete made before the freeze (the clip added above kept, so that there is something
    // to freeze).
    {
        const phazite::UndoStep step (model, "Delete Clip");
        phazite::deleteClips (model, { song.clip }, false);
    }

    phazite::test::dispatchPendingMessages();
    song.freeze();
    phazite::undo (model);
    const auto clips = phazite::getClips (model, song.bass);
    CHECK (std::find (clips.begin(), clips.end(), song.clip) != clips.end());
    unfreezes();
}

TEST_CASE ("A frozen track unfreezes at the next turn of the message loop when the song's tempo changes, by an undo too, and stays "
           "frozen when it changes and changes back within one turn, or its time signature changes")
{
    // The tempo map is heard from the song's values (SongValues.h: Listener::tempoMapChanged()), every frozen track compared
    // at the next turn with the tempos its file was made with (Freeze.h: FreezeKeeper::Holds::tempos), as values: the file is
    // in seconds, which the tempos put its notes and clips at, and the time signatures put nothing anywhere.
    Song song;
    auto& model = song.getSong();
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };
    const auto unfreezes = [&isFrozen]
    {
        CHECK (isFrozen()); // not within the edit
        CHECK (phazite::test::dispatchMessagesUntil ([&isFrozen] { return ! isFrozen(); }));
    };

    // Changed and changed back in one turn: still frozen; and a time signature: still frozen.
    song.freeze();
    phazite::setTempo (model, 100.0);
    phazite::setTempo (model, 120.0);
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());
    phazite::setTimeSignature (model, { 3, 4 });
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());

    // Changed: unfrozen.
    phazite::setTempo (model, 100.0);
    unfreezes();

    // A tempo change added along the song: unfrozen.
    song.freeze();
    CHECK (phazite::addTempoChange (model, phazite::test::at (2.0), 90.0) == 1);
    unfreezes();

    // An undo of a tempo change made before the freeze: unfrozen.
    song.freeze();
    phazite::undo (model);
    CHECK_FALSE (phazite::hasTempoChanges (model));
    unfreezes();
}

TEST_CASE ("A frozen track unfreezes at the next turn of the message loop when a curve of one of its frozen devices changes, by an "
           "undo too, and stays frozen when another track's curve or its own fader's changes, or its device's changes and changes "
           "back within one turn")
{
    // The curves are heard from the song's values (SongValues.h: Listener::curvesChanged()), by their parameters: the frozen
    // tracks whose files play the automation of one of their devices are compared at the next turn with the curves each file
    // was made with (Freeze.h: FreezeKeeper::Holds::curves), as values.
    using phazite::test::at;
    Song song;
    auto& model = song.getSong();
    const auto gain = phazite::ParameterRef { song.stereo, "gain" };
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };
    const auto unfreezes = [&isFrozen]
    {
        CHECK (isFrozen()); // not within the edit
        CHECK (phazite::test::dispatchMessagesUntil ([&isFrozen] { return ! isFrozen(); }));
    };

    const auto edit = [&model] (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (model, name);
        change();
    };

    edit ("Automate Gain", [&] { phazite::addPoint (model, gain, { at (0.0), 0.5f, 0.0f }); });

    // Another track's curve, the track's own fader's (which plays after its file), and its device's changed and changed back
    // in one turn: still frozen.
    song.freeze();
    const auto volumeOf = [&model] (TrackID track)
    { return phazite::ParameterRef { phazite::getFader (model, track), phazite::getVolumeParameterID (model, track) }; };

    edit ("Automate the Reverb", [&] { phazite::addPoint (model, volumeOf (song.reverb), { at (1.0), 0.4f }); });
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());
    edit ("Automate the Bass's Volume", [&] { phazite::addPoint (model, volumeOf (song.bass), { at (1.0), 0.4f }); });
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());
    edit ("Bend Gain", [&] { phazite::setCurve (model, gain, 0, 0.5f); });
    edit ("Straighten Gain", [&] { phazite::setCurve (model, gain, 0, 0.0f); });
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());

    // Its device's curve changed: unfrozen.
    edit ("Automate Gain", [&] { phazite::addPoint (model, gain, { at (2.0), 0.8f, 0.0f }); });
    unfreezes();

    // An undo of a curve's change made before the freeze: unfrozen.
    song.freeze();
    phazite::undo (model);
    CHECK (phazite::getPoints (model, gain).size() == 1);
    unfreezes();

    // The device's curve's points all taken out, its curve with them: unfrozen.
    song.freeze();
    edit ("Clear Gain", [&] { phazite::deletePoints (model, gain, { 0 }); });
    CHECK_FALSE (phazite::getCurveValue (model, gain).has_value());
    unfreezes();
}

TEST_CASE ("A frozen track unfreezes at the next turn of the message loop when one of its modulators or macros, or a macro of its "
           "frozen container, changes, comes or goes, by an undo too, and stays frozen when another track's does")
{
    // The modulators are heard from the song's values (SongValues.h: Listener::modulationChanged()), by their places: a frozen
    // track a changed modulator was on or is on is compared at the next turn with the modulators its file was made with
    // (Freeze.h: FreezeKeeper::Holds::modulators), as values. An undo of the change that unfroze it leaves it playing live.
    Song song;
    auto& model = song.getSong();
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };
    const auto unfreezes = [&isFrozen]
    {
        CHECK (isFrozen()); // not within the edit
        CHECK (phazite::test::dispatchMessagesUntil ([&isFrozen] { return ! isFrozen(); }));
    };

    const auto edit = [&model] (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (model, name);
        change();
    };

    const auto ref = [] (phazite::ModulatorID modulator, const juce::String& parameter)
    { return phazite::ParameterRef { DeviceID::fromRaw (modulator.getRaw()), parameter }; };

    const auto macroValue = [&ref] (phazite::ModulatorID macro) { return ref (macro, juce::String (macro.getRaw())); };

    phazite::ModulatorID lfo, macro, other;
    DeviceID container;

    edit ("Add Modulators",
          [&]
          {
              lfo = phazite::addModulator (model, song.bass, phazite::ModulatorType::lfo);
              macro = phazite::addModulator (model, song.bass, phazite::ModulatorType::macro);
              other = phazite::addModulator (model, song.reverb, phazite::ModulatorType::lfo);
          });

    container = phazite::addContainer (song.context, song.bass);
    REQUIRE (container.isValid());
    const auto containerMacros = phazite::getContainerMacros (model, container);
    REQUIRE (containerMacros.size() == 8);
    song.freeze();
    REQUIRE (phazite::isFrozenDevice (model, container));

    // Another track's modulator: still frozen.
    edit ("Change Rate", [&] { phazite::setValueSetByHand (model, ref (other, "rate"), 2.0f); });
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());

    // Its macro turned: unfrozen; the Undo leaves it live.
    edit ("Change Macro", [&] { phazite::setValueSetByHand (model, macroValue (macro), 0.5f); });
    unfreezes();
    phazite::undo (model);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (isFrozen());

    // Its LFO's parameter, a macro of its frozen container, an LFO added and one removed: each unfreezes it.
    song.freeze();
    edit ("Change Rate", [&] { phazite::setValueSetByHand (model, ref (lfo, "rate"), 3.0f); });
    unfreezes();
    song.freeze();
    edit ("Change Macro", [&] { phazite::setValueSetByHand (model, macroValue (containerMacros[1]), 0.25f); });
    unfreezes();
    song.freeze();
    edit ("Add Random", [&] { phazite::addModulator (model, song.bass, phazite::ModulatorType::random); });
    unfreezes();
    song.freeze();
    edit ("Remove LFO", [&] { phazite::removeModulator (model, lfo); });
    unfreezes();

    // An undo of a modulator's change made before the freeze: unfrozen.
    song.freeze();
    phazite::undo (model);
    CHECK (phazite::getModulatorValue (model, lfo).has_value());
    unfreezes();
}

TEST_CASE ("A frozen track's fader moved by hand is compared without reading its curves, which are read once one of them "
           "changes")
{
    // What a frozen track's file plays the automation of is compared as values (Freeze.h: FreezeKeeper::Holds::curves), read
    // only where one of its curves was told changed since its last comparison (Freeze.cpp: FreezeKeeper::compare()), not at
    // every turn of a fader dragged on it. Counted as the points read into values (CurveValues.h: countCurvePointsRead()).
    using phazite::test::at;
    Song song;
    auto& model = song.getSong();
    const auto gain = phazite::ParameterRef { song.stereo, "gain" };
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };

    {
        const phazite::UndoStep step (model, "Automate Gain");
        phazite::addPoint (model, gain, { at (0.0), 0.5f, 0.0f });
        phazite::addPoint (model, gain, { at (1.0), 0.25f, 0.0f });
    }

    song.freeze();
    phazite::test::dispatchPendingMessages();

    // Each move of its fader, and its comparison at once (Song::writePending(), as a save or a plan's compile asks): none of
    // its curves read, and still frozen.
    for (const auto volume : { -3.0f, -4.0f, -5.0f })
    {
        const auto before = phazite::countCurvePointsRead();

        {
            const phazite::UndoStep step (model, "Change Volume");
            phazite::setChannelVolume (model, song.bass, volume);
        }

        model.writePending();
        CHECK (phazite::countCurvePointsRead() == before);
        CHECK (isFrozen());
    }

    // Gain's curve bent: its comparison reads the curves, and finds them changed.
    {
        const phazite::UndoStep step (model, "Bend Gain");
        phazite::setCurve (model, gain, 0, 0.5f);
    }

    const auto before = phazite::countCurvePointsRead();
    model.writePending();
    CHECK (phazite::countCurvePointsRead() > before);
    CHECK_FALSE (isFrozen());
}

TEST_CASE ("A frozen track's fader moved by hand is compared without reading its modulators, which are read once one of them "
           "changes, and another track's modulator changed reads none of them")
{
    // What a frozen track's file holds of its modulators and macros is compared as values (Freeze.h:
    // FreezeKeeper::Holds::modulators), read only where one of them was told changed since its last comparison (Freeze.cpp:
    // FreezeKeeper::compare(), modulatorsAsFrozen), not at every turn of a fader dragged on it. Counted as the modulators read
    // into values (ModulationValue.h: countModulatorsRead()).
    Song song;
    auto& model = song.getSong();
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };
    const auto rate = [] (phazite::ModulatorID modulator)
    { return phazite::ParameterRef { DeviceID::fromRaw (modulator.getRaw()), "rate" }; };

    const auto edit = [&model] (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (model, name);
        change();
    };

    // Bass with three, and the return with one, whose first value set by hand is set here.
    phazite::ModulatorID lfo, other;

    edit ("Add Modulators",
          [&]
          {
              lfo = phazite::addModulator (model, song.bass, phazite::ModulatorType::lfo);
              phazite::addModulator (model, song.bass, phazite::ModulatorType::macro);
              phazite::addModulator (model, song.bass, phazite::ModulatorType::random);
              other = phazite::addModulator (model, song.reverb, phazite::ModulatorType::lfo);
              phazite::setValueSetByHand (model, rate (other), 2.0f);
          });

    // The modulators an edit of the return's LFO reads, compared at once (Song::writePending()), with no track frozen.
    const auto readsOfOther = [&] (float value)
    {
        const auto before = phazite::countModulatorsRead();
        edit ("Change Rate", [&] { phazite::setValueSetByHand (model, rate (other), value); });
        model.writePending();
        return phazite::countModulatorsRead() - before;
    };

    phazite::test::dispatchPendingMessages();
    const auto unfrozenReads = readsOfOther (3.0f);
    phazite::test::dispatchPendingMessages();
    song.freeze();
    phazite::test::dispatchPendingMessages();

    // Each move of its fader, and its comparison at once: none of its modulators read, and still frozen.
    for (const auto volume : { -3.0f, -4.0f, -5.0f })
    {
        const auto before = phazite::countModulatorsRead();
        edit ("Change Volume", [&] { phazite::setChannelVolume (model, song.bass, volume); });
        model.writePending();
        CHECK (phazite::countModulatorsRead() == before);
        CHECK (isFrozen());
    }

    // The return's LFO changed: what the edit reads with no track frozen, none of Bass's; still frozen.
    CHECK (readsOfOther (4.0f) == unfrozenReads);
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());

    // Its LFO changed: its comparison looks its modulators up in modulation's area (none read from an element: the song's
    // tree holds none), and finds them changed.
    edit ("Change Rate", [&] { phazite::setValueSetByHand (model, rate (lfo), 5.0f); });
    const auto before = phazite::countModulatorsRead();
    model.writePending();
    CHECK (phazite::countModulatorsRead() == before);
    CHECK_FALSE (isFrozen());
}

TEST_CASE ("A frozen track whose clip changed plays live in what is saved, and in the plan compiled, before the next turn of the "
           "message loop")
{
    // What waits for the next turn is written as the song is stored and as a plan of it is compiled (Song::writePending()):
    // a save made before that turn wrote the track frozen, playing a file that no longer holds what it plays, and the
    // playback session's plan played the file until another plan followed the unfreezing.
    Song song;
    const auto moveClip = [&song] (double beat) // the project's song, which reopen() makes anew
    {
        auto& model = song.getSong();
        const phazite::UndoStep step (model, "Move Clip");
        phazite::moveClip (model, song.clip, phazite::test::at (beat), song.bass);
    };

    // Saved.
    song.freeze();
    REQUIRE (song.project->save().wasOk());
    moveClip (2.0);
    REQUIRE (song.project->save().wasOk());
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (song.project->hasUnsavedChanges());
    song.reopen();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));

    // Played: one plan, which plays it live.
    song.freeze();
    phazite::test::PlanCounter plans (song.context);
    moveClip (0.0);
    plans.makePlanNow();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK (plans.take() == 1);
}

TEST_CASE ("A frozen track moved into a folder and out again stays frozen, whatever order the song's listeners are told in")
{
    // JUCE tells the listeners of the song's root in the order of their handles' addresses (SongValues.h), so the keeper of
    // the song's values may give a track JUCE moves (taken out, put in again) its clips back before or after the freezes'
    // keeper hears of it; which comes first depends on where they were made. The project context, which makes the freezes'
    // keeper and the song's other listeners, is made again for each round, after allocations of other sizes, so that the
    // rounds meet both orders. Two frozen tracks, each moved in its own step, the moves undone and redone.
    Song song;
    auto& model = song.getSong();
    song.freeze();
    TrackID keys, folder;

    {
        const phazite::UndoStep step (model, "Add Tracks");
        keys = phazite::insertTrack (model, song.bass, nullptr);
        folder = phazite::insertFolder (model, keys, nullptr);
        const auto sound = phazite::test::writeSound (song.temp.folder.getChildFile ("Keys.wav"), 1.0);
        REQUIRE (phazite::test::addAudioClip (model, keys, "Keys", sound, 0.0, 1.0).isValid());
    }

    phazite::freezeFromFile (model, keys, phazite::test::writeSound (song.temp.folder.getChildFile ("Frozen.wav"), 1.0));
    phazite::test::dispatchPendingMessages();
    const auto isFrozen = [&song] (TrackID track) { return phazite::isFrozen (song.context, track); };
    REQUIRE (isFrozen (song.bass));
    REQUIRE (isFrozen (keys));
    std::vector<std::unique_ptr<char[]>> ballast;

    for (int round = 0; round < 8; ++round)
    {
        CAPTURE (round);
        song.close();

        for (int i = 0; i <= round; ++i)
            ballast.push_back (std::make_unique<char[]> ((size_t) (48 + 40 * round + 16 * i)));

        song.open();

        for (const auto track : { song.bass, keys })
        {
            {
                const phazite::UndoStep step (model, "Into the Folder");
                phazite::moveTrackIntoFolder (model, track, folder);
            }

            phazite::test::dispatchPendingMessages();
            CHECK (phazite::getParentFolder (model, track) == folder);
            CHECK (isFrozen (track));

            {
                const phazite::UndoStep step (model, "Out of the Folder");
                phazite::moveTracksAfter (model, { track }, folder);
            }

            phazite::test::dispatchPendingMessages();
            CHECK_FALSE (phazite::getParentFolder (model, track).isValid());
            CHECK (isFrozen (track));
        }

        // The last move undone (back into the folder) and redone (out again).
        phazite::undo (model);
        phazite::test::dispatchPendingMessages();
        CHECK (phazite::getParentFolder (model, keys) == folder);
        CHECK (isFrozen (keys));
        phazite::redo (model);
        phazite::test::dispatchPendingMessages();
        CHECK_FALSE (phazite::getParentFolder (model, keys).isValid());
        CHECK (isFrozen (song.bass));
        CHECK (isFrozen (keys));
    }
}

TEST_CASE ("A track frozen and its clip moved in the same turn of the message loop unfreezes, in one step too")
{
    // What a frozen track holds is taken as it is frozen (freezeFromFile(), which has the freezes' keeper take it then, not
    // in a callback of the song's tree, which reads no clip of the song's), so that what changes after it unfreezes it.
    Song song;
    auto& model = song.getSong();
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };

    phazite::freezeFromFile (model, song.bass, phazite::test::writeSound (song.temp.folder.getChildFile ("First.wav"), 2.0));
    REQUIRE (isFrozen());

    {
        const phazite::UndoStep step (model, "Move Clip");
        phazite::moveClip (model, song.clip, phazite::test::at (2.0), song.bass);
    }

    CHECK (phazite::test::dispatchMessagesUntil ([&isFrozen] { return ! isFrozen(); }));

    // Frozen and its clip moved in one step, as Bounce in Place freezes in a step of its own.
    {
        const phazite::UndoStep step (model, "Freeze and Move");
        phazite::freezeFromFile (model, song.bass, phazite::test::writeSound (song.temp.folder.getChildFile ("Second.wav"), 2.0),
                                 &model.getUndoManager());
        REQUIRE (isFrozen());
        phazite::moveClip (model, song.clip, phazite::test::at (0.0), song.bass);
    }

    CHECK (phazite::test::dispatchMessagesUntil ([&isFrozen] { return ! isFrozen(); }));
}

TEST_CASE ("A frozen track deleted and brought back by Undo is still frozen")
{
    Song song;
    auto& model = song.getSong();
    const auto isFrozen = [&song] { return phazite::isFrozen (song.context, song.bass); };
    song.freeze();

    {
        const phazite::UndoStep step (model, "Delete Track");
        phazite::deleteTracks (model, { song.bass }, nullptr);
    }

    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (phazite::test::hasTrack (model, song.bass));
    phazite::undo (model);
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());
    CHECK (phazite::getClips (model, song.bass) == std::vector { song.clip });

    // Again, through a Redo of the delete.
    phazite::redo (model);
    phazite::test::dispatchPendingMessages();
    phazite::undo (model);
    phazite::test::dispatchPendingMessages();
    CHECK (isFrozen());
}

TEST_CASE ("A frozen track opens frozen from the project's Cache folder, and unfrozen without its file")
{
    Song song;
    song.freeze();
    REQUIRE (song.project->save().wasOk());

    // Frozen in the song (Schema.h: trackFrozen): a song made from its state alone plays the file too.
    CHECK ((bool) phazite::test::getTrackProperty (song.getSong(), song.bass, phazite::schema::trackFrozen));

    {
        const auto copy =
            phazite::makeSong (phazite::test::copyWholeSong (song.getSong()), song.project->getFile(), song.project->getCacheFolder());
        REQUIRE (copy != nullptr);
        CHECK (phazite::getFrozenTracks (*copy) == std::vector { song.bass });
        CHECK (phazite::isFrozenDevice (*copy, song.stereo));
        CHECK (phazite::getFrozenFile (*copy, song.bass) == song.frozenFile());
    }

    song.reopen();
    CHECK (phazite::isFrozen (song.context, song.bass));
    CHECK (phazite::isFrozenDevice (song.getSong(), song.stereo));
    CHECK_FALSE (song.project->hasUnsavedChanges());
    CHECK (song.frozenFile().existsAsFile());
    CHECK (phazite::getFreezeCommandName (song.context) == "Unfreeze Track");

    // Without the Cache folder, it opens unfrozen, as it would play.
    song.close();
    song.project = nullptr;
    const auto file = song.temp.folder.getChildFile ("Song").getChildFile ("Song.phazite");
    REQUIRE (phazite::Project::getCacheFolder (file).deleteRecursively());

    juce::String error;
    song.project = phazite::Project::open (file, false, error);
    REQUIRE (song.project != nullptr);
    song.open();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isFrozenDevice (song.getSong(), song.stereo));
    CHECK_FALSE (phazite::test::copyTrack (song.getSong(), song.bass).hasProperty (phazite::schema::trackFrozen));
}

TEST_CASE ("Freezing stops before a device that listens to a sidechain; a send before the fader sends what the file plays")
{
    Song song;
    auto& model = song.getSong();
    const auto kick = phazite::insertPlainTrack (model, TrackID());
    REQUIRE (kick.isValid());

    const auto compressor = phazite::test::addOwnDevice (model, song.bass, phazite::devices::Compressor::typeID);
    REQUIRE (compressor.isValid());
    phazite::setSidechainSource (model, compressor, kick);
    CHECK (phazite::getFrozenDevices (model, song.bass) == std::vector { song.stereo });

    // A send before the fader doesn't stop it: frozen up to the fader, the send goes on sending what the file plays.
    phazite::setSidechainSource (model, compressor, TrackID());
    const auto preFader = phazite::addSend (model, song.bass, song.reverb, true);
    REQUIRE (preFader.isValid());
    CHECK (phazite::getFrozenDevices (model, song.bass) == std::vector { song.stereo, compressor });

    song.freeze();
    CHECK (phazite::isFrozenDevice (model, song.stereo));
    CHECK (phazite::isFrozenDevice (model, compressor));
    CHECK_FALSE (phazite::isFrozenDevice (model, preFader));
}

TEST_CASE ("Returns, buses and empty tracks aren't frozen or bounced, and say why")
{
    Song song;
    auto& model = song.getSong();
    CHECK (phazite::getWhyNotFreeze (song.context, song.reverb)
           == "Returns can't be frozen: their sound comes from the tracks that send to them.");
    CHECK (phazite::getWhyNotBounce (song.context, song.reverb)
           == "Returns can't be bounced: their sound comes from the tracks that send to them.");

    const auto bus = phazite::groupIntoFolder (model, { song.bass }, nullptr);
    REQUIRE (bus.isValid());
    CHECK (phazite::getWhyNotFreeze (song.context, bus) == "Folders and buses can't be frozen: their sound is their tracks'.");

    const auto empty = phazite::insertPlainTrack (model, TrackID());
    REQUIRE (empty.isValid());
    phazite::setTrackName (model, empty, "Empty");
    CHECK (phazite::getWhyNotBounce (song.context, empty) == "Empty has nothing to bounce: it has no clips.");

    // The command tells the host why.
    getSelection (song.context).selectOnly (empty);
    CHECK_FALSE (phazite::canToggleFreeze (song.context));
    phazite::toggleFreeze (song.context, song.host);
    CHECK (song.host.finished == juce::StringArray { "Freezing Empty: Empty has nothing to freeze: it has no clips." });
    CHECK (song.host.render == nullptr);
}

TEST_CASE ("A freeze cancelled leaves the track as it was")
{
    Song song;
    auto& model = song.getSong();
    phazite::setTrackMuted (model, song.bass, true);
    phazite::toggleFreeze (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    CHECK (phazite::isTrackMuted (model, song.bass, false)); // nothing of the song changes while Phazite's engine renders it

    song.host.render = nullptr;
    phazite::test::dispatchPendingMessages();
    CHECK (song.host.finished.isEmpty());
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isFrozenDevice (model, song.stereo));
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK (phazite::isDeviceOn (model, song.stereo));
    CHECK_FALSE (song.frozenFile().existsAsFile());

    for (const auto& file : song.project->getCacheFolder().findChildFiles (juce::File::findFiles, false))
        CHECK_FALSE (file.getFileName().startsWith ("Frozen"));
}

TEST_CASE ("A freeze makes the render of a clip that isn't there yet on its own thread, and freezes the clip's sound")
{
    // The audit of 2026-11-20: a freeze made the clips' renders that weren't there on the message thread before it started.
    Song song;
    auto& model = song.getSong();
    phazite::setClipPitch (model, song.clip, 12.0f); // the A an octave up: 440 Hz
    const auto before = phazite::countClipRendersMade();
    song.freeze();
    CHECK (phazite::countClipRendersMade().onMessageThread == before.onMessageThread);

    // The pitched sine is what was frozen: 440 rises a second.
    double sampleRate = 0.0;
    const auto audio = readAudio (song.frozenFile(), sampleRate);
    const auto first = juce::roundToInt (0.1 * sampleRate), last = juce::roundToInt (1.1 * sampleRate);
    auto rises = 0;

    for (int i = first + 1; i < last; ++i)
        if (audio.getSample (0, i - 1) < 0.0f && audio.getSample (0, i) >= 0.0f)
            ++rises;

    CHECK (rises == doctest::Approx (440.0).epsilon (0.01));
    CHECK (getLevel (song.frozenFile(), 0.1, 1.4) > 0.05);
}

TEST_CASE ("A track frozen while it is muted, or while another track is soloed, freezes its sound, not silence")
{
    // A render before the fader hears the track whatever its mute switch or a solo elsewhere says: muted, it rendered its
    // first block alone; Bounce in Place mutes the original, so a second bounce of it was silent.
    for (const auto soloElsewhere : { false, true })
    {
        CAPTURE (soloElsewhere);
        Song song;
        auto& model = song.getSong();

        if (soloElsewhere)
            phazite::setTrackSoloed (model, song.reverb, true);
        else
            phazite::setTrackMuted (model, song.bass, true);

        song.freeze();
        CHECK (getLevel (song.frozenFile(), 0.5, 1.0) > 0.05);
    }
}

TEST_CASE ("Bounce in Place renders a track before its fader onto a new audio track under it, and mutes and freezes the original")
{
    Song song;
    auto& model = song.getSong();
    CHECK (phazite::canBounceSelectedTrack (song.context));
    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    CHECK (song.host.render->getTitle() == "Bouncing Bass");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });

    // A new track right under Bass, playing the file from the song's start, selected.
    const auto tracks = phazite::getTrackIDs (model);
    REQUIRE (tracks.size() == 3);
    const auto bounced = tracks[1];
    CHECK (phazite::getTrackName (model, bounced) == "Bass (Bounce)");
    CHECK (getSelection (song.context).isSelected (bounced));
    const auto clips = phazite::getClips (model, bounced);
    REQUIRE (clips.size() == 1);
    REQUIRE (phazite::isAudioClip (model, clips[0]));
    CHECK (phazite::getClipRange (model, clips[0]).getStart() == phazite::BeatPosition());
    const auto file = phazite::getClipFile (model, clips[0]);
    CHECK (file.getParentDirectory() == song.project->getAudioFolder());
    CHECK (file.getFileName() == "Bass (Bounce).wav");

    // The sound before the fader: the sine at a quarter of full scale in each channel, with Stereo's 6 dB, without the
    // fader's -6 dB (or its pan law). The fader comes along, on the new track, and the send moves there (known bug 194).
    const auto sineDecibels = juce::Decibels::gainToDecibels (0.25 / std::sqrt (2.0));
    CHECK (juce::Decibels::gainToDecibels (getLevel (file, 0.1, 1.4)) == doctest::Approx (sineDecibels + 6.0).epsilon (0.05));
    CHECK (phazite::getChannelVolume (model, bounced) == doctest::Approx (-6.0f).epsilon (0.01));
    const auto sends = phazite::getSends (model, bounced);
    REQUIRE (sends.size() == 1);
    CHECK (phazite::getSendTarget (model, sends[0]) == song.reverb);
    CHECK (phazite::getSends (model, song.bass).empty());
    CHECK (phazite::getDevices (model, bounced).empty());

    // The original muted, and shown as it was: collapsing it was view state, outside the step, which Undo left collapsed
    // (known bug 138). One undo step, which takes it all back, mute included, though a mute alone is no undo step; Redo
    // brings it back.
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK_FALSE (phazite::isCollapsed (model, song.bass));
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Bounce in Place"));

    // And frozen in the same step, the bounce its frozen file: a muted track plays on underneath, and so would cost its
    // devices for nothing (the review of known bug 78). The frozen file is the bounce, sample for sample.
    CHECK (phazite::isFrozen (song.context, song.bass));
    REQUIRE (song.frozenFile().existsAsFile());
    CHECK (getLevel (song.frozenFile(), 0.1, 1.4) == doctest::Approx (getLevel (file, 0.1, 1.4)));
    phazite::test::dispatchPendingMessages(); // what keeps frozen tracks honest has heard the step: it stays frozen
    CHECK (phazite::isFrozen (song.context, song.bass));

    phazite::undo (model);
    CHECK (phazite::getTrackIDs (model).size() == 2);
    CHECK_FALSE (phazite::isTrackMuted (model, song.bass, false));
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isCollapsed (model, song.bass));
    CHECK (phazite::getSends (model, song.bass).size() == 1);
    REQUIRE (phazite::canRedo (model));
    phazite::redo (model);
    CHECK (phazite::getTrackIDs (model).size() == 3);
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK (phazite::isFrozen (song.context, song.bass));
}

namespace
{
// Bounces Bass in place with its command's function, as the app does, and waits for it.
void bounce (Song& song)
{
    song.host.finished.clear();
    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });
    phazite::test::dispatchPendingMessages();
}

// What the engine plays of a track: whether it plays a file (its frozen file, in place of its clips) and how many devices.
std::pair<bool, size_t> getPlayed (phazite::Song& song, TrackID track)
{
    phazite::SongPlayer player (song);
    const auto plan = player.compile (48000.0, 256, phazite::SongPlayer::Live {});

    for (const auto& strip : plan->strips)
        if (strip.id == (std::int64_t) track.getRaw())
            return { ! strip.audio.empty() && strip.audio.front().file != nullptr, strip.devices.size() };

    return { false, 0 };
}
} // namespace

TEST_CASE ("Bounce in Place's freeze is kept across a save, holds a track's containers, and isn't brought back without its file")
{
    // The review of known bug 78 (E): what the freeze that Bounce in Place leaves the original with does where it was not
    // looked at.
    SUBCASE ("Saved and opened again: still frozen, playing its file")
    {
        Song song;
        bounce (song);
        REQUIRE (song.project->save().wasOk());
        song.reopen();
        CHECK (phazite::isFrozen (song.context, song.bass));
        CHECK (song.frozenFile().existsAsFile());
        CHECK (getPlayed (song.getSong(), song.bass) == std::pair<bool, size_t> { true, 0 });
    }

    SUBCASE ("A track holding a container: frozen too, the container in its file")
    {
        Song song;
        REQUIRE (phazite::groupIntoContainer (song.context, { song.stereo }).isValid());
        bounce (song);
        CHECK (phazite::isFrozen (song.context, song.bass));
        CHECK (getPlayed (song.getSong(), song.bass) == std::pair<bool, size_t> { true, 0 });
    }

    SUBCASE ("Undone, its file let go of, and redone: not frozen without the file, which would play nothing")
    {
        Song song;
        auto& model = song.getSong();
        bounce (song);
        phazite::undo (model);
        phazite::purgeCache (model); // as a project closing does: the file of a track that isn't frozen goes
        REQUIRE_FALSE (song.frozenFile().existsAsFile());
        phazite::redo (model);
        phazite::test::dispatchPendingMessages();
        CHECK (phazite::isTrackMuted (model, song.bass, false));
        CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
        CHECK (getPlayed (model, song.bass).second == 1); // its devices play live, for when it is unmuted
    }
}

TEST_CASE ("Bounce in Place goes through where its frozen copy can't be made, and leaves the original muted and unfrozen")
{
    // The review of the audit's fix: the bounce's copy into the Cache folder, as the original's frozen file, is made on the
    // render's thread; one that can't be made throws nothing of the bounce away, and the original just isn't frozen.
    Song song;
    auto& model = song.getSong();
    const auto cache = song.project->getCacheFolder();
    REQUIRE (cache.deleteRecursively());
    REQUIRE (cache.replaceWithText ("a file where the Cache folder goes")); // so no copy can go there
    bounce (song);

    const auto tracks = phazite::getTrackIDs (model);
    REQUIRE (tracks.size() == 3);
    const auto clips = phazite::getClips (model, tracks[1]);
    REQUIRE (clips.size() == 1);
    const auto file = phazite::getClipFile (model, clips[0]);
    CHECK (file.existsAsFile());
    CHECK (getLevel (file, 0.1, 1.4) > 0.05);
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Bounce in Place"));
    CHECK (cache.existsAsFile());
    REQUIRE (cache.deleteFile());
}

TEST_CASE ("Bounce in Place takes the automation and the modulation of the track's fader and sends along to the new track")
{
    // The render is the track before its fader: a volume fade, a pan move or a send throw is no part of it, and stayed with
    // the muted original, so the bounce played without it.
    Song song;
    auto& model = song.getSong();
    const auto fader = phazite::getFader (model, song.bass);
    const auto send = phazite::getSends (model, song.bass).front();
    const phazite::ParameterRef volume { fader, phazite::getVolumeParameterID (model, song.bass) };
    const phazite::ParameterRef pan { fader, phazite::getPanParameterID (model, song.bass) };
    const phazite::ParameterRef level { send, phazite::schema::sendLevelParameter };
    const phazite::ParameterRef gain { song.stereo, "gain" };
    const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };

    phazite::addPoint (model, volume, { at (0.0), 0.7f, 0.0f });
    phazite::addPoint (model, volume, { at (4.0), 0.2f, 0.5f });
    phazite::addPoint (model, level, { at (2.0), 0.6f, 0.0f });
    phazite::addPoint (model, gain, { at (0.0), 0.9f, 0.0f }); // a device's: rendered, not copied

    const auto lfo = phazite::addModulator (model, song.bass, phazite::ModulatorType::lfo);
    const auto macro = phazite::addModulator (model, song.bass, phazite::ModulatorType::macro);
    const auto other = phazite::addModulator (model, song.bass, phazite::ModulatorType::random);
    REQUIRE (lfo.isValid());
    REQUIRE (macro.isValid());
    REQUIRE (other.isValid());
    phazite::setModulation (model, lfo, pan, 0.25f);
    phazite::setModulation (model, lfo, gain, 0.5f);
    phazite::setModulation (model, macro, level, -0.5f);
    phazite::setModulation (model, other, gain, 0.5f); // modulates the devices alone: stays behind
    model.getUndoManager().clearUndoHistory();
    const auto levelPoints = phazite::getPoints (model, level);

    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });

    const auto tracks = phazite::getTrackIDs (model);
    REQUIRE (tracks.size() == 3);
    const auto bounced = tracks[1];
    const auto newFader = phazite::getFader (model, bounced);
    const auto newSends = phazite::getSends (model, bounced);
    REQUIRE (newSends.size() == 1);
    REQUIRE (newFader != fader);
    const phazite::ParameterRef newVolume { newFader, volume.parameter }, newPan { newFader, pan.parameter };
    const phazite::ParameterRef newLevel { newSends[0], level.parameter };

    // The fader's and the send's curves, point for point; the original fader keeps its own, and the send moved (known bug
    // 194: a copy left with the muted original sent before its fader too).
    const auto samePoints = [] (const auto& first, const auto& second)
    {
        return first.size() == second.size()
               && std::equal (first.begin(), first.end(), second.begin(),
                              [] (const auto& x, const auto& y)
                              {
                                  return x.beat == y.beat && juce::approximatelyEqual (x.position, y.position)
                                         && juce::approximatelyEqual (x.curve, y.curve);
                              });
    };

    CHECK (phazite::getPoints (model, newVolume).size() == 2);
    CHECK (samePoints (phazite::getPoints (model, newVolume), phazite::getPoints (model, volume)));
    CHECK (phazite::getPoints (model, newLevel).size() == 1);
    CHECK (samePoints (phazite::getPoints (model, newLevel), levelPoints));
    CHECK (phazite::getPoints (model, newPan).empty());
    CHECK (phazite::getSends (model, song.bass).empty());

    // The copies after the song's other curves, in the originals' order (Automation.h: copyCurves(), through setCurveValue()); the
    // moved send's own kept, as a removed send's is, for an undo to bring it back.
    CHECK (phazite::getCurveKeys (model) == std::vector { volume, level, gain, newVolume, newLevel });

    // The modulators that moved the fader and the send, copied onto the new track with those modulations alone; the
    // original's macro modulated the send alone, which has gone, and modulates nothing now.
    const auto modulators = phazite::getModulators (model, bounced);
    REQUIRE (modulators.size() == 2);
    CHECK (phazite::getModulatorType (model, modulators[0]) == phazite::ModulatorType::lfo);
    CHECK (phazite::getModulatorType (model, modulators[1]) == phazite::ModulatorType::macro);
    const auto ofLfo = phazite::getModulations (model, modulators[0]), ofMacro = phazite::getModulations (model, modulators[1]);
    REQUIRE (ofLfo.size() == 1);
    CHECK (ofLfo[0].parameter == newPan);
    CHECK (ofLfo[0].amount == doctest::Approx (0.25f));
    REQUIRE (ofMacro.size() == 1);
    CHECK (ofMacro[0].parameter == newLevel);
    CHECK (ofMacro[0].amount == doctest::Approx (-0.5f));
    CHECK (phazite::getModulators (model, song.bass).size() == 3);
    CHECK (phazite::getModulations (model, lfo).size() == 2);

    // Each copy set as its original is, but for its ID, its place and what it modulates (modulation's setters:
    // Modulation.h, copyModulators()).
    for (const auto& [original, copy] : { std::pair { lfo, modulators[0] }, std::pair { macro, modulators[1] } })
    {
        auto expected = *phazite::getModulatorValue (model, original);
        const auto copied = phazite::getModulatorValue (model, copy);
        REQUIRE (copied.has_value());
        expected.id = copied->id;
        expected.place = copied->place;
        expected.modulations = copied->modulations;
        CHECK (*copied == expected);
        CHECK (copied->place == phazite::ModulatorPlace { .track = bounced });
    }
    CHECK (phazite::getModulations (model, macro).empty());

    // All of it in the bounce's one undo step, the send and its modulation back on the original.
    phazite::undo (model);
    CHECK (phazite::getTrackIDs (model).size() == 2);
    CHECK (phazite::getPoints (model, newVolume).empty());
    CHECK (phazite::getPoints (model, newLevel).empty());
    CHECK (phazite::getCurveKeys (model) == std::vector { volume, level, gain });
    CHECK (phazite::getSends (model, song.bass) == std::vector { send });
    CHECK (samePoints (phazite::getPoints (model, level), levelPoints));
    CHECK (phazite::getModulations (model, macro).size() == 1);
    CHECK_FALSE (phazite::canUndo (model));
}

TEST_CASE ("Bounce in Place gives a macro copied onto the new track its original's automation, under the copy's own ID")
{
    // A macro's value is named by the macro's own ID (Parameters.h): the curves were copied before the modulators, whose
    // copies' IDs the copied IDs didn't hold yet, so the copied macro that moved the new track's fader kept no automation,
    // and stood where the original's curve had moved it.
    Song song;
    auto& model = song.getSong();
    const auto fader = phazite::getFader (model, song.bass);
    const phazite::ParameterRef volume { fader, phazite::getVolumeParameterID (model, song.bass) };
    const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };
    const auto macro = phazite::addModulator (model, song.bass, phazite::ModulatorType::macro);
    REQUIRE (macro.isValid());
    phazite::setModulation (model, macro, volume, 0.5f);
    const auto valueOf = [] (phazite::ModulatorID id)
    { return phazite::ParameterRef { DeviceID::fromRaw (id.getRaw()), juce::String (id.getRaw()) }; };
    phazite::addPoint (model, valueOf (macro), { at (0.0), 0.2f, 0.0f });
    phazite::addPoint (model, valueOf (macro), { at (4.0), 0.8f, 0.5f });
    model.getUndoManager().clearUndoHistory();
    const auto curve = phazite::getCurveValue (model, valueOf (macro));
    REQUIRE (curve.has_value());
    phazite::test::UndoCheck undo (song.context);

    undo.check ("Bounce in Place",
                [&]
                {
                    phazite::bounceSelectedTrack (song.context, song.host);
                    REQUIRE (song.host.render != nullptr);
                    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
                    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });
                });

    const auto tracks = phazite::getTrackIDs (model);
    REQUIRE (tracks.size() == 3);
    const auto bounced = tracks[1];
    const auto modulators = phazite::getModulators (model, bounced);
    REQUIRE (modulators.size() == 1);
    const auto copy = modulators[0];
    CHECK (copy != macro);
    const auto ofCopy = phazite::getModulations (model, copy);
    REQUIRE (ofCopy.size() == 1);
    CHECK (ofCopy[0].parameter == phazite::ParameterRef { phazite::getFader (model, bounced), volume.parameter });

    // The copy's value automated as the original's is; the original's as it was.
    const auto copiedCurve = phazite::getCurveValue (model, valueOf (copy));
    REQUIRE (copiedCurve.has_value());
    CHECK (*copiedCurve == *curve);
    CHECK (phazite::getCurveValue (model, valueOf (macro)) == curve);
    phazite::test::checkSong (model);

    // Undone, the copy and its curve gone with the step; redone, both back.
    phazite::undo (model);
    CHECK_FALSE (phazite::getCurveValue (model, valueOf (copy)).has_value());
    phazite::redo (model);
    CHECK (phazite::getCurveValue (model, valueOf (copy)) == curve);
    undo.walkBackAndForth();
}

namespace
{
template <typename ComponentType>
juce::Array<ComponentType*> findAll (juce::Component& parent)
{
    juce::Array<ComponentType*> found;

    for (auto* child : parent.getChildren())
    {
        if (auto* wanted = dynamic_cast<ComponentType*> (child))
            found.add (wanted);

        found.addArray (findAll<ComponentType> (*child));
    }

    return found;
}
} // namespace

TEST_CASE ("A frozen track shows a snowflake that unfreezes it, frosted clips, and frozen devices with their controls off")
{
    Song song;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Playlist playlist { commands, song.context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
    phazite::ui::DevicesView devices { commands, song.context };
    phazite::test::TestWindow devicesWindow { devices, 1000, 400 };
    phazite::test::dispatchPendingMessages();

    const auto findHeader = [&playlist, &song]() -> phazite::ui::TrackHeader*
    {
        for (auto* header : findAll<phazite::ui::TrackHeader> (playlist))
            if (header->getTrack() == song.bass)
                return header;

        return nullptr;
    };

    auto* header = findHeader();
    REQUIRE (header != nullptr);
    CHECK_FALSE (header->getFrozenButton().isVisible());

    const auto clipsAreFrosted = [&playlist]
    {
        const auto clips = findAll<phazite::ui::ClipComponent> (playlist);
        return ! clips.isEmpty() && std::all_of (clips.begin(), clips.end(), [] (auto* clip) { return clip->isFrozen(); });
    };

    CHECK_FALSE (clipsAreFrosted());

    song.freeze();
    phazite::test::dispatchPendingMessages();
    header = findHeader();
    REQUIRE (header != nullptr);
    CHECK (header->getFrozenButton().isVisible());
    CHECK (header->getFrozenButton().getTooltip().startsWith ("Bass is frozen"));
    CHECK (clipsAreFrosted());

    // Stereo's controls are off, in the Devices view.
    devices.showDevice (song.stereo);
    phazite::test::dispatchPendingMessages();
    auto* panel = devices.findPanel (song.stereo);
    REQUIRE (panel != nullptr);
    auto* power = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "On"));
    REQUIRE (power != nullptr);
    CHECK_FALSE (power->isEnabled());

    // The snowflake plays it live again (the views show it anew).
    {
        phazite::test::Mouse mouse (header->getFrozenButton());
        mouse.click (header->getFrozenButton().getLocalBounds().getCentre());
    }

    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (clipsAreFrosted());
    panel = devices.findPanel (song.stereo);
    REQUIRE (panel != nullptr);
    power = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "On"));
    REQUIRE (power != nullptr);
    CHECK (power->isEnabled());
}
