// SPDX-License-Identifier: AGPL-3.0-or-later
// The reverb's editor (src/ui/devices/ReverbEditor.h).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "model/commands/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/mixer/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ReverbEditor.h"

using phazite::devices::Reverb;

TEST_CASE ("The reverb's editor shows a knob for each control, and its graph follows the decay")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Reverb::typeID, {});
    REQUIRE (phazite::insertDevice (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();

    const phazite::DeviceHandle device (*plugin);
    CHECK (dynamic_cast<phazite::ui::ReverbEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::ReverbEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    const auto knobs = phazite::test::findAll<phazite::ui::Knob> (editor);
    REQUIRE (knobs.size() == Reverb::info.parameters.size());
    phazite::test::checkChildrenApart (editor);

    // A longer decay, as an undo step the model names, and the graph shows more time.
    const auto before = editor.getShownSeconds();
    knobs[Reverb::decay]->setValue (8.0, juce::sendNotificationSync);
    editor.refresh();
    CHECK (plugin->getAutomatableParameterByID ("decay")->getCurrentValue() == doctest::Approx (8.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Decay"));
    CHECK (editor.getShownSeconds() > 3.0 * before);
}
