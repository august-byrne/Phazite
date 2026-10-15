// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter as controls use it (src/model/devices/ParameterHandle.h): its value, range and default, and its changes
// as named undo steps that the model, not the control, begins.
#include "GoldenFiles.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "model/commands/Undo.h"
#include "model/devices/ParameterHandle.h"

namespace
{
te::Plugin::Ptr addReverb (te::Edit& edit)
{
    auto plugin = edit.getPluginCache().createNewPlugin (te::ReverbPlugin::xmlTypeName, {});
    te::getAudioTracks (edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
    return plugin;
}
} // namespace

TEST_CASE ("A parameter handle shows its parameter, and nothing once the plug-in is gone")
{
    const phazite::ParameterHandle empty;
    CHECK_FALSE (empty.isValid());
    CHECK (empty.getValue() == doctest::Approx (0.0f));

    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto reverb = addReverb (*edit);
    phazite::ParameterHandle handle (reverb.get(), "room size");

    REQUIRE (handle.isValid());
    CHECK (handle.refersTo (reverb.get(), "room size"));
    CHECK_FALSE (handle.refersTo (reverb.get(), "damping"));
    CHECK (handle.getName() == juce::String ("Room Size"));
    CHECK (handle.getRange().start == doctest::Approx (0.0f));
    CHECK (handle.getRange().end == doctest::Approx (1.0f));
    CHECK (handle.getDefaultValue().value_or (-1.0f) == doctest::Approx (0.3f));
    CHECK (handle.getValue() == doctest::Approx (0.3f));
    CHECK (handle.getText (0.5f) == juce::String ("6")); // Tracktion's reverb writes its room size from 1 to 11
    CHECK_FALSE (handle.getInfo().has_value());          // not one of Phazite's own devices

    // When the project closes, the handle finds nothing and changes nothing.
    reverb = nullptr;
    edit = nullptr;
    CHECK_FALSE (handle.isValid());
    CHECK (handle.getName().isEmpty());
    handle.beginGesture();
    handle.setValue (0.5f);
    handle.endGesture();
    CHECK_FALSE (handle.isInGesture());
}

TEST_CASE ("Changes through a parameter handle are named undo steps: one per gesture, and one per single change")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto reverb = addReverb (*edit);
    phazite::test::settle();
    edit->getUndoManager().clearUndoHistory();

    phazite::ParameterHandle handle (reverb.get(), "room size"); // its steps are called "Change Room Size"

    // A drag: every value it passes through is one step, however long it takes.
    handle.beginGesture();
    CHECK (handle.isInGesture());
    handle.setValue (0.4f);
    phazite::test::settle (500);
    handle.setValue (0.6f);
    handle.setValue (0.8f);
    handle.endGesture();
    CHECK_FALSE (handle.isInGesture());
    CHECK (handle.getValue() == doctest::Approx (0.8f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Room Size"));

    phazite::undo (*edit);
    CHECK (handle.getValue() == doctest::Approx (0.3f)); // where the drag began, played again
    phazite::redo (*edit);
    CHECK (handle.getValue() == doctest::Approx (0.8f));

    // A key press: a step of its own, so undo goes back that far only.
    handle.setValue (0.5f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Room Size"));
    phazite::undo (*edit);
    CHECK (handle.getValue() == doctest::Approx (0.8f));

    // A name given for the steps.
    phazite::ParameterHandle damping (reverb.get(), "damping", "Change Reverb Damping");
    damping.setValue (0.9f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Reverb Damping"));

    // A handle that goes away during a drag keeps the drag's change.
    {
        phazite::ParameterHandle width (reverb.get(), "width");
        width.beginGesture();
        width.setValue (0.25f);
    }

    CHECK (phazite::ParameterHandle (reverb.get(), "width").getValue() == doctest::Approx (0.25f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Width"));
}

TEST_CASE ("A parameter handle describes the parameters of Phazite's own devices")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    auto& device = phazite::test::createTestDevice (*edit, phazite::test::TestGain::typeID, keep);

    const phazite::ParameterHandle mode (&device, "mode");
    const auto info = mode.getInfo();
    REQUIRE (info.has_value());
    CHECK (info->unit == phazite::devices::Unit::choice);
    REQUIRE (info->choices.size() == 2);
    CHECK (info->choices[1] == "Inverted");
    CHECK (mode.getRange().interval == doctest::Approx (1.0f));

    const phazite::ParameterHandle gain (&device, "gain");
    CHECK (gain.getRange().start == doctest::Approx (-60.0f));
    CHECK (gain.getInfo()->unit == phazite::devices::Unit::decibels);
}

TEST_CASE ("A gesture begun during another for the same project joins it: a drag that moves two parameters is one step")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto reverb = addReverb (*edit);
    phazite::test::settle();
    edit->getUndoManager().clearUndoHistory();

    phazite::ParameterHandle room (reverb.get(), "room size", "Move Both"), damping (reverb.get(), "damping");
    room.beginGesture();
    damping.beginGesture();
    CHECK (room.isInGesture());
    CHECK (damping.isInGesture());
    room.setValue (0.9f);
    damping.setValue (0.1f);
    damping.endGesture();
    room.endGesture();
    CHECK_FALSE (damping.isInGesture());

    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Move Both"));
    phazite::undo (*edit);
    CHECK (room.getValue() == doctest::Approx (0.3f));
    CHECK (damping.getValue() == doctest::Approx (0.5f));
}
