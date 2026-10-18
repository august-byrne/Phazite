// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "devices/synth/Synth.h"
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
