// SPDX-License-Identifier: AGPL-3.0-or-later
// Device editors made of titled groups of controls (src/ui/devices/DevicePanel.h), as the synth's is.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/commands/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/mixer/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DevicePanel.h"

using phazite::devices::Synth;

TEST_CASE ("The synth's editor has a control for each parameter, in groups, and each control changes its parameter")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
    REQUIRE (phazite::setInstrument (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();

    auto editor = phazite::ui::createDeviceEditor (phazite::DeviceHandle (*plugin));
    auto* panel = dynamic_cast<phazite::ui::DevicePanel*> (editor.get());
    REQUIRE (panel != nullptr);
    phazite::test::TestWindow window (*panel, panel->getWidth(), panel->getHeight());
    phazite::test::checkChildrenApart (*panel);

    // Every parameter has its control: a list for a choice, a knob for the rest, titled with its whole name.
    for (const auto& parameter : Synth::info.parameters)
    {
        CAPTURE (juce::String (parameter.id.data(), parameter.id.size()).toStdString());
        auto* control = panel->findControl (parameter.id);
        REQUIRE (control != nullptr);
        CHECK (control->getTitle() == juce::String (parameter.name.data(), parameter.name.size()));
        CHECK ((dynamic_cast<juce::ComboBox*> (control) != nullptr) == (parameter.unit == phazite::devices::Unit::choice));
    }

    // Under "Filter Envelope", the filter's attack is just "Attack".
    juce::StringArray names;

    for (auto* label : phazite::test::findAll<juce::Label> (*panel))
        names.add (label->getText());

    CHECK (names.contains ("Filter Envelope"));
    CHECK (names.contains ("Shape")); // the LFO's
    CHECK_FALSE (names.contains ("Filter Attack"));

    // A knob and a list each change their parameter, as an undo step the model names.
    auto* cutoff = dynamic_cast<phazite::ui::Knob*> (panel->findControl ("cutoff"));
    REQUIRE (cutoff != nullptr);
    cutoff->setValue (800.0, juce::sendNotificationSync);
    CHECK (plugin->getAutomatableParameterByID ("cutoff")->getCurrentValue() == doctest::Approx (800.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Cutoff"));

    auto* wave = dynamic_cast<juce::ComboBox*> (panel->findControl ("osc1Wave"));
    REQUIRE (wave != nullptr);
    CHECK (wave->getText() == juce::String ("Saw"));
    wave->setSelectedItemIndex (3, juce::sendNotificationSync); // Square
    CHECK (plugin->getAutomatableParameterByID ("osc1Wave")->getCurrentValue() == doctest::Approx (3.0f));

    // Undo shows on the panel.
    phazite::undo (*edit);
    panel->refresh();
    CHECK (wave->getText() == juce::String ("Saw"));
}
