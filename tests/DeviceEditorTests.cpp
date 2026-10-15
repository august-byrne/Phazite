// SPDX-License-Identifier: AGPL-3.0-or-later
// How editors reach Phazite's own devices (src/model/devices/DeviceHandle.h), and the knobs a device gets until it has
// an editor of its own (src/ui/devices/DeviceEditors.h).
#include "TestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "model/commands/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "ui/devices/DeviceEditors.h"

namespace
{
using phazite::devices::Equalizer;

te::Plugin::Ptr addDevice (te::Edit& edit, const char* type)
{
    auto plugin = edit.getPluginCache().createNewPlugin (type, {});
    te::getAudioTracks (edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
    return plugin;
}
} // namespace

TEST_CASE ("A device handle reaches one of Phazite's devices, and nothing else")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto eq = addDevice (*edit, Equalizer::typeID);
    const phazite::DeviceHandle device (*eq);

    REQUIRE (device.isValid());
    REQUIRE (device.getInfo() != nullptr);
    CHECK (device.getInfo()->name == "EQ");
    CHECK (device.getParameter ("band3Gain").getInfo()->unit == phazite::devices::Unit::decibels);
    CHECK_FALSE (device.getParameter ("volume").isValid());
    CHECK (device.getProcessor<Equalizer>() != nullptr);
    CHECK (device.getProcessor<phazite::devices::Processor>() != nullptr);

    // Tracktion's own devices aren't Phazite's.
    auto reverb = addDevice (*edit, te::ReverbPlugin::xmlTypeName);
    const phazite::DeviceHandle notOurs (*reverb);
    CHECK_FALSE (notOurs.isValid());
    CHECK (notOurs.getInfo() == nullptr);
    CHECK (notOurs.getProcessor<Equalizer>() == nullptr);

    // When the project closes, the handle has nothing.
    eq = nullptr;
    reverb = nullptr;
    edit = nullptr;
    CHECK_FALSE (device.isValid());
    CHECK (device.getProcessor<Equalizer>() == nullptr);
    CHECK_FALSE (device.getParameter ("band3Gain").isValid());
}

TEST_CASE ("A device without an editor of its own gets a knob for each parameter, showing its unit")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto eq = addDevice (*edit, Equalizer::typeID);
    edit->getUndoManager().clearUndoHistory();

    phazite::ui::DeviceKnobs knobs ((phazite::DeviceHandle (*eq)));
    REQUIRE (knobs.getNumKnobs() == 50);

    auto& frequency = knobs.getKnob (Equalizer::getParameterIndex (0, Equalizer::frequency));
    CHECK (frequency.getTitle() == juce::String ("Band 1 Frequency"));
    CHECK (frequency.getTextFromValue (frequency.getValue()) == juce::String ("30.0 Hz"));
    CHECK (frequency.getValueFromText ("1.2k") == doctest::Approx (1200.0));

    // Turning a knob changes its parameter, as an undo step the model names.
    auto& gain = knobs.getKnob (Equalizer::getParameterIndex (2, Equalizer::gain));
    gain.setValue (6.0, juce::sendNotificationSync);
    CHECK (eq->getAutomatableParameterByID ("band3Gain")->getCurrentValue() == doctest::Approx (6.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Band 3 Gain"));

    CHECK (phazite::ui::createDeviceEditor (phazite::DeviceHandle (*eq)) != nullptr);
    CHECK (phazite::ui::createDeviceEditor (phazite::DeviceHandle()) == nullptr);
}
