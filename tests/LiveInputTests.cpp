// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HostedDevice.h"
#include "TestHelpers.h"
#include "devices/dynamics/Limiter.h"
#include "devices/synth/Synth.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/engine/internal/LiveInput.h"
#include "model/project/internal/Project.h"

namespace
{
// Tracktion's merged MIDI input, which it creates on a timer once asked to scan the MIDI devices.
std::shared_ptr<te::MidiInputDevice> findAllMidiInputs()
{
    auto& engine = phazite::test::getEngine();

    if (phazite::getAllMidiInputs (engine) == nullptr)
    {
        engine.getDeviceManager().rescanMidiDeviceList();
        phazite::test::runTimersUntil ([&engine] { return phazite::getAllMidiInputs (engine) != nullptr; });
    }

    return phazite::getAllMidiInputs (engine);
}

void addInstrument (te::AudioTrack& track)
{
    track.pluginList.insertPlugin (track.edit.getPluginCache().createNewPlugin (phazite::devices::Synth::typeID, {}), 0, nullptr);
}
} // namespace

TEST_CASE ("Live MIDI goes to the first track with an instrument")
{
    const auto allMidiInputs = findAllMidiInputs();
    REQUIRE (allMidiInputs != nullptr);
    CHECK (allMidiInputs->getMonitorMode() == te::InputDevice::MonitorMode::on); // heard without arming

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (3);
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[1]);

    phazite::routeLiveMidiInput (*edit);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[1]);

    // Another track can take it over, and it then plays only there.
    REQUIRE (phazite::setLiveMidiTrack (*tracks[2]));
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);

    // A project that already sends live MIDI somewhere keeps that.
    phazite::routeLiveMidiInput (*edit);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);
}

TEST_CASE ("Without an instrument, live MIDI goes nowhere")
{
    REQUIRE (findAllMidiInputs() != nullptr);

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::routeLiveMidiInput (*edit);
    CHECK (phazite::getLiveMidiTrack (*edit) == nullptr);
}

TEST_CASE ("Playing and routing live MIDI don't count as unsaved changes")
{
    REQUIRE (findAllMidiInputs() != nullptr);

    const auto folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteLiveInputTests", {}, false);
    folder.createDirectory();

    {
        juce::String error;
        auto project = phazite::Project::create (phazite::test::getEngine(), folder, "Live", error);
        REQUIRE (project != nullptr);

        auto& edit = project->getEdit();
        edit.ensureNumberOfAudioTracks (1);
        addInstrument (*te::getAudioTracks (edit)[0]);
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());

        // What opening a project and playing it do: Tracktion records the inputs and where live MIDI
        // goes, and moves the playhead.
        phazite::routeLiveMidiInput (edit);
        CHECK (phazite::getLiveMidiTrack (edit) == te::getAudioTracks (edit)[0]);
        edit.getTransport().setPosition (te::TimePosition::fromSeconds (2.0));

        CHECK_FALSE (project->hasUnsavedChanges());
    }

    folder.deleteRecursively();
}

TEST_CASE ("Live MIDI follows the selected track, if it has an instrument")
{
    REQUIRE (findAllMidiInputs() != nullptr);

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (3);
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[0]);
    addInstrument (*tracks[2]);
    phazite::routeLiveMidiInput (*edit);
    REQUIRE (phazite::getLiveMidiTrack (*edit) == tracks[0]);

    te::SelectionManager selection (phazite::test::getEngine());
    selection.edit = edit.get();

    selection.selectOnly (tracks[2]);
    phazite::followSelectedTrack (selection);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);

    // A track without an instrument can't play it, so live MIDI stays where it was.
    CHECK_FALSE (phazite::hasInstrument (*tracks[1]));
    selection.selectOnly (tracks[1]);
    phazite::followSelectedTrack (selection);
    CHECK (phazite::getLiveMidiTrack (*edit) == tracks[2]);

    selection.deselectAll();
}

TEST_CASE ("The computer keyboard's notes play into All MIDI Ins")
{
    const auto allMidiInputs = findAllMidiInputs();
    REQUIRE (allMidiInputs != nullptr);
    auto& engine = phazite::test::getEngine();
    const auto& keys = allMidiInputs->keyboardState;

    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 60, 0.8f));
    CHECK (keys.isNoteOn (1, 60));

    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 60));
    CHECK_FALSE (keys.isNoteOn (1, 60));

    // Only notes: a controller, even All Notes Off, changes nothing.
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 62, 0.8f));
    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::allNotesOff (1));
    CHECK (keys.isNoteOn (1, 62));

    phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 62));
    CHECK_FALSE (keys.isNoteOn (1, 62));
}

