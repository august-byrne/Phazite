// SPDX-License-Identifier: AGPL-3.0-or-later
// How editors reach Phazite's own devices (src/model/devices/DeviceHandle.h), the knobs a device gets until it has an
// editor of its own (src/ui/devices/DeviceEditors.h), and what every device's editor repaints.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "ui/components/DisplayTicker.h"
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
    phazite::test::TestPlugin testPlugin;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto eq = addDevice (*edit, Equalizer::typeID);
    const auto device = phazite::makeDeviceHandle (*eq);

    REQUIRE (device.isValid());
    REQUIRE (device.getInfo() != nullptr);
    CHECK (device.getInfo()->name == "EQ");
    CHECK (device.getParameter ("band3Gain").getInfo()->unit == phazite::devices::Unit::decibels);
    CHECK_FALSE (device.getParameter ("volume").isValid());
    CHECK (device.getProcessor<Equalizer>() != nullptr);
    CHECK (device.getProcessor<phazite::devices::Processor>() != nullptr);

    // A plug-in isn't one of Phazite's devices.
    te::Plugin::Ptr plugin = phazite::test::TestPlugin::create (*edit);
    te::getAudioTracks (*edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
    const auto notOurs = phazite::makeDeviceHandle (*plugin);
    CHECK_FALSE (notOurs.isValid());
    CHECK (notOurs.getInfo() == nullptr);
    CHECK (notOurs.getProcessor<Equalizer>() == nullptr);

    // When the project closes, the handle has nothing.
    eq = nullptr;
    plugin = nullptr;
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

    phazite::ui::DeviceKnobs knobs ((phazite::makeDeviceHandle (*eq)));
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

    CHECK (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*eq)) != nullptr);
    CHECK (phazite::ui::createDeviceEditor (phazite::DeviceHandle()) == nullptr);
}

TEST_CASE ("Every device's editor rests while nothing it shows moves, and shows a value as it changes")
{
    // Their pictures once repainted at each tick of the displays' ticker, 30 times a second, with nothing playing and nothing
    // turned (docs/performance-audit.md, section 8).
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    const auto tick = [&ticker] (int times)
    {
        for (int i = 0; i < times; ++i)
            ticker->tickNow();
    };

    for (const auto* info : phazite::getOwnDevices())
    {
        CAPTURE (std::string (info->name));
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto plugin = addDevice (*edit, std::string (info->typeID).c_str());
        phazite::test::settle();
        auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*plugin));
        REQUIRE (editor != nullptr);

        {
            phazite::test::TestWindow window (*editor, std::max (1200, editor->getWidth()), std::max (700, editor->getHeight()));

            // Long enough for what falls back to rest: a compressor's dot falls from its lowest level shown to silence.
            tick (200);
            window.takeRepaintedArea();
            tick (30);
            CHECK (window.takeRepaintedArea().isEmpty());

            // A value turned elsewhere (automation, say), other than the mix, which the header shows: its control at least
            // shows it.
            const auto shown = std::find_if (info->parameters.begin(), info->parameters.end(),
                                             [] (const auto& parameter) { return parameter.id != phazite::devices::mixParameterID; });
            REQUIRE (shown != info->parameters.end());
            const auto parameter = plugin->getAutomatableParameterByID (juce::String (shown->id.data(), shown->id.size()));
            REQUIRE (parameter != nullptr);
            parameter->setParameter (juce::approximatelyEqual (shown->defaultValue, shown->maximum) ? shown->minimum : shown->maximum,
                                     juce::sendNotificationSync);
            tick (1);
            CHECK_FALSE (window.takeRepaintedArea().isEmpty());
        }

        editor = nullptr;
        plugin = nullptr; // before its Edit
    }
}
