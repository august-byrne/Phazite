// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/LiveInput.h"
#include "model/Project.h"

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
    track.pluginList.insertPlugin (track.edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}), 0, nullptr);
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