TEST_CASE ("A live note sounds at once, and waits only for devices that are switched on and look ahead")
{
    auto& engine = phazite::test::getEngine();
    REQUIRE (findAllMidiInputs() != nullptr);
    phazite::test::HostedDevice device;

    auto edit = te::Edit::createSingleTrackEdit (engine);
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[0]);
    phazite::routeLiveMidiInput (*edit);
    edit->getTransport().ensureContextAllocated();
    phazite::test::settle (500);

    // A note played between two blocks, as the computer keyboard plays one: when it sounds, from the next block's start.
    const auto playNote = [&]
    {
        device.process (50);
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        const auto samples = device.samplesToFirstSound();
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        return samples;
    };

    const auto atOnce = playNote();
    CHECK (atOnce >= 0);
    CHECK (atOnce < 16);

    // A limiter on another track looks ahead, and delay compensation holds what is played back to match it...
    auto limiter = edit->getPluginCache().createNewPlugin (phazite::devices::Limiter::typeID, {});
    tracks[1]->pluginList.insertPlugin (limiter, 0, nullptr);
    phazite::test::settle (500);
    const auto latency = juce::roundToInt (limiter->getLatencySeconds() * phazite::test::HostedDevice::sampleRate);
    REQUIRE (latency > 0);
    CHECK (playNote() == atOnce + latency);

    // ...but not once it is switched off.
    limiter->setEnabled (false);
    phazite::test::settle (500);
    CHECK (playNote() == atOnce);
}

TEST_CASE ("Low latency plays live notes without delay compensation, and back with it when it is off")
{
    auto& engine = phazite::test::getEngine();
    REQUIRE (findAllMidiInputs() != nullptr);
    phazite::test::HostedDevice device;

    // A limiter, which looks ahead, on another track. (Two limiters lose live notes: docs/known-bugs.md.)
    auto edit = te::Edit::createSingleTrackEdit (engine);
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);
    addInstrument (*tracks[0]);
    tracks[1]->pluginList.insertPlugin (edit->getPluginCache().createNewPlugin (phazite::devices::Limiter::typeID, {}), 0, nullptr);
    phazite::routeLiveMidiInput (*edit);
    edit->getTransport().ensureContextAllocated();
    phazite::test::settle (500);

    const auto playNote = [&]
    {
        device.process (50);
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        const auto samples = device.samplesToFirstSound();
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        return samples;
    };

    const auto compensated = playNote();
    CHECK (compensated > 90); // the limiter's look-ahead, about 99 samples

    phazite::setLowLatency (engine, true);
    phazite::test::settle (500);
    CHECK (phazite::isLowLatency (engine));
    const auto low = playNote();
    CHECK (low >= 0);
    CHECK (low < 16);

    phazite::setLowLatency (engine, false);
    phazite::test::settle (500);
    CHECK (playNote() == compensated);
}

TEST_CASE ("Low latency leaves what renders play as the song has it")
{
    // A synth's chord into a limiter on the master, which low latency would bypass while playing.
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto& track = *te::getAudioTracks (*edit)[0];
    addInstrument (track);
    auto clip =
        track.insertMIDIClip ("Chord", edit->tempoSequence.toTime ({ te::BeatPosition(), te::BeatPosition::fromBeats (4.0) }), nullptr);

    for (int note : { 48, 55, 60, 64 })
        clip->getSequence().addNote (note, te::BeatPosition(), te::BeatDuration::fromBeats (3.0), 120, 0, nullptr);

    auto limiter = edit->getPluginCache().createNewPlugin (phazite::devices::Limiter::typeID, {});
    edit->getMasterPluginList().insertPlugin (limiter, 0, nullptr);
    limiter->getAutomatableParameterByID ("gain")->setParameter (12.0f, juce::sendNotification);

    const auto render = [&edit]
    {
        juce::TemporaryFile wav (".wav");
        te::Renderer::Parameters params (*edit);
        params.destFile = wav.getFile();
        params.audioFormat = edit->engine.getAudioFileFormatManager().getWavFormat();
        params.bitDepth = 32;
        params.sampleRateForAudio = 48000.0;
        params.blockSizeForAudio = 512;
        params.time = te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.0));
        params.tracksToDo = te::toBitSet (te::getAllTracks (*edit));
        params.useMasterPlugins = true;
        params.canRenderInMono = false;
        params.ditheringEnabled = false;
        REQUIRE (te::Renderer::renderToFile ("Low latency test", params) == params.destFile);

        const std::unique_ptr<juce::AudioFormatReader> reader (
            edit->engine.getAudioFileFormatManager().readFormatManager.createReaderFor (wav.getFile()));
        REQUIRE (reader != nullptr);
        juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
        REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
        return audio;
    };

    const auto asItIs = render();
    phazite::setLowLatency (engine, true);
    const auto whileLow = render();
    phazite::setLowLatency (engine, false);

    REQUIRE (asItIs.getNumSamples() == whileLow.getNumSamples());
    CHECK (asItIs.getMagnitude (0, asItIs.getNumSamples()) > 0.5f);
    CHECK (asItIs.getMagnitude (0, asItIs.getNumSamples()) < 0.95f); // the limiter's ceiling, -1 dB

    auto largest = 0.0f;

    for (int i = 0; i < asItIs.getNumSamples(); ++i)
        largest = std::max (largest, std::abs (asItIs.getSample (0, i) - whileLow.getSample (0, i)));

    CHECK (largest < 1.0e-6f);
}
