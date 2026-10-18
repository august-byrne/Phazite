// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter as controls use it (src/model/devices/ParameterHandle.h): its value, range and default, and its changes
// as named undo steps that the model, not the control, begins.
#include "GoldenFiles.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/reverb/Reverb.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/ParameterHandle.h"

namespace
{
te::Plugin::Ptr addReverb (te::Edit& edit)
{
    auto plugin = edit.getPluginCache().createNewPlugin (phazite::devices::Reverb::typeID, {});
    te::getAudioTracks (edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
    return plugin;
}
} // namespace

TEST_CASE ("A parameter handle shows its parameter, and nothing once the plug-in is gone")
{
    const phazite::ParameterHandle empty;
    CHECK_FALSE (empty.isValid());
    CHECK (empty.getValue() == doctest::Approx (0.0f));

    phazite::test::TestPlugin testPlugin;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr plugin = phazite::test::TestPlugin::create (*edit);
    te::getAudioTracks (*edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
    auto handle = phazite::makeParameterHandle (plugin.get(), phazite::test::TestPlugin::toneID);

    REQUIRE (handle.isValid());
    CHECK (handle.refersToSameParameter (phazite::makeParameterHandle (plugin.get(), phazite::test::TestPlugin::toneID, "Another Name")));
    CHECK_FALSE (handle.refersToSameParameter (phazite::makeParameterHandle (plugin.get(), phazite::test::TestPlugin::gainID)));
    CHECK_FALSE (handle.refersToSameParameter (empty));
    CHECK (empty.refersToSameParameter (phazite::ParameterHandle()));
    CHECK (handle.getName() == juce::String ("Tone"));
    CHECK (handle.getRange().start == doctest::Approx (0.0f));
    CHECK (handle.getRange().end == doctest::Approx (1.0f));
    CHECK (handle.getDefaultValue().value_or (-1.0f) == doctest::Approx (0.5f));
    CHECK (handle.getValue() == doctest::Approx (0.5f));
    CHECK (handle.getText (0.25f).startsWith ("0.25")); // as the plug-in writes it
    CHECK_FALSE (handle.getInfo().has_value());         // not one of Phazite's own devices

    // When the project closes, the handle finds nothing and changes nothing.
    plugin = nullptr;
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

    auto handle = phazite::makeParameterHandle (reverb.get(), "size"); // its steps are called "Change Size"

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
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Size"));

    phazite::undo (*edit);
    CHECK (handle.getValue() == doctest::Approx (0.5f)); // where the drag began, played again
    phazite::redo (*edit);
    CHECK (handle.getValue() == doctest::Approx (0.8f));

    // A key press: a step of its own, so undo goes back that far only.
    handle.setValue (0.5f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Size"));
    phazite::undo (*edit);
    CHECK (handle.getValue() == doctest::Approx (0.8f));

    // A name given for the steps.
    auto damping = phazite::makeParameterHandle (reverb.get(), "damping", "Change Reverb Damping");
    damping.setValue (9000.0f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Reverb Damping"));

    // A handle that goes away during a drag keeps the drag's change.
    {
        auto width = phazite::makeParameterHandle (reverb.get(), "width");
        width.beginGesture();
        width.setValue (0.25f);
    }

    CHECK (phazite::makeParameterHandle (reverb.get(), "width").getValue() == doctest::Approx (0.25f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Width"));
}

TEST_CASE ("A parameter handle describes the parameters of Phazite's own devices")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    auto& device = phazite::test::createTestDevice (*edit, phazite::test::TestGain::typeID, keep);

    const auto mode = phazite::makeParameterHandle (&device, "mode");
    const auto info = mode.getInfo();
    REQUIRE (info.has_value());
    CHECK (info->unit == phazite::devices::Unit::choice);
    REQUIRE (info->choices.size() == 2);
    CHECK (info->choices[1] == "Inverted");
    CHECK (mode.getRange().interval == doctest::Approx (1.0f));

    const auto gain = phazite::makeParameterHandle (&device, "gain");
    CHECK (gain.getRange().start == doctest::Approx (-60.0f));
    CHECK (gain.getInfo()->unit == phazite::devices::Unit::decibels);
}

TEST_CASE ("A gesture begun during another for the same project joins it: a drag that moves two parameters is one step")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto reverb = addReverb (*edit);
    phazite::test::settle();
    edit->getUndoManager().clearUndoHistory();

    auto size = phazite::makeParameterHandle (reverb.get(), "size", "Move Both");
    auto damping = phazite::makeParameterHandle (reverb.get(), "damping");
    size.beginGesture();
    damping.beginGesture();
    CHECK (size.isInGesture());
    CHECK (damping.isInGesture());
    size.setValue (0.9f);
    damping.setValue (2000.0f);
    damping.endGesture();
    size.endGesture();
    CHECK_FALSE (damping.isInGesture());

    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Move Both"));
    phazite::undo (*edit);
    CHECK (size.getValue() == doctest::Approx (0.5f));
    CHECK (damping.getValue() == doctest::Approx (6000.0f));
}
